// SPDX-License-Identifier: GPL-2.0-only
//
// Copyright(c) 2024-2026 Intel Corporation
//
// Authors: Cezary Rojewski <cezary.rojewski@intel.com>
//          Amadeusz Slawinski <amade@asmblr.net>
//

#include <linux/device.h>
#include <linux/soundwire/sdw.h>
#include <linux/soundwire/sdw_intel.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-acpi.h>
#include <sound/soc-component.h>
#include <sound/soc-dai.h>
#include "avs.h"
#include "path.h"
#include "pcm.h"
#include "soundwire.h"
#include "topology.h"

static int avs_sdw_dai_startup(struct snd_pcm_substream *substream, struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct sdw_stream_runtime *sdw_stream;
	struct snd_soc_acpi_mach *mach;
	struct snd_soc_dai *codec_dai;
	struct avs_dma_data *data;
	struct sdw_cdns_pdi *pdi;
	struct sdw_slave *codec;
	int dir, ret;

	mach = dev_get_platdata(dai->component->card->dev);
	codec = mach->pdata;
	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		dir = SDW_DATA_DIR_RX;
	else
		dir = SDW_DATA_DIR_TX;

	ret = avs_dai_startup(substream, dai);
	if (ret)
		return ret;

	sdw_stream = sdw_alloc_stream(substream->name, SDW_STREAM_PCM);
	if (!sdw_stream) {
		ret = -ENOMEM;
		goto err_alloc_stream;
	}

	pdi = sdw_cdns_assign_pdi(codec->bus, dir);
	if (!pdi) {
		ret = -EBUSY;
		goto err_assign_pdi;
	}

	/* Fulfill the codec needs by setting the sdw_stream. */
	codec_dai = snd_soc_rtd_to_codec(rtd, 0);
	snd_soc_dai_set_stream(codec_dai, sdw_stream, substream->stream);

	data = snd_soc_dai_get_dma_data(dai, substream);
	data->sdw_stream = sdw_stream;
	data->pdi = pdi;
	return 0;

err_assign_pdi:
	sdw_release_stream(sdw_stream);
err_alloc_stream:
	avs_dai_shutdown(substream, dai);
	return ret;
}

static void avs_sdw_dai_shutdown(struct snd_pcm_substream *substream, struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_acpi_mach *mach;
	struct snd_soc_dai *codec_dai;
	struct avs_dma_data *data;
	struct sdw_slave *codec;

	codec_dai = snd_soc_rtd_to_codec(rtd, 0);
	data = snd_soc_dai_get_dma_data(dai, substream);
	mach = dev_get_platdata(dai->component->card->dev);
	codec = mach->pdata;

	snd_soc_dai_set_stream(codec_dai, NULL, substream->stream);
	sdw_cdns_release_pdi(codec->bus, data->pdi);
	sdw_release_stream(data->sdw_stream);
	avs_dai_shutdown(substream, dai);
}

static int avs_sdw_dai_hw_params(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *hw_params,
				 struct snd_soc_dai *dai)
{
	struct sdw_stream_config sconfig = {0};
	struct sdw_port_config pconfig = {0};
	struct snd_soc_acpi_mach *mach;
	struct avs_dma_data *data;
	struct sdw_cdns_pdi *pdi;
	struct sdw_slave *codec;
	int ret;

	data = snd_soc_dai_get_dma_data(dai, substream);
	if (data->path)
		return 0;

	mach = dev_get_platdata(dai->component->card->dev);
	codec = mach->pdata;
	pdi = data->pdi;
	pdi->ch_count = params_channels(hw_params);
	pdi->h_ch_num = pdi->ch_count - 1;
	pdi->l_ch_num = 0;
	sdw_intel_pdi_config(codec->bus, pdi);

	if (data->sdw_suspended)
		return avs_dai_be_hw_params(substream, hw_params, dai, pdi->intel_alh_id);

	sconfig.direction = pdi->dir;
	sconfig.ch_count = pdi->ch_count;
	sconfig.frame_rate = params_rate(hw_params);
	sconfig.type = SDW_STREAM_PCM;
	sconfig.bps = snd_pcm_format_width(params_format(hw_params));
	pconfig.num = pdi->num;
	pconfig.ch_mask = (1 << pdi->ch_count) - 1;

	ret = sdw_stream_add_master(codec->bus, &sconfig, &pconfig, 1, data->sdw_stream);
	if (ret)
		return ret;

	ret = avs_dai_be_hw_params(substream, hw_params, dai, pdi->intel_alh_id);
	if (ret)
		sdw_stream_remove_master(codec->bus, data->sdw_stream);
	return ret;
}

