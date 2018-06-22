// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
/*
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * Copyright(c) 2017 Intel Corporation. All rights reserved.
 *
 * Author: Liam Girdwood <liam.r.girdwood@linux.intel.com>
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/acpi.h>
#include <linux/device.h>
#include <linux/dmi.h>
#include <linux/slab.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/jack.h>
#include <sound/sof.h>
#include "sof-priv.h"

static int sof_nocodec_codec_fixup(struct snd_soc_pcm_runtime *rtd,
				   struct snd_pcm_hw_params *params)
{
	// TODO: read this from topology
	return 0;
}

static struct snd_soc_ops sof_nocodec_ops = {};

static int nocodec_rtd_init(struct snd_soc_pcm_runtime *rtd)
{
	snd_soc_set_dmi_name(rtd->card, NULL);

	return 0;
}

static struct snd_soc_card sof_nocodec_card = {
	.name = "sof-nocodec",
};

int sof_nocodec_setup(struct device *dev,
		      struct snd_sof_pdata *sof_pdata,
		      struct snd_soc_acpi_mach *mach,
		      const struct sof_dev_desc *desc,
		      struct snd_sof_dsp_ops *ops)
{
	struct snd_soc_dai_link *links;
	char name[32];
	int i;

	if (!mach)
		return -EINVAL;

	sof_pdata->drv_name = "sof-nocodec";

	mach->drv_name = "sof-nocodec";
	mach->sof_fw_filename = desc->nocodec_fw_filename;
	mach->sof_tplg_filename = desc->nocodec_tplg_filename;

	/* create dummy BE dai_links */
	links = devm_kzalloc(dev, sizeof(struct snd_soc_dai_link) *
			     ops->num_drv, GFP_KERNEL);
	if (!links)
		return -ENOMEM;

	for (i = 0; i < ops->num_drv; i++) {
		snprintf(name, 32, "NoCodec-%d", i);
		links[i].name = kmemdup(name, sizeof(name), GFP_KERNEL);
		links[i].id = i;
		links[i].init = nocodec_rtd_init;
		links[i].no_pcm = 1;
		links[i].cpu_dai_name = ops->drv[i].name;
		links[i].platform_name = "sof-audio";
		links[i].codec_dai_name = "snd-soc-dummy-dai";
		links[i].codec_name = "snd-soc-dummy";
		links[i].ops = &sof_nocodec_ops;
		links[i].dai_fmt = SND_SOC_DAIFMT_DSP_B | SND_SOC_DAIFMT_NB_NF |
			SND_SOC_DAIFMT_CBS_CFS;
		links[i].ignore_suspend = 1;
		links[i].be_hw_params_fixup = sof_nocodec_codec_fixup;
		links[i].dpcm_playback = 1;
		links[i].dpcm_capture = 1;
	}

	sof_nocodec_card.dai_link = links;
	sof_nocodec_card.num_links = ops->num_drv;

	return 0;
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
