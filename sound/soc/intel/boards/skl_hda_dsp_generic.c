// SPDX-License-Identifier: GPL-2.0-only
// Copyright(c) 2015-18 Intel Corporation.

/*
 * Machine Driver for SKL+ platforms with DSP and iDisp, HDA Codecs
 */

#include <linux/acpi.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <sound/core.h>
#include <sound/hda_codec.h>
#include <sound/jack.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-acpi.h>
#include "../../codecs/hdac_hda.h"
#include "../../sof/intel/hda.h"
#include "sof_board_helpers.h"

static int skl_hda_card_late_probe(struct snd_soc_card *card)
{
	return sof_intel_board_card_late_probe(card);
}

#define HDA_CODEC_AUTOSUSPEND_DELAY_MS 1000

static void skl_set_hda_codec_autosuspend_delay(struct snd_soc_card *card)
{
	struct snd_soc_pcm_runtime *rtd;
	struct hdac_hda_priv *hda_pvt;
	struct snd_soc_dai *dai;

	for_each_card_rtds(card, rtd) {
		if (!strstr(rtd->dai_link->codecs->name, "ehdaudio0D0"))
			continue;
		dai = snd_soc_rtd_to_codec(rtd, 0);
		hda_pvt = snd_soc_component_get_drvdata(dai->component);
		if (hda_pvt) {
			/*
			 * all codecs are on the same bus, so it's sufficient
			 * to look up only the first one
			 */
			snd_hda_set_power_save(hda_pvt->codec->bus,
					       HDA_CODEC_AUTOSUSPEND_DELAY_MS);
			break;
		}
	}
}

#define IDISP_HDMI_BE_ID	1
#define HDA_BE_ID		4
#define SSP_AMP_BE_ID		5
#define DMIC01_BE_ID		6
#define DMIC16K_BE_ID		7
#define BT_OFFLOAD_BE_ID	8

#define HDA_LINK_ORDER	SOF_LINK_ORDER(SOF_LINK_IDISP_HDMI,  \
				       SOF_LINK_HDA,        \
				       SOF_LINK_AMP,        \
				       SOF_LINK_DMIC01,     \
				       SOF_LINK_DMIC16K,    \
				       SOF_LINK_BT_OFFLOAD, \
				       SOF_LINK_NONE)

#define HDA_LINK_IDS	SOF_LINK_ORDER(IDISP_HDMI_BE_ID,  \
				       HDA_BE_ID,        \
				       SSP_AMP_BE_ID,    \
				       DMIC01_BE_ID,     \
				       DMIC16K_BE_ID,    \
				       BT_OFFLOAD_BE_ID, \
				       0)

static unsigned long
skl_hda_get_board_quirk(struct snd_soc_acpi_mach_params *mach_params)
{
	unsigned long board_quirk = 0;
	int ssp_bt;
	int ssp_amp;

	if (mach_params->i2s_link_mask) {
		ssp_amp = fls(mach_params->i2s_link_mask) - 1;
		if (ssp_amp >= 0)
			board_quirk |= SOF_SSP_PORT_AMP(ssp_amp);
	}

	if (hweight_long(mach_params->bt_link_mask) == 1) {
		ssp_bt = fls(mach_params->bt_link_mask) - 1;
		board_quirk |= SOF_SSP_PORT_BT_OFFLOAD(ssp_bt) |
				SOF_BT_OFFLOAD_PRESENT;
	}

	return board_quirk;
}

static int skl_hda_set_aw88399_dai_link(struct device *dev,
					struct snd_soc_dai_link *link)
{
	struct snd_soc_dai_link_component *codecs;
	struct acpi_device *adev;
	int count = 0;
	int i = 0;

	for_each_acpi_dev_match(adev, AW88399_ACPI_HID, NULL, -1) {
		count++;
		acpi_dev_put(adev);
	}

	if (!count)
		return -ENODEV;

	codecs = devm_kcalloc(dev, count, sizeof(*codecs), GFP_KERNEL);
	if (!codecs)
		return -ENOMEM;

	for_each_acpi_dev_match(adev, AW88399_ACPI_HID, NULL, -1) {
		/* Use ACPI device name directly to avoid I2C enumeration race */
		codecs[i].name = devm_kasprintf(dev, GFP_KERNEL, "i2c-%s",
						acpi_dev_name(adev));
		acpi_dev_put(adev);
		if (!codecs[i].name)
			return -ENOMEM;

		codecs[i].dai_name = "aw88399-aif";
		i++;
	}

	link->codecs = codecs;
	link->num_codecs = i;

	return 0;
}

static int skl_hda_add_dai_link(struct snd_soc_card *card,
				struct snd_soc_dai_link *link)
{
	struct sof_card_private *ctx = snd_soc_card_get_drvdata(card);

