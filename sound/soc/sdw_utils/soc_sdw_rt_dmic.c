// SPDX-License-Identifier: GPL-2.0-only
// This file incorporates work covered by the following copyright notice:
// Copyright (c) 2024 Intel Corporation
// Copyright (c) 2024 Advanced Micro Devices, Inc.

/*
 * soc_sdw_rt_dmic - Helpers to handle Realtek SDW DMIC from generic machine driver
 */

#include <linux/device.h>
#include <linux/errno.h>
#include <sound/soc.h>
#include <sound/soc-acpi.h>
#include <sound/soc_sdw_utils.h>
#include <sound/sdca_function.h>

int asoc_sdw_rt_dmic_rtd_init(struct snd_soc_pcm_runtime *rtd, struct snd_soc_dai *dai)
{
	struct snd_soc_card *card = rtd->card;
	struct snd_soc_component *component;
	struct sdw_slave *sdw_peripheral;
	struct asoc_sdw_codec_info *codec_info;
	char *mic_name;
	int rt1320_dmic_num = 0, part_id, i;

	component = dai->component;

	/*
	 * rt715-sdca (aka rt714) is a special case that uses different name in card->components
	 * and component->name_prefix.
	 */
	if (!strcmp(component->name_prefix, "rt714"))
		mic_name = devm_kasprintf(card->dev, GFP_KERNEL, "rt715-sdca");
	/*
	 * If there is any rt1320 DMIC belonging to this card, try to count the `cfg-mics` to be used in
	 * card->components.
	 */
	else if (!strcmp(dai->name, "rt1320-aif2")) {
		codec_info = asoc_sdw_find_codec_info_dai(dai->name, &i);
		part_id = codec_info->part_id;
		mic_name = devm_kasprintf(card->dev, GFP_KERNEL, "%s", codec_info->dais[i].component_name);

		// count the rt1320 with SDCA function SmartMic type in this card
		for_each_card_components(card, component) {
			sdw_peripheral = dev_to_sdw_dev(component->dev);
			if (sdw_peripheral->id.part_id != part_id)
				continue;
			for (i = 0; i < sdw_peripheral->sdca_data.num_functions; i++) {
				if (sdw_peripheral->sdca_data.function[i].type == SDCA_FUNCTION_TYPE_SMART_MIC) {
					rt1320_dmic_num++;
					break;
				}
			}
		}
	} else
		mic_name = devm_kasprintf(card->dev, GFP_KERNEL, "%s", component->name_prefix);
	if (!mic_name)
		return -ENOMEM;

	if (!strcmp(dai->name, "rt1320-aif2"))
		card->components = devm_kasprintf(card->dev, GFP_KERNEL,
						  "%s mic:%s cfg-mics:%d", card->components,
						  mic_name, rt1320_dmic_num);
	else
		card->components = devm_kasprintf(card->dev, GFP_KERNEL,
						  "%s mic:%s", card->components,
						  mic_name);
	if (!card->components)
		return -ENOMEM;

	dev_dbg(card->dev, "card->components: %s\n", card->components);

	return 0;
}
EXPORT_SYMBOL_NS(asoc_sdw_rt_dmic_rtd_init, "SND_SOC_SDW_UTILS");
