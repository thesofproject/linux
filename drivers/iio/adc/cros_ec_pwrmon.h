
/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Host communication PWRMON command constants for ChromeOS EC
 *
 * Copyright (C) 2026 Google, Inc
 */

#ifndef __CROS_EC_PWRMON_H
#define __CROS_EC_PWRMON_H

#include <linux/bits.h>
#include <linux/types.h>

/**
 * Power monitoring. Used to read power consumptions on rails
 */
#define EC_CMD_PWRMON 0x0607

enum ec_pwrmon_cmd {
	EC_PWRMON_GET_CHANNEL_COUNT = 0,
	EC_PWRMON_DUMP_INFO = 1,
	EC_PWRMON_SET_RATE = 2,
	EC_PWRMON_GET_RATE = 3,
	EC_PWRMON_START = 4,
	EC_PWRMON_STOP = 5,
	EC_PWRMON_GET = 6,
	EC_PWRMON_LATCH = 7,
};
struct ec_params_pwrmon {
	uint8_t cmd;
	union {
		struct {
			uint16_t rate;
		} set_rate;
		struct {
			uint8_t channel_id;
		} get_dump_info;
		struct {
			uint8_t channel_id;
		} get_channel_info;
	} __ec_align2;
	/*
	 * The following commands have no args:
	 *
	 * start, stop, latch
	 *
	 */
} __ec_align4;
struct pwrmon_get_rate {
	uint16_t rate;
} __ec_align4;
struct pwrmon_channel_info {
	uint8_t channel_id;
	uint64_t samples;
	int64_t value;
} __ec_align4;
struct pwrmon_get_channel_count {
	uint8_t count;
} __ec_align2;
struct pwrmon_dump_info {
	uint8_t channel_id;
	char channel_name[32];
} __ec_align4;
struct ec_response_pwrmon {
	union {
		struct pwrmon_get_rate get_rate;
		struct pwrmon_get_channel_count channel_count;
		struct pwrmon_channel_info channel_info;
		struct pwrmon_dump_info dump_info;
	} __ec_align4;
} __ec_align4;

#endif /* __CROS_EC_PWRMON_H */
