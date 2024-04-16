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

static int find_sdca_function(struct acpi_device *adev, void *data)
{
	struct fwnode_handle *function_node = acpi_fwnode_handle(adev);
	struct sdca_data *sdca = data;
	struct device *parent = sdca->parent;
	struct fwnode_handle *control5; /* used to identify topology type */
	bool valid_function_found = false;
	u32 function_topology = 0;
	acpi_status status;
	u64 addr;

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

	if (valid_function_found)
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
