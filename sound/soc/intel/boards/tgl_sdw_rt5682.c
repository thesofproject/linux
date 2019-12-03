// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2020 Intel Corporation

/*
 *  sdw_tgl_rt5682 - ASOC Machine driver for Intel SoundWire platforms
 * connected to rt5682 Realtek device
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
#include <linux/soundwire/sdw.h>
#include <linux/soundwire/sdw_type.h>
#include <sound/jack.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-acpi.h>
#include "../../codecs/hdac_hdmi.h"
#include "hda_dsp_common.h"

struct snd_soc_jack rt5682_headset;

struct tgl_card_private {
	struct list_head hdmi_pcm_list;
	bool common_hdmi_codec_drv;
};

#if IS_ENABLED(CONFIG_SND_HDA_CODEC_HDMI)
static struct snd_soc_jack tgl_hdmi[4];

struct tgl_hdmi_pcm {
	struct list_head head;
	struct snd_soc_dai *codec_dai;
	int device;
};

#define NAME_SIZE	32
static int card_late_probe(struct snd_soc_card *card)
{
	struct tgl_card_private *ctx = snd_soc_card_get_drvdata(card);
	struct tgl_hdmi_pcm *pcm;
	struct snd_soc_component *component = NULL;
	int err, i = 0;
	char jack_name[NAME_SIZE];

	if (list_empty(&ctx->hdmi_pcm_list))
		return -EINVAL;

	if (ctx->common_hdmi_codec_drv) {
		pcm = list_first_entry(&ctx->hdmi_pcm_list, struct tgl_hdmi_pcm,
				       head);
		component = pcm->codec_dai->component;
		return hda_dsp_hdmi_build_controls(card, component);
	}

	list_for_each_entry(pcm, &ctx->hdmi_pcm_list, head) {
		component = pcm->codec_dai->component;
		snprintf(jack_name, sizeof(jack_name),
			 "HDMI/DP, pcm=%d Jack", pcm->device);
		err = snd_soc_card_jack_new(card, jack_name,
					    SND_JACK_AVOUT, &tgl_hdmi[i],
					    NULL, 0);

		if (err)
			return err;

		err = hdac_hdmi_jack_init(pcm->codec_dai, pcm->device,
					  &tgl_hdmi[i]);
		if (err < 0)
			return err;

		i++;
	}

	if (!component)
		return -EINVAL;

	return hdac_hdmi_jack_port_init(component, &card->dapm);
}
#else
static int card_late_probe(struct snd_soc_card *card)
{
	return 0;
}
#endif

static int tgl_hdmi_init(struct snd_soc_pcm_runtime *rtd)
{
	struct tgl_card_private *ctx = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *dai = rtd->codec_dai;
	struct tgl_hdmi_pcm *pcm;

	pcm = devm_kzalloc(rtd->card->dev, sizeof(*pcm), GFP_KERNEL);
	if (!pcm)
		return -ENOMEM;

	/* dai_link id is 1:1 mapped to the PCM device */
	pcm->device = rtd->dai_link->id;
	pcm->codec_dai = dai;

	list_add_tail(&pcm->head, &ctx->hdmi_pcm_list);

	return 0;
}

static struct snd_soc_jack_pin sdw_jack_pins[] = {
	{
		.pin    = "Headphone",
		.mask   = SND_JACK_HEADPHONE,
	},
	{
		.pin    = "Headset Mic",
		.mask   = SND_JACK_MICROPHONE,
	},
};

static int rt5682_codec_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_component *component = rtd->codec_dai->component;
	struct snd_soc_jack *jack;
	int ret;

	/*
	 * Headset buttons map to the google Reference headset.
	 * These can be configured by userspace.
	 */
	ret = snd_soc_card_jack_new(rtd->card, "Headset Jack",
				    SND_JACK_HEADSET | SND_JACK_BTN_0 |
				    SND_JACK_BTN_1 | SND_JACK_BTN_2 |
				    SND_JACK_BTN_3,
				    &rt5682_headset,
				    sdw_jack_pins,
				    ARRAY_SIZE(sdw_jack_pins));
	if (ret) {
		dev_err(rtd->dev, "Headset Jack creation failed: %d\n", ret);
		return ret;
	}

	jack = &rt5682_headset;

	snd_jack_set_key(jack->jack, SND_JACK_BTN_0, KEY_VOLUMEUP);
	snd_jack_set_key(jack->jack, SND_JACK_BTN_1, KEY_PLAYPAUSE);
	snd_jack_set_key(jack->jack, SND_JACK_BTN_2, KEY_VOLUMEDOWN);
	snd_jack_set_key(jack->jack, SND_JACK_BTN_3, KEY_PLAYPAUSE);
	ret = snd_soc_component_set_jack(component, jack, NULL);

	if (ret == -EAGAIN) {
		msleep(500);
		ret = snd_soc_component_set_jack(component, jack, NULL);
	}

	if (ret) {
		dev_err(rtd->dev, "Headset Jack call-back failed: %d\n", ret);
		return ret;
	}

	return ret;
}

static const struct snd_soc_dapm_widget widgets[] = {
	SND_SOC_DAPM_HP("Headphone", NULL),
	SND_SOC_DAPM_MIC("Headset Mic", NULL),
};

static const struct snd_soc_dapm_route map[] = {
	/*Headphones*/
	{ "Headphone", NULL, "HPOL" },
	{ "Headphone", NULL, "HPOR" },
	{ "IN1P", NULL, "Headset Mic" },
};

