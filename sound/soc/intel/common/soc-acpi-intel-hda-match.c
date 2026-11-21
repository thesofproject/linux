// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2018, Intel Corporation.

/*
 * soc-acpi-intel-hda-match.c - tables and support for HDA+ACPI enumeration.
 *
 */

#include <linux/dmi.h>
#include <sound/soc-acpi.h>
#include <sound/soc-acpi-intel-match.h>

static const struct dmi_system_id hda_pdm1_dmic[] = {
	{
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "XIAOMI"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Redmi Book Pro 16 2025"),
		},
	},
	{ }
};

static struct snd_soc_acpi_mach intel_hda_dmic_4ch_pdm1_mach = {
	/* .id is not used in this file */
	.drv_name = "skl_hda_dsp_generic",
	.sof_tplg_filename = "sof-hda-generic-4ch-pdm1",
};

static struct snd_soc_acpi_mach *hda_dmic_pdm1_quirk(void *arg)
{
	struct snd_soc_acpi_mach *mach = arg;

	if (dmi_check_system(hda_pdm1_dmic))
		return &intel_hda_dmic_4ch_pdm1_mach;

	return mach;
};

struct snd_soc_acpi_mach snd_soc_acpi_intel_hda_machines[] = {
	{
		/* .id is not used in this file */
		.drv_name = "skl_hda_dsp_generic",
		.machine_quirk = hda_dmic_pdm1_quirk,
		.sof_tplg_filename = "sof-hda-generic", /* the tplg suffix is added at run time */
		.tplg_quirk_mask = SND_SOC_ACPI_TPLG_INTEL_DMIC_NUMBER,
	},
	{},
};
EXPORT_SYMBOL_GPL(snd_soc_acpi_intel_hda_machines);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Intel Common ACPI Match module");
