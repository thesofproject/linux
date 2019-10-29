/* SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
 *
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * Copyright(c) 2018-2019 Intel Corporation. All rights reserved.
 *
 *  Contact Information:
 *  Author:	Luo Xionghu <xionghu.luo@intel.com>
 *		Liam Girdwood <liam.r.girdwood@linux.intel.com>
 *		Guennadi Liakhovetski <guennadi.liakhovetski@linux.intel.com>
 */

#ifndef _SOF_VIRTIO_H
#define _SOF_VIRTIO_H

#include <sound/sof/header.h>

/*
 * Currently we define 3 vqs: one for handling of IPC commands, one for
 * handling of stream position updates, and one for audio data.
 */
enum {
	SOF_VIRTIO_IPC_CMD_VQ,	/* IPC commands and replies */
	SOF_VIRTIO_IPC_PSN_VQ,	/* Stream position updates */
	SOF_VIRTIO_DATA_VQ,	/* Audio data */
	/* Keep last */
	SOF_VIRTIO_NUM_OF_VQS,
};

/* command messages from FE to BE, trigger/open/hw_params and so on */
#define SOF_VIRTIO_IPC_CMD_VQ_NAME  "sof-ipc-cmd"

/* the vq to get stream position updates */
#define SOF_VIRTIO_IPC_PSN_VQ_NAME  "sof-ipc-psn"

/* the vq for audio data */
#define SOF_VIRTIO_DATA_VQ_NAME  "sof-data"

/**
 * struct sof_vfe_ipc_tplg_req - request for topology data
 * @hdr:	the standard SOF IPC header
 * @file_name:	the name of the topology file
 * @offset:	the current offset when transferring a split file
 */
struct sof_vfe_ipc_tplg_req {
	struct sof_ipc_cmd_hdr hdr;
	char file_name[64];
	size_t offset;
} __packed;

/**
 * struct sof_vfe_ipc_tplg_resp - response to a topology file request
 * @reply:	the standard SOF IPC response header
 * @data:	the complete topology file
 *
 * The topology file is transferred from the host to the guest over a virtual
 * queue in chunks of SOF_IPC_MSG_MAX_SIZE - sizeof(struct sof_ipc_reply), so
 * for data transfer the @data array is much smaller than 64KiB. 64KiB is what
 * is included in struct sof_vfe for permanent storagy of the complete file.
 */
struct sof_vfe_ipc_tplg_resp {
	struct sof_ipc_reply reply;
	/* There exist topology files already larger than 40KiB */
	uint8_t data[64 * 1024 - sizeof(struct sof_ipc_reply)];
} __packed;

#define SOF_VFE_MAX_DATA_SIZE (16 * 1024)

/**
 * struct dsp_sof_data_req - Audio data request
 *
 * @size:	the size of audio data sent or requested, excluding the header
 * @offset:	offset in the DMA buffer
 * @comp_id:	component ID, used to identify the stream
 *
 * When used during playback, the data array actually contains audio data, when
 * used for capture, the data part isn't sent.
 */
struct dsp_sof_data_req {
	u32 size;
	u32 offset;
	u32 comp_id;
	/* Only included for playback */
	u8 data[SOF_VFE_MAX_DATA_SIZE];
} __packed;

/**
 * struct dsp_sof_data_req - Audio data request
 *
 * @size:	the size of audio data sent, excluding the header
 * @error:	response error
 *
 * When used during capture, the data array actually contains audio data, when
 * used for playback, the data part isn't sent.
 */
struct dsp_sof_data_resp {
	u32 size;
	u32 error;
	/* Only included for capture */
	u8 data[SOF_VFE_MAX_DATA_SIZE];
} __packed;

#define HDR_SIZE_REQ offsetof(struct dsp_sof_data_req, data)
#define HDR_SIZE_RESP offsetof(struct dsp_sof_data_resp, data)

#endif
