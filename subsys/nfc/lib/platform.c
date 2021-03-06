/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */
#include <drivers/clock_control.h>
#include <nrfx_nfct.h>
#include <nrfx_timer.h>

#include <logging/log.h>

LOG_MODULE_REGISTER(nfc_platform, CONFIG_NFC_PLATFORM_LOG_LEVEL);

static struct device *clock;

static void clock_handler(struct device *dev, void *user_data)
{
	/* Activate NFCT only when HFXO is running */
	nrfx_nfct_state_force(NRFX_NFCT_STATE_ACTIVATED);
}


static struct clock_control_async_data clock_ctrl = {
	.cb = clock_handler
};


nrfx_err_t nfc_platform_setup(void)
{
	clock = device_get_binding(DT_INST_0_NORDIC_NRF_CLOCK_LABEL "_16M");
	__ASSERT_NO_MSG(clock);

	IRQ_CONNECT(NFCT_IRQn, CONFIG_NFCT_IRQ_PRIORITY,
			   nrfx_nfct_irq_handler, NULL,  0);
	IRQ_CONNECT(TIMER4_IRQn, CONFIG_NFCT_IRQ_PRIORITY,
			   nrfx_timer_4_irq_handler, NULL,  0);

	LOG_DBG("NFC platform initialized");
	return NRFX_SUCCESS;
}


void nfc_platform_event_handler(nrfx_nfct_evt_t const *event)
{
	int err;

	switch (event->evt_id) {
	case NRFX_NFCT_EVT_FIELD_DETECTED:
		LOG_DBG("Field detected");

		err = clock_control_async_on(clock, NULL, &clock_ctrl);
		__ASSERT_NO_MSG(!err);

		break;

	case NRFX_NFCT_EVT_FIELD_LOST:
		LOG_DBG("Field lost");

		err = clock_control_off(clock, NULL);
		__ASSERT_NO_MSG(!err);

		break;

	default:
		/* No implementation required */
		break;
	}
}
