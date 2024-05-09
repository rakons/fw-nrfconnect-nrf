/*
 * Copyright (c) 2024 Nordic Semiconductor ASA.
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */


#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <stdio.h>
#include <zephyr/sys/__assert.h>



static K_SEM_DEFINE(sem, 0, 1);

static void trigger_handler(const struct device *dev,
			    const struct sensor_trigger *trigger)
{
	ARG_UNUSED(dev);
	switch (trigger->type) {
	case SENSOR_TRIG_DATA_READY:
		printk("Data ready trigger\r\n");
		break;
	default:
		printk("Unknown trigger event %d\r\n", trigger->type);
		break;
	}
	k_sem_give(&sem);
}


int main(void)
{
	const struct device *dev = DEVICE_DT_GET_ANY(st_lps22hh);

	if (IS_ENABLED(CONFIG_LOG_BACKEND_RTT)) {
		/* Give RTT log time to be flushed before executing tests */
		k_sleep(K_MSEC(500));
	}

	if (!device_is_ready(dev)) {
		printk("Sensor device not ready\n");
		return 0;
	}

	printk("device is %p, name is %s\n", dev, dev->name);

	return 0;
}
