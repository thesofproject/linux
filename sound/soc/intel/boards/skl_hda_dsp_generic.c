// SPDX-License-Identifier: GPL-2.0-only
// Copyright(c) 2015-18 Intel Corporation.

/*
 * Machine Driver for SKL+ platforms with DSP and iDisp, HDA Codecs
 */

#include <linux/acpi.h>
#include <linux/dmi.h>
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

/*
 * Module parameter to disable SSP quirks for testing HDA side-codec approach.
 * Default: false (SSP enabled)
 * Set to true to skip SSP configuration and use HDA side-codec binding.
 */
static bool disable_ssp_quirks;
module_param(disable_ssp_quirks, bool, 0644);
MODULE_PARM_DESC(disable_ssp_quirks, "Disable SSP quirks for Legion (for HDA side-codec testing)");

static const struct dmi_system_id legion_aw88399_dmi_table[] = {
	{
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
			DMI_MATCH(DMI_PRODUCT_NAME, "83F5"),
		},
	},
	{ }
};

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

/* Name prefixes for AW88399 codec controls (left/right channel) */
static const char * const aw88399_name_prefixes[] = { "Left", "Right" };

/*
 * Manual DAPM routes for AW88399 multi-codec setup.
 * With NULL sname in codec DAPM widgets (required to avoid infinite dirty
 * propagation in multi-codec scenarios), auto-linking is disabled. These
 * manual routes connect the prefixed DAI widgets to prefixed codec AIF widgets.
 */
static const struct snd_soc_dapm_route aw88399_dapm_routes[] = {
	/* Left channel: DAI widget -> codec AIF_RX widget */
	{"Left AIF_RX", NULL, "Left Speaker_Playback"},

	/* Right channel: DAI widget -> codec AIF_RX widget */
	{"Right AIF_RX", NULL, "Right Speaker_Playback"},
};

static int aw88399_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_card *card = rtd->card;
	int ret;

	/*
	 * Add manual DAPM routes since codec uses NULL sname.
	 * This creates the missing link between DAI widgets and codec AIF widgets
	 * that would normally be created by auto-linking.
	 */
	ret = snd_soc_dapm_add_routes(&card->dapm, aw88399_dapm_routes,
				      ARRAY_SIZE(aw88399_dapm_routes));
	if (ret)
		dev_err(rtd->dev, "Failed to add AW88399 DAPM routes: %d\n", ret);

	return ret;
}

static int skl_hda_set_aw88399_dai_link(struct device *dev,
					struct snd_soc_dai_link *link,
					struct sof_card_private *ctx)
{
	struct snd_soc_dai_link_component *codecs;
	struct snd_soc_codec_conf *codec_conf;
	int count;
	int i;
	bool legion_quirk;

	/*
	 * Skip SSP DAI link setup when disable_ssp_quirks=1 to allow
	 * HDA side-codec approach instead. Return 0 (success) to allow
	 * card registration to continue without this link.
	 */
	if (disable_ssp_quirks) {
		dev_info(dev, "SSP quirks disabled, skipping AW88399 DAI link setup\n");
		return 0;
	}

	/*
	 * Use cached ACPI scan results if available. This prevents repeated
	 * ACPI scanning during deferred probe retries which can cause system
	 * instability.
	 */
	if (ctx->aw88399.acpi_scanned) {
		count = ctx->aw88399.codec_count;
		legion_quirk = ctx->aw88399.is_legion_quirk;
		goto skip_acpi_scan;
	}

	/* First call: scan ACPI and cache results */
	{
		struct acpi_device *adev;
		int scan_count = 0;

		for_each_acpi_dev_match(adev, AW88399_ACPI_HID, NULL, -1) {
			if (scan_count < 2) {
				/* Cache ACPI device name */
				strscpy(ctx->aw88399.acpi_names[scan_count],
					acpi_dev_name(adev),
					sizeof(ctx->aw88399.acpi_names[0]));
			}
			scan_count++;
			acpi_dev_put(adev);
		}

		if (!scan_count)
			return -ENODEV;

		ctx->aw88399.codec_count = scan_count;
		count = scan_count;
	}

