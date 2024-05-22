// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
// Copyright(c) 2024 Intel Corporation

/*
 * The MIPI SDCA specification is available for public downloads at
 * https://www.mipi.org/mipi-sdca-v1-0-download
 */

#include <linux/acpi.h>
#include <linux/soundwire/sdw.h>
#include <sound/sdca_function.h>
#include <sound/sdca.h>

static int patch_sdca_function_type(struct device *dev,
				    u32 interface_revision,
				    u32 *function_type,
				    const char **function_name)
{
	unsigned long function_type_patch = 0;

	/*
	 * Unfortunately early SDCA specifications used different indices for Functions,
	 * for backwards compatibility we have to reorder the values found
	 */
	if (interface_revision >= 0x0801)
		goto skip_early_draft_order;

	switch (*function_type) {
	case 1:
		function_type_patch = SDCA_FUNCTION_TYPE_SMART_AMP;
		break;
	case 2:
		function_type_patch = SDCA_FUNCTION_TYPE_SMART_MIC;
		break;
	case 3:
		function_type_patch = SDCA_FUNCTION_TYPE_SPEAKER_MIC;
		break;
	case 4:
		function_type_patch = SDCA_FUNCTION_TYPE_UAJ;
		break;
	case 5:
		function_type_patch = SDCA_FUNCTION_TYPE_RJ;
		break;
	case 6:
		function_type_patch = SDCA_FUNCTION_TYPE_HID;
		break;
	default:
		dev_warn(dev, "%s: SDCA version %#x unsupported function type %d, skipped\n",
			 __func__, interface_revision, *function_type);
		return -EINVAL;
	}

skip_early_draft_order:
	if (function_type_patch)
		*function_type = function_type_patch;

	/* now double-check the values */
	switch (*function_type) {
	case SDCA_FUNCTION_TYPE_SMART_AMP:
		*function_name = SDCA_FUNCTION_TYPE_SMART_AMP_NAME;
		break;
	case SDCA_FUNCTION_TYPE_SMART_MIC:
		*function_name = SDCA_FUNCTION_TYPE_SMART_MIC_NAME;
		break;
	case SDCA_FUNCTION_TYPE_UAJ:
		*function_name = SDCA_FUNCTION_TYPE_UAJ_NAME;
		break;
	case SDCA_FUNCTION_TYPE_HID:
		*function_name = SDCA_FUNCTION_TYPE_HID_NAME;
		break;
	case SDCA_FUNCTION_TYPE_SIMPLE_AMP:
	case SDCA_FUNCTION_TYPE_SIMPLE_MIC:
	case SDCA_FUNCTION_TYPE_SPEAKER_MIC:
	case SDCA_FUNCTION_TYPE_RJ:
	case SDCA_FUNCTION_TYPE_IMP_DEF:
		dev_warn(dev, "%s: found unsupported SDCA function type %d, skipped\n",
			 __func__, *function_type);
		return -EINVAL;
	default:
		dev_err(dev, "%s: found invalid SDCA function type %d, skipped\n",
			__func__, *function_type);
		return -EINVAL;
	}

	dev_info(dev, "%s: found SDCA function %s (type %d)\n",
		 __func__, *function_name, *function_type);

	return 0;
}

