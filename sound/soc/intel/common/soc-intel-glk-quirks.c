// SPDX-License-Identifier: GPL-2.0
/*
 * soc-intel-glk-quirks.c - tables and support for SOF autodetection
 *
 * Copyright (c) 2019, Intel Corporation.
 *
 */

#include <linux/dmi.h>
#include <linux/module.h>
#include <asm/cpu_device_id.h>
#include <asm/intel-family.h>
#include "soc-intel-quirks.h"

static const struct dmi_system_id glk_quirk_table[] = {
	{
		.ident = "Google Chromebooks",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Google")
		}
	},
	{}
};

static const struct x86_cpu_id glk_ids[] = {
	{ X86_VENDOR_INTEL, 6, INTEL_FAM6_ATOM_GOLDMONT_PLUS}, /* Gemini Lake */
	{}
};

void sof_intel_quirk_glk(bool *is_glk, bool *sof)
{
	*is_glk = false;
	*sof = false;

	if (x86_match_cpu(glk_ids)) {
		*is_glk = true;

		if (dmi_check_system(glk_quirk_table))
			*sof = true;
	}
}
EXPORT_SYMBOL_GPL(sof_intel_quirk_glk);

MODULE_DESCRIPTION("ASoC Intel(R) quirks");
MODULE_AUTHOR("Pierre-Louis Bossart <pierre-louis.bossart@linux.intel.com>");
MODULE_LICENSE("GPL v2");

