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

#define MAX_98373_PIN_NAME 16
#define MAXIM_MAX_CODECS 4

static const struct {
	const char *hid;
	const char *dai_name;
} maxim_codec_info[] = {
	{.hid = "MX98373", .dai_name = "max98373-aif1"},
	{.hid = "MX98390", .dai_name = "max98390-aif1"},
	{.hid = "ADS8396", .dai_name = "max98396-aif1"},
};

static struct snd_soc_codec_conf codec_conf[MAXIM_MAX_CODECS];
static struct snd_soc_dai_link_component codec_component[MAXIM_MAX_CODECS];

const struct snd_kcontrol_new maxim_kcontrols[] = {
	SOC_DAPM_PIN_SWITCH("Left Spk"),
	SOC_DAPM_PIN_SWITCH("Right Spk"),
};
EXPORT_SYMBOL_NS(maxim_kcontrols, SND_SOC_INTEL_SOF_MAXIM_COMMON);

const struct snd_soc_dapm_widget maxim_dapm_widgets[] = {
	SND_SOC_DAPM_SPK("Left Spk", NULL),
	SND_SOC_DAPM_SPK("Right Spk", NULL),
};
EXPORT_SYMBOL_NS(maxim_dapm_widgets, SND_SOC_INTEL_SOF_MAXIM_COMMON);

const struct snd_soc_dapm_route maxim_dapm_routes[] = {
	/* speaker */
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

static const struct snd_soc_dapm_route maxim_tt_dapm_routes[] = {
	/* Tweeter speaker */
	{ "TL Spk", NULL, "Tweeter Left BE_OUT" },
	{ "TR Spk", NULL, "Tweeter Right BE_OUT" },
};

static int max_98373_hw_params(struct snd_pcm_substream *substream,
			       struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = asoc_substream_to_rtd(substream);
	struct snd_soc_dai *codec_dai;
	int j;

	for_each_rtd_codec_dais(rtd, j, codec_dai) {
		if (!strcmp(codec_dai->component->name, MAX_98373_DEV0_NAME) ||
		    !strcmp(codec_dai->component->name, MAX_98396_DEV0_NAME)) {
			/* DEV0 tdm slot configuration */
			snd_soc_dai_set_tdm_slot(codec_dai, 0x03, 3, 8, 32);
		}
		if (!strcmp(codec_dai->component->name, MAX_98373_DEV1_NAME) ||
		    !strcmp(codec_dai->component->name, MAX_98396_DEV1_NAME)) {
			/* DEV1 tdm slot configuration */
			snd_soc_dai_set_tdm_slot(codec_dai, 0x0C, 3, 8, 32);
		}
	}
	return 0;
}

static int max_98373_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct snd_soc_pcm_runtime *rtd = asoc_substream_to_rtd(substream);
	struct snd_soc_dai *codec_dai;
	struct snd_soc_dai *cpu_dai;
	int j;
	int ret = 0;

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
}

static const struct snd_soc_ops max_98373_ops = {
	.hw_params = max_98373_hw_params,
	.trigger = max_98373_trigger,
};

/*
 * Find present Maxim codecs on board
 *
 * input: device name
 * output: return available codec numbers, support 2 or 4 codecs.
 */
static int get_num_codecs(const char *adev_name)
{
	const char * const CODEC_NUMS[] = { "0", "1", "2", "3" };
	const char * const codec_prefixes[] = { "Right", "Left",
						"Tweeter Right", "Tweeter Left" };

	struct acpi_device *adev;
	unsigned int codec_nums = 0;
	int index = 0;
	int i;
	struct device *physdev;

	/* finding the first matched codec device */
	adev = acpi_dev_get_first_match_dev(adev_name, NULL, -1);

	if (!adev)
		return -ENODEV;

	for (i = 0; i < ARRAY_SIZE(maxim_codec_info); i++) {
		if (!strcmp(adev_name, maxim_codec_info[i].hid)) {
			/* matched: scanning each available codec */
			while (index < ARRAY_SIZE(CODEC_NUMS)) {
				adev = acpi_dev_get_first_match_dev(adev_name,
								    CODEC_NUMS[index++], -1);

				/* continue in case of nonsequential uids,
				 * e.g. [0,2], [0,3], [1,0]...
				 */
				if (!adev)
					continue;

				/* get device info */
				physdev = get_device(acpi_get_first_physical_node(adev));

				if (!physdev)
					return -ENODEV;

				/* set component info */
				codec_component[codec_nums].name = dev_name(physdev);
				codec_component[codec_nums].dai_name = maxim_codec_info[i].dai_name;

				/* set codec info */
				codec_conf[codec_nums].dlc.name = dev_name(physdev);
				codec_conf[codec_nums].name_prefix = codec_prefixes[codec_nums];

				put_device(physdev);

				codec_nums++;
			}
		}
	}

	if (codec_nums != 2 && codec_nums != 4) {
		pr_err("Invalid number of amps found: %d, expected 2 or 4\n", codec_nums);
		return -EINVAL;
	}

	pr_info("found number of available codecs: %d\n", codec_nums);
	return codec_nums;
}

