/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * Copyright(c) 2021 Intel Corporation. All rights reserved.
 */

#ifndef __SOF_CLIENT_DMA_TRACE_H
#define __SOF_CLIENT_DMA_TRACE_H

struct snd_dma_buffer;
struct sof_client_dev;

/* Platform callbacks */
struct sof_dma_trace_host_ops {
	int (*init)(struct sof_client_dev *cdev, struct snd_dma_buffer *dmab,
		    u32 *stream_tag);
	int (*release)(struct sof_client_dev *cdev);
	int (*start)(struct sof_client_dev *cdev);
	int (*stop)(struct sof_client_dev *cdev);
};

#endif
