/* SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause) */
/*
 * Copyright(c) 2018-2020 Intel Corporation. All rights reserved.
 *
 *  Contact Information:
 *  Author:	Luo Xionghu <xionghu.luo@intel.com>
 *		Liam Girdwood <liam.r.girdwood@linux.intel.com>
 *		Guennadi Liakhovetski <guennadi.liakhovetski@linux.intel.com>
 */

#ifndef _SOF_RPMSG_H
#define _SOF_RPMSG_H

#include <linux/virtio_rpmsg.h>

#include <sound/sof/header.h>

/* host endpoint addresses */
enum {
	SOF_RPMSG_ADDR_IPC,	/* IPC commands and replies */
	SOF_RPMSG_ADDR_POSN,	/* Stream position updates */
	SOF_RPMSG_ADDR_DATA,	/* Audio data */
	SOF_RPMSG_ADDR_COUNT,	/* Number of RPMsg endpoints */
};

/**
 * struct sof_rpmsg_ipc_tplg_req - request for topology data
 * @hdr:	the standard SOF IPC header
 * @offset:	the current offset when transferring a split file
 */
struct sof_rpmsg_ipc_tplg_req {
	struct sof_ipc_cmd_hdr hdr;
	size_t offset;
} __packed;

/**
 * struct sof_rpmsg_ipc_tplg_resp - response to a topology file request
 * @reply:	the standard SOF IPC response header
 * @data:	the complete topology file
 *
 * The topology file is transferred from the host to the guest over a virtual
 * queue in chunks of SOF_IPC_MSG_MAX_SIZE - sizeof(struct sof_ipc_reply), so
 * for data transfer the @data array is much smaller than 64KiB. 64KiB is what
 * is included in struct sof_vfe for permanent storage of the complete file.
 */
struct sof_rpmsg_ipc_tplg_resp {
	struct sof_ipc_reply reply;
	/* There exist topology files already larger than 40KiB */
	uint8_t data[64 * 1024 - sizeof(struct sof_ipc_reply)];
} __packed;

/**
 * struct sof_rpmsg_ipc_power_req - power status change IPC
 * @hdr:	the standard SOF IPC header
 * @power:	1: on, 0: off
 */
struct sof_rpmsg_ipc_power_req {
	struct sof_ipc_cmd_hdr hdr;
	uint32_t power;
} __packed;

enum sof_rpmsg_ipc_reset_status {
	SOF_RPMSG_IPC_RESET_NONE,	/* Host hasn't been reset */
	SOF_RPMSG_IPC_RESET_DONE,	/* Host has been reset */
};

/**
 * struct sof_rpmsg_ipc_power_resp - response to a power status request
 * @reply:	the standard SOF IPC response header
 * @reset_status: enum sof_rpmsg_ipc_reset_status
 */
struct sof_rpmsg_ipc_power_resp {
	struct sof_ipc_reply reply;
	uint32_t reset_status;
} __packed;

#define SOF_RPMSG_MAX_DATA_SIZE MAX_RPMSG_BUF_SIZE

/**
 * struct sof_rpmsg_data_req - Audio data request
 *
 * @size:	the size of audio data sent or requested, excluding the header
 * @offset:	offset in the DMA buffer
 * @comp_id:	component ID, used to identify the stream
 * @data:	audio data
 *
 * When used during playback, the data array actually contains audio data, when
 * used for capture, the data part isn't sent.
 */
struct sof_rpmsg_data_req {
	u32 size;
	u32 offset;
	u32 comp_id;
	/* Only included for playback */
	u8 data[];
} __packed;

/**
 * struct sof_rpmsg_data_resp - Audio data response
 *
 * @size:	the size of audio data sent, excluding the header
 * @error:	response error
 * @data:	audio data
 *
 * When used during capture, the data array actually contains audio data, when
 * used for playback, the data part isn't sent.
 */
struct sof_rpmsg_data_resp {
	u32 size;
	u32 error;
	/* Only included for capture */
	u8 data[];
} __packed;

struct sof_rpmsg_ipc_req {
	u32 reply_size;
	u8 ipc_msg[SOF_IPC_MSG_MAX_SIZE];
} __packed;

#endif
