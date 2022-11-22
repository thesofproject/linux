// SPDX-License-Identifier: GPL-2.0-only
//
// Copyright(c) 2020 Intel Corporation. All rights reserved.
#include <linux/module.h>
#include <linux/string.h>
#include <sound/pcm.h>
#include <sound/soc.h>
#include <sound/soc-acpi.h>
#include <sound/soc-dai.h>
#include <sound/soc-dapm.h>
#include <uapi/sound/asound.h>
#include "sof_maxim_common.h"
#include <sound/pcm_params.h>

#define MAX_98373_PIN_NAME 16
#define MAXIM_HID_NUMS 3
#define MAXIM_MAX_AMPS 4
#define MAXIM_NUM_CODECS 2

static char maxim_codec_name[SND_ACPI_I2C_ID_LEN];
static struct snd_soc_codec_conf maxim_codec_conf[MAXIM_MAX_AMPS];
static struct snd_soc_dai_link_component maxim_components[MAXIM_MAX_AMPS];

static const char * const maxim_name_prefixes[] = { "Right", "Left", "Tweeter Right", "Tweeter Left" };
static unsigned int num_codecs = 2;

static const struct {
        unsigned int tx;
        unsigned int rx;
} maxim_tdm_mask[] = {
        {.tx = 0x3, .rx = 0x3},
        {.tx = 0xC, .rx = 0x3},
};

static const struct {
        unsigned int tx;
        unsigned int rx;
} maxim_dsp_tdm_mask[] = {
        {.tx = 0x1, .rx = 0x3},
        {.tx = 0x2, .rx = 0x3},
        {.tx = 0x4, .rx = 0x3},
        {.tx = 0x8, .rx = 0x3},
};

static const struct snd_kcontrol_new maxim_kcontrols[] = {
	SOC_DAPM_PIN_SWITCH("Left Spk"),
	SOC_DAPM_PIN_SWITCH("Right Spk"),
};

static const struct snd_soc_dapm_widget maxim_dapm_widgets[] = {
	SND_SOC_DAPM_SPK("Left Spk", NULL),
	SND_SOC_DAPM_SPK("Right Spk", NULL),
};

const struct snd_soc_dapm_route maxim_dapm_routes[] = {
	{ "Left Spk", NULL, "Left BE_OUT" },
	{ "Right Spk", NULL, "Right BE_OUT" },
};
EXPORT_SYMBOL_NS(maxim_dapm_routes, SND_SOC_INTEL_SOF_MAXIM_COMMON);

static const struct snd_kcontrol_new maxim_tt_kcontrols[] = {
	SOC_DAPM_PIN_SWITCH("TL Spk"),
	SOC_DAPM_PIN_SWITCH("TR Spk"),
};

static const struct snd_soc_dapm_widget maxim_tt_dapm_widgets[] = {
	SND_SOC_DAPM_SPK("TL Spk", NULL),
	SND_SOC_DAPM_SPK("TR Spk", NULL),
};

const struct snd_soc_dapm_route maxim_tt_dapm_routes[] = {
	/* Tweeter speaker */
	{ "TL Spk", NULL, "Tweeter Left BE_OUT" },
	{ "TR Spk", NULL, "Tweeter Right BE_OUT" },
};

int maxim_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct snd_soc_pcm_runtime *rtd = asoc_substream_to_rtd(substream);
	struct snd_soc_dai *codec_dai;
	struct snd_soc_dai *cpu_dai;
	int j;
	int ret = 0;

	if (strcmp(maxim_components[0].dai_name, "max98373-aif1"))
		return 0;

	/* set spk pin by playback only */
	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		return 0;

	cpu_dai = asoc_rtd_to_cpu(rtd, 0);
	for_each_rtd_codec_dais(rtd, j, codec_dai) {
		struct snd_soc_dapm_context *dapm =
				snd_soc_component_get_dapm(cpu_dai->component);
		char pin_name[MAX_98373_PIN_NAME];

		snprintf(pin_name, ARRAY_SIZE(pin_name), "%s Spk",
			 codec_dai->component->name_prefix);

		switch (cmd) {
		case SNDRV_PCM_TRIGGER_START:
		case SNDRV_PCM_TRIGGER_RESUME:
		case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
			ret = snd_soc_dapm_enable_pin(dapm, pin_name);
			if (!ret)
				snd_soc_dapm_sync(dapm);
			break;
		case SNDRV_PCM_TRIGGER_STOP:
		case SNDRV_PCM_TRIGGER_SUSPEND:
		case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
			ret = snd_soc_dapm_disable_pin(dapm, pin_name);
			if (!ret)
				snd_soc_dapm_sync(dapm);
			break;
		default:
			break;
		}
	}

	return ret;
};
EXPORT_SYMBOL_NS(maxim_trigger, SND_SOC_INTEL_SOF_MAXIM_COMMON);

