/* SPDX-License-Identifier: GPL-2.0-only
 *  Copyright (c) 2024 Advanced Micro Devices, Inc. All rights reserved
 */

/*
 *  soc_amd_sdw_common.h - prototypes for common helpers
 */

#ifndef SOC_AMD_SDW_COMMON_H
#define SOC_AMD_SDW_COMMON_H

#include <linux/bits.h>
#include <linux/types.h>
#include <sound/soc.h>
#include "../../sdw_utils/soc_sdw_utils.h"

#define MAX_NO_PROPS		2
#define SDW_MAX_CPU_DAIS	8
#define SDW_MAX_LINKS		2

#define SDW_MAX_GROUPS 9

#define ACP63_PCI_REV		0x63
#define SOC_JACK_JDSRC(quirk)		((quirk) & GENMASK(3, 0))
#define SOC_SDW_FOUR_SPK		BIT(4)
#define SOC_SDW_ACP_DMIC		BIT(5)
#define SOC_SDW_NO_AGGREGATION		BIT(6)

#define AMD_SDW0	0
#define AMD_SDW1	1
#define SW0_AUDIO0_TX	0
#define SW0_AUDIO1_TX	1
#define SW0_AUDIO2_TX	2

#define SW0_AUDIO0_RX	3
#define SW0_AUDIO1_RX	4
#define SW0_AUDIO2_RX	5

#define SW1_AUDIO0_TX	0
#define SW1_AUDIO0_RX	1

struct amd_mc_ctx {
	unsigned int acp_rev;
	unsigned int max_sdw_links;
};

extern unsigned long sof_sdw_quirk;
#endif
