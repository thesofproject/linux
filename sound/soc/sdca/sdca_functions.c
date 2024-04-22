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

int sdca_alloc(void **context,
	       struct device *parent,
	       struct sdw_slave *slave)
{
	struct sdca_data *sdca;

	sdca = devm_kzalloc(parent, sizeof(*sdca), GFP_KERNEL);
	if (!sdca)
		return -ENOMEM;

	sdca->parent = parent;
	sdca->slave = slave;

	*context = sdca;

	return 0;
}
EXPORT_SYMBOL_NS(sdca_alloc, SND_SOC_SDCA);

static int find_sdca_entities(struct device *dev,
			      struct fwnode_handle *function_node,
			      struct sdca_function *function)
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

static struct sdca_entity *find_sdca_entity_by_label(struct sdca_function *function,
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
				       struct sdca_function *function,
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

	for(pin = 1; pin < 64; pin++) {
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

		ret = fwnode_property_read_string(connected_node, "mipi-sdca-entity-label", &connected_label);
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
					  struct sdca_function *function)
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
						   struct sdca_function *function)
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

	dev_info(dev, "%s: %pfwP: initialization table size %#x\n",
		 __func__, function_node, nval);

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

	function->initialization_table = initialization_table;
	function->initialization_table_size = nval;

	return 0;
}

static int find_sdca_function(struct acpi_device *adev, void *data)
{
	struct fwnode_handle *function_node = acpi_fwnode_handle(adev);
	struct sdca_data *sdca = data;
	struct device *parent = sdca->parent;
	struct fwnode_handle *control5; /* used to identify topology type */
	bool valid_function_found = false;
	u32 function_topology = 0;
	u64 topology_features = 0;
	u32 max_busy_delay = 0;
	acpi_status status;
	u64 addr;
	int ret;

	status = acpi_evaluate_integer(adev->handle,
				       METHOD_NAME__ADR, NULL, &addr);

	if (ACPI_FAILURE(status)) {
		dev_err(parent, "_ADR resolution failed: %x\n", status);
		return -ENODEV;
	}

	if (!addr) {
		dev_err(parent, "%s: no addr\n", __func__);
		return -ENODEV;
	}

	/*
	 * Extracting the topology type for an SDCA function is a
	 * convoluted process.
	 * The topology type is only visible as a result of a read
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

	fwnode_property_read_u32(control5, "mipi-sdca-control-dc-value",
				 &function_topology);

	switch (function_topology) {
	case SDCA_FUNCTION_TYPE_SMART_AMP:
	case SDCA_FUNCTION_TYPE_SMART_MIC:
	case SDCA_FUNCTION_TYPE_UAJ:
	case SDCA_FUNCTION_TYPE_HID:
		dev_info(parent, "found SDCA function %pfwP type %d ADR %lld\n",
			 function_node, function_topology, addr);
		sdca->functions[sdca->function_count].topology_type = function_topology;
		sdca->functions[sdca->function_count].adr = addr;
		valid_function_found = true;
		break;
	case SDCA_FUNCTION_TYPE_SPEAKER_MIC:
	case SDCA_FUNCTION_TYPE_RJ:
	case SDCA_FUNCTION_TYPE_IMP_DEF:
		dev_warn(parent, "found unsupported SDCA function %pfwP type %d ADR %lld, skipped\n",
			 function_node, function_topology, addr);
		break;
	default:
		dev_err(parent, "found invalid SDCA function %pfwP type %d ADR %lld, skipped\n",
			function_node, function_topology, addr);
		break;
	}

	fwnode_handle_put(control5);

	if (!valid_function_found)
		return 0;

	fwnode_property_read_u32(function_node, "mipi-sdca-function-busy-max-delay",
				 &max_busy_delay);
	sdca->functions[sdca->function_count].function_busy_max_delay_us =
		max_busy_delay;
	if (max_busy_delay > 0) {
		dev_err(parent,
			"%pfwP: unsupported non-zero delay %d us for Function_Busy\n",
			function_node, max_busy_delay);
		dev_err(parent,
			"please report this on https://github.com/thesofproject/linux/issues\n");
	}

	fwnode_property_read_u64(function_node, "mipi-sdca-function-topology-features",
				 &topology_features);
	if (topology_features) {
		dev_info(parent, "%pfwP: SDCA function has topology-features mask %#llx\n",
			 function_node,
			 topology_features);
		sdca->functions[sdca->function_count].topology_features =
			topology_features;
	}

	ret = find_sdca_function_initialization_table(parent, function_node,
						      &sdca->functions[sdca->function_count]);
	if (ret < 0)
		return ret;

	ret = find_sdca_entities(&adev->dev,
				 function_node,
				 &sdca->functions[sdca->function_count]);
	if (ret < 0)
		return ret;

	ret = find_sdca_entities_connections(&adev->dev,
					     function_node,
					     &sdca->functions[sdca->function_count]);
	if (ret < 0)
		return ret;

	sdca->function_count++;

	return 0;
}

int sdca_find_functions(struct sdca_data *sdca)
{
	struct acpi_device *adev = to_acpi_device_node(sdca->parent->fwnode);

	return acpi_dev_for_each_child(adev, find_sdca_function, sdca);
}
EXPORT_SYMBOL_NS(sdca_find_functions, SND_SOC_SDCA);

MODULE_LICENSE("Dual BSD/GPL");
