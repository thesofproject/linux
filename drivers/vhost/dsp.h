// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
//
// This file is provided under a dual BSD/GPLv2 license.  When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2019 Intel Corporation. All rights reserved.
//
// Author: Guennadi Liakhovetski <guennadi.liakhovetski@linux.intel.com>
//
// vhost-SOF internal header

#ifndef __VHOST_DSP_H__
#define __VHOST_DSP_H__

#include <linux/list.h>
#include <linux/spinlock.h>

#include <sound/sof/stream.h>

#include <sound/sof/virtio.h>

#include "vhost.h"

struct vhost_dsp_virtqueue {
	struct vhost_virtqueue vq;
};

struct snd_sof_dev;
struct firmware;

struct vhost_dsp {
	struct vhost_dev dev;
	struct vhost_dsp_virtqueue vqs[SOF_VIRTIO_NUM_OF_VQS];
	struct vhost_work work;
	struct vhost_virtqueue *vq_p[SOF_VIRTIO_NUM_OF_VQS];
	spinlock_t posn_lock;			/* Protects posn_list */
	struct list_head posn_list;
	struct list_head posn_buf_list;

	const struct firmware *fw;
	struct snd_sof_dev *sdev;
	/* List of guest endpoints, connecting to the host mixer or demux */
	struct list_head pipe_conn;
	/* List of vhost instances on a DSP */
	struct list_head list;

	/* the comp_ids for this vm audio */
	int comp_id_begin;
	int comp_id_end;

	u8 ipc_buf[SOF_IPC_MSG_MAX_SIZE];
	u8 reply_buf[SOF_IPC_MSG_MAX_SIZE];

	union {
		struct dsp_sof_data_req data_req;
		struct dsp_sof_data_resp data_resp;
	};
};

/* A stream position message, waiting to be sent to a guest */
struct vhost_dsp_posn {
	struct list_head list;
	struct sof_ipc_stream_posn posn;
};

/* A guest buffer, waiting to be filled with a stream position message */
struct vhost_dsp_iovec {
	struct list_head list;
	int head;
};

/* Forward an IPC message from a guest to the DSP */
int dsp_sof_ipc_fwd(struct vhost_dsp *dsp, int vq_idx,
		    void *ipc_buf, void *reply_buf,
		    size_t count, size_t reply_sz);
int dsp_sof_ipc_stream_data(struct snd_sof_dev *sdev,
			    struct dsp_sof_data_req *req,
			    struct dsp_sof_data_resp *reply);
#endif
