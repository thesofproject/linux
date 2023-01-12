// SPDX-License-Identifier: GPL-2.0-only
/*
 * soc-apci-intel-match-common.c - helper functions for ACPI enumeration.
 *
 * Copyright (c) 2023, Intel Corporation.
 */

#include <sound/soc-acpi.h>
#include <sound/soc-acpi-intel-match.h>

struct acpi_intel_codec_info {
	const char *acpi_hid;
	const char *drv_name;
	const char *tplg_name;
	enum snd_soc_acpi_intel_codec codec_type;
};

static const struct acpi_intel_codec_info codec_list[] = {
	{
		.acpi_hid = "10EC5682",
		.drv_name = "rt5682",
		.tplg_name = "rt5682",
		.codec_type = RT5682,
	},
	{
		.acpi_hid = "RTL5682",
		.drv_name = "rt5682",
		.tplg_name = "rt5682",
		.codec_type = RT5682S,
	},
	{
		.acpi_hid = "10134242",
		.drv_name = "cs4242",
		.tplg_name = "cs42l42",
		.codec_type = CS42L42,
	},
	{
		.acpi_hid = "10508825",
		.drv_name = "nau8825",
		.tplg_name = "nau8825",
		.codec_type = NAU8825,
	},
};

static const struct acpi_intel_codec_info amp_list[] = {
	{
		.acpi_hid = "RTL1015",
		.drv_name = "rt1015p",
		.tplg_name = "rt1015",
		.codec_type = RT1015,
	},
	{
		.acpi_hid = "RTL1019",
		.drv_name = "rt1019p",
		.tplg_name = "rt1019",
		.codec_type = RT1019P,
	},
	{
		.acpi_hid = "MX98357A",
		.drv_name = "mx98357",
		.tplg_name = "max98357a",
		.codec_type = MAX98357A,
	},
	{
		.acpi_hid = "MX98360A",
		.drv_name = "mx98360",
		.tplg_name = "max98360a",
		.codec_type = MAX98360A,
	},
	{
		.acpi_hid = "MX98373",
		.drv_name = "mx98373",
		.tplg_name = "max98373",
		.codec_type = MAX98373,
	},
	{
		.acpi_hid = "MX98390",
		.drv_name = "mx98390",
		.tplg_name = "max98390",
		.codec_type = MAX98390,
	},
	{
		.acpi_hid = "CSC3541",
		.drv_name = "cs35l41",
		.tplg_name = "cs35l41",
		.codec_type = CS35L41,
	},
};

static const struct acpi_intel_codec_info *
snd_soc_acpi_find_codec(const struct acpi_intel_codec_info *codec_info,
			int codec_num, int *ssp_port)
{
	struct acpi_device *adev;
	struct device *dev;
	int i, ret;

	for (i = 0; i < codec_num; i++) {
		adev = acpi_dev_get_first_match_dev(codec_info->acpi_hid, NULL,
						    -1);
		if (!adev) {
			codec_info++;
			continue;
		}

		dev = acpi_get_first_physical_node(adev);

		ret = device_property_read_u32(dev, "intel,ssp-port", ssp_port);
		if (ret)
			*ssp_port = -ENODATA;

		acpi_dev_put(adev);
		return codec_info;
	}

	return NULL;
}

struct snd_soc_acpi_mach *snd_soc_acpi_intel_codec_search(void *arg)
{
	struct snd_soc_acpi_mach *mach = arg;
	const struct acpi_intel_codec_info *codec_info, *amp_info;
	char *platform_name, *drv_name, *tplg_name;
	int codec_ssp, amp_ssp;

	platform_name = (char *)mach->quirk_data;

	codec_info = snd_soc_acpi_find_codec(codec_list, ARRAY_SIZE(codec_list),
					     &codec_ssp);
	amp_info = snd_soc_acpi_find_codec(amp_list, ARRAY_SIZE(amp_list),
					   &amp_ssp);

	if (codec_info)
		drv_name = kasprintf(GFP_KERNEL, "%s_acpi_%s", platform_name,
				     codec_info->drv_name);
	else if (amp_info)
		drv_name = kasprintf(GFP_KERNEL, "%s_acpi_ssp_amp", platform_name);
	else
		return NULL;

	if (!drv_name)
		return NULL;

	if (codec_info && amp_info)
		tplg_name = kasprintf(GFP_KERNEL, "sof-%s-%s-ssp%d-%s-ssp%d.tplg",
				      platform_name, amp_info->tplg_name, amp_ssp,
				      codec_info->tplg_name, codec_ssp);
	else if (codec_info)
		tplg_name = kasprintf(GFP_KERNEL, "sof-%s-%s-ssp%d.tplg",
				      platform_name, codec_info->tplg_name, codec_ssp);
	else if (amp_info)
		tplg_name = kasprintf(GFP_KERNEL, "sof-%s-%s-ssp%d.tplg",
				      platform_name, amp_info->tplg_name, amp_ssp);
	else
		return NULL;

	if (!tplg_name)
		return NULL;

	mach->drv_name = drv_name;
	mach->sof_tplg_filename = tplg_name;
	mach->mach_params.codec_type = codec_info ? codec_info->codec_type : NONE;
	mach->mach_params.codec_ssp = codec_info ? codec_ssp : -ENODATA;
	mach->mach_params.amp_type = amp_info ? amp_info->codec_type : NONE;
	mach->mach_params.amp_ssp = amp_info ? amp_ssp : -ENODATA;

	return mach;
}
EXPORT_SYMBOL_GPL(snd_soc_acpi_intel_codec_search);