static int avs_sdw_dai_hw_free(struct snd_pcm_substream *substream, struct snd_soc_dai *dai)
{
	struct snd_soc_acpi_mach *mach;
	struct avs_dma_data *data;
	struct sdw_slave *codec;

	data = snd_soc_dai_get_dma_data(dai, substream);
	if (!data->path)
		return 0;

	mach = dev_get_platdata(dai->component->card->dev);
	codec = mach->pdata;

	avs_path_reset(data->path);
	avs_path_free(data->path);
	data->path = NULL;
	sdw_deprepare_stream(data->sdw_stream);
	if (!data->sdw_suspended)
		sdw_stream_remove_master(codec->bus, data->sdw_stream);

	return 0;
}

static int avs_sdw_dai_prepare(struct snd_pcm_substream *substream, struct snd_soc_dai *dai)
{
	struct avs_dma_data *data;
	int ret;

	data = snd_soc_dai_get_dma_data(dai, substream);

	/*
	 * There is no guarantee trigger(RESUME) fires, so reset the flag here.
	 * Ensures there is no sdw_master leak should hw_free() occur without resuming.
	 */
	data->sdw_suspended = false;

	ret = sdw_prepare_stream(data->sdw_stream);
	if (ret) {
		dev_err(dai->dev, "sdw stream prepare failed: %d\n", ret);
		return ret;
	}

	ret = avs_dai_prepare(substream, dai);
	if (ret)
		sdw_deprepare_stream(data->sdw_stream);
	return ret;
}

static int avs_sdw_dai_trigger(struct snd_pcm_substream *substream, int cmd,
			       struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct avs_dma_data *data;
	int ret = 0;

	data = snd_soc_dai_get_dma_data(dai, substream);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_RESUME:
		if (rtd->dai_link->ignore_suspend)
			break;
		fallthrough;
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		ret = sdw_enable_stream(data->sdw_stream);
		if (ret)
			break;

		ret = avs_path_run(data->path, AVS_TPLG_TRIGGER_AUTO);
		if (ret)
			sdw_disable_stream(data->sdw_stream);
		break;

	case SNDRV_PCM_TRIGGER_SUSPEND:
		if (rtd->dai_link->ignore_suspend)
			break;
		data->sdw_suspended = true;
		fallthrough;
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
	case SNDRV_PCM_TRIGGER_STOP:
		avs_path_pause(data->path);

		ret = sdw_disable_stream(data->sdw_stream);
		if (ret)
			dev_err(dai->dev, "disable sdw stream failed: %d\n", ret);
		break;

	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static const struct snd_soc_dai_ops avs_sdw_dai_ops = {
	.startup = avs_sdw_dai_startup,
	.shutdown = avs_sdw_dai_shutdown,
	.hw_params = avs_sdw_dai_hw_params,
	.hw_free = avs_sdw_dai_hw_free,
	.prepare = avs_sdw_dai_prepare,
	.trigger = avs_sdw_dai_trigger,
};

static const struct snd_soc_dai_driver avs_sdw_dai_template = {
	.playback = {
		.channels_min	= 1,
		.channels_max	= 8,
		.rates		= SNDRV_PCM_RATE_8000_192000 |
				  SNDRV_PCM_RATE_KNOT,
		.formats	= SNDRV_PCM_FMTBIT_S16_LE |
				  SNDRV_PCM_FMTBIT_S32_LE,
		.subformats	= SNDRV_PCM_SUBFMTBIT_MSBITS_20 |
				  SNDRV_PCM_SUBFMTBIT_MSBITS_24 |
				  SNDRV_PCM_SUBFMTBIT_MSBITS_MAX,
	},
	.capture = {
		.channels_min	= 1,
		.channels_max	= 8,
		.rates		= SNDRV_PCM_RATE_8000_192000 |
				  SNDRV_PCM_RATE_KNOT,
		.formats	= SNDRV_PCM_FMTBIT_S16_LE |
				  SNDRV_PCM_FMTBIT_S32_LE,
		.subformats	= SNDRV_PCM_SUBFMTBIT_MSBITS_20 |
				  SNDRV_PCM_SUBFMTBIT_MSBITS_24 |
				  SNDRV_PCM_SUBFMTBIT_MSBITS_MAX,
	},
	.ops = &avs_sdw_dai_ops,
};

int avs_register_sdw_component(struct avs_dev *adev, const char *name, u32 link_id)
{
	struct snd_soc_dai_driver *dai;
	struct device *dev = adev->dev;

	dai = devm_kzalloc(dev, sizeof(*dai), GFP_KERNEL);
	if (!dai)
		return -ENOMEM;

	memcpy(dai, &avs_sdw_dai_template, sizeof(*dai));

	dai->name = devm_kasprintf(dev, GFP_KERNEL, "avs-sdw%d-pin", link_id);
	if (!dai->name)
		return -ENOMEM;

	dai->playback.stream_name = devm_kasprintf(dev, GFP_KERNEL, "%s Tx", dai->name);
	dai->capture.stream_name = devm_kasprintf(dev, GFP_KERNEL, "%s Rx", dai->name);

	if (!dai->playback.stream_name || !dai->capture.stream_name)
		return -ENOMEM;

	return avs_register_component(dev, name, &avs_component_driver, dai, 1);
}
