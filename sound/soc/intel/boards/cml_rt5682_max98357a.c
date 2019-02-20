// SPDX-License-Identifier: GPL-2.0
// Copyright(c) 2018 Intel Corporation.

/*
 * Intel Cometlake I2S Machine driver for RT5682 + Maxim98357a codec
 *
 * Modified from:
 *   Intel I2S Machine driver for RT5682
 *   Intel Geminilake I2S Machine driver for RT5682 + Maxim98357a codec
 */

#include <linux/input.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <sound/core.h>
#include <sound/jack.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/rt5682.h>
#include <sound/soc-acpi.h>
#include "../skylake/skl.h"
#include "../../codecs/rt5682.h"
#include "../../codecs/hdac_hdmi.h"

/* The platform clock outputs 24Mhz clock to codec as I2S MCLK */
#define CML_REALTEK_CODEC_DAI "rt5682-aif1"
#define CML_MAXIM_CODEC_DAI "HiFi"
#define MAXIM_DEV0_NAME "MX98357A:00"
#define DUAL_CHANNEL 2
#define QUAD_CHANNEL 4
#define NAME_SIZE 32

static struct snd_soc_jack cml_hdmi[3];

struct cml_hdmi_pcm {
	struct list_head head;
	struct snd_soc_dai *codec_dai;
	int device;
};

struct cml_card_private {
	struct snd_soc_jack cml_headset;
	struct list_head hdmi_pcm_list;
};

static const struct snd_kcontrol_new cml_controls[] = {
	SOC_DAPM_PIN_SWITCH("Headphone Jack"),
	SOC_DAPM_PIN_SWITCH("Headset Mic"),
	SOC_DAPM_PIN_SWITCH("Spk"),
};

static const struct snd_soc_dapm_widget cml_widgets[] = {
	SND_SOC_DAPM_HP("Headphone Jack", NULL),
	SND_SOC_DAPM_MIC("Headset Mic", NULL),
	SND_SOC_DAPM_SPK("Spk", NULL),
};

static const struct snd_soc_dapm_route cml_map[] = {
	/* HP jack connectors */
	{ "Headphone Jack", NULL, "HPOL" },
	{ "Headphone Jack", NULL, "HPOR" },

	/* speaker */
	{ "Spk", NULL, "Speaker" },

	/* HS Mic jacks */
	{ "IN1P", NULL, "Headset Mic" },

};

static const unsigned int rates[] = {
        48000,
};

static const struct snd_pcm_hw_constraint_list constraints_rates = {
        .count = ARRAY_SIZE(rates),
        .list  = rates,
        .mask = 0,
};

static const unsigned int channels[] = {
        DUAL_CHANNEL,
};

static const struct snd_pcm_hw_constraint_list constraints_channels = {
        .count = ARRAY_SIZE(channels),
        .list = channels,
        .mask = 0,
};

static unsigned int channels_quad[] = {
        QUAD_CHANNEL,
};

static struct snd_pcm_hw_constraint_list constraints_channels_quad = {
        .count = ARRAY_SIZE(channels_quad),
        .list = channels_quad,
        .mask = 0,
};

static int cml_ssp_fixup(struct snd_soc_pcm_runtime *rtd,
			struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
			SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
			SNDRV_PCM_HW_PARAM_CHANNELS);
	struct snd_mask *fmt = hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT);

	/* The ADSP will convert the FE rate to 48k, stereo */
	rate->min = rate->max = 48000;
	channels->min = channels->max = DUAL_CHANNEL;

	/* set SSP to 24 bit */
	snd_mask_none(fmt);
	snd_mask_set(fmt, SNDRV_PCM_FORMAT_S24_LE);

	return 0;
}

static int cml_rt5682_codec_init(struct snd_soc_pcm_runtime *rtd)
{
	struct cml_card_private *ctx = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_component *component = rtd->codec_dai->component;
	struct snd_soc_jack *jack;
	int ret;

	/* need to enable ASRC function for 24MHz mclk rate */
	rt5682_sel_asrc_clk_src(component, RT5682_DA_STEREO1_FILTER,
					RT5682_CLK_SEL_I2S1_ASRC);


	/*
	 * Headset buttons map to the google Reference headset.
	 * These can be configured by userspace.
	 */
	ret = snd_soc_card_jack_new(rtd->card, "Headset Jack",
			SND_JACK_HEADSET | SND_JACK_BTN_0 | SND_JACK_BTN_1 |
			SND_JACK_BTN_2 | SND_JACK_BTN_3 | SND_JACK_LINEOUT,
			&ctx->cml_headset, NULL, 0);
	if (ret) {
		dev_err(rtd->dev, "Headset Jack creation failed: %d\n", ret);
		return ret;
	}

	jack = &ctx->cml_headset;

	snd_jack_set_key(jack->jack, SND_JACK_BTN_0, KEY_PLAYPAUSE);
	snd_jack_set_key(jack->jack, SND_JACK_BTN_1, KEY_VOLUMEUP);
	snd_jack_set_key(jack->jack, SND_JACK_BTN_2, KEY_VOLUMEDOWN);
	snd_jack_set_key(jack->jack, SND_JACK_BTN_3, KEY_VOICECOMMAND);
	ret = snd_soc_component_set_jack(component, jack, NULL);

	if (ret) {
		dev_err(rtd->dev, "Headset Jack call-back failed: %d\n", ret);
		return ret;
	}

	return ret;
};

