/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause) */
/*
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * Copyright(c) 2018 Intel Corporation. All rights reserved.
 */

#ifndef __INCLUDE_SOUND_SOF_INPUT_ENVENT_H__
#define __INCLUDE_SOUND_SOF_INPUT_ENVENT_H__

#include <sound/sof/header.h>
#include <sound/sof/stream.h>

struct sof_ipc_input_event {
	struct sof_ipc_reply rhdr;
	uint32_t code;		/* 'code' for Linux input_report_key() */
	int32_t value;		/* 'value' for Linux input_report_key() */
} __packed;

#endif
