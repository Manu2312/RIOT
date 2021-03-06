/*
 * Copyright (C) 2016-2018 Unwired Devices LLC <info@unwds.com>

 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software
 * is furnished to do so, subject to the following conditions:

 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.

 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
 * PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE
 * FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/**
 * @defgroup    
 * @ingroup     
 * @brief       
 * @{
 * @file		umdk-meteo.h
 * @brief       umdk-meteo driver module definitions
 * @author      Oleg Artamonov
 */
#ifndef UMDK_METEO_H
#define UMDK_METEO_H

#include "unwds-common.h"

#define UMDK_METEO_STACK_SIZE 1024

#define UMDK_METEO_PUBLISH_PERIOD_MIN 1

#define UMDK_METEO_I2C 1
#define UMDK_METEO_I2C_ADDR        (0x76)
#define METEO_PARAMS_BOARD               \
    {                                      \
        .i2c_dev = UMDK_METEO_I2C,   \
        .i2c_addr = UMDK_METEO_I2C_ADDR, \
        .t_sb = BMX280_SB_0_5,             \
        .filter = BMX280_FILTER_OFF,       \
        .run_mode = BMX280_MODE_FORCED,     \
        .temp_oversample = BMX280_OSRS_X1,  \
        .press_oversample = BMX280_OSRS_X1, \
        .humid_oversample = BMX280_OSRS_X1, \
    }

typedef enum {
    UMDK_METEO_DATA = 0,
	UMDK_METEO_COMMAND = 1,
	UMDK_METEO_POLL = 2,
    UMDK_METEO_FAIL = 0xFF,
} umdk_meteo_cmd_t;

void umdk_meteo_init(uwnds_cb_t *event_callback);
bool umdk_meteo_cmd(module_data_t *data, module_data_t *reply);

#endif /* UMDK_METEO_H */
