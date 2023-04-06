/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause) */
/*
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * Copyright(c) 2023 Intel Corporation. All rights reserved.
 *
 * telemetry data in debug windows
 */

#ifndef _SOF_INTEL_TELEMETRY_H
#define _SOF_INTEL_TELEMETRY_H

/* Xtensa dsp AR register count */
#define SOF_IPC4_FW_AR_REGS_COUNT			64
#define SOF_TELEMETRY_SEPARATOR		0x00E0DE0D
#define SOF_TELEMETRY_SEPARATOR_OFFSET		1692

struct core_exception_record {
	u32 version;
	u32 stackdump_completion;
	u64 timestamp;
	u32 rec_state;
	u32 exec_ctx;
	u32 epc_1;
	u32 eps_2;
	u32 epc_2;
	u32 depc;
	u32 debugcause;
	u32 exccause;
	u32 excvaddr;
	u32 excsave;
	u32 interrupt;
	u32 ar[SOF_IPC4_FW_AR_REGS_COUNT];
	u32 windowbase;
	u32 windowstart;
	/* Dumped piece of memory around EPC, beginning from [-1..2] */
	u32 mem_epc[4];
	u32 stack_base_addr;
}__packed __aligned(4);

void sof_ipc4_intel_dump_telemetry_exception_state(struct snd_sof_dev *sdev, u32 flags);

#endif /* _SOF_INTEL_TELEMETRY_H */
