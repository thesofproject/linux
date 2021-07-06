/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __SOF_CLIENT_DMA_TRACE_H
#define __SOF_CLIENT_DMA_TRACE_H

#include <sound/sof/trace.h>

struct snd_dma_buffer;
struct sof_client_dev;

/* Platform callbacks */
struct sof_dma_trace_host_ops {
	int (*init)(struct sof_client_dev *cdev, struct snd_dma_buffer *dmab,
		    struct sof_ipc_dma_trace_params_ext *dtrace_params);
	int (*release)(struct sof_client_dev *cdev);

	/* Optional */
	int (*start)(struct sof_client_dev *cdev);
	int (*stop)(struct sof_client_dev *cdev);
	void (*available)(struct sof_client_dev *cdev, bool available);
};

#endif