/*
 * 2/4 Maxim codecs
 */
static int maxim_hw_params(struct snd_pcm_substream *substream,
			       struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = asoc_substream_to_rtd(substream);
	struct snd_soc_dai *codec_dai;
	int i;


	for_each_rtd_codec_dais(rtd, i, codec_dai) {
		// max98390 codec support feedback path internally
		if (!strcmp(maxim_components[i].dai_name, "max98390-aif1")) {
			snd_soc_dai_set_tdm_slot(codec_dai, maxim_dsp_tdm_mask[i].tx,
					maxim_dsp_tdm_mask[i].rx, 4, params_width(params));
		} else {
			snd_soc_dai_set_tdm_slot(codec_dai, maxim_tdm_mask[i].tx,
						maxim_tdm_mask[i].rx, 8, params_width(params));
		}
	}
	return 0;
}

int maxim_spk_codec_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_card *card = rtd->card;
	int ret;

	ret = snd_soc_add_card_controls(card, maxim_kcontrols,
					ARRAY_SIZE(maxim_kcontrols));
	if (ret) {
		dev_err(rtd->dev, "unable to add card controls, ret %d\n", ret);
		return ret;
	}

	ret = snd_soc_dapm_new_controls(&card->dapm, maxim_dapm_widgets,
					ARRAY_SIZE(maxim_dapm_widgets));
	if (ret) {
		dev_err(rtd->dev, "unable to add dapm controls, ret %d\n", ret);
		return ret;
	}

	ret = snd_soc_dapm_add_routes(&card->dapm, maxim_dapm_routes,
				      ARRAY_SIZE(maxim_dapm_routes));
	if (ret) {
		dev_err(rtd->dev, "Speaker map addition failed: %d\n", ret);
		return ret;
	}

	if (num_codecs == 4) {
		ret = snd_soc_dapm_new_controls(&card->dapm, maxim_tt_dapm_widgets,
							ARRAY_SIZE(maxim_tt_dapm_widgets));
		if (ret) {
			dev_err(rtd->dev, "unable to add tweeter dapm controls, ret %d\n", ret);
			return ret;
		}

		ret = snd_soc_add_card_controls(card, maxim_tt_kcontrols,
						ARRAY_SIZE(maxim_tt_kcontrols));
		if (ret) {
			dev_err(rtd->dev, "unable to add tweeter card controls, ret %d\n", ret);
			return ret;
		}

		ret = snd_soc_dapm_add_routes(&card->dapm, maxim_tt_dapm_routes,
						ARRAY_SIZE(maxim_tt_dapm_routes));
		if (ret)
			dev_err(rtd->dev,
				"unable to add Tweeter Left/Right Speaker dapm, ret %d\n", ret);
	}

	return ret;
};
EXPORT_SYMBOL_NS(maxim_spk_codec_init, SND_SOC_INTEL_SOF_MAXIM_COMMON);

static int maxim_compute_codec_conf(void)
{
	const char * const uid_strings[] = { "0", "1", "2", "3" };
	const char * const MAXIM_HID[] = {"MX98373", "MX98390", "ADS8396"};
	const char * const CODEC_DAI[] = {"max98373", "max98390", "max98396"};

	unsigned int uid, amp_nums = 0;
	unsigned int hid, cur = 0;
	struct acpi_device *adev;
	struct device *physdev;

	// finding the first match codec HID in MAXIM_HID table.
	for (uid = 0; uid < MAXIM_MAX_AMPS; uid++) {
		for(hid = cur; hid < MAXIM_HID_NUMS; hid++) {
			adev = acpi_dev_get_first_match_dev(MAXIM_HID[hid], uid_strings[uid], -1);

			if (!adev)
				continue;

			// found, checking matched codec numbers.
			cur = hid;
			break;
		}

		if (!adev) return amp_nums;

		physdev = get_device(acpi_get_first_physical_node(adev));

		if (!physdev)
			return amp_nums;

		// component info
		maxim_components[amp_nums].name = dev_name(physdev);

		snprintf(maxim_codec_name, sizeof(maxim_codec_name),
						"%s-aif1", CODEC_DAI[cur]);
		maxim_components[amp_nums].dai_name = maxim_codec_name;

		// codec info
		maxim_codec_conf[amp_nums].dlc.name = dev_name(physdev);
		maxim_codec_conf[amp_nums].name_prefix = maxim_name_prefixes[uid];

		acpi_dev_put(adev);
		amp_nums++;
	}

	if (amp_nums != 2 && amp_nums != 4)
		pr_warn("Invalid number of amps found: %d, expected 2 or 4\n", amp_nums);

	return amp_nums;
}

