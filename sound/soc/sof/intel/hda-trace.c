// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
//
// This file is provided under a dual BSD/GPLv2 license.  When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2018 Intel Corporation. All rights reserved.
//
// Authors: Liam Girdwood <liam.r.girdwood@linux.intel.com>
//	    Ranjani Sridharan <ranjani.sridharan@linux.intel.com>
//	    Rander Wang <rander.wang@intel.com>
//          Keyon Jie <yang.jie@linux.intel.com>
//

/*
 * Hardware interface for generic Intel audio DSP HDA IP
 */

#include <sound/hdaudio_ext.h>
#include "../ops.h"
#include "../sof-client-dma-trace.h"
#include "../sof-client.h"
#include "hda.h"

static int hda_dsp_trace_prepare(struct snd_sof_dev *sdev, struct snd_dma_buffer *dmab)
{
	struct sof_intel_hda_dev *hda = sdev->pdata->hw_pdata;
	struct hdac_ext_stream *stream = hda->dtrace_stream;
	struct hdac_stream *hstream = &stream->hstream;
	int ret;

	hstream->period_bytes = 0;/* initialize period_bytes */
	hstream->bufsize = dmab->bytes;

	ret = hda_dsp_stream_hw_params(sdev, stream, dmab, NULL);
	if (ret < 0)
		dev_err(sdev->dev, "error: hdac prepare failed: %d\n", ret);

	return ret;
}

int hda_dsp_trace_init(struct snd_sof_dev *sdev, u32 *stream_tag)
{
	struct sof_intel_hda_dev *hda = sdev->pdata->hw_pdata;
	int ret;

	hda->dtrace_stream = hda_dsp_stream_get(sdev, SNDRV_PCM_STREAM_CAPTURE,
						SOF_HDA_STREAM_DMI_L1_COMPATIBLE);

	if (!hda->dtrace_stream) {
		dev_err(sdev->dev,
			"error: no available capture stream for DMA trace\n");
		return -ENODEV;
	}

	*stream_tag = hda->dtrace_stream->hstream.stream_tag;

	/*
	 * initialize capture stream, set BDL address and return corresponding
	 * stream tag which will be sent to the firmware by IPC message.
	 */
	ret = hda_dsp_trace_prepare(sdev, &sdev->dmatb);
	if (ret < 0) {
		dev_err(sdev->dev, "error: hdac trace init failed: %d\n", ret);
		hda_dsp_stream_put(sdev, SNDRV_PCM_STREAM_CAPTURE, *stream_tag);
		hda->dtrace_stream = NULL;
		*stream_tag = 0;
	}

	return ret;
}

int hda_dsp_trace_release(struct snd_sof_dev *sdev)
{
	struct sof_intel_hda_dev *hda = sdev->pdata->hw_pdata;
	struct hdac_stream *hstream;

	if (hda->dtrace_stream) {
		hstream = &hda->dtrace_stream->hstream;
		hda_dsp_stream_put(sdev,
				   SNDRV_PCM_STREAM_CAPTURE,
				   hstream->stream_tag);
		hda->dtrace_stream = NULL;
		return 0;
	}

	dev_dbg(sdev->dev, "DMA trace stream is not opened!\n");
	return -ENODEV;
}

int hda_dsp_trace_trigger(struct snd_sof_dev *sdev, int cmd)
{
	struct sof_intel_hda_dev *hda = sdev->pdata->hw_pdata;

	return hda_dsp_stream_trigger(sdev, hda->dtrace_stream, cmd);
}

#if IS_ENABLED(CONFIG_SND_SOC_SOF_HDA_DMA_TRACE)
static int hda_dma_trace_init(struct sof_client_dev *cdev,
			      struct snd_dma_buffer *dmab, u32 *stream_tag)
{
	struct snd_sof_dev *sdev = sof_client_dev_to_sof_dev(cdev);
	struct sof_intel_hda_dev *hda = sdev->pdata->hw_pdata;
	int ret;

	hda->dtrace_stream = hda_dsp_stream_get(sdev, SNDRV_PCM_STREAM_CAPTURE,
						SOF_HDA_STREAM_DMI_L1_COMPATIBLE);

	if (!hda->dtrace_stream) {
		dev_err(sdev->dev,
			"error: no available capture stream for DMA trace\n");
		return -ENODEV;
	}

	*stream_tag = hda->dtrace_stream->hstream.stream_tag;

	/*
	 * initialize capture stream, set BDL address and return corresponding
	 * stream tag which will be sent to the firmware by IPC message.
	 */
	ret = hda_dsp_trace_prepare(sdev, dmab);
	if (ret < 0) {
		dev_err(sdev->dev, "error: hdac trace init failed: %d\n", ret);
		hda_dsp_stream_put(sdev, SNDRV_PCM_STREAM_CAPTURE, *stream_tag);
		hda->dtrace_stream = NULL;
		*stream_tag = 0;
	}

	return ret;
}

static int hda_dma_trace_release(struct sof_client_dev *cdev)
{
	struct snd_sof_dev *sdev = sof_client_dev_to_sof_dev(cdev);

	return hda_dsp_trace_release(sdev);
}

static int hda_dma_trace_start(struct sof_client_dev *cdev)
{
	struct snd_sof_dev *sdev = sof_client_dev_to_sof_dev(cdev);

	return hda_dsp_trace_trigger(sdev, SNDRV_PCM_TRIGGER_START);
}

static int hda_dma_trace_stop(struct sof_client_dev *cdev)
{
	struct snd_sof_dev *sdev = sof_client_dev_to_sof_dev(cdev);

	return hda_dsp_trace_trigger(sdev, SNDRV_PCM_TRIGGER_STOP);
}

static const struct sof_dma_trace_host_ops hda_dma_trace_ops = {
	.init = hda_dma_trace_init,
	.release = hda_dma_trace_release,
	.start = hda_dma_trace_start,
	.stop = hda_dma_trace_stop,
};

int hda_dma_trace_register(struct snd_sof_dev *sdev)
{
	return sof_client_dev_register(sdev, "hda-dma-trace", 0, &hda_dma_trace_ops,
				       sizeof(hda_dma_trace_ops));
}

void hda_dma_trace_unregister(struct snd_sof_dev *sdev)
{
	sof_client_dev_unregister(sdev, "hda-dma-trace", 0);
}

MODULE_IMPORT_NS(SND_SOC_SOF_CLIENT);
#endif
