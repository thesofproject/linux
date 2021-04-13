// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
//
// This file is provided under a dual BSD/GPLv2 license.  When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2021 Intel Corporation. All rights reserved.
//

#ifndef __SOF_CLIENT_PROBES_H
#define __SOF_CLIENT_PROBES_H

#include <linux/auxiliary_bus.h>
#include <sound/compress_driver.h>
#include <sound/soc.h>
#include <sound/soc-dai.h>
#include <sound/sof/header.h>

struct snd_sof_dev;
struct dentry;

/* Platform callbacks */
struct sof_probes_ops {
	int (*assign)(struct snd_sof_dev *sdev,
		      struct snd_compr_stream *cstream,
		      struct snd_soc_dai *dai);
	int (*free)(struct snd_sof_dev *sdev,
		    struct snd_compr_stream *cstream,
		    struct snd_soc_dai *dai);
	int (*set_params)(struct snd_sof_dev *sdev,
			  struct snd_compr_stream *cstream,
			  struct snd_compr_params *params,
			  struct snd_soc_dai *dai);
	int (*trigger)(struct snd_sof_dev *sdev,
		       struct snd_compr_stream *cstream, int cmd,
		       struct snd_soc_dai *dai);
	int (*pointer)(struct snd_sof_dev *sdev,
		       struct snd_compr_stream *cstream,
		       struct snd_compr_tstamp *tstamp,
		       struct snd_soc_dai *dai);
};

int sof_probes_client_probe(struct auxiliary_device *auxdev,
			    const struct auxiliary_device_id *id);
void sof_probes_client_remove(struct auxiliary_device *auxdev);

#endif
