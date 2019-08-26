// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2016-19 Intel Corporation

/*
 *  cnl_rt700.c - ASOC Machine driver for Intel cnl_rt700 platform
 *		with ALC700 SoundWire codec.
 */

#include <linux/acpi.h>
#include <linux/async.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/io.h>
#include <linux/dmi.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <sound/jack.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-acpi.h>
#include "../../codecs/hdac_hdmi.h"

struct cnl_rt700_mc_private {
	struct list_head hdmi_pcm_list;
};

#if IS_ENABLED(CONFIG_SND_SOC_HDAC_HDMI)
static struct snd_soc_jack cnl_hdmi[3];

struct cnl_hdmi_pcm {
	struct list_head head;
	struct snd_soc_dai *codec_dai;
	int device;
};

static int cnl_hdmi_init(struct snd_soc_pcm_runtime *rtd)
{
	struct cnl_rt700_mc_private *ctx = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *dai = rtd->codec_dai;
	struct cnl_hdmi_pcm *pcm;

	pcm = devm_kzalloc(rtd->card->dev, sizeof(*pcm), GFP_KERNEL);
	if (!pcm)
		return -ENOMEM;

	/* dai_link id is 1:1 mapped to the PCM device */
	pcm->device = rtd->dai_link->id;
	pcm->codec_dai = dai;

	list_add_tail(&pcm->head, &ctx->hdmi_pcm_list);

	return 0;
}

#define NAME_SIZE	32
static int cnl_card_late_probe(struct snd_soc_card *card)
{
	struct cnl_rt700_mc_private *ctx = snd_soc_card_get_drvdata(card);
	struct cnl_hdmi_pcm *pcm;
	struct snd_soc_component *component = NULL;
	int err, i = 0;
	char jack_name[NAME_SIZE];

	list_for_each_entry(pcm, &ctx->hdmi_pcm_list, head) {
		component = pcm->codec_dai->component;
		snprintf(jack_name, sizeof(jack_name),
			 "HDMI/DP, pcm=%d Jack", pcm->device);
		err = snd_soc_card_jack_new(card, jack_name,
					    SND_JACK_AVOUT, &cnl_hdmi[i],
					    NULL, 0);

		if (err)
			return err;

		err = hdac_hdmi_jack_init(pcm->codec_dai, pcm->device,
					  &cnl_hdmi[i]);
		if (err < 0)
			return err;

		i++;
	}

	if (!component)
		return -EINVAL;

	return hdac_hdmi_jack_port_init(component, &card->dapm);
}
#else
static int cnl_card_late_probe(struct snd_soc_card *card)
{
	return 0;
}
#endif

static const struct snd_soc_dapm_widget cnl_rt700_widgets[] = {
	SND_SOC_DAPM_HP("Headphones", NULL),
	SND_SOC_DAPM_MIC("AMIC", NULL),
	SND_SOC_DAPM_SPK("Speaker", NULL),
};

static const struct snd_soc_dapm_route cnl_rt700_map[] = {
	/*Headphones*/
	{ "Headphones", NULL, "HP" },
	{ "Speaker", NULL, "SPK" },
	{ "MIC2", NULL, "AMIC" },
};

static const struct snd_kcontrol_new cnl_rt700_controls[] = {
	SOC_DAPM_PIN_SWITCH("Headphones"),
	SOC_DAPM_PIN_SWITCH("AMIC"),
	SOC_DAPM_PIN_SWITCH("Speaker"),
};

SND_SOC_DAILINK_DEF(sdw0_pin2,
	DAILINK_COMP_ARRAY(COMP_CPU("SDW0 Pin2")));
SND_SOC_DAILINK_DEF(sdw0_pin3,
	DAILINK_COMP_ARRAY(COMP_CPU("SDW0 Pin3")));
SND_SOC_DAILINK_DEF(sdw0_codec,
	DAILINK_COMP_ARRAY(COMP_CODEC("sdw:0:25d:700:0:0", "rt700-aif1")));

SND_SOC_DAILINK_DEF(sdw1_pin2,
	DAILINK_COMP_ARRAY(COMP_CPU("SDW1 Pin2")));