static int find_sdca_function(struct acpi_device *adev, void *data)
{
	struct fwnode_handle *function_node = acpi_fwnode_handle(adev);
	struct sdca_device_data *sdca_data = data;
	struct device *dev = &adev->dev;
	struct fwnode_handle *control5; /* used to identify function type */
	const char *function_name;
	u32 function_type;
	int func_index;
	u64 addr;
	int ret;

	if (sdca_data->num_functions >= SDCA_MAX_FUNCTION_COUNT) {
		dev_err(dev, "%s: maximum number of functions exceeded\n", __func__);
		return -EINVAL;
	}

	/*
	 * The number of functions cannot exceed 8, we could use
	 * acpi_get_local_address() but the value is stored as u64 so
	 * we might as well avoid casts and intermediate levels
	 */
	ret = acpi_get_local_u64_address(adev->handle, &addr);
	if (ret < 0)
		return ret;

	if (!addr) {
		dev_err(dev, "%s: no addr\n", __func__);
		return -ENODEV;
	}

	/*
	 * Extracting the topology type for an SDCA function is a
	 * convoluted process.
	 * The Function type is only visible as a result of a read
	 * from a control. In theory this would mean reading from the hardware,
	 * but the SDCA/DisCo specs defined the notion of "DC value" - a constant
	 * represented with a DSD subproperty.
	 * Drivers have to query the properties for the control
	 * SDCA_CONTROL_ENTITY_0_FUNCTION_TOPOLOGY (0x05)
	 */
	control5 = fwnode_get_named_child_node(function_node,
					       "mipi-sdca-control-0x5-subproperties");
	if (!control5)
		return -ENODEV;

	ret = fwnode_property_read_u32(control5, "mipi-sdca-control-dc-value",
				       &function_type);

	fwnode_handle_put(control5);

	if (ret < 0) {
		dev_err(dev, "%s: the function type can only be determined from ACPI information\n",
			__func__);
		return ret;
	}

	ret = patch_sdca_function_type(dev, sdca_data->interface_revision,
				       &function_type, &function_name);
	if (ret < 0)
		return ret;

	/* store results */
	func_index = sdca_data->num_functions;
	sdca_data->sdca_func[func_index].adr = addr;
	sdca_data->sdca_func[func_index].type = function_type;
	sdca_data->sdca_func[func_index].name = function_name;
	sdca_data->sdca_func[func_index].function_node = function_node;
	sdca_data->num_functions++;

	return 0;
}

void sdca_lookup_functions(struct sdw_slave *slave)
{
	struct device *dev = &slave->dev;
	struct acpi_device *adev = to_acpi_device_node(dev->fwnode);

	acpi_dev_for_each_child(adev, find_sdca_function, &slave->sdca_data);
}
EXPORT_SYMBOL_NS(sdca_lookup_functions, SND_SOC_SDCA);

static int find_sdca_entities(struct device *dev,
			      struct fwnode_handle *function_node,
			      struct sdca_function_data *function)
{
	struct fwnode_handle *entity_node;
	struct sdca_entity *entities;
	char entity_property[40];
	int num_entities;
	u32 *entity_list;
	u32 entity_type;
	int ret;
	int i;

	num_entities = fwnode_property_count_u32(function_node,
						 "mipi-sdca-entity-id-list");
	if (num_entities <= 0) {
		dev_err(dev, "%s: %s property access failed %d\n",
			__func__, "mipi-sdca-entity-id-list",
			num_entities);
		return -EINVAL;
	}
	if (num_entities >  SDCA_MAX_ENTITY_COUNT) {
		dev_err(dev, "%s: invalid entity count %d, max allowed %d\n",
			__func__, num_entities, SDCA_MAX_ENTITY_COUNT);
		return -EINVAL;
	}

	entities = devm_kcalloc(dev, num_entities, sizeof(*entities), GFP_KERNEL);
	if (!entities)
		return -ENOMEM;

	entity_list = kcalloc(num_entities, sizeof(u32), GFP_KERNEL);
	if (!entity_list)
		return -ENOMEM;

	fwnode_property_read_u32_array(function_node,
				       "mipi-sdca-entity-id-list",
				       entity_list,
				       num_entities);

	for (i = 0; i < num_entities; i++)
		entities[i].id = entity_list[i];

	kfree(entity_list);

	/* now read subproperties */
	for (i = 0; i < num_entities; i++) {
		const char *label;

		/* DisCo uses upper-case for hex numbers */
		snprintf(entity_property, sizeof(entity_property),
			 "mipi-sdca-entity-id-0x%X-subproperties",
			 entities[i].id);

		entity_node = fwnode_get_named_child_node(function_node,
							  entity_property);
		if (!entity_node) {
			dev_err(dev, "%s: %pfwP: property %s not found\n",
				__func__, function_node, entity_property);
			return -EINVAL;
		}

		fwnode_property_read_u32(entity_node, "mipi-sdca-entity-type",
					 &entity_type);
		entities[i].entity_type = entity_type;

		ret = fwnode_property_read_string(entity_node, "mipi-sdca-entity-label", &label);
		if (ret) {
			/* Not all entities have labels, log and ignore */
			dev_dbg(dev, "%pfwP: entity %#x property %s not found\n",
				function, entities[i].id,
				"mipi-sdca-entity-label");
		} else {
			entities[i].label = devm_kasprintf(dev, GFP_KERNEL, "%s", label);
			if (!entities[i].label) {
				fwnode_handle_put(entity_node);
				return -ENOMEM;
			}
		}

		fwnode_handle_put(entity_node);

		dev_info(dev, "%s: %pfwP: found entity %#x type %#x label %s\n",
			 __func__, function_node,
			 entities[i].id,
			 entities[i].entity_type,
			 entities[i].label);
	}