static const struct snd_kcontrol_new controls[] = {
	SOC_DAPM_PIN_SWITCH("Headphone"),
	SOC_DAPM_PIN_SWITCH("Headset Mic"),
};

SND_SOC_DAILINK_DEF(sdw0_pin2,
	DAILINK_COMP_ARRAY(COMP_CPU("SDW0 Pin2")));
SND_SOC_DAILINK_DEF(sdw0_pin3,
	DAILINK_COMP_ARRAY(COMP_CPU("SDW0 Pin3")));
SND_SOC_DAILINK_DEF(sdw0_codec,
	DAILINK_COMP_ARRAY(COMP_CODEC("sdw:0:25d:5682:0", "rt5682-sdw")));

SND_SOC_DAILINK_DEF(platform,
		DAILINK_COMP_ARRAY(COMP_PLATFORM("0000:00:1f.3")));

SND_SOC_DAILINK_DEF(dmic_pin,
	DAILINK_COMP_ARRAY(COMP_CPU("DMIC01 Pin")));
SND_SOC_DAILINK_DEF(dmic_codec,
	DAILINK_COMP_ARRAY(COMP_CODEC("dmic-codec", "dmic-hifi")));

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

SND_SOC_DAILINK_DEF(idisp4_pin,
	DAILINK_COMP_ARRAY(COMP_CPU("iDisp4 Pin")));
SND_SOC_DAILINK_DEF(idisp4_codec,
	DAILINK_COMP_ARRAY(COMP_CODEC("ehdaudio0D2", "intel-hdmi-hifi4")));

struct snd_soc_dai_link dailink[] = {
	{
		.name = "SDW0-Playback",
		.id = 0,
		.init = rt5682_codec_init,
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
		.id = 4,
		.ignore_suspend = 1,
		.dpcm_capture = 1,
		.no_pcm = 1,
		SND_SOC_DAILINK_REG(dmic_pin, dmic_codec, platform),
	},
#if IS_ENABLED(CONFIG_SND_SOC_HDAC_HDMI)
	{
		.name = "iDisp1",
		.id = 5,
		.init = tgl_hdmi_init,
		.dpcm_playback = 1,
		.no_pcm = 1,
		SND_SOC_DAILINK_REG(idisp1_pin, idisp1_codec, platform),
	},
	{
		.name = "iDisp2",
		.id = 6,
		.init = tgl_hdmi_init,
		.dpcm_playback = 1,
		.no_pcm = 1,
		SND_SOC_DAILINK_REG(idisp2_pin, idisp2_codec, platform),
	},
	{
		.name = "iDisp3",
		.id = 7,
		.init = tgl_hdmi_init,
		.dpcm_playback = 1,
		.no_pcm = 1,
		SND_SOC_DAILINK_REG(idisp3_pin, idisp3_codec, platform),
	},
	{
		.name = "iDisp4",
		.id = 8,
		.init = tgl_hdmi_init,
		.dpcm_playback = 1,
		.no_pcm = 1,
		SND_SOC_DAILINK_REG(idisp4_pin, idisp4_codec, platform),
	},
#endif
};

/* SoC card */
static struct snd_soc_card card_rt5682 = {
	.name = "tgl-sdw-rt5682",
	.dai_link = dailink,
	.num_links = ARRAY_SIZE(dailink),
	.controls = controls,
	.num_controls = ARRAY_SIZE(controls),
	.dapm_widgets = widgets,
	.num_dapm_widgets = ARRAY_SIZE(widgets),
	.dapm_routes = map,
	.num_dapm_routes = ARRAY_SIZE(map),
	.late_probe = card_late_probe,
};

static int mc_probe(struct platform_device *pdev)
{
	struct tgl_card_private *ctx;
	struct snd_soc_acpi_mach *mach;
	const char *platform_name;
	struct snd_soc_card *card = &card_rt5682;
	int ret = 0;

	dev_dbg(&pdev->dev, "Entry %s\n", __func__);

	ctx = devm_kzalloc(&pdev->dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

#if IS_ENABLED(CONFIG_SND_HDA_CODEC_HDMI)
	INIT_LIST_HEAD(&ctx->hdmi_pcm_list);
#endif

	card->dev = &pdev->dev;

	/* override platform name, if required */
	mach = (&pdev->dev)->platform_data;
	platform_name = mach->mach_params.platform;

	ret = snd_soc_fixup_dai_links_platform_name(card, platform_name);
	if (ret)
		return ret;

	snd_soc_card_set_drvdata(card, ctx);
	ctx->common_hdmi_codec_drv = mach->mach_params.common_hdmi_codec_drv;

	/* Register the card */
	ret = devm_snd_soc_register_card(&pdev->dev, card);
	if (ret) {
		dev_err(card->dev, "snd_soc_register_card failed %d\n", ret);
		return ret;
	}

	platform_set_drvdata(pdev, card);

	return ret;
}

static struct platform_driver sdw_rt5682_driver = {
	.driver = {
		.name = "tgl_sdw_rt5682",
		.pm = &snd_soc_pm_ops,
	},
	.probe = mc_probe,
};

module_platform_driver(sdw_rt5682_driver);

MODULE_DESCRIPTION("TGL ASoC SoundWire RT5682 Machine driver");
MODULE_AUTHOR("Bard Liao <yung-chuan.liao@linux.intel.com>");
MODULE_AUTHOR("Naveen Manohar <naveen.m@intel.com>");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:tgl_sdw_rt5682");
