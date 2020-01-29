// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
//
// This file is provided under a dual BSD/GPLv2 license.  When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2018 Intel Corporation. All rights reserved.
//
// Author: Liam Girdwood <liam.r.girdwood@linux.intel.com>
//

#include <linux/module.h>
#include <sound/pcm_params.h>
#include <sound/sof.h>
#include <sound/sof/dai.h>
#include "sof-priv.h"
#include "sof-audio.h"

static struct snd_soc_card sof_nocodec_card = {
	.name = "nocodec", /* the sof- prefix is added by the core */
};

static struct snd_sof_dai *x_snd_sof_find_dai(struct snd_soc_component *scomp,
				     const char *name)
{
	struct snd_sof_dev *sdev = snd_soc_component_get_drvdata(scomp);
	struct snd_sof_dai *dai;

	list_for_each_entry(dai, &sdev->dai_list, list) {
		if (dai->name && (strcmp(name, dai->name) == 0))
			return dai;
	}

	return NULL;
}

/* fixup the BE DAI link to match any values from topology */
static int sof_pcm_dai_link_fixup(struct snd_soc_pcm_runtime *rtd,
				  struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
			SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
						SNDRV_PCM_HW_PARAM_CHANNELS);
	struct snd_mask *fmt = hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT);
	struct snd_soc_component *component =
		snd_soc_rtdcom_lookup(rtd, SOF_AUDIO_PCM_DRV_NAME);
	struct snd_sof_dai *dai =
		x_snd_sof_find_dai(component, (char *)rtd->dai_link->name);

	/* no topology exists for this BE, try a common configuration */
	if (!dai) {
		dev_warn(component->dev,
			 "warning: no topology found for BE DAI %s config\n",
			 rtd->dai_link->name);

		/*  set 48k, stereo, 16bits by default */
		rate->min = 48000;
		rate->max = 48000;

		channels->min = 2;
		channels->max = 2;

		snd_mask_none(fmt);
		snd_mask_set_format(fmt, SNDRV_PCM_FORMAT_S16_LE);

		return 0;
	}

	/* read format from topology */
	snd_mask_none(fmt);

	switch (dai->comp_dai.config.frame_fmt) {
	case SOF_IPC_FRAME_S16_LE:
		snd_mask_set_format(fmt, SNDRV_PCM_FORMAT_S16_LE);
		break;
	case SOF_IPC_FRAME_S24_4LE:
		snd_mask_set_format(fmt, SNDRV_PCM_FORMAT_S24_LE);
		break;
	case SOF_IPC_FRAME_S32_LE:
		snd_mask_set_format(fmt, SNDRV_PCM_FORMAT_S32_LE);
		break;
	default:
		dev_err(component->dev, "error: No available DAI format!\n");
		return -EINVAL;
	}

	/* read rate and channels from topology */
	switch (dai->dai_config->type) {
	case SOF_DAI_INTEL_SSP:
		rate->min = dai->dai_config->ssp.fsync_rate;
		rate->max = dai->dai_config->ssp.fsync_rate;
		channels->min = dai->dai_config->ssp.tdm_slots;
		channels->max = dai->dai_config->ssp.tdm_slots;

		dev_dbg(component->dev,
			"rate_min: %d rate_max: %d\n", rate->min, rate->max);
		dev_dbg(component->dev,
			"channels_min: %d channels_max: %d\n",
			channels->min, channels->max);

		break;
	case SOF_DAI_INTEL_DMIC:
		/* DMIC only supports 16 or 32 bit formats */
		if (dai->comp_dai.config.frame_fmt == SOF_IPC_FRAME_S24_4LE) {
			dev_err(component->dev,
				"error: invalid fmt %d for DAI type %d\n",
				dai->comp_dai.config.frame_fmt,
				dai->dai_config->type);
		}
		break;
	case SOF_DAI_INTEL_HDA:
		/* do nothing for HDA dai_link */
		break;
	case SOF_DAI_INTEL_ALH:
		/* do nothing for ALH dai_link */
		break;
	case SOF_DAI_IMX_ESAI:
		channels->min = dai->dai_config->esai.tdm_slots;
		channels->max = dai->dai_config->esai.tdm_slots;

		dev_dbg(component->dev,
			"channels_min: %d channels_max: %d\n",
			channels->min, channels->max);
		break;
	case SOF_DAI_IMX_SAI:
		channels->min = dai->dai_config->sai.tdm_slots;
		channels->max = dai->dai_config->sai.tdm_slots;

		dev_dbg(component->dev,
			"channels_min: %d channels_max: %d\n",
			channels->min, channels->max);
		break;
	default:
		dev_err(component->dev, "error: invalid DAI type %d\n",
			dai->dai_config->type);
		break;
	}

	return 0;
}

static int sof_nocodec_bes_setup(struct device *dev,
				 const struct snd_sof_dsp_ops *ops,
				 struct snd_soc_dai_link *links,
				 int link_num, struct snd_soc_card *card)
{
	struct snd_soc_dai_link_component *dlc;
	int i;

	if (!ops || !links || !card)
		return -EINVAL;

	/* set up BE dai_links */
	for (i = 0; i < link_num; i++) {
		dlc = devm_kzalloc(dev, 3 * sizeof(*dlc), GFP_KERNEL);
		if (!dlc)
			return -ENOMEM;

		links[i].name = devm_kasprintf(dev, GFP_KERNEL,
					       "NoCodec-%d", i);
		if (!links[i].name)
			return -ENOMEM;

		links[i].stream_name = links[i].name;

		links[i].cpus = &dlc[0];
		links[i].codecs = &dlc[1];
		links[i].platforms = &dlc[2];

		links[i].num_cpus = 1;
		links[i].num_codecs = 1;
		links[i].num_platforms = 1;

		links[i].id = i;
		links[i].no_pcm = 1;
		links[i].cpus->dai_name = ops->drv[i].name;
		links[i].platforms->name = dev_name(dev);
		links[i].codecs->dai_name = "snd-soc-dummy-dai";
		links[i].codecs->name = "snd-soc-dummy";
		links[i].dpcm_playback = 1;
		links[i].dpcm_capture = 1;
		links[i].be_hw_params_fixup = sof_pcm_dai_link_fixup;
	}

	card->dai_link = links;
	card->num_links = link_num;

	return 0;
}

int sof_nocodec_setup(struct device *dev,
		      const struct snd_sof_dsp_ops *ops)
{
	struct snd_soc_dai_link *links;
	int ret;

	/* create dummy BE dai_links */
	links = devm_kzalloc(dev, sizeof(struct snd_soc_dai_link) *
			     ops->num_drv, GFP_KERNEL);
	if (!links)
		return -ENOMEM;

	ret = sof_nocodec_bes_setup(dev, ops, links, ops->num_drv,
				    &sof_nocodec_card);
	return ret;
}
EXPORT_SYMBOL(sof_nocodec_setup);

static int sof_nocodec_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card = &sof_nocodec_card;

	card->dev = &pdev->dev;

	return devm_snd_soc_register_card(&pdev->dev, card);
}

static int sof_nocodec_remove(struct platform_device *pdev)
{
	return 0;
}

static struct platform_driver sof_nocodec_audio = {
	.probe = sof_nocodec_probe,
	.remove = sof_nocodec_remove,
	.driver = {
		.name = "sof-nocodec",
		.pm = &snd_soc_pm_ops,
	},
};
module_platform_driver(sof_nocodec_audio)

MODULE_DESCRIPTION("ASoC sof nocodec");
MODULE_AUTHOR("Liam Girdwood");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_ALIAS("platform:sof-nocodec");