	/*
	 * Lenovo Legion Pro 7 16IAX10H quirk: BIOS exposes only one ACPI device
	 * (at I2C address 0x35) but hardware has two physical AW88399 chips
	 * (0x34 and 0x35) for stereo woofer configuration. When only 1 ACPI
	 * device is found on Legion, hardcode both I2C addresses.
	 */
	legion_quirk = (count == 1 && dmi_check_system(legion_aw88399_dmi_table));
	ctx->aw88399.is_legion_quirk = legion_quirk;
	ctx->aw88399.acpi_scanned = true;

skip_acpi_scan:
	if (legion_quirk) {
		dev_info(dev, "Lenovo Legion: forcing 2-chip stereo configuration for AW88399\n");
		count = 2;
	}

	codecs = devm_kcalloc(dev, count, sizeof(*codecs), GFP_KERNEL);
	if (!codecs)
		return -ENOMEM;

	/*
	 * Allocate codec_conf for name prefixes to avoid control name
	 * conflicts between multiple AW88399 chips
	 */
	codec_conf = devm_kcalloc(dev, count, sizeof(*codec_conf), GFP_KERNEL);
	if (!codec_conf)
		return -ENOMEM;

	if (legion_quirk) {
		/* Hardcode both I2C addresses for Legion stereo setup */
		codecs[0].name = devm_kstrdup(dev, "aw88399.2-0034", GFP_KERNEL);
		if (!codecs[0].name)
			return -ENOMEM;
		codecs[0].dai_name = "aw88399-aif";

		/* Use cached ACPI name for second device */
		codecs[1].name = devm_kasprintf(dev, GFP_KERNEL, "i2c-%s",
						ctx->aw88399.acpi_names[0]);
		if (!codecs[1].name)
			return -ENOMEM;
		codecs[1].dai_name = "aw88399-aif";
	} else {
		/* Normal path: use cached ACPI device names */
		for (i = 0; i < count && i < 2; i++) {
			codecs[i].name = devm_kasprintf(dev, GFP_KERNEL, "i2c-%s",
						       ctx->aw88399.acpi_names[i]);
			if (!codecs[i].name)
				return -ENOMEM;
			codecs[i].dai_name = "aw88399-aif";
		}
	}

	/* Set up codec_conf with name prefixes to avoid control name conflicts */
	for (i = 0; i < count; i++) {
		codec_conf[i].dlc.name = codecs[i].name;
		codec_conf[i].name_prefix = aw88399_name_prefixes[i];
	}

	link->codecs = codecs;
	link->num_codecs = count;
	link->init = aw88399_init;  /* Add manual DAPM routes */

	/* Store codec_conf for card setup */
	ctx->aw88399.codec_conf = codec_conf;
	ctx->aw88399.num_codecs = count;

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

	/*
	 * Override amp detection when testing HDA side-codec approach.
	 * sof_intel_board_get_ctx() sets amp_type via ACPI scan before we
	 * check disable_ssp_quirks. Force CODEC_NONE to prevent creation of
	 * invalid SSP DAI link (would have num_cpus=1, num_codecs=0).
	 */
	if (disable_ssp_quirks) {
		ctx->amp_type = CODEC_NONE;
		ctx->ssp_amp = 0;
		ctx->amp_link = NULL;
		dev_info(&pdev->dev, "SSP quirks disabled: forcing amp_type=CODEC_NONE for HDA side-codec\n");
	}

	if (HDA_EXT_CODEC(mach->mach_params.codec_mask))
		ctx->hda_codec_present = true;

	if (mach->mach_params.codec_mask & IDISP_CODEC_MASK)
		ctx->hdmi.idisp_codec = true;

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

		ret = skl_hda_set_aw88399_dai_link(&pdev->dev, ctx->amp_link, ctx);
		if (ret)
			return dev_err_probe(&pdev->dev, ret,
					     "failed to configure AW88399 link\n");

		/*
		 * Set codec_conf for name prefixes to avoid control name
		 * conflicts between multiple AW88399 codecs
		 */
		if (ctx->aw88399.codec_conf && ctx->aw88399.num_codecs > 0) {
			card->codec_conf = ctx->aw88399.codec_conf;
			card->num_configs = ctx->aw88399.num_codecs;
		}
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
