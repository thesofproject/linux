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


static int find_sdca_function(struct acpi_device *adev, void *data)
{
	struct fwnode_handle *function_node = acpi_fwnode_handle(adev);
	struct device *dev = &adev->dev;
	struct fwnode_handle *control5; /* used to identify topology type */
	unsigned long *sdca_function_mask = data;
	u32 function_topology;
	u64 addr;
	int ret;

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

	ret = fwnode_property_read_u32(control5, "mipi-sdca-control-dc-value",
				       &function_topology);

	fwnode_handle_put(control5);

	if (ret < 0) {
		dev_err(dev, "%s: the function topology can only be determined from ACPI information\n",
			__func__);
		return ret;
	}

	*sdca_function_mask |= BIT(function_topology);

	return 0;
}

void sdca_lookup_function_mask(struct sdw_slave *slave)
{
	struct device *dev = &slave->dev;
	struct acpi_device *adev = to_acpi_device_node(dev->fwnode);
	unsigned long function_mask_patch = 0;
	unsigned long function_mask_found;
	unsigned long function_mask;
	u32 function_topology;

	acpi_dev_for_each_child(adev, find_sdca_function, &function_mask_found);

	/*
	 * Unfortunately early SDCA specifications used different indices for Functions,
	 * for backwards compatibility we have to reorder the values found
	 */
	if (slave->sdca_data.interface_revision >= 0x0801)
		goto skip_early_draft_order;

	for_each_set_bit(function_topology, &function_mask_found, 32) {
		switch (function_topology) {
		case 1:	function_mask_patch |= BIT(SDCA_FUNCTION_TYPE_SMART_AMP);
			break;
		case 2: function_mask_patch |= BIT(SDCA_FUNCTION_TYPE_SMART_MIC);
			break;
		case 3: function_mask_patch |= BIT(SDCA_FUNCTION_TYPE_SPEAKER_MIC);
			break;
		case 4: function_mask_patch |= BIT(SDCA_FUNCTION_TYPE_UAJ);
			break;
		case 5: function_mask_patch |= BIT(SDCA_FUNCTION_TYPE_RJ);
			break;
		case 6: function_mask_patch |= BIT(SDCA_FUNCTION_TYPE_HID);
			break;
		default:
			dev_warn(dev,
				 "%s: SDCA version %#x unsupported function type %d, skipped\n",
				 __func__,
				 slave->sdca_data.interface_revision,
				 function_topology);
			break;
		}
	};

skip_early_draft_order:
	if (function_mask_patch)
		function_mask = function_mask_patch;
	else
		function_mask = function_mask_found;

	/* now double-check the values */
	for_each_set_bit(function_topology, &function_mask, 32) {
		const char *function_name;

		switch (function_topology) {
		case SDCA_FUNCTION_TYPE_SMART_AMP:
			function_name = "SmartAmp";
			break;
		case SDCA_FUNCTION_TYPE_SMART_MIC:
			function_name = "SmartMic";
			break;
		case SDCA_FUNCTION_TYPE_UAJ:
			function_name = "UAJ";
			break;
		case SDCA_FUNCTION_TYPE_HID:
			function_name = "HID";
			break;
		case SDCA_FUNCTION_TYPE_SIMPLE_AMP:
		case SDCA_FUNCTION_TYPE_SIMPLE_MIC:
		case SDCA_FUNCTION_TYPE_SPEAKER_MIC:
		case SDCA_FUNCTION_TYPE_RJ:
		case SDCA_FUNCTION_TYPE_IMP_DEF:
			dev_warn(dev, "%s: found unsupported SDCA function type %d, skipped\n",
				 __func__, function_topology);
			continue;
		default:
			dev_err(dev, "%s: found invalid SDCA function type %d, skipped\n",
				__func__, function_topology);
			continue;
		}
		dev_info(dev, "%s: found SDCA function %s (type %d)\n",
			 __func__, function_name, function_topology);
		slave->sdca_data.function_mask |= BIT(function_topology);
	}
}
EXPORT_SYMBOL_NS(sdca_lookup_function_mask, SND_SOC_SDCA);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("SDCA library");
