// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
//
// This file is provided under a dual BSD/GPLv2 license.  When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2019-2021 Intel Corporation. All rights reserved.
// Author: Cezary Rojewski <cezary.rojewski@intel.com>
//

#ifndef __SOF_PROBES_H
#define __SOF_PROBES_H

#include <sound/compress_driver.h>
#include <sound/soc.h>
#include <sound/soc-dai.h>
#include <sound/sof/header.h>

struct snd_sof_dev;
struct sof_client_dev;
struct dentry;

#define SOF_PROBE_INVALID_NODE_ID UINT_MAX

struct sof_probes_data {
	struct dentry *dfs_root;
	unsigned int extractor_stream_tag;

	const struct sof_probes_ops *ops;
};

struct sof_probe_point_desc {
	unsigned int buffer_id;
	unsigned int purpose;
	unsigned int stream_tag;
} __packed;

int sof_probe_points_info(struct sof_client_dev *cdev,
			  struct sof_probe_point_desc **desc,
			  size_t *num_desc);
int sof_probe_points_add(struct sof_client_dev *cdev,
			 struct sof_probe_point_desc *desc,
			 size_t num_desc);
int sof_probe_points_remove(struct sof_client_dev *cdev,
			    unsigned int *buffer_id, size_t num_buffer_id);

extern struct snd_soc_cdai_ops sof_probe_compr_ops;
extern struct snd_compress_ops sof_probe_compressed_ops;

#endif
