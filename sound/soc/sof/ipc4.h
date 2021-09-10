/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause) */
/*
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * Copyright(c) 2021 Intel Corporation. All rights reserved.
 *
 * Author: Rander Wang <rander.wang@linux.intel.com>
 */

#ifndef __SOF_IPC4_H
#define __SOF_IPC4_H

struct sof_ipc4_fw_status {
	int status;
	char *msg;
};

void sof_ipc4_process_reply(struct snd_sof_dev *sdev, u32 msg);

#endif // __SOF_IPC4_H
