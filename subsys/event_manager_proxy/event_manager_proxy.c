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
	/**
	 * @brief Register for an event
	 *
	 * This command is used to register for an event between cores.
	 */
	EMP_CMD_REGISTER,
	/**
	 * @brief Start event transmission
	 *
	 * After this command is sent, there the data that follows would be
	 * treated as event data. No more registering commands may be sent.
	 */
	EMP_CMD_START,
	/**
	 * @brief Command response
	 *
	 * This command is used as an response to the command.
	 */
	EMP_CMD_RSP,
};

/**
 * @brief The command base structure
 *
 * The command structure used to decode the command type
 */
struct emp_cmd {
	/** @brief The command code */
	enum emp_cmd_code cmd;
};

/**
 * @brief The command structure used to register
 *
 * The command to register the event listener.
 */
struct emp_cmd_register {
	/** @brief The command code */
	enum emp_cmd_code cmd;
	/** @brief The identifier to use to send the event */
	const struct event_type *id;
	/** @brief The name of the event */
	char name[];
};

/**
 * @brief The command structure used for the response
 *
 * The "command" that would be sent as a response to the command received.
 */
struct emp_cmd_rsp {
	/** @brief The command code */
	enum emp_cmd_code cmd;
	/** @brief The command identifier
	 *
	 *  The identifier of the command for this response:
	 *  - For the @ref EMB_CMD_REGISTER command it is set to the value of
	 *    @ref event_manager_proxy_cmd_register::id.
	 *  - For the @ref EMB_CMD_START command it is set to NULL.
	 */
	const struct event_type *id;
	/** @brief The operation result code */
	int res;
};

/**
 * @brief IPC channel data
 *
 * The data needed to organize communication between cores
 */
struct emp_ipc_data {
	/**
	 * @brief Endpoint
	 *
	 * The endpoint used for communication
	 */
	struct ipc_ept ept;
	/**
	 * @brief Endpoint configuration
	 *
	 * Local copy of the configuration of the endpoint.
	 * We need to set @ref ipc_ept_cfg::priv field here at runtime.
	 */
	struct ipc_ept_cfg ept_cfg;
	/**
	 * @brief Flag that marks that this structure is used
	 */
	bool used;
	/**
	 * @brief Flag that marks the endpoint as started
	 *
	 * Started endpoint sends no more configuration commands.
	 * All data received here has to be treated as an event.
	 */
	bool started;
	/**
	 * @brief Event used to mark endpoint bonded
	 *
	 * This event is set when the endpoint is connected
	 * on the other core and ready to transmit data.
	 * Never clear this event.
	 */
	struct k_event bonded;
	/**
	 * @brief Semaphore that marks received response
	 */
	struct k_sem rsp_ready;
	/**
	 * @brief Response structure
	 *
	 * The structure serves 2 purposes:
	 * 1. It holds last response received.
	 * 2. It serves the data required to send response from workqueue.
	 */
	struct {
		const struct event_type *id;
		int res;
	} rsp;
	/**
	 * @brief Response work
	 *
	 * The work used to send the response.
	 * @note
	 * We wish to send responses to the command received from IPC.
	 * The issue is that sending it directly from the @em received callback
	 * may put this thread to wait for the buffer being available.
	 * If the only buffer available is the one used by the callback
	 * we may wait here forever as it would be freed only when the callback function returns.
	 */
	struct k_work rsp_work;
};


/* Create structures just for the linker to count its sizes */
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

/**
 * @brief The configuration of the endpoint
 *
 * The base configuration of the endpoint.
 */
static const struct ipc_ept_cfg emp_ipc_ept_cfg_base = {
	.name = "event_manager_proxy",
	.cb = {
		.bound    = ipc_ept_bound,
		.received = ipc_ept_recv,
		.error    = ipc_ept_error
	},
};

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
	if (!emp_started)
		return;

	size_t idx = ev2idx(eh->type_id);
	size_t n;

	for (n = 0; n < CONFIG_EVENT_MANAGER_PROXY_CH_COUNT; ++n) {
		const struct event_type *remote_ev = event_manager_proxy_array[idx].event[n];
		struct emp_ipc_data *ipc = &emp_ipc_data[n];

		if (!ipc->used || !ipc->started) {
			continue;
		}
		if (remote_ev) {
			size_t len = event_manager_event_size(eh);
			struct event_header *remote_eh = event_manager_alloc(len);
			int ret;

			memcpy(remote_eh, eh, len);
			remote_eh->type_id = remote_ev;
			ret = ipc_service_send(&ipc->ept, remote_eh, len);
			event_manager_free(remote_eh);
			if (ret < 0) {
				LOG_ERR("Cannot send event to remote %d", n);
				__ASSERT_NO_MSG(false);
			}
		}
	}
}