static int cml_rt5682_hw_params(struct snd_pcm_substream *substream,
	struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	int clk_id, clk_freq, pll_out, ret;

	clk_id = RT5682_PLL1_S_MCLK;
	clk_freq = 24000000;


	pll_out = params_rate(params) * 512;

	ret = snd_soc_dai_set_pll(codec_dai, 0, clk_id, clk_freq, pll_out);
	if (ret < 0)
		dev_err(rtd->dev, "snd_soc_dai_set_pll err = %d\n", ret);

	/* Configure sysclk for codec */
	ret = snd_soc_dai_set_sysclk(codec_dai, RT5682_SCLK_S_PLL1,
				     pll_out, SND_SOC_CLOCK_IN);
	if (ret < 0)
		dev_err(rtd->dev, "snd_soc_dai_set_sysclk err = %d\n", ret);

	/*
	 * slot_width should equal or large than data length, set them
	 * be the same
	 */
	ret = snd_soc_dai_set_tdm_slot(codec_dai, 0x0, 0x0, 2,
				       params_width(params));
	if (ret < 0) {
		dev_err(rtd->dev, "set TDM slot err:%d\n", ret);
		return ret;
	}

	return ret;
}

static struct snd_soc_ops cml_rt5682_ops = {
	.hw_params = cml_rt5682_hw_params,
};

static int cml_hdmi_init(struct snd_soc_pcm_runtime *rtd)
{
	struct cml_card_private *ctx = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *dai = rtd->codec_dai;
	struct cml_hdmi_pcm *pcm;

	pcm = devm_kzalloc(rtd->card->dev, sizeof(*pcm), GFP_KERNEL);
	if (!pcm)
		return -ENOMEM;

	pcm->device = dai->id;
	pcm->codec_dai = dai;

	list_add_tail(&pcm->head, &ctx->hdmi_pcm_list);

	return 0;
}

static int cml_dmic_fixup(struct snd_soc_pcm_runtime *rtd,
		struct snd_pcm_hw_params *params)
{
	struct snd_interval *channels = hw_param_interval(params,
				SNDRV_PCM_HW_PARAM_CHANNELS);

	/*
	 * set BE channel constraint as user FE channels
	 */
	channels->min = channels->max = QUAD_CHANNEL;

	return 0;
}

static int cml_dmic_startup(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;

	runtime->hw.channels_min = runtime->hw.channels_max = QUAD_CHANNEL;
	snd_pcm_hw_constraint_list(runtime, 0, SNDRV_PCM_HW_PARAM_CHANNELS,
			&constraints_channels_quad);

	return snd_pcm_hw_constraint_list(substream->runtime, 0,
			SNDRV_PCM_HW_PARAM_RATE, &constraints_rates);
}

static const struct snd_soc_ops cml_dmic_ops = {
	.startup = cml_dmic_startup,
};

/* Cometlake digital audio interface glue - connects codec <--> CPU */
static struct snd_soc_dai_link cml_dais[] = {
	/* Back End DAI links */
        {
                /* SSP0 - Codec */
                .name = "SSP0-Codec",
                .id = 0,
                .cpu_dai_name = "SSP0 Pin",
                .platform_name = "0000:00:1f.3",
                .no_pcm = 1,
                .codec_name = "i2c-10EC5682:00",
                .codec_dai_name = CML_REALTEK_CODEC_DAI,
                .init = cml_rt5682_codec_init,
                .dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
                        SND_SOC_DAIFMT_CBS_CFS,
                .ignore_pmdown_time = 1,
                .be_hw_params_fixup = cml_ssp_fixup,
                .ops = &cml_rt5682_ops,
                .dpcm_playback = 1,
                .dpcm_capture = 1,
        },
	{
		/* SSP1 - Codec */
		.name = "SSP1-Codec",
		.id = 1,
		.cpu_dai_name = "SSP1 Pin",
		.platform_name = "0000:00:1f.3",
		.no_pcm = 1,
		.codec_name = MAXIM_DEV0_NAME,
		.codec_dai_name = CML_MAXIM_CODEC_DAI,
		.dai_fmt = SND_SOC_DAIFMT_I2S |
			SND_SOC_DAIFMT_NB_NF |
			SND_SOC_DAIFMT_CBS_CFS,
		.ignore_pmdown_time = 1,
		.dpcm_playback = 1,
	},
	{
		.name = "dmic01",
		.id = 2,
		.cpu_dai_name = "DMIC01 Pin",
		.codec_name = "dmic-codec",
		.codec_dai_name = "dmic-hifi",
		.platform_name = "0000:00:1f.3",
		.ignore_suspend = 1,
		.be_hw_params_fixup = cml_dmic_fixup,
		.dpcm_capture = 1,
		.no_pcm = 1,
	},
	{
		.name = "iDisp1",
		.id = 3,
		.cpu_dai_name = "iDisp1 Pin",
		.codec_name = "ehdaudio0D2",
		.codec_dai_name = "intel-hdmi-hifi1",
		.platform_name = "0000:00:1f.3",
		.init = cml_hdmi_init,
		.dpcm_playback = 1,
		.no_pcm = 1,
	},
	{
		.name = "iDisp2",
		.id = 4,
		.cpu_dai_name = "iDisp2 Pin",
		.codec_name = "ehdaudio0D2",
		.codec_dai_name = "intel-hdmi-hifi2",
		.platform_name = "0000:00:1f.3",
		.init = cml_hdmi_init,
		.dpcm_playback = 1,
		.no_pcm = 1,
	},
	{
		.name = "iDisp3",
		.id = 5,
		.cpu_dai_name = "iDisp3 Pin",
		.codec_name = "ehdaudio0D2",
		.codec_dai_name = "intel-hdmi-hifi3",
		.platform_name = "0000:00:1f.3",
		.init = cml_hdmi_init,
		.dpcm_playback = 1,
		.no_pcm = 1,
	},
};

