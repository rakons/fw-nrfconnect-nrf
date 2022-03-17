/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr.h>
#include <spinlock.h>
#include <event_manager.h>
#include <event_manager_proxy.h>
#include <logging/log.h>
#include <ipc/ipc_service.h>

LOG_MODULE_REGISTER(event_manager_proxy, CONFIG_EVENT_MANAGER_LOG_LEVEL);


#define EMP_BOND_TIMEOUT K_MSEC(CONFIG_EVENT_MANAGER_PROXY_BOND_TIMEOUT_MS)
#define EMP_RSP_TIMEOUT  K_MSEC(CONFIG_EVENT_MANAGER_PROXY_RSP_TIMEOUT_MS)

/**
 * @brief Data structure used by event manager proxy
 */
struct emp_data {
	const struct event_type *event[CONFIG_EVENT_MANAGER_PROXY_CH_COUNT];
};

/**
 * @brief Command codes used by the proxy
 *
 * Commands used for data transfer between cores.
 */
enum emp_cmd_code {
	/** Register for an event on another core. */
	EMP_CMD_REGISTER,

	/** Start event transmission.
	 * No commands are allowed after this point. The following data is related to events.
	 */
	EMP_CMD_START,

	/** Command response. */
	EMP_CMD_RSP,

	/** Number of commands. */
	EMP_CMD_COUNT
};

/**
 * @brief The command base structure.
 */
struct emp_cmd {
	/** The command code */
	enum emp_cmd_code cmd;
};

/**
 * @brief The command structure used to register
 */
struct emp_cmd_register {
	/** The command code. */
	enum emp_cmd_code cmd;

	/** The event identifier to be used for the matching name. */
	const struct event_type *id;

	/** The name of the event. */
	char name[];
};

/**
 * @brief The response command structure.
 */
struct emp_cmd_rsp {
	/** The command code. */
	enum emp_cmd_code cmd;

	/** The command identifier.
	 *
	 *  The identifier of the command for this response:
	 *  - For the @ref EMB_CMD_REGISTER command it is set to the value of
	 *    @ref event_manager_proxy_cmd_register::id.
	 *  - For the @ref EMB_CMD_START command it is set to NULL.
	 */
	const struct event_type *id;

	/** The operation result code. */
	int res;
};

/**
 * @brief IPC channel data
 *
 * The data needed to organize communication between cores
 */
struct emp_ipc_data {
	/** Endpoint used for communication. */
	struct ipc_ept ept;

	/** Endpoint configuration - local copy.
	 * We need to set @ref ipc_ept_cfg::priv field here at runtime.
	 */
	struct ipc_ept_cfg ept_cfg;

	/** Flag marking this structure is used. */
	bool used;

	/** Flag marking the endpoint is started
	 *
	 * Started endpoint sends no more configuration commands.
	 * All data received here has to be treated as events.
	 */
	bool started;

	/** Bonded endpoint kernel event.
	 *
	 * This event is triggered when the endpoint is connected
	 * on the other core and ready to transmit data.
	 * Never clear this event.
	 */
	struct k_event bonded;

	/** Semaphore synchronizing responses. */
	struct k_sem rsp_ready;

	/** Response structure.
	 *
	 * The structure serves 2 purposes:
	 * 1. It holds last response received.
	 * 2. It serves the data required to send response from workqueue.
	 */
	struct {
		const struct event_type *id;
		int res;
	} rsp;

	/** Response work
	 *
	 * The work used to send the response.
	 *
	 * @note
	 * We wish to send responses to the command received from IPC.
	 * The issue is that sending it directly from the @em received callback
	 * may put this thread to wait for the buffer being available.
	 * If the only buffer available is the one used by the callback
	 * we may wait here forever as it would be freed only when the callback function returns.
	 */
	struct k_work rsp_work;
};


/* Helpers - allow linker to get information about these structure sizes. */
struct event_type event_manager_type_size_check
	__attribute__((__section__("event_manager_type_size")));