	/* Ignore the HDMI PCM link if iDisp is not present */
	if (strstr(link->stream_name, "HDMI") && !ctx->hdmi.idisp_codec)
		link->ignore = true;

	return 0;
}

static int skl_hda_audio_probe(struct platform_device *pdev)
{
	struct snd_soc_acpi_mach *mach = pdev->dev.platform_data;
	struct sof_card_private *ctx;
	struct snd_soc_card *card;
	unsigned long board_quirk = skl_hda_get_board_quirk(&mach->mach_params);
	int ret;
 	int i;

	card = devm_kzalloc(&pdev->dev, sizeof(struct snd_soc_card), GFP_KERNEL);
	if (!card)
		return -ENOMEM;

	card->name = "hda-dsp";
	card->owner = THIS_MODULE;
	card->fully_routed = true;
	card->late_probe = skl_hda_card_late_probe;
	card->add_dai_link = skl_hda_add_dai_link;

	dev_dbg(&pdev->dev, "board_quirk = %lx\n", board_quirk);

	/* initialize ctx with board quirk */
	ctx = sof_intel_board_get_ctx(&pdev->dev, board_quirk);
	if (!ctx)
		return -ENOMEM;

	if (HDA_EXT_CODEC(mach->mach_params.codec_mask))
		ctx->hda_codec_present = true;

	if (mach->mach_params.codec_mask & IDISP_CODEC_MASK)
		ctx->hdmi.idisp_codec = true;

	/* Force AW88399 detection for Lenovo Legion - auto-detection may fail due to timing */
	if (mach->mach_params.subsystem_vendor == 0x17aa &&
	    (mach->mach_params.subsystem_device == 0x3906 ||
	     mach->mach_params.subsystem_device == 0x3907 ||
	     mach->mach_params.subsystem_device == 0x3d6c)) {
		if (ctx->amp_type == CODEC_NONE) {
			dev_info(&pdev->dev, "Lenovo Legion: forcing AW88399 amp detection\n");
			ctx->amp_type = CODEC_AW88399;
		}
	}

	if (ctx->amp_type == CODEC_AW88399 && !ctx->ssp_amp) {
		int ssp_port = fls(mach->mach_params.i2s_link_mask) - 1;

		if (ssp_port >= 0)
			ctx->ssp_amp = ssp_port;
	}

	ctx->link_order_overwrite = HDA_LINK_ORDER;
	ctx->link_id_overwrite = HDA_LINK_IDS;

	/* update dai_link */
	ret = sof_intel_board_set_dai_link(&pdev->dev, card, ctx);
	if (ret)
		return ret;

	if (ctx->amp_type == CODEC_AW88399) {
		if (!ctx->amp_link) {
			dev_err(&pdev->dev, "AW88399 amp link missing\n");
			return -EINVAL;
		}

		ret = skl_hda_set_aw88399_dai_link(&pdev->dev, ctx->amp_link);
		if (ret)
			return dev_err_probe(&pdev->dev, ret,
					     "failed to configure AW88399 link\n");
	}

	card->dev = &pdev->dev;

	if (mach->mach_params.dmic_num > 0) {
		card->components = devm_kasprintf(card->dev, GFP_KERNEL,
					  "cfg-dmics:%d",
					  mach->mach_params.dmic_num);
		if (!card->components)
			return -ENOMEM;
	}

	if (ctx->amp_type == CODEC_AW88399 && ctx->amp_link &&
	    ctx->amp_link->num_codecs > 0) {
		for (i = 0; i < ctx->amp_link->num_codecs; i++) {
			const char *codec = ctx->amp_link->codecs[i].name;
			const char *old = card->components;

			if (!codec)
				continue;

			card->components = devm_kasprintf(card->dev, GFP_KERNEL,
						       "%s%s%s",
						       old ? old : "",
						       old ? " " : "",
						       codec);
			if (!card->components)
				return -ENOMEM;
		}
	}

	ret = snd_soc_fixup_dai_links_platform_name(card,
						    mach->mach_params.platform);
	if (ret)
		return ret;

	snd_soc_card_set_drvdata(card, ctx);

	ret = devm_snd_soc_register_card(&pdev->dev, card);
	if (!ret)
		skl_set_hda_codec_autosuspend_delay(card);

	return ret;
}

static struct platform_driver skl_hda_audio = {
	.probe = skl_hda_audio_probe,
	.driver = {
		.name = "skl_hda_dsp_generic",
		.pm = &snd_soc_pm_ops,
	},
};

module_platform_driver(skl_hda_audio)

/* Module information */
MODULE_DESCRIPTION("SKL/KBL/BXT/APL HDA Generic Machine driver");
MODULE_AUTHOR("Rakesh Ughreja <rakesh.a.ughreja@intel.com>");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:skl_hda_dsp_generic");
MODULE_IMPORT_NS("SND_SOC_INTEL_SOF_BOARD_HELPERS");