SND_SOC_DAILINK_DEF(sdw1_pin3,
	DAILINK_COMP_ARRAY(COMP_CPU("SDW1 Pin3")));
SND_SOC_DAILINK_DEF(sdw1_codec,
	DAILINK_COMP_ARRAY(COMP_CODEC("sdw:1:25d:700:0:0", "rt700-aif1")));

SND_SOC_DAILINK_DEF(dmic_pin,
	DAILINK_COMP_ARRAY(COMP_CPU("DMIC01 Pin")));

SND_SOC_DAILINK_DEF(dmic16k_pin,
	DAILINK_COMP_ARRAY(COMP_CPU("DMIC16k Pin")));

SND_SOC_DAILINK_DEF(dmic_codec,
	DAILINK_COMP_ARRAY(COMP_CODEC("dmic-codec", "dmic-hifi")));

#if IS_ENABLED(CONFIG_SND_SOC_HDAC_HDMI)
SND_SOC_DAILINK_DEF(idisp1_pin,
	DAILINK_COMP_ARRAY(COMP_CPU("iDisp1 Pin")));
SND_SOC_DAILINK_DEF(idisp1_codec,
	DAILINK_COMP_ARRAY(COMP_CODEC("ehdaudio0D2", "intel-hdmi-hifi1")));

SND_SOC_DAILINK_DEF(idisp2_pin,
	DAILINK_COMP_ARRAY(COMP_CPU("iDisp2 Pin")));
SND_SOC_DAILINK_DEF(idisp2_codec,
	DAILINK_COMP_ARRAY(COMP_CODEC("ehdaudio0D2", "intel-hdmi-hifi2")));

SND_SOC_DAILINK_DEF(idisp3_pin,
	DAILINK_COMP_ARRAY(COMP_CPU("iDisp3 Pin")));
SND_SOC_DAILINK_DEF(idisp3_codec,
	DAILINK_COMP_ARRAY(COMP_CODEC("ehdaudio0D2", "intel-hdmi-hifi3")));
#endif

SND_SOC_DAILINK_DEF(platform,
		DAILINK_COMP_ARRAY(COMP_PLATFORM("0000:00:1f.3")));

struct snd_soc_dai_link cnl_rt700_msic_dailink[] = {
	{
		.name = "SDW0-Playback",
		.id = 0,
		.no_pcm = 1,
		.dpcm_playback = 1,
		.nonatomic = true,
		SND_SOC_DAILINK_REG(sdw0_pin2, sdw0_codec, platform),
	},
	{
		.name = "SDW0-Capture",
		.id = 1,
		.no_pcm = 1,
		.dpcm_capture = 1,
		.nonatomic = true,
		SND_SOC_DAILINK_REG(sdw0_pin3, sdw0_codec, platform),
	},
	{
		.name = "dmic01",
		.id = 2,
		.ignore_suspend = 1,
		.dpcm_capture = 1,
		.no_pcm = 1,
		SND_SOC_DAILINK_REG(dmic_pin, dmic_codec, platform),
	},
	{
		.name = "dmic16k",
		.id = 3,
		.ignore_suspend = 1,
		.dpcm_capture = 1,
		.no_pcm = 1,
		SND_SOC_DAILINK_REG(dmic16k_pin, dmic_codec, platform),
	},
#if IS_ENABLED(CONFIG_SND_SOC_HDAC_HDMI)
	{
		.name = "iDisp1",
		.id = 4,
		.init = cnl_hdmi_init,
		.no_pcm = 1,
		.dpcm_playback = 1,
		SND_SOC_DAILINK_REG(idisp1_pin, idisp1_codec, platform),
	},
	{
		.name = "iDisp2",
		.id = 5,
		.init = cnl_hdmi_init,
		.no_pcm = 1,
		.dpcm_playback = 1,
		SND_SOC_DAILINK_REG(idisp2_pin, idisp2_codec, platform),
	},
	{
		.name = "iDisp3",
		.id = 6,
		.init = cnl_hdmi_init,
		.no_pcm = 1,
		.dpcm_playback = 1,
		SND_SOC_DAILINK_REG(idisp3_pin, idisp3_codec, platform),
	},
#endif
};

