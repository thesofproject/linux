// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
//
// This file is provided under a dual BSD/GPLv2 license.  When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2019 Intel Corporation. All rights reserved.
//
// Author: Cezary Rojewski <cezary.rojewski@intel.com>
//

#include <sound/hdaudio_ext.h>
#include <sound/soc.h>
#include "../sof-priv.h"
#include "hda.h"

static inline struct hdac_ext_stream *
hda_compr_get_stream(struct snd_compr_stream *cstream)
{
	return cstream->runtime->private_data;
}

int hda_probe_compr_assign(struct snd_sof_dev *sdev,
			   struct snd_compr_stream *cstream,
			   struct snd_soc_dai *dai)
{
	struct hdac_ext_stream *stream;
	struct hdac_bus *bus = sof_to_bus(sdev);

	stream = snd_hdac_ext_cstream_assign(bus, cstream);
	if (!stream)
		return -EBUSY;

	hdac_stream(stream)->curr_pos = 0;
	cstream->runtime->private_data = stream;

	return hdac_stream(stream)->stream_tag;
}

int hda_probe_compr_free(struct snd_sof_dev *sdev,
			 struct snd_compr_stream *cstream,
			 struct snd_soc_dai *dai)
{
	struct hdac_ext_stream *stream = hda_compr_get_stream(cstream);

	snd_hdac_stream_cleanup(hdac_stream(stream));
	hdac_stream(stream)->prepared = 0;
	snd_hdac_ext_stream_release(stream, HDAC_EXT_STREAM_TYPE_HOST);

	return 0;
}

int hda_probe_compr_set_params(struct snd_sof_dev *sdev,
			       struct snd_compr_stream *cstream,
			       struct snd_compr_params *params,
			       struct snd_soc_dai *dai)
{
	struct hdac_ext_stream *stream = hda_compr_get_stream(cstream);
	unsigned int format_val;
	int bps, ret;
	/* compr params do not store bit depth, default to S32_LE */
	snd_pcm_format_t format = SNDRV_PCM_FORMAT_S32_LE;

	hdac_stream(stream)->bufsize = 0;
	hdac_stream(stream)->period_bytes = 0;
	hdac_stream(stream)->format_val = 0;

	bps = snd_pcm_format_physical_width(format);
	if (bps < 0)
		return bps;
	format_val = snd_hdac_calc_stream_format(params->codec.sample_rate,
			params->codec.ch_out, format, bps, 0);
	ret = snd_hdac_stream_set_params(hdac_stream(stream), format_val);
	if (ret < 0)
		return ret;
	ret = snd_hdac_stream_setup(hdac_stream(stream));
	if (ret < 0)
		return ret;

	hdac_stream(stream)->prepared = 1;

	return 0;
}

int hda_probe_compr_trigger(struct snd_sof_dev *sdev,
			    struct snd_compr_stream *cstream, int cmd,
			    struct snd_soc_dai *dai)
{
	struct hdac_ext_stream *stream = hda_compr_get_stream(cstream);
	struct hdac_bus *bus = sof_to_bus(sdev);
	unsigned long cookie;

	if (!hdac_stream(stream)->prepared)
		return -EPIPE;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
	case SNDRV_PCM_TRIGGER_RESUME:
		spin_lock_irqsave(&bus->reg_lock, cookie);
		snd_hdac_stream_start(hdac_stream(stream), true);
		spin_unlock_irqrestore(&bus->reg_lock, cookie);
		break;

	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_STOP:
		spin_lock_irqsave(&bus->reg_lock, cookie);
		snd_hdac_stream_stop(hdac_stream(stream));
		spin_unlock_irqrestore(&bus->reg_lock, cookie);
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

int hda_probe_compr_pointer(struct snd_sof_dev *sdev,
			    struct snd_compr_stream *cstream,
			    struct snd_compr_tstamp *tstamp,
			    struct snd_soc_dai *dai)
{
	struct hdac_ext_stream *stream = hda_compr_get_stream(cstream);
	struct snd_soc_pcm_stream *pstream;

	if (cstream->direction == SND_COMPRESS_PLAYBACK)
		pstream = &dai->driver->playback;
	else
		pstream = &dai->driver->capture;

	tstamp->copied_total = hdac_stream(stream)->curr_pos;
	tstamp->sampling_rate = snd_pcm_rate_bit_to_rate(pstream->rates);
	return 0;
}
