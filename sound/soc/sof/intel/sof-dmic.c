// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
//
// This file is provided under a dual BSD/GPLv2 license.  When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2019 Intel Corporation. All rights reserved.
//
// Author: Ranjani Sridharan <ranjani.sridharan@linux.intel.com>
//

#include <linux/module.h>
#include <sound/intel-nhlt.h>
#include <sound/sof.h>
#include "../sof-priv.h"
#include "../sof-audio.h"
#include "shim.h"

static int hda_dmic_num = -1;
module_param_named(dmic_num, hda_dmic_num, int, 0444);
MODULE_PARM_DESC(dmic_num, "SOF DMIC number");

static struct snd_soc_card sof_dmic_card = {
	.name = "dmic", /* the sof- prefix is added by the core */
};

static int sof_dmic_bes_setup(struct device *dev,
			      const struct snd_sof_audio_ops *audio_ops,
			      struct snd_soc_dai_link *links,
			      int link_num, struct snd_soc_card *card,
			      const struct sof_intel_dsp_desc *chip)
{
	struct snd_soc_dai_link_component *dlc;
	int dai_offset;
	int i;

	if (!audio_ops || !links || !card)
		return -EINVAL;

	/* set up BE dai_links */
	for (i = 0; i < link_num; i++) {
		dlc = devm_kzalloc(dev, 3 * sizeof(*dlc), GFP_KERNEL);
		if (!dlc)
			return -ENOMEM;

		links[i].name = devm_kasprintf(dev, GFP_KERNEL,
					       "DMIC-%d", i);
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
		dai_offset = snd_sof_get_dai_drv_offset(audio_ops,
							SOF_DAI_INTEL_DMIC);
		links[i].cpus->dai_name = audio_ops->drv[i + dai_offset].name;
		links[i].platforms->name = dev_name(dev);
		links[i].codecs->dai_name = "snd-soc-dummy-dai";
		links[i].codecs->name = "snd-soc-dummy";
		links[i].dpcm_capture = 1;
	}

	card->dai_link = links;
	card->num_links = link_num;

	return 0;
}

static int check_nhlt_dmic(struct device *dev)
{
	struct nhlt_acpi_table *nhlt;
	int dmic_num;

	nhlt = intel_nhlt_init(dev->parent);
	if (nhlt) {
		dmic_num = intel_nhlt_get_dmic_geo(dev->parent, nhlt);
		intel_nhlt_free(nhlt);
		if (dmic_num == 2 || dmic_num == 4)
			return dmic_num;
	}

	return 0;
}

int sof_dmic_setup(struct device *dev,
		   struct sof_audio_dev *sof_audio,
		   struct snd_soc_acpi_mach *mach,
		   const struct sof_intel_dsp_desc *chip)
{
	const struct snd_sof_audio_ops *audio_ops = sof_audio->audio_ops;
	struct snd_soc_dai_link *links;
	const char *dmic_str;
	int dmic_num;
	int num_drv;
	int ret;

	if (!mach)
		return -EINVAL;

	sof_audio->drv_name = "sof-dmic";
	mach->drv_name = "sof-dmic";

	dmic_num = check_nhlt_dmic(dev);

	/* allow for module parameter override */
	if (hda_dmic_num != -1)
		dmic_num = hda_dmic_num;

	switch (dmic_num) {
	case 2:
		dmic_str = "-2ch";
		break;
	case 4:
		dmic_str = "-4ch";
		break;
	default:
		dmic_num = 0;
		dmic_str = "";
		break;
	}

	sof_audio->tplg_filename = devm_kasprintf(dev, GFP_KERNEL,
						  "sof-dmic-generic%s.tplg",
						  dmic_str);

	/* create dummy BE dai_links */
	num_drv = snd_sof_get_dai_drv_count(sof_audio->audio_ops,
					    SOF_DAI_INTEL_DMIC);
	links = devm_kzalloc(dev, sizeof(struct snd_soc_dai_link) *
			     num_drv, GFP_KERNEL);
	if (!links)
		return -ENOMEM;

	ret = sof_dmic_bes_setup(dev, audio_ops, links, num_drv,
				 &sof_dmic_card, chip);
	return ret;
}
EXPORT_SYMBOL(sof_dmic_setup);

static int sof_dmic_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card = &sof_dmic_card;

	card->dev = &pdev->dev;

	return devm_snd_soc_register_card(&pdev->dev, card);
}

static int sof_dmic_remove(struct platform_device *pdev)
{
	return 0;
}

static struct platform_driver sof_dmic_audio = {
	.probe = sof_dmic_probe,
	.remove = sof_dmic_remove,
	.driver = {
		.name = "sof-dmic",
		.pm = &snd_soc_pm_ops,
	},
};
module_platform_driver(sof_dmic_audio)

MODULE_DESCRIPTION("ASoC SOF DMIC");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_ALIAS("platform:sof-dmic");