/* SoC card */
static struct snd_soc_card snd_soc_card_cnl_rt700 = {
	.name = "cnl_rt700-audio",
	.dai_link = cnl_rt700_msic_dailink,
	.num_links = ARRAY_SIZE(cnl_rt700_msic_dailink),
	.controls = cnl_rt700_controls,
	.num_controls = ARRAY_SIZE(cnl_rt700_controls),
	.dapm_widgets = cnl_rt700_widgets,
	.num_dapm_widgets = ARRAY_SIZE(cnl_rt700_widgets),
	.dapm_routes = cnl_rt700_map,
	.num_dapm_routes = ARRAY_SIZE(cnl_rt700_map),
	.late_probe = cnl_card_late_probe,
};

static int snd_cnl_rt700_mc_probe(struct platform_device *pdev)
{
	struct cnl_rt700_mc_private *ctx;
	struct snd_soc_acpi_mach *mach;
	const char *platform_name;
	struct snd_soc_card *card;
	const char *product;
	int ret;

	dev_dbg(&pdev->dev, "Entry %s\n", __func__);

	ctx = devm_kzalloc(&pdev->dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

#if IS_ENABLED(CONFIG_SND_SOC_HDAC_HDMI)
	INIT_LIST_HEAD(&ctx->hdmi_pcm_list);
#endif

	product = dmi_get_system_info(DMI_PRODUCT_NAME);
	if (strstr(product, "CometLake")) { /* FIXME: test against RVP */
		cnl_rt700_msic_dailink[0].name = "SDW1-Playback";
		cnl_rt700_msic_dailink[0].codecs = sdw1_codec;
		cnl_rt700_msic_dailink[0].num_codecs =
			ARRAY_SIZE(sdw1_codec);
		cnl_rt700_msic_dailink[0].cpus = sdw1_pin2;
		cnl_rt700_msic_dailink[0].num_cpus =
			ARRAY_SIZE(sdw1_pin2);

		cnl_rt700_msic_dailink[1].name = "SDW1-Capture";
		cnl_rt700_msic_dailink[1].codecs = sdw1_codec;
		cnl_rt700_msic_dailink[1].num_codecs =
			ARRAY_SIZE(sdw1_codec);
		cnl_rt700_msic_dailink[1].cpus = sdw1_pin3;
		cnl_rt700_msic_dailink[1].num_cpus =
			ARRAY_SIZE(sdw1_pin3);
	}

	card = &snd_soc_card_cnl_rt700;
	card->dev = &pdev->dev;

	snd_soc_card_cnl_rt700.dev = &pdev->dev;

	/* override platform name, if required */
	mach = (&pdev->dev)->platform_data;
	platform_name = mach->mach_params.platform;

	ret = snd_soc_fixup_dai_links_platform_name(card, platform_name);
	if (ret)
		return ret;

	snd_soc_card_set_drvdata(card, ctx);

	/* Register the card */
	ret = devm_snd_soc_register_card(&pdev->dev, card);
	if (ret) {
		dev_err(card->dev, "snd_soc_register_card failed %d\n", ret);
		return ret;
	}
	platform_set_drvdata(pdev, &snd_soc_card_cnl_rt700);
	return ret;
}

static const struct platform_device_id cnl_board_ids[] = {
	{ .name = "cnl_rt700" },
	{ .name = "icl_rt700" },
	{ }
};

static struct platform_driver snd_cnl_rt700_mc_driver = {
	.driver = {
		.name = "cnl_rt700",
	},
	.probe = snd_cnl_rt700_mc_probe,
	.id_table = cnl_board_ids
};

module_platform_driver(snd_cnl_rt700_mc_driver)

MODULE_DESCRIPTION("ASoC CNL Machine driver");
MODULE_AUTHOR("Hardik Shah <hardik.t.shah>");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:cnl_rt700");
MODULE_ALIAS("platform:icl_rt700");
