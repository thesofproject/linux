/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause) */
/*
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * Copyright(c) 2022 Intel Corporation. All rights reserved.
 */

#ifndef __SOUND_SOC_SOF_IPC4_OPS_H
#define __SOUND_SOC_SOF_IPC4_OPS_H

#include "sof-priv.h"

/**
 * struct sof_ipc4_private_data - IPC4-specific data
 * @num_fw_modules : Number of modules in base FW
 * @fw_modules: Array of base FW modules
 * @manifest_fw_hdr_offset: FW header offset in the manifest
 */
struct sof_ipc4_data {
	int num_fw_modules;
	void *fw_modules;
	u32 manifest_fw_hdr_offset;
};

extern const struct sof_ipc_ops ipc4_ops;

#endif
