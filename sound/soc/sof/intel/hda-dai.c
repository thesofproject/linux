// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
/*
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * Copyright(c) 2018 Intel Corporation. All rights reserved.
 *
 * Authors: Keyon Jie <yang.jie@linux.intel.com>
 */

#include <sound/pcm_params.h>
#include <sound/hdaudio_ext.h>
#include <sound/hda_register.h>
#include "../sof-priv.h"
#include "hda.h"

#define SKL_FORMATS (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE | \
	SNDRV_PCM_FMTBIT_S32_LE)

#if IS_ENABLED(CONFIG_SND_SOC_SOF_HDA)

struct hda_pipe_params {
	u8 host_dma_id;
	u8 link_dma_id;
	u32 ch;
	u32 s_freq;
	u32 s_fmt;
	u8 linktype;
	snd_pcm_format_t format;
	int link_index;
	int stream;
	unsigned int host_bps;
	unsigned int link_bps;
};

/* TODO: add hda dai params in tplg, and configure this in topology parsing */
static int hda_link_dma_params(struct hdac_ext_stream *stream,
			       struct hda_pipe_params *params)
{
	struct hdac_stream *hstream = &stream->hstream;
	struct hdac_bus *bus = hstream->bus;
	unsigned int format_val;
	struct hdac_ext_link *link;

	snd_hdac_ext_stream_decouple(bus, stream, true);
	snd_hdac_ext_link_stream_reset(stream);

	format_val = snd_hdac_calc_stream_format(params->s_freq, params->ch,
					params->format, params->link_bps, 0);

	dev_dbg(bus->dev, "format_val=%d, rate=%d, ch=%d, format=%d\n",
		format_val, params->s_freq, params->ch, params->format);

	snd_hdac_ext_link_stream_setup(stream, format_val);

	list_for_each_entry(link, &bus->hlink_list, list) {
		if (link->index == params->link_index)
			snd_hdac_ext_link_set_stream_id(link,
					hstream->stream_tag);
	}

	stream->link_prepared = 1;

	return 0;
}

static int hda_link_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params,
				struct snd_soc_dai *dai)
{
	struct hdac_stream *hstream = substream->runtime->private_data;
	struct hdac_bus *bus = hstream->bus;
	struct hdac_ext_stream *link_dev;
	struct snd_soc_pcm_runtime *rtd = snd_pcm_substream_chip(substream);
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct hda_pipe_params p_params = {0};
	struct hdac_ext_link *link;
	int stream_tag;

	link_dev = snd_hdac_ext_stream_assign(bus, substream,
					HDAC_EXT_STREAM_TYPE_LINK);
	if (!link_dev)
		return -EBUSY;

	snd_soc_dai_set_dma_data(dai, substream, (void *)link_dev);

	link = snd_hdac_ext_bus_get_link(bus, codec_dai->component->name);
	if (!link)
		return -EINVAL;

	stream_tag = hdac_stream(link_dev)->stream_tag;

	/* set the stream tag in the codec dai dma params  */
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		snd_soc_dai_set_tdm_slot(codec_dai, stream_tag, 0, 0, 0);
	else
		snd_soc_dai_set_tdm_slot(codec_dai, 0, stream_tag, 0, 0);

	p_params.s_fmt = snd_pcm_format_width(params_format(params));
	p_params.ch = params_channels(params);
	p_params.s_freq = params_rate(params);
	p_params.stream = substream->stream;
	p_params.link_dma_id = stream_tag - 1;
	p_params.link_index = link->index;
	p_params.format = params_format(params);

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		p_params.link_bps = codec_dai->driver->playback.sig_bits;
	else
		p_params.link_bps = codec_dai->driver->capture.sig_bits;

	return hda_link_dma_params(link_dev, &p_params);
}

static int hda_link_pcm_trigger(struct snd_pcm_substream *substream,
	int cmd, struct snd_soc_dai *dai)
{
	struct hdac_ext_stream *link_dev =
				snd_soc_dai_get_dma_data(dai, substream);
	struct hdac_stream *hstream = substream->runtime->private_data;
	struct hdac_bus *bus = hstream->bus;
	struct hdac_ext_stream *stream = stream_to_hdac_ext_stream(hstream);

	dev_dbg(dai->dev, "In %s cmd=%d\n", __func__, cmd);
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		snd_hdac_ext_link_stream_start(link_dev);
		break;

	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_STOP:
		snd_hdac_ext_link_stream_clear(link_dev);
		if (cmd == SNDRV_PCM_TRIGGER_SUSPEND)
			snd_hdac_ext_stream_decouple(bus, stream, false);
		break;

	default:
		return -EINVAL;
	}
	return 0;
}

static int hda_link_hw_free(struct snd_pcm_substream *substream,
		struct snd_soc_dai *dai)
{
	struct hdac_stream *hstream = substream->runtime->private_data;
	struct hdac_bus *bus = hstream->bus;
	struct snd_soc_pcm_runtime *rtd = snd_pcm_substream_chip(substream);
	struct hdac_ext_stream *link_dev =
				snd_soc_dai_get_dma_data(dai, substream);
	struct hdac_ext_link *link;

	link = snd_hdac_ext_bus_get_link(bus, rtd->codec_dai->component->name);
	if (!link)
		return -EINVAL;

	snd_hdac_ext_link_clear_stream_id(link,
					  hdac_stream(link_dev)->stream_tag);
	snd_hdac_ext_stream_release(link_dev, HDAC_EXT_STREAM_TYPE_LINK);