static int maxim_spk_codec_init(struct snd_soc_pcm_runtime *rtd)
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
		dev_err(rtd->dev, "unable to add widgets controls, ret %d\n", ret);
		return ret;
	}

	ret = snd_soc_dapm_add_routes(&card->dapm, maxim_dapm_routes,
				      ARRAY_SIZE(maxim_dapm_routes));
	if (ret) {
		dev_err(rtd->dev, "Speaker map addition failed: %d\n", ret);
		return ret;
	}

	/* add widgets/controls/dapm for tweeter speakers */
	if (get_num_codecs("MX98390") == 4) {
		ret = snd_soc_dapm_new_controls(&card->dapm, maxim_tt_dapm_widgets,
						ARRAY_SIZE(maxim_tt_dapm_widgets));

		if (ret) {
			dev_err(rtd->dev, "unable to add tweeter dapm controls, ret %d\n", ret);
			/* Don't need to add routes if widget addition failed */
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
}

void max_98373_dai_link(struct snd_soc_dai_link *link)
{
	link->codecs = codec_component;
	link->num_codecs = get_num_codecs("MX98373");
	link->init = maxim_spk_codec_init;
	link->ops = &max_98373_ops;
}
EXPORT_SYMBOL_NS(max_98373_dai_link, SND_SOC_INTEL_SOF_MAXIM_COMMON);

void sof_max98373_codec_conf(struct snd_soc_card *card)
{
	card->codec_conf = codec_conf;
	card->num_configs = ARRAY_SIZE(codec_conf);
}
EXPORT_SYMBOL_NS(sof_max98373_codec_conf, SND_SOC_INTEL_SOF_MAXIM_COMMON);

static int max_98390_hw_params(struct snd_pcm_substream *substream,
			       struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = asoc_substream_to_rtd(substream);
	struct snd_soc_dai *codec_dai;
	int i;

	for_each_rtd_codec_dais(rtd, i, codec_dai) {
		if (i >= ARRAY_SIZE(codec_component)) {
			dev_err(codec_dai->dev, "invalid codec index %d\n", i);
			return -ENODEV;
		}

		if (!strcmp(codec_dai->component->name, MAX_98390_DEV0_NAME)) {
			/* DEV0 tdm slot configuration Right */
			snd_soc_dai_set_tdm_slot(codec_dai, 0x01, 3, 4, 32);
		}
		if (!strcmp(codec_dai->component->name, MAX_98390_DEV1_NAME)) {
			/* DEV1 tdm slot configuration Left */
			snd_soc_dai_set_tdm_slot(codec_dai, 0x02, 3, 4, 32);
		}

		if (!strcmp(codec_dai->component->name, MAX_98390_DEV2_NAME)) {
			/* DEVi2 tdm slot configuration Tweeter Right */
			snd_soc_dai_set_tdm_slot(codec_dai, 0x04, 3, 4, 32);
		}
		if (!strcmp(codec_dai->component->name, MAX_98390_DEV3_NAME)) {
			/* DEV3 tdm slot configuration Tweeter Left */
			snd_soc_dai_set_tdm_slot(codec_dai, 0x08, 3, 4, 32);
		}
	}
	return 0;
}

static const struct snd_soc_ops max_98390_ops = {
	.hw_params = max_98390_hw_params,
};

void max_98390_dai_link(struct snd_soc_dai_link *link)
{
	link->codecs = codec_component;
	link->num_codecs = get_num_codecs("MX98390");
	link->init = maxim_spk_codec_init;
	link->ops = &max_98390_ops;
}
EXPORT_SYMBOL_NS(max_98390_dai_link, SND_SOC_INTEL_SOF_MAXIM_COMMON);

void sof_max98390_codec_conf(struct snd_soc_card *card)
{
	card->codec_conf = codec_conf;
	card->num_configs = ARRAY_SIZE(codec_conf);
}
EXPORT_SYMBOL_NS(sof_max98390_codec_conf, SND_SOC_INTEL_SOF_MAXIM_COMMON);

/*
 * Maxim MAX98396
 */
static const struct snd_soc_ops max_98396_ops = {
	.hw_params = max_98373_hw_params,
	.trigger = max_98373_trigger,
};

void max_98396_dai_link(struct snd_soc_dai_link *link)
{
	link->codecs = codec_component;
	link->num_codecs = get_num_codecs("ADS8396");
	link->init = maxim_spk_codec_init;
	link->ops = &max_98396_ops;
}
EXPORT_SYMBOL_NS(max_98396_dai_link, SND_SOC_INTEL_SOF_MAXIM_COMMON);

void sof_max98396_codec_conf(struct snd_soc_card *card)
{
	card->codec_conf = codec_conf;
	card->num_configs = ARRAY_SIZE(codec_conf);
}
EXPORT_SYMBOL_NS(sof_max98396_codec_conf, SND_SOC_INTEL_SOF_MAXIM_COMMON);

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
