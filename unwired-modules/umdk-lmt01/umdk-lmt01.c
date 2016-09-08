/*
 * Copyright (C) 2016 Unwired Devices [info@unwds.com]
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License v2.1. See the file LICENSE in the top level
 * directory for more details.
 */

/**
 * @defgroup    
 * @ingroup     
 * @brief       
 * @{
 * @file		umdk-lmt01.c
 * @brief       umdk-lmt01 module implementation
 * @author      Eugene Ponomarev
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include "periph/gpio.h"

#include "board.h"

#include "lmt01.h"

#include "unwds-common.h"
#include "umdk-lmt01.h"
#include "unwds-gpio.h"

#include "thread.h"
#include "xtimer.h"

static gpio_t en_pins[UMDK_LMT01_MAX_SENSOR_COUNT] = UMDK_LMT01_SENSOR_EN_PINS;
static lmt01_t sensors[UMDK_LMT01_MAX_SENSOR_COUNT];

static bool detected[UMDK_LMT01_MAX_SENSOR_COUNT];
static int num_sensors = 0;

static uwnds_cb_t *callback;

static kernel_pid_t timer_pid;
static char timer_stack[THREAD_STACKSIZE_MAIN];

static int publish_period_min;

static msg_t timer_msg = {};
static xtimer_t timer;

static int init_sensors(void) {
	int i = 0;
	uint8_t detected_count = 0;

	for (i = 0; i < UMDK_LMT01_MAX_SENSOR_COUNT; i++) {
		lmt01_t *dev = &sensors[i];

		/* Skip disabled */
		if (!en_pins[i])
			continue;

		/* Initialize */
		if (lmt01_init(dev, en_pins[i], UMDK_LMT01_INT_PIN) < 0) {
			printf("[umdk-lmt01] Failed to initialize sensor #%d\n", i);
		}

		/* Detect */
		printf("[umdk-lmt01] Detecting %d...\n", i);
		if (lmt01_detect(dev, UMDK_LMT01_DETECT_TIMEOUT_MS)) {
			detected[i] = true;
			printf("[umdk-lmt01] Detected sensor #%d\n", i);

			detected_count++;
		}

		/* Delay between sensor switching */
		xtimer_usleep(1e3 * 100);
	}

	return detected_count;
}

static uint16_t convert_temp(float temp) {
	return (temp + 100) * 16;
}

static void prepare_result(module_data_t *buf) {
	int results = 0;
	int i;

	uint16_t res[4] = { 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF };

	for (i = 0; i < UMDK_LMT01_MAX_SENSOR_COUNT; i++) {
		if (!detected[i]) {
			continue;
		}

		float temp;
		int pulses;
		if ((pulses = lmt01_get_temp(&sensors[i], &temp)) > 0) {
			printf("[umdk-lmt01] Measured %d pulses on #%d: %.02f\n", pulses, i, temp);
			res[i] = convert_temp(temp);
			results++;
		} else {
			continue;
		}

		/* Delay between sensor switching */
		xtimer_usleep(1e3 * 100);
	}

	/* Try to re-detect sensors */
	if ((num_sensors = init_sensors()) == 0) {
		puts("[umdk-lmt01] Unable to detect sensor(s)");
	}

	buf->data[0] = UNWDS_LMT01_MODULE_ID;
	memcpy(buf->data + 1, (uint8_t *) res, sizeof(res));
	buf->length = sizeof(res) + 1;
}

void *timer_thread(void *arg) {
    msg_t msg;
    msg_t msg_queue[8];
    msg_init_queue(msg_queue, 8);

    puts("[umdk-lmt01] Periodic publisher thread started");

    while (1) {
        msg_receive(&msg);

        xtimer_remove(&timer);

        module_data_t data = {};
        prepare_result(&data);

        /* Notify the application */
        callback(&data);

        /* Restart after delay */
        xtimer_set_msg(&timer, 1e6 * 60 * publish_period_min, &timer_msg, timer_pid);
    }
}

void umdk_lmt01_init(uint32_t *non_gpio_pin_map, uwnds_cb_t *event_callback) {
	(void) non_gpio_pin_map;

	callback = event_callback;
	publish_period_min = UMDK_LMT01_PUBLISH_PERIOD_MIN; /* Set to default */

	if ((num_sensors = init_sensors()) == 0) {
		puts("[umdk-lmt01] Unable to detect sensor(s)");
	}

	/* Create handler thread */
	timer_pid = thread_create(timer_stack, sizeof(timer_stack), THREAD_PRIORITY_MAIN - 1, 0, timer_thread, NULL, "lmt01 publisher thread");

    /* Start publishing timer */
	xtimer_set_msg(&timer, 1e6 * 60 * publish_period_min, &timer_msg, timer_pid);
}

bool umdk_lmt01_cmd(module_data_t *cmd, module_data_t *reply) {
	if (cmd->length < 1)
		return false;

	umdk_lmt01_cmd_t c = cmd->data[0];
	switch (c) {
	case UMDK_LMT01_CMD_SET_PERIOD: {
		if (cmd->length != 2)
			return false;

		uint8_t period = cmd->data[1];
		xtimer_remove(&timer);

		publish_period_min = period;

		/* Don't restart timer if new period is zero */
		if (publish_period_min) {
			xtimer_set_msg(&timer, 1e6 * 60 * publish_period_min, &timer_msg, timer_pid);
			printf("[lmt01] Period set to %d seconds\n", publish_period_min);
		} else
			puts("[lmt01] Timer stopped");

		reply->length = 4;
		reply->data[0] = UNWDS_LMT01_MODULE_ID;
		reply->data[1] = 'o';
		reply->data[2] = 'k';
		reply->data[3] = '\0';

		break;
	}

	case UMDK_LMT01_CMD_POLL:
		/* Send signal to publisher thread */
		msg_send(&timer_msg, timer_pid);

		return false; /* Don't reply */

		break;

	case UMDK_LMT01_CMD_SET_GPIOS: {
		uint8_t *gpios = &cmd->data[1];
		int num_gpios = cmd->length - 1;

		if (!num_gpios)
			return false;

		int i;
		for (i = 0; i < num_gpios; i++) {
			if (gpios[i]) {
				gpio_t gpio = unwds_gpio_pin(gpios[i]);
				en_pins[i] = gpio;
			} else
				en_pins[i] = 0;	/* Disable this pin */
		}

		/* Re-initialize sensors */
		init_sensors();

		break;
	}

	default:
		break;
	}

	return true;
}

#ifdef __cplusplus
}
#endif
