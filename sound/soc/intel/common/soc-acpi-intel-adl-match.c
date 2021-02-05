// SPDX-License-Identifier: GPL-2.0-only
/*
 * soc-apci-intel-adl-match.c - tables and support for ADL ACPI enumeration.
 *
 * Copyright (c) 2020, Intel Corporation.
 */

#include <sound/soc-acpi.h>
#include <sound/soc-acpi-intel-match.h>

static const struct snd_soc_acpi_endpoint single_endpoint = {
	.num = 0,
	.aggregated = 0,
	.group_position = 0,
	.group_id = 0,
};

static const struct snd_soc_acpi_adr_device rt711_0_adr[] = {
	{
		.adr = 0x000020025D071100,
		.num_endpoints = 1,
		.endpoints = &single_endpoint,
		.name_prefix = "rt711"
	}
};

static const struct snd_soc_acpi_link_adr adl_rvp[] = {
	{
		.mask = BIT(0),
		.num_adr = ARRAY_SIZE(rt711_0_adr),
		.adr_d = rt711_0_adr,
	},
	{}
};

static const struct snd_soc_acpi_codecs adl_max98373_amp = {
	.num_codecs = 1,
	.codecs = {"MX98373"}
};

static const struct snd_soc_acpi_codecs adl_max98357a_amp = {
	.num_codecs = 1,
	.codecs = {"MX98357A"}
};

struct snd_soc_acpi_mach snd_soc_acpi_intel_adl_machines[] = {
	{
		.id = "10EC5682",
		.drv_name = "adl_max98373_rt5682",
		.machine_quirk = snd_soc_acpi_codec_list,
		.quirk_data = &adl_max98373_amp,
		.sof_fw_filename = "sof-adl.ri",
		.sof_tplg_filename = "sof-adl-max98373-rt5682.tplg",
	},
	{
		.id = "10EC5682",
		.drv_name = "adl_max98357a_rt5682",
		.machine_quirk = snd_soc_acpi_codec_list,
		.quirk_data = &adl_max98357a_amp,
		.sof_fw_filename = "sof-adl.ri",
		.sof_tplg_filename = "sof-adl-max98357a-rt5682.tplg",
	},
	{},
};
EXPORT_SYMBOL_GPL(snd_soc_acpi_intel_adl_machines);

/* this table is used when there is no I2S codec present */
struct snd_soc_acpi_mach snd_soc_acpi_intel_adl_sdw_machines[] = {
	{
		.link_mask = 0x1, /* link0 required */
		.links = adl_rvp,
		.drv_name = "sof_sdw",
		.sof_tplg_filename = "sof-adl-rt711.tplg",
	},
	{},
};
EXPORT_SYMBOL_GPL(snd_soc_acpi_intel_adl_sdw_machines);
