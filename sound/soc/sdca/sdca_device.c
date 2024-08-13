// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
// Copyright(c) 2024 Intel Corporation

/*
 * The MIPI SDCA specification is available for public downloads at
 * https://www.mipi.org/mipi-sdca-v1-0-download
 */

#include <linux/acpi.h>
#include <linux/bits.h>
#include <linux/soundwire/sdw.h>
#include <linux/soundwire/sdw_registers.h>
#include <sound/sdca.h>
#include <sound/sdca_function.h>

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
EXPORT_SYMBOL_NS(sdca_lookup_interface_revision, SND_SOC_SDCA);

bool sdca_device_quirk_match(struct sdw_slave *slave, enum sdca_quirk quirk)
{
	struct sdw_slave_id *id = &slave->id;
	int i;

	switch (quirk) {
	case SDCA_QUIRKS_RT712_VB:
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
			if (slave->sdca_data.sdca_func[i].type ==
			    SDCA_FUNCTION_TYPE_SMART_MIC)
				return true;
		}

		break;
	default:
		break;
	}
	return false;
}
EXPORT_SYMBOL_NS(sdca_device_quirk_match, SND_SOC_SDCA);

static struct sdca_function_data *
sdca_data_find_function(struct sdca_device_data *sdca_data, unsigned int reg)
{
	int i;

	for (i = 0; i < sdca_data->num_functions; i++)
		if (SDW_SDCA_CTL_FUNC(reg) == sdca_data->sdca_func[i].adr)
			return sdca_data->sdca_func[i].function;

	return NULL;
}

static struct sdca_entity *
sdca_function_find_entity(struct sdca_function_data *function, unsigned int reg)
{
	int i;

	for (i = 0; i < function->num_entities; i++)
		if (SDW_SDCA_CTL_ENT(reg) == function->entities[i].id)
			return &function->entities[i];

	return NULL;
}

static struct sdca_control *
sdca_entity_find_control(struct sdca_entity *entity, unsigned int reg)
{
	int i;

	for (i = 0; i < entity->num_controls; i++) {
		if (SDW_SDCA_CTL_CSEL(reg) == entity->controls[i].id)
			return &entity->controls[i];
	}

	return NULL;
}

static struct sdca_control *
sdca_device_find_control(struct device *dev, unsigned int reg)
{
	struct sdw_slave *sdw = dev_to_sdw_dev(dev);
	struct sdca_device_data *sdca_data = &sdw->sdca_data;
	struct sdca_function_data *function;
	struct sdca_entity *entity;

	function = sdca_data_find_function(sdca_data, reg);
	if (!function)
		return NULL;

	entity = sdca_function_find_entity(function, reg);
	if (!entity)
		return NULL;

	return sdca_entity_find_control(entity, reg);
}

static bool sdca_valid_address(unsigned int reg)
{
	if ((reg & (GENMASK(31, 25) | BIT(18) | BIT(13))) != BIT(30))
		return false;

	return true;
}

bool sdca_disco_regmap_readable(struct device *dev, unsigned int reg)
{
	struct sdca_control *control;

	if (!sdca_valid_address(reg))
		return false;

	control = sdca_device_find_control(dev, reg);
	if (!control)
		return false;

	switch(control->mode) {
	case SDCA_CONTROL_ACCESS_MODE_RW:
	case SDCA_CONTROL_ACCESS_MODE_RO:
	case SDCA_CONTROL_ACCESS_MODE_DUAL:
	case SDCA_CONTROL_ACCESS_MODE_RW1S:
	case SDCA_CONTROL_ACCESS_MODE_RW1C:
		return true;
	default:
		return false;
	}
}
EXPORT_SYMBOL_NS(sdca_disco_regmap_readable, SND_SOC_SDCA);

bool sdca_disco_regmap_writeable(struct device *dev, unsigned int reg)
{
	struct sdca_control *control;

	if (!sdca_valid_address(reg))
		return false;

	control = sdca_device_find_control(dev, reg);
	if (!control)
		return false;

	switch(control->mode) {
	case SDCA_CONTROL_ACCESS_MODE_RW:
	case SDCA_CONTROL_ACCESS_MODE_DUAL:
	case SDCA_CONTROL_ACCESS_MODE_RW1S:
	case SDCA_CONTROL_ACCESS_MODE_RW1C:
		return true;
	default:
		return false;
	}
}
EXPORT_SYMBOL_NS(sdca_disco_regmap_writeable, SND_SOC_SDCA);

bool sdca_disco_regmap_volatile(struct device *dev, unsigned int reg)
{
	struct sdca_control *control;

	if (!sdca_valid_address(reg))
		return false;

	control = sdca_device_find_control(dev, reg);
	if (!control)
		return false;

	switch(control->mode) {
	case SDCA_CONTROL_ACCESS_MODE_RO:
	case SDCA_CONTROL_ACCESS_MODE_RW1S:
	case SDCA_CONTROL_ACCESS_MODE_RW1C:
		return true;
	default:
		return false;
	}
}
EXPORT_SYMBOL_NS(sdca_disco_regmap_volatile, SND_SOC_SDCA);

bool sdca_disco_regmap_deferrable(struct device *dev, unsigned int reg)
{
	struct sdca_control *control;

	if (!sdca_valid_address(reg))
		return false;

	control = sdca_device_find_control(dev, reg);
	if (!control)
		return false;

	return control->deferrable;
}
EXPORT_SYMBOL_NS(sdca_disco_regmap_deferrable, SND_SOC_SDCA);

#define CTLTYPE(ent, sel) (SDCA_ENTITY_TYPE_##ent << 8 | SDCA_CONTROL_##ent##_##sel)

int sdca_disco_regmap_mbq_size(struct device *dev, unsigned int reg)
{
	struct sdw_slave *sdw = dev_to_sdw_dev(dev);
	struct sdca_device_data *sdca_data = &sdw->sdca_data;
	struct sdca_function_data *function;
	struct sdca_entity *entity;
	unsigned int ctl;

	if (!sdca_valid_address(reg))
		return -EINVAL;

	function = sdca_data_find_function(sdca_data, reg);
	if (!function)
		return -EINVAL;

	entity = sdca_function_find_entity(function, reg);
	if (!entity)
		return -EINVAL;

	ctl = entity->entity_type << 8 | SDW_SDCA_CTL_CSEL(reg);

	switch (ctl) {
	case CTLTYPE(FU, LATENCY):
		return 4;
	case CTLTYPE(FU, CHANNEL_VOLUME):
		return 2;
	default:
		return 1;
	}
}
EXPORT_SYMBOL_NS(sdca_disco_regmap_mbq_size, SND_SOC_SDCA);
