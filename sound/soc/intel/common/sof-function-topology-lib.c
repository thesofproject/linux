// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
//
// This file is provided under a dual BSD/GPLv2 license.  When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2025 Intel Corporation.
//

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/firmware.h>
#include <sound/soc.h>
#include <sound/soc-acpi.h>
#include "sof-function-topology-lib.h"

enum tplg_device_id {
	TPLG_DEVICE_SDCA_JACK,
	TPLG_DEVICE_SDCA_AMP,
	TPLG_DEVICE_SDCA_MIC,
	TPLG_DEVICE_INTEL_PCH_DMIC,
	TPLG_DEVICE_HDMI,
	TPLG_DEVICE_MAX
};

#define SDCA_DEVICE_MASK (BIT(TPLG_DEVICE_SDCA_JACK) | BIT(TPLG_DEVICE_SDCA_AMP) | \
			  BIT(TPLG_DEVICE_SDCA_MIC))

#define SOF_INTEL_PLATFORM_NAME_MAX 4

struct tplg_device {
	enum tplg_device_id id;
	int dai_link_id;
	const char *name;
};

int sof_sdw_get_tplg_files(struct snd_soc_card *card, const struct snd_soc_acpi_mach *mach,
			   const char *prefix, const char ***tplg_files, bool best_effort)
{
	struct snd_soc_acpi_mach_params mach_params = mach->mach_params;
	struct snd_soc_dai_link *dai_link;
	struct tplg_device *tplg_devs;
	const struct firmware *fw;
	char platform[SOF_INTEL_PLATFORM_NAME_MAX];
	unsigned long tplg_mask = 0;
	int tplg_num = 0;
	int ret;
	int i;

	ret = sscanf(mach->sof_tplg_filename, "sof-%3s-*.tplg", platform);
	if (ret != 1) {
		dev_err(card->dev, "Invalid platform name %s of tplg %s\n",
			platform, mach->sof_tplg_filename);
		return -EINVAL;
	}

	/* The worst case is that each dai link uses a topology */
	tplg_devs = devm_kcalloc(card->dev, card->num_links, sizeof(*tplg_devs), GFP_KERNEL);
	if (!tplg_devs)
		return -ENOMEM;

	for_each_card_prelinks(card, i, dai_link) {
		dev_dbg(card->dev, "dai_link %s id %d\n", dai_link->name, dai_link->id);
		if (strstr(dai_link->name, "SimpleJack")) {
			tplg_devs[tplg_num].id = TPLG_DEVICE_SDCA_JACK;
			tplg_devs[tplg_num].name = "sdca-jack";

		} else if (strstr(dai_link->name, "SmartAmp")) {
			tplg_devs[tplg_num].id = TPLG_DEVICE_SDCA_AMP;
			tplg_devs[tplg_num].name = devm_kasprintf(card->dev, GFP_KERNEL,
						       "sdca-%damp", dai_link->num_cpus);
			if (!tplg_devs[tplg_num].name)
				return -ENOMEM;
		} else if (strstr(dai_link->name, "SmartMic")) {
			tplg_devs[tplg_num].id = TPLG_DEVICE_SDCA_MIC;
			tplg_devs[tplg_num].name = "sdca-mic";
		} else if (strstr(dai_link->name, "dmic")) {
			switch (mach_params.dmic_num) {
			case 2:
				tplg_devs[tplg_num].name = "dmic-2ch";
				break;
			case 4:
				tplg_devs[tplg_num].name = "dmic-4ch";
				break;
			default:
				dev_warn(card->dev,
					 "unsupported number of dmics: %d\n",
					 mach_params.dmic_num);
				continue;
			}
			tplg_devs[tplg_num].id = TPLG_DEVICE_INTEL_PCH_DMIC;
		} else if (strstr(dai_link->name, "iDisp")) {
			tplg_devs[tplg_num].id = TPLG_DEVICE_HDMI;
			tplg_devs[tplg_num].name = "hdmi-pcm5";
		} else {
			/* The dai link is not supported by separated tplg yet */
			dev_dbg(card->dev,
				"dai_link %s is not supported by separated tplg yet\n",
				dai_link->name);
			if (best_effort)
				continue;

			return 0;
		}
		if (tplg_mask & BIT(tplg_devs[tplg_num].id))
			continue;

		tplg_devs[tplg_num].dai_link_id = dai_link->id;
		tplg_mask |= BIT(tplg_devs[tplg_num].id);
		tplg_num++;
	}

	dev_dbg(card->dev, "tplg_mask %#lx tplg_num %d\n", tplg_mask, tplg_num);

	/* Check presence of sub-topologies */
	for (i = 0; i < tplg_num; i++) {
		/*
		 * The tplg file naming rule is sof-<platform>-<function>-id<BE id number>.tplg
		 * where <platform> is only required for the DMIC function as the nhlt blob
		 * is platform dependent.
		 */
		switch (tplg_devs[i].id) {
		case TPLG_DEVICE_INTEL_PCH_DMIC:
			(*tplg_files)[i] = devm_kasprintf(card->dev, GFP_KERNEL,
								 "%s/sof-%s-%s-id%d.tplg",
								 prefix, platform,
								 tplg_devs[i].name,
								 tplg_devs[i].dai_link_id);
			break;
		default:
			(*tplg_files)[i] = devm_kasprintf(card->dev, GFP_KERNEL,
								 "%s/sof-%s-id%d.tplg",
								 prefix, tplg_devs[i].name,
								 tplg_devs[i].dai_link_id);
			break;
		}
		if (!(*tplg_files)[i])
			return -ENOMEM;
		ret = firmware_request_nowarn(&fw, (*tplg_files)[i], card->dev);
		if (!ret) {
			release_firmware(fw);
		} else {
			dev_warn(card->dev,
				 "Failed to open topology file: %s, you might need to\n",
				 (*tplg_files)[i]);
			dev_warn(card->dev,
				 "download it from https://github.com/thesofproject/sof-bin/\n");
			return 0;
		}
	}

	return tplg_num;
}
EXPORT_SYMBOL_GPL(sof_sdw_get_tplg_files);
