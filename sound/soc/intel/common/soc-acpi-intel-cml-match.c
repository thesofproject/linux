// SPDX-License-Identifier: GPL-2.0
/*
 * soc-acpi-intel-cml-match.c - tables and support for CML ACPI enumeration.
 *
 * Copyright (c) 2019, Intel Corporation.
 *
 */

#include <sound/soc-acpi.h>
#include <sound/soc-acpi-intel-match.h>

static struct snd_soc_acpi_codecs cml_codecs = {
	.num_codecs = 1,
	.codecs = {"10EC5682"}
};

static struct snd_soc_acpi_codecs cml_spk_codecs = {
	.num_codecs = 1,
	.codecs = {"MX98357A"}
};

struct snd_soc_acpi_mach snd_soc_acpi_intel_cml_machines[] = {
	{
		.id = "DLGS7219",
		.drv_name = "cml_da7219_max98357a",
		.quirk_data = &cml_spk_codecs,
		.sof_fw_filename = "sof-cml.ri",
		.sof_tplg_filename = "sof-cml-da7219-max98357a.tplg",
	},
	{
		.id = "MX98357A",
		.drv_name = "sof_rt5682",
		.quirk_data = &cml_codecs,
		.sof_fw_filename = "sof-cml.ri",
		.sof_tplg_filename = "sof-cml-rt5682-max98357a.tplg",
	},
	{
		.id = "10EC1011",
		.drv_name = "cml_rt1011_rt5682",
		.quirk_data = &cml_codecs,
		.sof_fw_filename = "sof-cml.ri",
		.sof_tplg_filename = "sof-cml-rt1011-rt5682.tplg",
	},
	{
		.id = "10EC5682",
		.drv_name = "sof_rt5682",
		.sof_fw_filename = "sof-cml.ri",
		.sof_tplg_filename = "sof-cml-rt5682.tplg",
	},

	{},
};
EXPORT_SYMBOL_GPL(snd_soc_acpi_intel_cml_machines);

struct snd_soc_acpi_mach snd_soc_acpi_intel_cml_sdw_machines[] = {
	{
		.link_mask = 0xF, /* 4 active links required */
		.drv_name = "sdw_rt711_rt1308_rt715",
		.sof_fw_filename = "sof-cml.ri",
		.sof_tplg_filename = "sof-cml-rt711-rt1308-rt715.tplg",
	},
	{
		.link_mask = 0xB, /* 3 active links required */
		.drv_name = "sdw_rt711_rt1308_rt715",
		.sof_fw_filename = "sof-cml.ri",
		.sof_tplg_filename = "sof-cml-rt711-rt1308-mono-rt715.tplg",
	},
	{
		.link_mask = 0x2, /* RT700 connected on Link1 */
		.drv_name = "sdw_rt700",
		.sof_fw_filename = "sof-cml.ri",
		.sof_tplg_filename = "sof-cml-rt700.tplg",
	},
	{}
};
EXPORT_SYMBOL_GPL(snd_soc_acpi_intel_cml_sdw_machines);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Intel Common ACPI Match module");
