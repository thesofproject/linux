// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2024 Advanced Micro Devices, Inc. All rights reserved

/*
 *  sof_sdw_rt_amp - Helpers to handle RT1308/RT1316/RT1318 from generic machine driver
 */

#include <linux/device.h>
#include <linux/errno.h>
#include <sound/control.h>
#include <sound/soc.h>
#include <sound/soc-acpi.h>
#include <sound/soc-dapm.h>
#include <linux/soundwire/sdw.h>
#include <linux/soundwire/sdw_type.h>
#include <linux/dmi.h>
#include "sof_amd_sdw_common.h"

#define CODEC_NAME_SIZE	7

/* choose a larger value to resolve compatibility issues */
#define RT_AMP_MAX_BQ_REG RT1316_MAX_BQ_REG

static const struct snd_kcontrol_new rt_amp_controls[] = {
	SOC_DAPM_PIN_SWITCH("Speaker"),
};

static const struct snd_soc_dapm_widget rt_amp_widgets[] = {
	SND_SOC_DAPM_SPK("Speaker", NULL),
};

/*
 * dapm routes for rt1316 will be registered dynamically
 * according to the number of rt1316 used. The first two
 * entries will be registered for one codec case, and the last two entries
 * are also registered if two 1316s are used.
 */

static const struct snd_soc_dapm_route rt1316_map[] = {
	{ "Speaker", NULL, "rt1316-1 SPOL" },
	{ "Speaker", NULL, "rt1316-1 SPOR" },
	{ "Speaker", NULL, "rt1316-2 SPOL" },
	{ "Speaker", NULL, "rt1316-2 SPOR" },
};

static const struct snd_soc_dapm_route *get_codec_name_and_route(struct snd_soc_pcm_runtime *rtd,
								 char *codec_name)
{
	const char *dai_name;

	dai_name = rtd->dai_link->codecs->dai_name;

	/* get the codec name */
	snprintf(codec_name, CODEC_NAME_SIZE, "%s", dai_name);

	/* choose the right codec's map  */
	if (strcmp(codec_name, "rt1316") == 0)
		return rt1316_map;

	return NULL;
}

int rt_amp_spk_rtd_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_card *card = rtd->card;
	const struct snd_soc_dapm_route *rt_amp_map;
	char codec_name[CODEC_NAME_SIZE];
	struct snd_soc_dai *dai;
	int ret;
	int i;

	rt_amp_map = get_codec_name_and_route(rtd, codec_name);

	if (!rt_amp_map)
		return -ENODEV;

	card->components = devm_kasprintf(card->dev, GFP_KERNEL,
					  "%s spk:%s",
					  card->components, codec_name);
	if (!card->components)
		return -ENOMEM;

	ret = snd_soc_add_card_controls(card, rt_amp_controls,
					ARRAY_SIZE(rt_amp_controls));
	if (ret) {
		dev_err(card->dev, "%s controls addition failed: %d\n", codec_name, ret);
		return ret;
	}

	ret = snd_soc_dapm_new_controls(&card->dapm, rt_amp_widgets,
					ARRAY_SIZE(rt_amp_widgets));
	if (ret) {
		dev_err(card->dev, "%s widgets addition failed: %d\n", codec_name, ret);
		return ret;
	}

	for_each_rtd_codec_dais(rtd, i, dai) {
		if (strstr(dai->component->name_prefix, "-1"))
			ret = snd_soc_dapm_add_routes(&card->dapm, rt_amp_map, 2);
		else if (strstr(dai->component->name_prefix, "-2"))
			ret = snd_soc_dapm_add_routes(&card->dapm, rt_amp_map + 2, 2);
	}

	return ret;
}

int sof_sdw_rt_amp_exit(struct snd_soc_card *card, struct snd_soc_dai_link *dai_link)
{
	struct mc_private *ctx = snd_soc_card_get_drvdata(card);

	if (ctx->amp_dev1)
		put_device(ctx->amp_dev1);

	if (ctx->amp_dev2)
		put_device(ctx->amp_dev2);

	return 0;
}

int sof_sdw_rt_amp_init(struct snd_soc_card *card,
			const struct snd_soc_acpi_link_adr *link,
			struct snd_soc_dai_link *dai_links,
			struct sof_sdw_codec_info *info,
			bool playback)
{
	struct mc_private *ctx = snd_soc_card_get_drvdata(card);
	struct device *sdw_dev1, *sdw_dev2;

	/* Count amp number and do init on playback link only. */
	if (!playback)
		return 0;

	info->amp_num++;

	if (info->amp_num == 2) {
		sdw_dev1 = bus_find_device_by_name(&sdw_bus_type, NULL, dai_links->codecs[0].name);
		if (!sdw_dev1)
			return -EPROBE_DEFER;

		ctx->amp_dev1 = sdw_dev1;

		sdw_dev2 = bus_find_device_by_name(&sdw_bus_type, NULL, dai_links->codecs[1].name);
		if (!sdw_dev2)
			return -EPROBE_DEFER;

		ctx->amp_dev2 = sdw_dev2;
	}

	return 0;
}