	link_dev->link_prepared = 0;

	return 0;
}

static const struct snd_soc_dai_ops hda_link_dai_ops = {
	.hw_params = hda_link_hw_params,
	.hw_free = hda_link_hw_free,
	.trigger = hda_link_pcm_trigger,
};
#endif

/*
 * common dai driver for skl+ platforms.
 * some products who use this DAI array only physically have a subset of
 * the DAIs, but no harm is done here by adding the whole set.
 */
struct snd_soc_dai_driver skl_dai[] = {
{
	.name = "SSP0 Pin",
	.playback = SOF_DAI_STREAM("ssp0 Tx", 1, 8,
				   SNDRV_PCM_RATE_8000_192000, SKL_FORMATS),
	.capture = SOF_DAI_STREAM("ssp0 Rx", 1, 8,
				  SNDRV_PCM_RATE_8000_192000, SKL_FORMATS),
},
{
	.name = "SSP1 Pin",
	.playback = SOF_DAI_STREAM("ssp1 Tx", 1, 8,
				   SNDRV_PCM_RATE_8000_192000, SKL_FORMATS),
	.capture = SOF_DAI_STREAM("ssp1 Rx", 1, 8,
				  SNDRV_PCM_RATE_8000_192000, SKL_FORMATS),
},
{
	.name = "SSP2 Pin",
	.playback = SOF_DAI_STREAM("ssp2 Tx", 1, 8,
				   SNDRV_PCM_RATE_8000_192000, SKL_FORMATS),
	.capture = SOF_DAI_STREAM("ssp2 Rx", 1, 8,
				  SNDRV_PCM_RATE_8000_192000, SKL_FORMATS),
},
{
	.name = "SSP3 Pin",
	.playback = SOF_DAI_STREAM("ssp3 Tx", 1, 8,
				   SNDRV_PCM_RATE_8000_192000, SKL_FORMATS),
	.capture = SOF_DAI_STREAM("ssp3 Rx", 1, 8,
				  SNDRV_PCM_RATE_8000_192000, SKL_FORMATS),
},
{
	.name = "SSP4 Pin",
	.playback = SOF_DAI_STREAM("ssp4 Tx", 1, 8,
				   SNDRV_PCM_RATE_8000_192000, SKL_FORMATS),
	.capture = SOF_DAI_STREAM("ssp4 Rx", 1, 8,
				  SNDRV_PCM_RATE_8000_192000, SKL_FORMATS),
},
{
	.name = "SSP5 Pin",
	.playback = SOF_DAI_STREAM("ssp5 Tx", 1, 8,
				   SNDRV_PCM_RATE_8000_192000, SKL_FORMATS),
	.capture = SOF_DAI_STREAM("ssp5 Rx", 1, 8,
				  SNDRV_PCM_RATE_8000_192000, SKL_FORMATS),
},
{
	.name = "DMIC01 Pin",
	.capture = SOF_DAI_STREAM("DMIC01 Rx", 1, 4,
				  SNDRV_PCM_RATE_8000_192000, SKL_FORMATS),
},
{
	.name = "DMIC16k Pin",
	.capture = SOF_DAI_STREAM("DMIC16k Rx", 1, 4,
				  SNDRV_PCM_RATE_16000, SKL_FORMATS),
},
#if IS_ENABLED(CONFIG_SND_SOC_SOF_HDA)
{
	.name = "iDisp1 Pin",
	.ops = &hda_link_dai_ops,
	.playback = SOF_DAI_STREAM("iDisp1 Tx", 1, 8,
				   SNDRV_PCM_RATE_8000_192000, SKL_FORMATS),
},
{
	.name = "iDisp2 Pin",
	.ops = &hda_link_dai_ops,
	.playback = SOF_DAI_STREAM("iDisp2 Tx", 1, 8,
				   SNDRV_PCM_RATE_8000_192000, SKL_FORMATS),
},
{
	.name = "iDisp3 Pin",
	.ops = &hda_link_dai_ops,
	.playback = SOF_DAI_STREAM("iDisp3 Tx", 1, 8,
				   SNDRV_PCM_RATE_8000_192000, SKL_FORMATS),
},
{
	.name = "Analog CPU DAI",
	.ops = &hda_link_dai_ops,
	.playback = SOF_DAI_STREAM("Analog CPU Playback", 1, 16,
				   SNDRV_PCM_RATE_8000_192000, SKL_FORMATS),
	.capture = SOF_DAI_STREAM("Analog CPU Capture", 1, 16,
				   SNDRV_PCM_RATE_8000_192000, SKL_FORMATS),
},
{
	.name = "Digital CPU DAI",
	.ops = &hda_link_dai_ops,
	.playback = SOF_DAI_STREAM("Digital CPU Playback", 1, 16,
				   SNDRV_PCM_RATE_8000_192000, SKL_FORMATS),
	.capture = SOF_DAI_STREAM("Digital CPU Capture", 1, 16,
				   SNDRV_PCM_RATE_8000_192000, SKL_FORMATS),
},
{
	.name = "Alt Analog CPU DAI",
	.ops = &hda_link_dai_ops,
	.playback = SOF_DAI_STREAM("Alt Analog CPU Playback", 1, 16,
				   SNDRV_PCM_RATE_8000_192000, SKL_FORMATS),
	.capture = SOF_DAI_STREAM("Alt Analog CPU Capture", 1, 16,
				   SNDRV_PCM_RATE_8000_192000, SKL_FORMATS),
},
#endif
};
