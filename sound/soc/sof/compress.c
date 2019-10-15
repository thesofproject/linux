// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
//
// This file is provided under a dual BSD/GPLv2 license.  When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2019 Intel Corporation. All rights reserved.
//
// Author: Cezary Rojewski <cezary.rojewski@intel.com>
//

#include <sound/soc.h>
#include "compress.h"
#include "ops.h"
#include "probe.h"

int sof_probe_compr_open(struct snd_compr_stream *cstream,
		struct snd_soc_dai *dai)
{
	struct snd_sof_dev *sdev =
				snd_soc_component_get_drvdata(dai->component);
	/* with invalid id, extraction will not be supported */
	int ret = SOF_PROBE_INVALID_NODE_ID;

	if (sdev->disable_extraction)
		goto exit;

	ret = snd_sof_probe_compr_assign(sdev, cstream, dai);
	if (ret < 0) {
		dev_err(dai->dev, "Failed to assign probe stream: %d\n", ret);
		return ret;
	}

exit:
	sdev->extractor = ret;
	return 0;
}
EXPORT_SYMBOL(sof_probe_compr_open);

int sof_probe_compr_free(struct snd_compr_stream *cstream,
		struct snd_soc_dai *dai)
{
	struct snd_sof_dev *sdev =
				snd_soc_component_get_drvdata(dai->component);
	struct sof_probe_point_desc *desc;
	struct sof_probe_dma *dma;
	size_t num_desc, num_dma;
	int i, ret;

	/* disconnect all probe points */
	ret = sof_ipc_probe_get_points(sdev, &desc, &num_desc);
	if (ret < 0) {
		dev_err(dai->dev, "Failed to get probe points: %d\n", ret);
		goto exit;
	}

	for (i = 0; i < num_desc; i++)
		sof_ipc_probe_points_disconnect(sdev, &desc[i].buffer_id, 1);
	kfree(desc);

	/* detach from all dmas */
	ret = sof_ipc_probe_get_dma(sdev, &dma, &num_dma);
	if (ret < 0) {
		dev_err(dai->dev, "Failed to get inject dma: %d\n", ret);
		goto exit;
	}

	for (i = 0; i < num_dma; i++)
		sof_ipc_probe_dma_detach(sdev, &dma[i].stream_tag, 1);
	kfree(dma);

exit:
	ret = sof_ipc_probe_deinit(sdev);
	if (ret < 0)
		dev_err(dai->dev, "Failed to deinit probe: %d\n", ret);

	snd_compr_free_pages(cstream);

	return snd_sof_probe_compr_free(sdev, cstream, dai);
}
EXPORT_SYMBOL(sof_probe_compr_free);

int sof_probe_compr_set_params(struct snd_compr_stream *cstream,
		struct snd_compr_params *params, struct snd_soc_dai *dai)
{
	struct snd_compr_runtime *rtd = cstream->runtime;
	struct snd_sof_dev *sdev =
				snd_soc_component_get_drvdata(dai->component);
	int ret;

	cstream->dma_buffer.dev.type = SNDRV_DMA_TYPE_DEV_SG;
	cstream->dma_buffer.dev.dev = sdev->dev;
	ret = snd_compr_malloc_pages(cstream, rtd->buffer_size);
	if (ret < 0)
		return ret;

	ret = snd_sof_probe_compr_set_params(sdev, cstream, params, dai);
	if (ret < 0)
		return ret;

	ret = sof_ipc_probe_init(sdev, sdev->extractor, rtd->dma_bytes);
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
	struct snd_sof_dev *sdev =
				snd_soc_component_get_drvdata(dai->component);

	return snd_sof_probe_compr_trigger(sdev, cstream, cmd, dai);
}
EXPORT_SYMBOL(sof_probe_compr_trigger);

int sof_probe_compr_pointer(struct snd_compr_stream *cstream,
		struct snd_compr_tstamp *tstamp, struct snd_soc_dai *dai)
{
	struct snd_sof_dev *sdev =
				snd_soc_component_get_drvdata(dai->component);

	return snd_sof_probe_compr_pointer(sdev, cstream, tstamp, dai);
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

	if (cstream->direction == SND_COMPRESS_CAPTURE) {
		div_u64_rem(rtd->total_bytes_transferred,
				rtd->buffer_size, &offset);
		ptr = rtd->dma_area + offset;
		n = rtd->buffer_size - offset;

		if (count < n) {
			ret = copy_to_user(buf, ptr, count);
		} else {
			ret = copy_to_user(buf, ptr, n);
			ret += copy_to_user(buf + n, rtd->dma_area, count - n);
		}
	} else {
		div_u64_rem(rtd->total_bytes_available,
				rtd->buffer_size, &offset);
		ptr = rtd->dma_area + offset;
		n = rtd->buffer_size - offset;

		if (count < n) {
			ret = copy_from_user(ptr, buf, count);
		} else {
			ret = copy_from_user(ptr, buf, n);
			ret += copy_from_user(rtd->dma_area,
					buf + n, count - n);
		}
	}

	if (ret)
		return count - ret;
	return count;
}
EXPORT_SYMBOL(sof_probe_compr_copy);