struct emp_data event_manger_proxy_type_ptr_size_check
	__attribute__((__section__("event_manger_proxy_type_ptr_size")));

/**
 * @brief Access to event proxy linker script section
 *
 * This section would contain an array of @ref event_manager_proxy_data
 * with the size matching number of events in the system.
 */
extern struct emp_data event_manager_proxy_array[];

extern struct emp_data __end_event_proxy_array[];

/**
 * @brief This instance of event manager was started
 *
 * The flag that holds the information that this instance was started.
 * Started means that configuration data cannot be sent anymore
 * and all the configured events are transmitted to the started cores.
 */
static bool emp_started;

/**
 * @brief The event that marks the fact that all remotes are ready
 *
 * This event would be set when we receive the information that the last
 * remote has sent the start command.
 */
static K_EVENT_DEFINE(emp_all_remotes_started);

/**
 * @brief Data required for IPC communication
 *
 * Endpoints and all the auxiliary data required for communication with other cores.
 */
static struct emp_ipc_data emp_ipc_data[CONFIG_EVENT_MANAGER_PROXY_CH_COUNT];

/**
 * @brief Find IPC structure by the given instance
 *
 * @param instance The instance used for IPC service to transfer data between cores.
 *
 * @retval NULL Instance not found (not added?).
 * @retval other The pointer to the requested instance.
 */
static struct emp_ipc_data *find_ipc_by_instance(const struct device *instance)
{
	struct emp_ipc_data *found = NULL;

	for (size_t n = 0; n < ARRAY_SIZE(emp_ipc_data); ++n) {
		if (emp_ipc_data[n].used) {
			if (emp_ipc_data[n].ept.instance == instance) {
				found = &emp_ipc_data[n];
				break;
			}
		}
	}
	return found;
}

/**
 * @brief Search for an event identified by its name
 *
 * @param name The name of the event
 *
 * @retval NULL Cannot find event with given name
 * @retval pointer Pointer to the event type structure
 */
static struct event_type *find_event_by_name(const char *name)
{
	struct event_type *found = NULL;

	STRUCT_SECTION_FOREACH(event_type, et) {
		if (!strcmp(et->name, name)) {
			found = et;
			break;
		}
	}
	return found;
}

/**
 * @brief Convert event type to its index
 *
 * @param et Event type pointer
 *
 * @return Index of the event
 */
static size_t ev2idx(const struct event_type *et)
{
	__ASSERT_NO_MSG(et >= _event_type_list_start);
	__ASSERT_NO_MSG(et <  _event_type_list_end);
	return et - _event_type_list_start;
}

/**
 * @brief Convert endpoint pointer to the index
 *
 * The function that changes the pointer used by ipc callbacks to the index
 * in @ref emp_ipc_data array.
 *
 * @param priv The pointer of the related element in the @ref emp_ipc_data array.
 *
 * @return The index of provided pointer in the array.
 */
static size_t ept2idx(const void *priv)
{
	const struct emp_ipc_data *ipc = priv;

	__ASSERT_NO_MSG(ipc >= emp_ipc_data);
	__ASSERT_NO_MSG(ipc < (emp_ipc_data + CONFIG_EVENT_MANAGER_PROXY_CH_COUNT));
	return ipc - emp_ipc_data;
}

/**
 * @brief Worker that sends the response to the command.
 *
 * The response work is submitted by @ref submit_rsp function.
 *
 * @param work The pointer to @ref emp_ipc_data::rsp_work.
 */
static void send_rsp_worker(struct k_work *work)
{
	struct emp_ipc_data *ipc = CONTAINER_OF(work, struct emp_ipc_data, rsp_work);
	const struct emp_cmd_rsp rsp = {
		.cmd = EMP_CMD_RSP,
		.id  = ipc->rsp.id,
		.res = ipc->rsp.res
	};
	int ret;

	__ASSERT_NO_MSG(k_sem_count_get(&ipc->rsp_ready) == 0);

	ret = ipc_service_send(&ipc->ept, &rsp, sizeof(rsp));
	__ASSERT_NO_MSG(ret >= 0);
}

/**
 * @brief Submit the response to the received command
 *
 * Function prepares response to the received command and passes its execution to the workqueue.
 *
 * @param ipc The structure that describes remote connection.
 * @param id  The identifier of the command for which we are responding.
 * @param res The result code.
 *
 * @retval 0 Function finished successfully.
 * @retval other The error code.
 */
static int submit_rsp(struct emp_ipc_data *ipc, const struct event_type *id, int res)
{
	int ret;

	/* When we are going to send the rsp - there should not be response prepared already */
	ret = k_sem_take(&ipc->rsp_ready, K_NO_WAIT);
	if (ret == 0) {
		LOG_ERR("Response received ready during response submitting");
	}

	ipc->rsp.id = id;
	ipc->rsp.res = res;

	ret = k_work_submit(&ipc->rsp_work);
	return ret;
}

/**
 * @brief The endpoint bound callback
 *
 * This callback is called when the endpoint is bonded on the remote core.
 * When this is received the communication between cores is possible.
 *
 * @param priv The pointer of the related element of the @ref emp_ipc_data array.
 */
static void ipc_ept_bound(void *priv)
{
	struct emp_ipc_data *ipc = priv;

	k_event_set(&(ipc->bonded), 0x1);
}

/**
 * @brief The data received on endpoint callback
 *
 * This callback is called when there is some data received from the remote core.
 *
 * @param data The pointer to the data received.
 * @param len  The length of the data received.
 * @param priv The pointer of the related element of the @ref emp_ipc_data array.
 */
static void ipc_ept_recv(const void *data, size_t len, void *priv)
{
	struct emp_ipc_data *ipc = priv;

	/* We are expecting this callback to be called on the thread context */
	__ASSERT_NO_MSG(!k_is_in_isr());

	if (ipc->started && emp_started) {
		/* Incoming event data - copy and submit */
		void *ev = event_manager_alloc(len);

		memcpy(ev, data, len);
		_event_submit(ev);
	} else {
		/* Incoming command - execute */
		if (len < sizeof(struct emp_cmd)) {
			LOG_ERR("Unexpected command size received: %u", len);
			__ASSERT_NO_MSG(false);
		}
		switch (((struct emp_cmd *)data)->cmd) {
		case EMP_CMD_REGISTER:
		{
			/* Only command responses are accepted after the interfacace is started */
			__ASSERT_NO_MSG(!ipc->started);
			/* Expecting valid command header and at least 1 name character */
			if (len < (sizeof(struct emp_cmd_register) + 2)) {
				LOG_ERR("Unexpected register command size (%u)", len);
				__ASSERT_NO_MSG(false);
			}
			const struct emp_cmd_register *cmd = data;
			struct event_type *et = find_event_by_name(cmd->name);
			int ret = 0;

			if (!et) {
				LOG_ERR("Cannot find requested event: \"%s\"",
					log_strdup(cmd->name));
				ret = -ENOENT;
			} else {
				size_t context_idx = ept2idx(priv);
				size_t ev_idx = ev2idx(et);

				event_manager_proxy_array[ev_idx].event[context_idx] = cmd->id;
				LOG_DBG("Remote event registered for \"%s\" -> ipc %d",
					log_strdup(cmd->name),
					context_idx);
			}
			ret = submit_rsp(ipc, cmd->id, ret);
			__ASSERT_NO_MSG(ret);
			break;
		}
		case EMP_CMD_START:
		{
			/* Only command responses are accepted after the interfacace is started */
			__ASSERT_NO_MSG(!ipc->started);
			unsigned int n;
			bool all_started;

			ipc->started = true;
			LOG_DBG("Event transmission on ipc %d started", ept2idx(priv));
			/* Check if all remote cores are started and mark the fact */
			for (n = 0, all_started = true; n < ARRAY_SIZE(emp_ipc_data); ++n) {
				struct emp_ipc_data *ipc_data = &emp_ipc_data[n];

				if (ipc_data->used && !ipc_data->started) {
					all_started = false;
					break;
				}
			}
			if (all_started) {
				k_event_set(&emp_all_remotes_started, 0x1);
			}
			break;
		}
		case EMP_CMD_RSP:
		{
			if (len > sizeof(struct emp_cmd_rsp)) {
				LOG_ERR("Unexpected command response received: %u", len);
				__ASSERT_NO_MSG(false);
			}
			/** This should never happen that we have a valid data in a response copy
			 *  when new response is received.
			 */
			__ASSERT_NO_MSG(k_sem_count_get(&ipc->rsp_ready) == 0);
			__ASSERT_NO_MSG(!k_work_is_pending(&ipc->rsp_work));
			const struct emp_cmd_rsp *rsp = data;

			ipc->rsp.id  = rsp->id;
			ipc->rsp.res = rsp->res;
			k_sem_give(&ipc->rsp_ready);
			break;
		}
		default:
			LOG_ERR("Unsupported command received: %u", ((struct emp_cmd *)data)->cmd);
			__ASSERT_NO_MSG(false);
			break;
		}
	}
}