EVENT_HOOK_POSTPROCESS_REGISTER(event_manager_proxy_on_event_process);

int event_manager_proxy_add_remote(const struct device *instance)
{
	int ret;

	__ASSERT(
		(__end_event_proxy_array - event_manager_proxy_array)
		==
		(_event_type_list_end - _event_type_list_start)
		, "Event manager proxy array size does not match event type array size (%u != %u)",
			(__end_event_proxy_array - event_manager_proxy_array),
			(_event_type_list_end - _event_type_list_start));

	__ASSERT_NO_MSG(find_ipc_by_instance(instance) == NULL);
	ret = ipc_service_open_instance(instance);
	if (ret && ret != -EALREADY) {
		LOG_ERR("IPC service open instance failure: %d", ret);
		return ret;
	}
	/* Searching for a space for the instance */
	for (size_t n = 0; n < ARRAY_SIZE(emp_ipc_data); ++n) {
		struct emp_ipc_data *ipc_data = &emp_ipc_data[n];

		if (!ipc_data->used) {
			int ret;

			ipc_data->ept_cfg = emp_ipc_ept_cfg_base;
			ipc_data->ept_cfg.priv = ipc_data;
			ret = ipc_service_register_endpoint(
				instance,
				&ipc_data->ept,
				&ipc_data->ept_cfg);
			if (ret) {
				LOG_ERR("Error registering endpoint in ipc service (%d)", ret);
				return ret;
			}
			ipc_data->used = true;
			ipc_data->started = false;
			k_event_init(&ipc_data->bonded);
			ret = k_sem_init(&ipc_data->rsp_ready, 0, 1);
			__ASSERT_NO_MSG(ret == 0);
			k_work_init(&ipc_data->rsp_work, send_rsp_worker);
			return 0;
		}
	}

	LOG_ERR("No free space for another remote");
	return -ENOMEM;
}

int event_manager_proxy_register_listener(
	const struct device *instance,
	const struct event_type *local_event_id,
	const char *remote_event_name)
{
	int ret;
	struct emp_ipc_data *ipc;
	struct emp_cmd_register *cmd;
	size_t cmd_size;

	__ASSERT_NO_MSG(!emp_started);
	ipc = find_ipc_by_instance(instance);
	__ASSERT_NO_MSG(ipc);

	/* Waiting till the endpoint is bonded */
	if (!k_event_wait(&ipc->bonded, 0x1, false, EMP_BOND_TIMEOUT)) {
		LOG_ERR("IPC bond timeout");
		return -EPIPE;
	}

	/* Preparing and sending the command */
	cmd_size = sizeof(*cmd) + strlen(remote_event_name) + 1;
	cmd = k_malloc(cmd_size);
	__ASSERT_NO_MSG(cmd);
	memset(cmd, 0, cmd_size);
	cmd->cmd = EMP_CMD_REGISTER;
	cmd->id  = local_event_id;
	strcpy(cmd->name, remote_event_name);

	ret = ipc_service_send(&ipc->ept, cmd, cmd_size);
	k_free(cmd);
	if (ret < 0) {
		return ret;
	}
	/* Waiting for response */
	if (k_sem_take(&ipc->rsp_ready, EMP_RSP_TIMEOUT)) {
		ret = -ETIME;
	} else {
		__ASSERT_NO_MSG(local_event_id == ipc->rsp.id);
		ret = ipc->rsp.res;
	}

	return ret;
}

int event_manager_proxy_start(void)
{
	/* Send start command to the all connected cores */
	int ret;
	static struct emp_cmd cmd = {.cmd = EMP_CMD_START};

	__ASSERT_NO_MSG(!emp_started);

	for (size_t n = 0; n < ARRAY_SIZE(emp_ipc_data); ++n) {
		if (emp_ipc_data[n].used) {
			struct emp_ipc_data *ipc = &emp_ipc_data[n];

			/* Waiting till the endpoint is bonded */
			if (!k_event_wait(&ipc->bonded, 0x1, false, EMP_BOND_TIMEOUT)) {
				LOG_ERR("IPC bond timeout");
				return -EPIPE;
			}

			ret = ipc_service_send(&ipc->ept, &cmd, sizeof(cmd));
			if (ret < 0) {
				return ret;
			}
		}
	}
	/* Mark this ipc instance started */
	emp_started = true;
	return 0;
}

int event_manager_proxy_wait_for_remotes(k_timeout_t timeout)
{
	__ASSERT_NO_MSG(emp_started);

	if (!k_event_wait(&emp_all_remotes_started, 0x1, false, timeout)) {
		return -ETIME;
	}
	return 0;
}
