/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause) */
/*
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * Copyright(c) 2019 Intel Corporation. All rights reserved.
 */

#ifndef __INCLUDE_UAPI_USER_DETECT_TEST_H__
#define __INCLUDE_UAPI_USER_DETECT_TEST_H__

/** IPC blob types */
#define SOF_DETECT_TEST_CONFIG	0
#define SOF_DETECT_TEST_MODEL	1

struct sof_detect_test_config {
	uint32_t size;

	/** synthetic system load settings */
	uint32_t load_mips;
	uint32_t load_memory_size;

	/** length of the keyphrase in milliseconds */
	uint32_t keyphrase_length;

	/** activation right shift, determines the speed of activation */
	uint16_t activation_shift;

	/** activation threshold */
	int16_t activation_threshold;

	/** reserved for future use */
	uint32_t reserved[3];
} __packed;

/** used for binary blob size sanity checks */
#define SOF_DETECT_TEST_MAX_CFG_SIZE sizeof(struct sof_detect_test_config)

#endif
