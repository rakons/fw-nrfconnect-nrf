.. _event_manager_proxy_sample:

Event Manager proxy
###################

.. contents::
   :local:
   :depth: 2

The Event Manager proxy sample demonstrates the functionality of event proxing :ref:`event_manager_proxy` to transport events between cores.
The sample demonstrates also the proposed application structure where common events declarations and definitions are available for both cores.

Requirements
************

The sample supports the following development kits:

.. table-from-rows: /includes/sample_board_rows.txt
   :header: heading
   :rows: nrf5340dk_nrf5340_cpuapp

Overview
********

Files and directories layout
============================

The application is divided into host and remote part.
The host part configuration is placed directly in main sample folder.
The remote part configuration is placed in remote folder.
The remote part is added into host part as a subproject - it means that it will be builded as a part of host application build.

Both parts uses common events declarations and definitions that are placed in common_events directory.
Both parts uses modules that are placed in modules directory.

Functionality
=============

Event Manager proxy sample copies the functionality from `event_manager_sample` sample but splits it between two cores.
On the remote core we have functionality that relates to simulated sensor, while host core takes care on configuration and calculates statistics.

The sample copies modules from `event_manager_sample` sample:

.. include:: ../event_manager/README.rst
   :start-after: event_manager_sample_modules_start
   :end-before: event_manager_sample_modules_end

The only change is that Statistic module now counts also control messages.
The change was implemented to have an opportunity to transfer more different messages between cores.

Configuration
*************

|config|

Building and running
********************
.. |sample path| replace:: :file: samples/event_manager_proxy

.. include:: /includes/build_and_run.txt

Please note that in current sample construction it is required to flash the board with host and remote code.
This can be done using west, starting from this sample directory:

.. msc::
   west build -b nrf5340dk_nrf5340_cpuapp .
   west flash
   cd build/event_manager_proxy_remote-prefix/src/event_manager_proxy_remote-build/
   west flash

Testing
=======

After programming both cores the sample prints logged data using two UARTS - one for host and one for remote core.
Connect terminal to the provided virtual ports and reset the board.

Now on the host core we expect following messages::

      *** Booting Zephyr OS build v2.7.99-ncs1-17-gc3208e7ff49d  ***
      Event Manager Proxy demo started
      [00:00:00.284,881] <inf> event_manager: e:config_event init_val_1=3
      [00:00:00.285,430] <inf> event_manager: e:measurement_event val1=3 val2=3 val3=3
      [00:00:00.785,675] <inf> event_manager: e:measurement_event val1=3 val2=6 val3=9
      [00:00:01.285,949] <inf> event_manager: e:measurement_event val1=3 val2=9 val3=18
      [00:00:01.786,254] <inf> event_manager: e:measurement_event val1=3 val2=12 val3=30
      [00:00:02.286,560] <inf> event_manager: e:measurement_event val1=3 val2=15 val3=45
      [00:00:02.286,682] <inf> event_manager: e: control_event
      [00:00:02.286,682] <inf> stats: Control event count: 1
      [00:00:02.787,017] <inf> event_manager: e:measurement_event val1=-3 val2=12 val3=57
      [00:00:03.287,322] <inf> event_manager: e:measurement_event val1=-3 val2=9 val3=66
      [00:00:03.787,597] <inf> event_manager: e:measurement_event val1=-3 val2=6 val3=72
      [00:00:04.287,872] <inf> event_manager: e:measurement_event val1=-3 val2=3 val3=75
      [00:00:04.788,177] <inf> event_manager: e:measurement_event val1=-3 val2=0 val3=75
      [00:00:04.788,208] <inf> stats: Average value3: 45
      [00:00:05.288,452] <inf> event_manager: e:measurement_event val1=-3 val2=-3 val3=72
      [00:00:05.788,726] <inf> event_manager: e:measurement_event val1=-3 val2=-6 val3=66
      [00:00:06.289,031] <inf> event_manager: e:measurement_event val1=-3 val2=-9 val3=57
      [00:00:06.789,306] <inf> event_manager: e:measurement_event val1=-3 val2=-12 val3=45
      [00:00:07.289,611] <inf> event_manager: e:measurement_event val1=-3 val2=-15 val3=30
      [00:00:07.289,733] <inf> event_manager: e: control_event
      [00:00:07.289,733] <inf> stats: Control event count: 2

On the remote core following messages are expected::

      *** Booting Zephyr OS build v2.7.99-ncs1-17-gc3208e7ff49d  ***
      Event Manager Proxy remote_core started
      [00:00:00.010,864] <inf> event_manager: e:config_event init_val_1=3
      [00:00:00.011,047] <inf> event_manager: e:measurement_event val1=3 val2=3 val3=3
      [00:00:00.511,322] <inf> event_manager: e:measurement_event val1=3 val2=6 val3=9
      [00:00:01.011,566] <inf> event_manager: e:measurement_event val1=3 val2=9 val3=18
      [00:00:01.511,871] <inf> event_manager: e:measurement_event val1=3 val2=12 val3=30
      [00:00:02.012,176] <inf> event_manager: e:measurement_event val1=3 val2=15 val3=45
      [00:00:02.012,298] <inf> event_manager: e: control_event
      [00:00:02.012,451] <inf> event_manager: e: ack_event
      [00:00:02.512,634] <inf> event_manager: e:measurement_event val1=-3 val2=12 val3=57
      [00:00:03.012,939] <inf> event_manager: e:measurement_event val1=-3 val2=9 val3=66
      [00:00:03.513,244] <inf> event_manager: e:measurement_event val1=-3 val2=6 val3=72
      [00:00:04.013,488] <inf> event_manager: e:measurement_event val1=-3 val2=3 val3=75
      [00:00:04.513,793] <inf> event_manager: e:measurement_event val1=-3 val2=0 val3=75
      [00:00:05.014,099] <inf> event_manager: e:measurement_event val1=-3 val2=-3 val3=72
      [00:00:05.514,343] <inf> event_manager: e:measurement_event val1=-3 val2=-6 val3=66
      [00:00:06.014,648] <inf> event_manager: e:measurement_event val1=-3 val2=-9 val3=57
      [00:00:06.514,953] <inf> event_manager: e:measurement_event val1=-3 val2=-12 val3=45
      [00:00:07.015,197] <inf> event_manager: e:measurement_event val1=-3 val2=-15 val3=30
      [00:00:07.015,350] <inf> event_manager: e: control_event
      [00:00:07.015,502] <inf> event_manager: e: ack_event

Host starts the communication by sending config event, that is also received and processed by remote core.
Now all the measurement, and control events are generated on the remote core and passed to host core, where statistics are performed.
On the remote core we can see also ack event, that is not passed to the host core.

Dependencies
************

This sample uses the following |NCS| subsystems:

* :ref:`event_manager`

In addition, it uses the following Zephyr subsystems:

* :ref:`zephyr:logging_api`