	function->num_entities = num_entities;
	function->entities = entities;

	return 0;
}

static struct sdca_entity *find_sdca_entity_by_label(struct sdca_function_data *function,
						     const char *label)
{
	int i;

	for (i = 0; i < function->num_entities; i++) {
		struct sdca_entity *entity;

		entity = &function->entities[i];

		if (!strcmp(entity->label, label))
			return entity;
	}

	return NULL;
}

static int find_sdca_entity_connection(struct device *dev,
				       struct fwnode_handle *function_node,
				       struct sdca_function_data *function,
				       struct fwnode_handle *entity_node,
				       struct sdca_entity *entity)
{
	u64 input_pin_list;
	int pin;

	fwnode_property_read_u64(entity_node, "mipi-sdca-input-pin-list",
				 &input_pin_list);

	if (!input_pin_list)
		return 0;

	/*
	 * Each bit set in the input-pin-list refers to an entity_id
	 *  in this Function. Entity0 is an illegal connection since
	 *  it is used for Function-level configurations
	 */
	if (input_pin_list & BIT(0)) {
		dev_err(dev, "%s: %pfwP: entity_id %#x has invalid input_pin 0\n",
			__func__, function_node, entity->id);
		return -EINVAL;
	}

	for (pin = 1; pin < 64; pin++) {
		struct fwnode_handle *connected_node;
		struct sdca_entity *connected_entity;
		const char *connected_label;
		char pin_property[40];
		int ret;

		if (!(input_pin_list & BIT(pin)))
			continue;

		snprintf(pin_property, sizeof(pin_property),
			 "mipi-sdca-input-pin-%d", pin);

		connected_node = fwnode_get_named_child_node(entity_node,
							     pin_property);
		if (!connected_node) {
			dev_err(dev, "%s: %pfwP: entity_id %#x: input pin %s not found\n",
				__func__, function_node, entity->id, pin_property);
			return -EINVAL;
		}

		ret = fwnode_property_read_string(connected_node,
						  "mipi-sdca-entity-label",
						  &connected_label);
		if (ret) {
			dev_err(dev, "%s: %pfwP: entity_id %#x: could not find label for connection %s\n",
				__func__, function_node, entity->id, pin_property);
			goto out;
		}

		connected_entity = find_sdca_entity_by_label(function, connected_label);
		if (!connected_entity) {
			dev_err(dev, "%s: %pfwP: entity_id %#x: could not find entity with label %s\n",
				__func__, function_node, entity->id, connected_label);
			ret = -EINVAL;
			goto out;
		}

		dev_info(dev, "%s: %pfwP: entity_id %#x: input entity_id %#x\n",
			 __func__, function_node, entity->id, connected_entity->id);

		entity->sources[entity->source_count++] = connected_entity->id;
		connected_entity->sinks[connected_entity->sink_count++] = entity->id;

out:
		fwnode_handle_put(connected_node);
		if (ret)
			return ret;
	}

	return 0;
}

static int find_sdca_entities_connections(struct device *dev,
					  struct fwnode_handle *function_node,
					  struct sdca_function_data *function)
{
	int ret;
	int i;

	for (i = 0; i < function->num_entities; i++) {
		struct fwnode_handle *entity_node;
		struct sdca_entity *entity;
		char entity_property[40];

		entity = &function->entities[i];

		/* DisCo uses upper-case for hex numbers */
		snprintf(entity_property, sizeof(entity_property),
			 "mipi-sdca-entity-id-0x%X-subproperties",
			 entity->id);

		entity_node = fwnode_get_named_child_node(function_node,
							  entity_property);
		if (!entity_node) {
			dev_err(dev, "%s: %pfwP: property %s not found\n",
				__func__, function_node, entity_property);
			return -EINVAL;
		}

		ret = find_sdca_entity_connection(dev,
						  function_node,
						  function,
						  entity_node,
						  entity);
		fwnode_handle_put(entity_node);

		if (ret)
			return ret;
	}
	return 0;
}