/**
 * @brief The error on the endpoint callback
 *
 * @param message The message from the backend.
 * @param priv    The pointer of the related element of the @ref emp_ipc_data array.
 */
static void ipc_ept_error(const char *message, void *priv)
{
	LOG_ERR("Endpoint error: \"%s\"", log_strdup(message));
	__ASSERT_NO_MSG(false);
}

static int event_manager_proxy_init(void)
{
	memset(event_manager_proxy_array,
	       0,
	       ((char *)__end_event_proxy_array - (char *)event_manager_proxy_array));
	return 0;
}

EVENT_MANAGER_HOOK_POSTINIT_REGISTER(event_manager_proxy_init);

static void event_manager_proxy_on_event_process(const struct event_header *eh)
{
	if (!emp_started) {
		return;
	}

	struct emp_data *emp = &event_manager_proxy_array[ev2idx(eh->type_id)];

	for (size_t i = 0; i < CONFIG_EVENT_MANAGER_PROXY_CH_COUNT; ++i) {
		const struct event_type *remote_ev = emp->event[i];
		struct emp_ipc_data *ipc = &emp_ipc_data[i];

		if (!ipc->used || !ipc->started || (remote_ev == NULL)) {
			continue;
		}

		size_t len = event_manager_event_size(eh);
		struct event_header *remote_eh = event_manager_alloc(len);

		memcpy(remote_eh, eh, len);
		remote_eh->type_id = remote_ev;

		int ret = ipc_service_send(&ipc->ept, remote_eh, len);
		event_manager_free(remote_eh);
		if (ret < 0) {
			LOG_ERR("Cannot send event to remote %d", i);
			__ASSERT_NO_MSG(false);
		}
	}
}

EVENT_HOOK_POSTPROCESS_REGISTER(event_manager_proxy_on_event_process);


static int add_ipc_instace(struct emp_ipc_data *ipc, const struct device *instance)
{
	int ret = ipc_service_open_instance(instance);
	if (ret && ret != -EALREADY) {
		LOG_ERR("IPC service open instance failure: %d", ret);
		return ret;
	}

	ipc->started = false;
	ipc->ept_cfg = (struct ipc_ept_cfg) {
		.name = "event_manager_proxy",
		.cb = {
			.bound    = ipc_ept_bound,
			.received = ipc_ept_recv,
			.error    = ipc_ept_error
		},
		.priv = ipc
	};

	ret = ipc_service_register_endpoint(instance, &ipc->ept, &ipc->ept_cfg);
	if (ret) {
		LOG_ERR("Error registering endpoint in ipc service (%d)", ret);
		return ret;
	}

	k_event_init(&ipc->bonded);
	ret = k_sem_init(&ipc->rsp_ready, 0, 1);
	__ASSERT_NO_MSG(ret == 0);
	k_work_init(&ipc->rsp_work, send_rsp_worker);

	ipc->used = true;

	return 0;
}