static int cml_card_late_probe(struct snd_soc_card *card)
{
	struct cml_card_private *ctx = snd_soc_card_get_drvdata(card);
	struct snd_soc_component *component = NULL;
	char jack_name[NAME_SIZE];
	struct cml_hdmi_pcm *pcm;
	int err = 0;
	int i = 0;

	list_for_each_entry(pcm, &ctx->hdmi_pcm_list, head) {
		component = pcm->codec_dai->component;
		snprintf(jack_name, sizeof(jack_name),
			"HDMI/DP, pcm=%d Jack", pcm->device);
		err = snd_soc_card_jack_new(card, jack_name,
					SND_JACK_AVOUT, &cml_hdmi[i],
					NULL, 0);

		if (err)
			return err;

		err = hdac_hdmi_jack_init(pcm->codec_dai, pcm->device,
						&cml_hdmi[i]);
		if (err < 0)
			return err;

		i++;
	}

	if (!component)
		return -EINVAL;

	return hdac_hdmi_jack_port_init(component, &card->dapm);
}

/* Cometlake audio machine driver for RT5682 + MAX98357A */
static struct snd_soc_card cml_audio_card_rt5682_m98357a = {
	.name = "cmlrt5682max",
	.owner = THIS_MODULE,
        .dai_link = cml_dais,
        .num_links = ARRAY_SIZE(cml_dais),
	.controls = cml_controls,
	.num_controls = ARRAY_SIZE(cml_controls),
	.dapm_widgets = cml_widgets,
	.num_dapm_widgets = ARRAY_SIZE(cml_widgets),
	.dapm_routes = cml_map,
	.num_dapm_routes = ARRAY_SIZE(cml_map),
	.fully_routed = true,
	.late_probe = cml_card_late_probe,
};

static int cml_audio_probe(struct platform_device *pdev)
{
	struct cml_card_private *ctx;
	struct snd_soc_acpi_mach *mach;
	const char *platform_name;
	struct snd_soc_card *card;
	int ret;

	ctx = devm_kzalloc(&pdev->dev, sizeof(*ctx), GFP_ATOMIC);
	if (!ctx)
		return -ENOMEM;

	INIT_LIST_HEAD(&ctx->hdmi_pcm_list);

	card = &cml_audio_card_rt5682_m98357a;
	card->dev = &pdev->dev;
	
	/* override plaform name, if required */
	mach = (&pdev->dev)->platform_data;
	platform_name = mach->mach_params.platform;

	ret = snd_soc_fixup_dai_links_platform_name(card, platform_name);
	if (ret)
		return ret;

	snd_soc_card_set_drvdata(card, ctx);

	return devm_snd_soc_register_card(&pdev->dev, card);
}

static const struct platform_device_id cml_board_ids[] = {
	{
		.name = "cml_rt5682_max98357a",
		.driver_data =
			(kernel_ulong_t)&cml_audio_card_rt5682_m98357a,
	},
	{ }
};

static struct platform_driver cml_audio = {
	.probe = cml_audio_probe,
	.driver = {
		.name = "cml_rt5682_max98357a",
		.pm = &snd_soc_pm_ops,
	},
	.id_table = cml_board_ids,
};
module_platform_driver(cml_audio)

/* Module information */
MODULE_DESCRIPTION("Cometlake Audio Machine driver-RT5682 & MAX98357A in I2S mode");
MODULE_AUTHOR(" Sathya Prakash M R <sathya.prakash.m.r@intel.com>");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:cml_rt5682_max98357a");
