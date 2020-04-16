// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
//
// This file is provided under a dual BSD/GPLv2 license.  When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2019-2020 Intel Corporation. All rights reserved.
//
// Author: Cezary Rojewski <cezary.rojewski@intel.com>
//

#include <sound/soc.h>
#include "compress.h"
#include "probe.h"
#include "sof-client.h"

struct snd_soc_cdai_ops sof_probe_compr_ops = {
	.startup	= sof_probe_compr_open,
	.shutdown	= sof_probe_compr_free,
	.set_params	= sof_probe_compr_set_params,
	.trigger	= sof_probe_compr_trigger,
	.pointer	= sof_probe_compr_pointer,
};
EXPORT_SYMBOL(sof_probe_compr_ops);

struct snd_compr_ops sof_probe_compressed_ops = {
	.copy		= sof_probe_compr_copy,
};
EXPORT_SYMBOL(sof_probe_compressed_ops);

int sof_probe_compr_open(struct snd_compr_stream *cstream,
		struct snd_soc_dai *dai)
{
	struct sof_client_dev *cdev =
		snd_soc_component_get_drvdata(dai->component);
	struct sof_probes_data *probes_data = cdev->data;
	int ret;

	ret = sof_client_probe_compr_assign(cdev, cstream, dai);
	if (ret < 0) {
		dev_err(dai->dev, "Failed to assign probe stream: %d\n", ret);
		return ret;
	}

	probes_data->extractor_stream_tag = ret;
	return 0;
}
EXPORT_SYMBOL(sof_probe_compr_open);

int sof_probe_compr_free(struct snd_compr_stream *cstream,
		struct snd_soc_dai *dai)
{
	struct sof_client_dev *cdev =
		snd_soc_component_get_drvdata(dai->component);
	struct sof_probes_data *probes_data = cdev->data;
	struct sof_probe_point_desc *desc;
	size_t num_desc;
	int i, ret;

	/* disconnect all probe points */
	ret = sof_probe_points_info(cdev, &desc, &num_desc);
	if (ret < 0) {
		dev_err(dai->dev, "Failed to get probe points: %d\n", ret);
		goto exit;
	}

	for (i = 0; i < num_desc; i++)
		sof_probe_points_remove(cdev, &desc[i].buffer_id, 1);
	kfree(desc);

exit:
	ret = sof_probe_deinit(cdev);
	if (ret < 0)
		dev_err(dai->dev, "Failed to deinit probe: %d\n", ret);

	probes_data->extractor_stream_tag = SOF_PROBE_INVALID_NODE_ID;
	snd_compr_free_pages(cstream);

	return sof_client_probe_compr_free(cdev, cstream, dai);
}
EXPORT_SYMBOL(sof_probe_compr_free);

int sof_probe_compr_set_params(struct snd_compr_stream *cstream,
		struct snd_compr_params *params, struct snd_soc_dai *dai)
{
	struct snd_compr_runtime *rtd = cstream->runtime;
	struct sof_client_dev *cdev =
		snd_soc_component_get_drvdata(dai->component);
	struct sof_probes_data *probes_data = cdev->data;
	int ret;

	cstream->dma_buffer.dev.type = SNDRV_DMA_TYPE_DEV_SG;
	cstream->dma_buffer.dev.dev = sof_client_get_dma_dev(cdev);
	ret = snd_compr_malloc_pages(cstream, rtd->buffer_size);
	if (ret < 0)
		return ret;

	ret = sof_client_probe_compr_set_params(cdev, cstream, params, dai);
	if (ret < 0)
		return ret;

	ret = sof_probe_init(cdev, probes_data->extractor_stream_tag,
			     rtd->dma_bytes);
	if (ret < 0) {
		dev_err(dai->dev, "Failed to init probe: %d\n", ret);
		return ret;
	}

	return 0;
}
EXPORT_SYMBOL(sof_probe_compr_set_params);

int sof_probe_compr_trigger(struct snd_compr_stream *cstream, int cmd,
		struct snd_soc_dai *dai)
{
	struct sof_client_dev *cdev =
		snd_soc_component_get_drvdata(dai->component);

	return sof_client_probe_compr_trigger(cdev, cstream, cmd, dai);
}
EXPORT_SYMBOL(sof_probe_compr_trigger);

int sof_probe_compr_pointer(struct snd_compr_stream *cstream,
		struct snd_compr_tstamp *tstamp, struct snd_soc_dai *dai)
{
	struct sof_client_dev *cdev =
		snd_soc_component_get_drvdata(dai->component);

	return sof_client_probe_compr_pointer(cdev, cstream, tstamp, dai);
}
EXPORT_SYMBOL(sof_probe_compr_pointer);

int sof_probe_compr_copy(struct snd_compr_stream *cstream,
		char __user *buf, size_t count)
{
	struct snd_compr_runtime *rtd = cstream->runtime;
	unsigned int offset, n;
	void *ptr;
	int ret;

	if (count > rtd->buffer_size)
		count = rtd->buffer_size;

	div_u64_rem(rtd->total_bytes_transferred, rtd->buffer_size, &offset);
	ptr = rtd->dma_area + offset;
	n = rtd->buffer_size - offset;

	if (count < n) {
		ret = copy_to_user(buf, ptr, count);
	} else {
		ret = copy_to_user(buf, ptr, n);
		ret += copy_to_user(buf + n, rtd->dma_area, count - n);
	}

	if (ret)
		return count - ret;
	return count;
}
EXPORT_SYMBOL(sof_probe_compr_copy);
