/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause) */
/*
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * Copyright(c) 2024 Intel Corporation
 *
 * Author: Kai Vehmanen <kai.vehmanen@linux.intel.com>
 */

/*
 * Shared mailbox layout for the DSP shell "llext_load" command.
 * Binary layout must match sof/zephyr/include/sof/shell_llext_load.h on
 * the DSP side.  Both files must be updated together.
 *
 * See shell_llext_load.h for the full protocol description.
 */

#ifndef __SOF_SHELL_LLEXT_SHM_H__
#define __SOF_SHELL_LLEXT_SHM_H__

#include <linux/types.h>

#define SOF_SHELL_LLEXT_MAGIC           0x4C454C44U /* 'LELD' */

enum sof_shell_llext_state {
	SOF_SHELL_LLEXT_IDLE       = 0,
	SOF_SHELL_LLEXT_REQUESTING = 1, /* DSP ready, waiting for host DMA */
	SOF_SHELL_LLEXT_DMA_ACTIVE = 2, /* host: DMA in progress */
	SOF_SHELL_LLEXT_DMA_DONE   = 3, /* host: DMA + IPC load complete */
	SOF_SHELL_LLEXT_ERROR      = 4, /* host: load failed */
};

struct sof_shell_llext_slot {
	__u32 magic;
	__u32 state;
	__u32 lib_id;
	__u32 xfer_bytes;
	__s32 result;
	__u32 reserved[3];
	char  name[64];
} __packed;

#endif /* __SOF_SHELL_LLEXT_SHM_H__ */
