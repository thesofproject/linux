// SPDX-License-Identifier: GPL-2.0
/*
 * soc-intel-apl-quirks.c - tables and support for SOF autodetection
 *
 * Copyright (c) 2019, Intel Corporation.
 *
 */

#include <linux/dmi.h>
#include <linux/module.h>
#include <asm/cpu_device_id.h>
#include <asm/intel-family.h>
#include "soc-intel-quirks.h"

static const struct dmi_system_id apl_quirk_table[] = {
	{
		.ident = "Up Squared",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "AAEON"),
			DMI_MATCH(DMI_BOARD_NAME, "UP-APL01"),
		}
	},
	{}
};

static const struct x86_cpu_id apl_ids[] = {
	{ X86_VENDOR_INTEL, 6, INTEL_FAM6_ATOM_GOLDMONT}, /* Apollo Lake */
	{}
};

void sof_intel_quirk_apl(bool *is_apl, bool *sof)
{
	*is_apl = false;
	*sof = false;

	if (x86_match_cpu(apl_ids)) {
		*is_apl = true;

		if (dmi_check_system(apl_quirk_table))
			*sof = true;
	}
}
EXPORT_SYMBOL_GPL(sof_intel_quirk_apl);

MODULE_DESCRIPTION("ASoC Intel(R) quirks");
MODULE_AUTHOR("Pierre-Louis Bossart <pierre-louis.bossart@linux.intel.com>");
MODULE_LICENSE("GPL v2");
