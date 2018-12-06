// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
//
// This file is provided under a dual BSD/GPLv2 license.  When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2018 Intel Corporation. All rights reserved.
//
// Authors: Keyon Jie <yang.jie@linux.intel.com>
//

#include <sound/pcm_params.h>
#include <sound/hdaudio_ext.h>
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

/* Unlike GP dma, there is a set of stream registers in hda controller to
 * control the link dma channels. Each register controls one link dma
 * channel and the relation is fixed. To make sure FW uses the correct
 * link dma channel, host allocates stream register and sends the
 * corresponding link dma channel to FW to allocate link dma channel
 *
 * FIXME: this API is abused in the sense that tx_num and rx_num are
 * passed as arguments, not returned. We need to find a better way to
 * retrieve the stream tag allocated for the link DMA
 *
 */
static int hda_link_dma_get_channels(struct snd_soc_dai *dai,
				     unsigned int *tx_num,
				     unsigned int *tx_slot,
				     unsigned int *rx_num,
				     unsigned int *rx_slot)
{
	struct hdac_bus *bus;
	struct hdac_ext_stream *stream;
	struct snd_pcm_substream substream;
	struct snd_sof_dev *sdev =
		snd_soc_component_get_drvdata(dai->component);

	bus = sof_to_bus(sdev);

	memset(&substream, 0, sizeof(substream));
	if (*tx_num == 1) {
		substream.stream = SNDRV_PCM_STREAM_PLAYBACK;
		stream = snd_hdac_ext_stream_assign(bus, &substream,
						    HDAC_EXT_STREAM_TYPE_LINK);
		if (!stream) {
			dev_err(bus->dev, "error: failed to find a free hda ext stream for playback");
			return -EBUSY;
		}

		snd_soc_dai_set_dma_data(dai, &substream, (void *)stream);
		*tx_slot = hdac_stream(stream)->stream_tag - 1;

		dev_dbg(bus->dev, "link dma channel %d for playback", *tx_slot);
	}

	if (*rx_num == 1) {
		substream.stream = SNDRV_PCM_STREAM_CAPTURE;
		stream = snd_hdac_ext_stream_assign(bus, &substream,
						    HDAC_EXT_STREAM_TYPE_LINK);
		if (!stream) {
			dev_err(bus->dev, "error: failed to find a free hda ext stream for capture");
			return -EBUSY;
		}

		snd_soc_dai_set_dma_data(dai, &substream, (void *)stream);
		*rx_slot = hdac_stream(stream)->stream_tag - 1;

		dev_dbg(bus->dev, "link dma channel %d for capture", *rx_slot);
	}

	return 0;
}

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
						 params->format,
						 params->link_bps, 0);

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

	link_dev = snd_soc_dai_get_dma_data(dai, substream);

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

/*
 * FIXME: This API is also abused since it's used for two purposes.
 * when the substream argument is NULL this function is used for cleanups
 * that aren't necessarily required, and called explicitly by handling
 * ASoC core structures, which is not recommended.
 * This part will be reworked in follow-up patches.
 */
static int hda_link_hw_free(struct snd_pcm_substream *substream,
			    struct snd_soc_dai *dai)
{
	const char *name;
	unsigned int stream_tag;
	struct hdac_bus *bus;
	struct hdac_ext_link *link;
	struct snd_sof_dev *sdev;
	struct hdac_stream *hstream;
	struct hdac_ext_stream *stream;
	struct snd_soc_pcm_runtime *rtd;
	struct hdac_ext_stream *link_dev;
	struct snd_pcm_substream pcm_substream;

	memset(&pcm_substream, 0, sizeof(pcm_substream));
	if (substream) {
		hstream = substream->runtime->private_data;
		bus = hstream->bus;
		rtd = snd_pcm_substream_chip(substream);
		link_dev = snd_soc_dai_get_dma_data(dai, substream);
		name = rtd->codec_dai->component->name;
		link = snd_hdac_ext_bus_get_link(bus, name);
		if (!link)
			return -EINVAL;

		stream_tag = hdac_stream(link_dev)->stream_tag;
		snd_hdac_ext_link_clear_stream_id(link, stream_tag);

		link_dev->link_prepared = 0;
	} else {
		/* release all hda streams when dai link is unloaded */
		sdev = snd_soc_component_get_drvdata(dai->component);
		pcm_substream.stream = SNDRV_PCM_STREAM_PLAYBACK;
		stream = snd_soc_dai_get_dma_data(dai, &pcm_substream);
		if (stream) {
			snd_soc_dai_set_dma_data(dai, &pcm_substream, NULL);
			snd_hdac_ext_stream_release(stream,
						    HDAC_EXT_STREAM_TYPE_LINK);
		}

		pcm_substream.stream = SNDRV_PCM_STREAM_CAPTURE;
		stream = snd_soc_dai_get_dma_data(dai, &pcm_substream);
		if (stream) {
			snd_soc_dai_set_dma_data(dai, &pcm_substream, NULL);
			snd_hdac_ext_stream_release(stream,
						    HDAC_EXT_STREAM_TYPE_LINK);
		}
	}

	return 0;
}

static const struct snd_soc_dai_ops hda_link_dai_ops = {
	.hw_params = hda_link_hw_params,
	.hw_free = hda_link_hw_free,
	.trigger = hda_link_pcm_trigger,
	.get_channel_map = hda_link_dma_get_channels,
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
