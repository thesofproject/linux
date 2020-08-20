// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
//
// This file is provided under a dual BSD/GPLv2 license.  When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2018 Intel Corporation. All rights reserved.
//
// Author: Liam Girdwood <liam.r.girdwood@linux.intel.com>
//

#include <linux/module.h>
#include <sound/sof.h>
#include "sof-priv.h"

static const struct snd_soc_dapm_widget sof_widgets[] = {
	SND_SOC_DAPM_MIC("SSP0 Input", NULL),
	SND_SOC_DAPM_SPK("SSP0 Output", NULL),
	SND_SOC_DAPM_MIC("SSP1 Input", NULL),
	SND_SOC_DAPM_SPK("SSP1 Output", NULL),
	SND_SOC_DAPM_MIC("SSP2 Input", NULL),
	SND_SOC_DAPM_SPK("SSP2 Output", NULL),

	SND_SOC_DAPM_INPUT("DMic"),
};

static const struct snd_soc_dapm_route sof_map[] = {
	/* SSPs */
	{ "SSP0 Output", NULL, "SSP0.OUT" },
	{ "SSP0.IN", NULL, "SSP0 Input" },
	{ "SSP1 Output", NULL, "SSP1.OUT" },
	{ "SSP1.IN", NULL, "SSP1 Input" },
	{ "SSP2 Output", NULL, "SSP2.OUT" },
	{ "SSP2.IN", NULL, "SSP2 Input" },

	/* digital mics */
	{"DMIC0.IN", NULL, "DMic"},
	{"DMIC1.IN", NULL, "DMic"},
};

static struct snd_soc_card sof_nocodec_card = {
	.name = "nocodec", /* the sof- prefix is added by the core */
	.owner = THIS_MODULE,

	.dapm_widgets = sof_widgets,
	.num_dapm_widgets = ARRAY_SIZE(sof_widgets),
	.dapm_routes = sof_map,
	.num_dapm_routes = ARRAY_SIZE(sof_map),
};

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
		if (ops->drv[i].playback.channels_min)
			links[i].dpcm_playback = 1;
		if (ops->drv[i].capture.channels_min)
			links[i].dpcm_capture = 1;
	}

	card->dai_link = links;
	card->num_links = link_num;

	return 0;
}

int sof_nocodec_setup(struct device *dev,
		      const struct snd_sof_dsp_ops *ops)
{
	struct snd_soc_dai_link *links;

	/* create dummy BE dai_links */
	links = devm_kzalloc(dev, sizeof(struct snd_soc_dai_link) *
			     ops->num_drv, GFP_KERNEL);
	if (!links)
		return -ENOMEM;

	return sof_nocodec_bes_setup(dev, ops, links, ops->num_drv,
				     &sof_nocodec_card);
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
