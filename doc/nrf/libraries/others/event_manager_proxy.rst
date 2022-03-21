.. _event_manager_proxy:

Event Manager proxy
###################

.. contents::
   :local:
   :depth: 2

The Event Manager proxy is a library that passes events between cores.
It connects two different instances on different cores passing registered messages using IPC Service.

See the :ref:`event_manager_proxy_sample` sample for and exaple how to use the library.

Configuration
*************

To use the Event Manager proxy, enable the :kconfig:`CONFIG_EVENT_MANAGER_PROXY` Kconfig option.
This option depends on :kconfig:`CONFIG_IPC_SERVICE`.
Note that the IPC Service has to be configured together with used backend.

When enabling Event Manager proxy the required hooks in `event_manager` would be also enabled.

Additional configuration
========================

You can also set the following Kconfig options when working with Event Manager proxy:

* :kconfig:`CONFIG_EVENT_MANAGER_PROXY_CH_COUNT` - Number of IPC instances that would be used.
  This option should reflect the number of cores where the events should be exchanged.
  Note that having 2 cores means that we exchange events with 1.
* :kconfig:`EVENT_MANAGER_PROXY_BOND_TIMEOUT_MS` - The timeout when bonding.

Implementing proxy for Event Manager
************************************

When compiling the code that uses Event Manager proxy make sure that the event definitions that would be shared between the cores are compiled for both cores.
The declarations should be accessible during both codes compilations.

Application that wish to use Event Manager proxy requires special initialization process.
Event Manager proxy it initialized together with the event_manager:

    .. code-block:: c

       /* Initialize Event Manager and Event Manager proxy */
       ret = event_manager_init();
       /* Error handling */

After initialization all remote IPC instances need to be added:

    .. code-block:: c

       ret = event_manager_proxy_add_remote(ipc1_instance);
       /* Error handling */
       ret = event_manager_proxy_add_remote(ipc2_instance);
       /* Error handling */

Then the listerner ask the submitter to pass the selected event.
This may be done by :c:func:`event_manager_proxy_register_listener` by passing all the required arguments.
The auxiliary macro :c:macro:`EVENT_MANAGER_PROXY_REGISTER_LISTENER` prepares all the arguments identifiers using the event definition:

    .. code-block:: c

       #include <event1_definition_file.h>
       #include <event2_definition_file.h>

       ret = EVENT_MANAGER_PROXY_REGISTER_LISTENER(ipc1_instance, event1);
       /* Error handling */
       ret = EVENT_MANAGER_PROXY_REGISTER_LISTENER(ipc1_instance, event2);
       /* Error handling */

When all the events are registered it is time to configure the Event Manager proxy into active state:

    .. code-block:: c

       ret = event_manager_proxy_start();
       /* Error handling */

Since that very moment, no more configuration messages are allowed between cores.
Now only registered events would be transmitted.

The events are transmitted between both cores only when the :c:func:`event_manager_proxy_start` functions are called on both cores.
If your code needs to be sure that the link between cores is active before continuing use :c:func:`event_manager_proxy_wait_for_remotes`.
This function would block until all registered instances report its readines.

The :c:func:`event_manager_proxy_wait_for_remotes` call is not required.
It might be used if we wish to send some event being sure that it would be transmitted to all the registed remotes.

After the link is initialized the registered events from remote core appears in the local event queue and are processed exactly like any other local message.

Event Manager proxy implementation details
******************************************

The proxy uses some of the Event Manager hooks to connect with the manager.

Initialization hook usage
=========================

.. include:: event_manager.rst
   :start-after: em_initialization_hook_start
   :end-before: em_initialization_hook_end

The Event Manager proxy uses the hook to append itself to the initialization procedure.

Tracing hook usage
==================

.. include:: event_manager.rst
   :start-after: em_tracing_hooks_start
   :end-before: em_tracing_hooks_end

The Event Manager proxy uses only postprocess hook to send the event after processing on local core to the remote core that registered as a listener.

Registering remote events listener
==================================

A core that wish to listen events from the remote, during the initialization process sends REGISTER command to that core.
The register command passes two arguments:

* The event id - it is the local core id that it wishes to receive when the event is postprocessed on the remote core.
* The event name - the name of the event to search for.

The remote core during the command processing searches for an event with given name and registers the given event id in an array of events.
The created array of events directly reflect the array of event types.
This way the complexity of remote event id searching that is connected with currently processed event has O(1) complexity.
The most time consuming searching is realized during initialization, where events are searched by name with O(N) complexity.

Sending the event to the remote core
====================================

After the event is processed locally, event postprocess hook in the Event Manager proxy is executed.
Proxy gets event index and then checks the matched position in remote array.
If the event is registered for this event for any of added remote IPC instance event is copied as it is, the event id is replaced by the id requested by the remote and in such a form it is transmitted to the remote.
This way remote can copy it as is and use it as its own local event.

Passing the event from remote core
==================================

After the remote and local core started the Event Manager proxy by the calling of the :c:func:`event_manager_proxy_start` function, every incoming data is treated as a single event.
New event is allocated by the usage of :c:func:`event_manager_alloc` function and the it is just submitted to the event queue by the :c:func:`_event_submit`.
Since that very moment the event is treated the same like any locally generated event.

Note about pointers in event
============================

Keep in mind that if any of the shared events between the cores provides any kind of memory pointer, the pointed memory has to be available for the target core if it wishes to access it.

.. _event_manager_proxy_api:

API documentation
*****************

| Header file: :file:`include/event_manager_proxy.h`
| Source files: :file:`subsys/event_manager_proxy/`

.. doxygengroup:: event_manager_proxy
   :project: nrf
   :members:
