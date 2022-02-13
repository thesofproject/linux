/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause) */
/*
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * Copyright(c) 2022 Intel Corporation. All rights reserved.
 */

#ifndef __SOUND_SOC_SOF_IPC4_OPS_H
#define __SOUND_SOC_SOF_IPC4_OPS_H

#include <sound/sof/ext_manifest4.h>
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

/**
 * struct sof_ipc4_fw_module - IPC4 module info
 * @sof_man4_module : Module info
 * @m_ida: Module instance identifier
 * @bss_size: Module object size
 * @private: Module private data
 */
struct sof_ipc4_fw_module {
	struct sof_man4_module man4_module_entry;
	struct ida m_ida;
	u32 bss_size;
	void *private;
};

extern const struct sof_ipc_ops ipc4_ops;
extern const struct sof_ipc_fw_loader_ops ipc4_loader_ops;
extern const struct sof_ipc_tplg_ops ipc4_tplg_ops;
extern const struct sof_ipc_tplg_control_ops tplg_ipc4_control_ops;

#endif
