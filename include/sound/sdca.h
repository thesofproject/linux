/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause) */
/*
 * The MIPI SDCA specification is available for public downloads at
 * https://www.mipi.org/mipi-sdca-v1-0-download
 *
 * Copyright(c) 2024 Intel Corporation
 */

#ifndef __SDCA_H__
#define __SDCA_H__

struct sdw_slave;

/**
 * sdca_device_data - structure containing all SDCA related information
 * @sdca_interface_revision: value read from _DSD property, mainly to check
 * for changes between silicon versions
 * @sdca_function_mask: shortcut to list all SDCA functions with a bitmask
 */
struct sdca_device_data {
	u32 interface_revision;
	u32 function_mask;
};

enum sdca_quirk {
	SDCA_QUIRKS_RT712_VB,
};

#if IS_ENABLED(CONFIG_ACPI) && IS_ENABLED(CONFIG_SND_SOC_SDCA)

void sdca_lookup_function_mask(struct sdw_slave *slave);
void sdca_lookup_interface_revision(struct sdw_slave *slave);
bool sdca_device_quirk_match(struct sdw_slave *slave, enum sdca_quirk quirk);

#else

static inline void sdca_lookup_function_mask(struct sdw_slave *slave) {}
static inline void sdca_lookup_interface_revision(struct sdw_slave *slave) {}
static inline bool sdca_device_quirk_match(struct sdw_slave *slave, enum sdca_quirk quirk)
{
	return false;
}
#endif

#endif