int event_manager_proxy_add_remote(const struct device *instance)
{
	__ASSERT(
		(__end_event_proxy_array - event_manager_proxy_array)
		==
		(_event_type_list_end - _event_type_list_start)
		, "Event manager proxy array size does not match event type array size (%u != %u)",
			(__end_event_proxy_array - event_manager_proxy_array),
			(_event_type_list_end - _event_type_list_start));
	__ASSERT_NO_MSG(find_ipc_by_instance(instance) == NULL);

	for (size_t i = 0; i < ARRAY_SIZE(emp_ipc_data); ++i) {
		if (!emp_ipc_data[i].used) {
			return add_ipc_instace(&emp_ipc_data[i], instance);
		}
	}

	LOG_ERR("No free space for another remote");

	return -ENOMEM;
}

static int send_register_command(struct emp_ipc_data *ipc, const struct event_type *local_event_id,
		const char *remote_event_name)
{
	__ASSERT_NO_MSG(ipc);

	if (!k_event_wait(&ipc->bonded, 0x1, false, EMP_BOND_TIMEOUT)) {
		LOG_ERR("IPC bond timeout");
		return -EPIPE;
	}

	/* Preparing and sending the command */
	struct emp_cmd_register *cmd;
	size_t size = sizeof(*cmd) + strlen(remote_event_name) + 1;

	cmd = k_malloc(size);

	if (cmd == NULL) {
		LOG_ERR("No mem");
		return -ENOMEM;
	}

	cmd->cmd = EMP_CMD_REGISTER;
	cmd->id  = local_event_id;
	strcpy(cmd->name, remote_event_name);

	int ret = ipc_service_send(&ipc->ept, cmd, size);

	k_free(cmd);

	if (ret < 0) {
		return ret;
	}

	return 0;
}

static int wait_for_register_respose(struct emp_ipc_data *ipc, const struct event_type *local_event_id)
{
	if (k_sem_take(&ipc->rsp_ready, EMP_RSP_TIMEOUT)) {
		return -ETIME;
	}

	__ASSERT_NO_MSG(local_event_id == ipc->rsp.id);
	if (local_event_id != ipc->rsp.id) {
		return -EFAULT;
	}

	return ipc->rsp.res;
}

int event_manager_proxy_register_listener(const struct device *instance,
		const struct event_type *local_event_id, const char *remote_event_name)
{
	__ASSERT_NO_MSG(!emp_started);

	struct emp_ipc_data *ipc = find_ipc_by_instance(instance);

	int ret = send_register_command(ipc, local_event_id, remote_event_name);

	if (!ret) {
		ret = wait_for_register_respose(ipc, local_event_id);
	}

	return ret;
}

static int send_start_command(struct emp_ipc_data *ipc)
{
	static const struct emp_cmd cmd = {.cmd = EMP_CMD_START};

	__ASSERT_NO_MSG(ipc);

	if (!ipc->used) {
		return 0;
	}

	if (!k_event_wait(&ipc->bonded, 0x1, false, EMP_BOND_TIMEOUT)) {
		LOG_ERR("IPC bond timeout");
		return -EPIPE;
	}

	int ret = ipc_service_send(&ipc->ept, &cmd, sizeof(cmd));
	if (ret < 0) {
		return ret;
	}

	return 0;
}

int event_manager_proxy_start(void)
{
	int ret = 0;

	__ASSERT_NO_MSG(!emp_started);

	for (size_t i = 0; (i < ARRAY_SIZE(emp_ipc_data)) && !ret; ++i) {
		ret = send_start_command(&emp_ipc_data[i]);
	}

	if (!ret) {
		emp_started = true;
	}

	return ret;
}

int event_manager_proxy_wait_for_remotes(k_timeout_t timeout)
{
	__ASSERT_NO_MSG(emp_started);

	if (!k_event_wait(&emp_all_remotes_started, 0x1, false, timeout)) {
		return -ETIME;
	}

	return 0;
}