/*
 * 2/4 Maxim codecs
 */
void maxim_set_codec_conf(struct snd_soc_card *card)
{
	card->codec_conf = maxim_codec_conf;
	card->num_configs = ARRAY_SIZE(maxim_codec_conf);
};
EXPORT_SYMBOL_NS(maxim_set_codec_conf, SND_SOC_INTEL_SOF_MAXIM_COMMON);

void maxim_dai_link(struct snd_soc_dai_link *link)
{
	num_codecs = maxim_compute_codec_conf();
	link->codecs = maxim_components;
	link->num_codecs = num_codecs;
	link->init = maxim_spk_codec_init;
	link->ops = &maxim_ops;
};
EXPORT_SYMBOL_NS(maxim_dai_link, SND_SOC_INTEL_SOF_MAXIM_COMMON);

const struct snd_soc_ops maxim_ops = {
	.hw_params = maxim_hw_params,
	.trigger = maxim_trigger,
};
EXPORT_SYMBOL_NS(maxim_ops, SND_SOC_INTEL_SOF_MAXIM_COMMON);

/*
 * Maxim MAX98357A/MAX98360A
 */
static const struct snd_kcontrol_new max_98357a_kcontrols[] = {
	SOC_DAPM_PIN_SWITCH("Spk"),
};

static const struct snd_soc_dapm_widget max_98357a_dapm_widgets[] = {
	SND_SOC_DAPM_SPK("Spk", NULL),
};

static const struct snd_soc_dapm_route max_98357a_dapm_routes[] = {
	/* speaker */
	{"Spk", NULL, "Speaker"},
};

static struct snd_soc_dai_link_component max_98357a_components[] = {
	{
		.name = MAX_98357A_DEV0_NAME,
		.dai_name = MAX_98357A_CODEC_DAI,
	}
};

static struct snd_soc_dai_link_component max_98360a_components[] = {
	{
		.name = MAX_98360A_DEV0_NAME,
		.dai_name = MAX_98357A_CODEC_DAI,
	}
};

static int max_98357a_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_card *card = rtd->card;
	int ret;

	ret = snd_soc_dapm_new_controls(&card->dapm, max_98357a_dapm_widgets,
					ARRAY_SIZE(max_98357a_dapm_widgets));
	if (ret) {
		dev_err(rtd->dev, "unable to add dapm controls, ret %d\n", ret);
		/* Don't need to add routes if widget addition failed */
		return ret;
	}

	ret = snd_soc_add_card_controls(card, max_98357a_kcontrols,
					ARRAY_SIZE(max_98357a_kcontrols));
	if (ret) {
		dev_err(rtd->dev, "unable to add card controls, ret %d\n", ret);
		return ret;
	}

	ret = snd_soc_dapm_add_routes(&card->dapm, max_98357a_dapm_routes,
				      ARRAY_SIZE(max_98357a_dapm_routes));

	if (ret)
		dev_err(rtd->dev, "unable to add dapm routes, ret %d\n", ret);

	return ret;
}

void max_98357a_dai_link(struct snd_soc_dai_link *link)
{
	link->codecs = max_98357a_components;
	link->num_codecs = ARRAY_SIZE(max_98357a_components);
	link->init = max_98357a_init;
}
EXPORT_SYMBOL_NS(max_98357a_dai_link, SND_SOC_INTEL_SOF_MAXIM_COMMON);

void max_98360a_dai_link(struct snd_soc_dai_link *link)
{
	link->codecs = max_98360a_components;
	link->num_codecs = ARRAY_SIZE(max_98360a_components);
	link->init = max_98357a_init;
}
EXPORT_SYMBOL_NS(max_98360a_dai_link, SND_SOC_INTEL_SOF_MAXIM_COMMON);

MODULE_DESCRIPTION("ASoC Intel SOF Maxim helpers");
MODULE_LICENSE("GPL");
