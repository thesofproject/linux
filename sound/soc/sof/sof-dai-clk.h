/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause) */
/*
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * Copyright(c) 2021 Intel Corporation. All rights reserved.
 */

#ifndef __SOUND_SOC_SOF_DAI_CLK_H
#define __SOUND_SOC_SOF_DAI_CLK_H

struct sof_dai_clk {
	const char *name;
	int type;
	int dai_index;
	int clk_id;
};

#endif