static int find_sdca_function_initialization_table(struct device *dev,
						   struct fwnode_handle *function_node,
						   u8 **table, int *table_size)
{
	u8 *initialization_table;
	int nval;
	int ret;

	nval = fwnode_property_count_u8(function_node,
					"mipi-sdca-function-initialization-table");
	if (nval <= 0)
		return nval;

	/* make sure the table contains a set of 4-byte addresses and one-byte value */
	if (nval % 5) {
		dev_err(dev, "%s: %pfwP: invalid initialization table size %#x\n",
			__func__, function_node, nval);
		return -EINVAL;
	}

	dev_dbg(dev, "%s: %pfwP: initialization table size %#x\n",
		__func__, function_node, nval);

	/*
	 * depending on the uses of this function, the dev could be
	 * the Slave parent, or alternatively the SDCA device if the
	 * codec driver registered one during its probe.
	 */
	initialization_table = devm_kcalloc(dev, nval,
					    sizeof(*initialization_table),
					    GFP_KERNEL);
	if (!initialization_table)
		return -ENOMEM;

	ret = fwnode_property_read_u8_array(function_node,
					    "mipi-sdca-function-initialization-table",
					    initialization_table,
					    nval);
	if (ret < 0)
		return ret;

	*table = initialization_table;
	*table_size = nval;

	return 0;
}

/**
 * sdca_function_extract_initialization_table - check if initialization table
 * is present for an SDCA function identified by an ADR and type, allocate the
 * required memory and extract the values from ACPI.
 *
 * @slave: parent SoundWire device
 * @adr: ACPI ADR for the Function. This is required to uniquify cases
 * where multiple Functions of the same @type are exposed in ACPI tables.
 * @type: SDCA_FUNCTION_TYPE_XXX
 * @table: devm_ allocated table of size @table_size, using the @slave device.
 * @table_size: bytes allocated and extracted from ACPI tables. Note that
 * this value SHALL be a multiple of 5, since the table include a set of 4-byte
 * addresses and 1-byte values.
 */

int sdca_function_extract_initialization_table(struct sdw_slave *slave,
					       u64 adr, u32 type,
					       u8 **table, int *table_size)
{
	struct sdca_device_data *sdca_data = &slave->sdca_data;
	int i;

	if (!sdca_data->num_functions) {
		dev_err(&slave->dev, "%s: device has no SDCA functions\n",
			__func__);
		return -EINVAL;
	}

	for (i = 0; i < sdca_data->num_functions; i++) {
		if (sdca_data->sdca_func[i].adr == adr &&
		    sdca_data->sdca_func[i].type == type)
			break;
	}

	if (i == sdca_data->num_functions) {
		dev_err(&slave->dev, "%s: No function found for adr %lld type %d\n",
			__func__, adr, type);
		return -EINVAL;
	}

	return find_sdca_function_initialization_table(&slave->dev,
						       sdca_data->sdca_func[i].function_node,
						       table, table_size);
}
EXPORT_SYMBOL_NS(sdca_function_extract_initialization_table, SND_SOC_SDCA);

/**
 * sdca_parse_function - parse SDCA information reported in ACPI in the scope of
 * a Function device
 *
 * @dev: a SDCA device (NOT the parent SoundWire device!)
 * @function_node: firmware node for the Function
 * @function: pointer to SDCA storage structure
 */
int sdca_parse_function(struct device *dev,
			struct fwnode_handle *function_node,
			struct sdca_function_data *function)
{
	int ret;

	ret = find_sdca_entities(dev, function_node, function);
	if (ret < 0) {
		dev_err(dev, "%s: find_sdca_entities failed: %d\n",
			__func__, ret);
		return ret;
	}

	ret = find_sdca_entities_connections(dev, function_node, function);
	if (ret < 0) {
		dev_err(dev, "%s: find_sdca_entities_connections failed: %d\n",
			__func__, ret);
		return ret;
	}

	ret = find_sdca_function_initialization_table(dev, function_node,
						      &function->initialization_table,
						      &function->initialization_table_size);
	if (ret < 0) {
		dev_err(dev, "%s: find_sdca_function_initialization_table failed: %d\n",
			__func__, ret);
		return ret;
	}

	return 0;
}
EXPORT_SYMBOL_NS(sdca_parse_function, SND_SOC_SDCA);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("SDCA library");
