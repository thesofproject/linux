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

static int find_sdca_function(struct acpi_device *adev, void *data)
{
	struct fwnode_handle *function_node = acpi_fwnode_handle(adev);
	struct sdca_data *sdca = data;
	struct device *parent = sdca->parent;
	struct fwnode_handle *control5; /* used to identify topology type */
	bool valid_function_found = false;
	u32 function_topology = 0;
	u64 topology_features = 0;
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

	fwnode_property_read_u64(function_node, "mipi-sdca-function-topology-features",
				 &topology_features);
	if (topology_features) {
		dev_info(parent, "%pfwP: SDCA function has topology-features mask %#llx\n",
			 function_node,
			 topology_features);
		sdca->functions[sdca->function_count].topology_features =
			topology_features;
	}

	ret = find_sdca_entities(&adev->dev,
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
