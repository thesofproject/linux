/* SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause) */
/*
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * Copyright(c) 2019 Intel Corporation. All rights reserved.
 *
 * Author: Cezary Rojewski <cezary.rojewski@intel.com>
 */

#ifndef __SOF_PROBE_H
#define __SOF_PROBE_H

#include <sound/sof/header.h>

struct snd_sof_dev;

#define SOF_PROBE_INVALID_NODE_ID UINT_MAX

struct sof_probe_dma {
	unsigned int stream_tag;
	unsigned int dma_buffer_size;
} __packed;

enum sof_connection_purpose {
	SOF_CONNECTION_PURPOSE_EXTRACT = 1,
	SOF_CONNECTION_PURPOSE_INJECT,
};

struct sof_probe_point_desc {
	unsigned int buffer_id;
	unsigned int purpose;
	unsigned int stream_tag;
} __packed;

struct sof_ipc_probe_dma_set_params {
	struct sof_ipc_cmd_hdr hdr;
	struct sof_probe_dma dma[0];
} __packed;

struct sof_ipc_probe_get_params {
	struct sof_ipc_reply rhdr;
	union {
		struct sof_probe_dma dma[0];
		struct sof_probe_point_desc desc[0];
	};
} __packed;

struct sof_ipc_probe_dma_detach_params {
	struct sof_ipc_cmd_hdr hdr;
	unsigned int stream_tag[0];
} __packed;

struct sof_ipc_probe_point_set_params {
	struct sof_ipc_cmd_hdr hdr;
	struct sof_probe_point_desc desc[0];
} __packed;

struct sof_ipc_probe_point_remove_params {
	struct sof_ipc_cmd_hdr hdr;
	unsigned int buffer_id[0];
} __packed;

int sof_ipc_probe_init(struct snd_sof_dev *sdev,
		u32 stream_tag, size_t buffer_size);
int sof_ipc_probe_deinit(struct snd_sof_dev *sdev);
int sof_ipc_probe_get_dma(struct snd_sof_dev *sdev,
		struct sof_probe_dma **dma, size_t *num_dma);
int sof_ipc_probe_dma_attach(struct snd_sof_dev *sdev,
		struct sof_probe_dma *dma, size_t num_dma);
int sof_ipc_probe_dma_detach(struct snd_sof_dev *sdev,
		unsigned int *stream_tag, size_t num_stream_tag);
int sof_ipc_probe_get_points(struct snd_sof_dev *sdev,
		struct sof_probe_point_desc **desc, size_t *num_desc);
int sof_ipc_probe_points_connect(struct snd_sof_dev *sdev,
		struct sof_probe_point_desc *desc, size_t num_desc);
int sof_ipc_probe_points_disconnect(struct snd_sof_dev *sdev,
		unsigned int *buffer_id, size_t num_id);

#endif
