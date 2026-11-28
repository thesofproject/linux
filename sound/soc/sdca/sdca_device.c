// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
// Copyright(c) 2024 Intel Corporation

/*
 * The MIPI SDCA specification is available for public downloads at
 * https://www.mipi.org/mipi-sdca-v1-0-download
 */

#include <linux/acpi.h>
#include <linux/device.h>
#include <linux/dmi.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/soundwire/sdw.h>
#include <sound/sdca.h>
#include <sound/sdca_function.h>

#if IS_ENABLED(CONFIG_ACPI)
void sdca_lookup_interface_revision(struct sdw_slave *slave)
{
	struct fwnode_handle *fwnode = slave->dev.fwnode;

	/*
	 * if this property is not present, then the sdca_interface_revision will
	 * remain zero, which will be considered as 'not defined' or 'invalid'.
	 */
	fwnode_property_read_u32(fwnode, "mipi-sdw-sdca-interface-revision",
				 &slave->sdca_data.interface_revision);
}
EXPORT_SYMBOL_NS(sdca_lookup_interface_revision, "SND_SOC_SDCA");

static void devm_acpi_table_put(void *ptr)
{
	acpi_put_table((struct acpi_table_header *)ptr);
}

void sdca_lookup_swft(struct sdw_slave *slave)
{
	acpi_status status;

	status = acpi_get_table(ACPI_SIG_SWFT, 0,
				(struct acpi_table_header **)&slave->sdca_data.swft);
	if (ACPI_FAILURE(status))
		dev_info(&slave->dev, "SWFT not available\n");
	else
		devm_add_action_or_reset(&slave->dev, devm_acpi_table_put,
					 slave->sdca_data.swft);
}
#else
void sdca_lookup_swft(struct sdw_slave *slave) { }
#endif
EXPORT_SYMBOL_NS(sdca_lookup_swft, "SND_SOC_SDCA");

static bool sdca_device_quirk_rt712_vb(struct sdw_slave *slave)
{
	struct sdw_slave_id *id = &slave->id;
	int i;

	/*
	 * The RT712_VA relies on the v06r04 draft, and the
	 * RT712_VB on a more recent v08r01 draft.
	 */
	if (slave->sdca_data.interface_revision < 0x0801)
		return false;

	if (id->mfg_id != 0x025d)
		return false;

	if (id->part_id != 0x712 &&
	    id->part_id != 0x713 &&
	    id->part_id != 0x716 &&
	    id->part_id != 0x717)
		return false;

	for (i = 0; i < slave->sdca_data.num_functions; i++) {
		if (slave->sdca_data.function[i].type == SDCA_FUNCTION_TYPE_SMART_MIC)
			return true;
	}

	return false;
}

static bool sdca_device_quirk_skip_func_type_patching(struct sdw_slave *slave)
{
	const char *vendor, *sku;

	vendor = dmi_get_system_info(DMI_SYS_VENDOR);
	sku = dmi_get_system_info(DMI_PRODUCT_SKU);

	if (vendor && sku &&
	    !strcmp(vendor, "Dell Inc.") &&
	    (!strcmp(sku, "0C62") || !strcmp(sku, "0C63") || !strcmp(sku, "0C6B")) &&
	    slave->sdca_data.interface_revision == 0x061c &&
	    slave->id.mfg_id == 0x01fa && slave->id.part_id == 0x4243)
		return true;

	return false;
}

/*
 * Boards whose firmware omits the DisCo constant (mipi-sdca-control-dc-value)
 * for the Function Topology control (0x05) of their TI TAS2783 amplifiers,
 * so the function type cannot be read. The TAS2783 only implements a
 * SmartAmp function, so it is safe to assume that type on these boards.
 */
static const struct dmi_system_id tas2783_smart_amp_func_type_dmi[] = {
	{
		/* HP OmniBook Ultra 14 */
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "HP"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "8EB4"),
		},
	},
	{}
};

static bool sdca_device_quirk_tas2783_smart_amp_func_type(struct sdw_slave *slave)
{
	if (slave->id.mfg_id != 0x0102 || slave->id.part_id != 0x0000)
		return false;

	return dmi_check_system(tas2783_smart_amp_func_type_dmi);
}

bool sdca_device_quirk_match(struct sdw_slave *slave, enum sdca_quirk quirk)
{
	switch (quirk) {
	case SDCA_QUIRKS_RT712_VB:
		return sdca_device_quirk_rt712_vb(slave);
	case SDCA_QUIRKS_SKIP_FUNC_TYPE_PATCHING:
		return sdca_device_quirk_skip_func_type_patching(slave);
	case SDCA_QUIRKS_TAS2783_SMART_AMP_FUNC_TYPE:
		return sdca_device_quirk_tas2783_smart_amp_func_type(slave);
	default:
		break;
	}
	return false;
}
EXPORT_SYMBOL_NS(sdca_device_quirk_match, "SND_SOC_SDCA");
