// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
// Copyright(c) 2015-17 Intel Corporation.

#include <linux/acpi.h>
#include <linux/of.h>
#include <linux/soundwire/sdw.h>
#include <linux/soundwire/sdw_type.h>
#include "bus.h"
#include "sysfs_local.h"

static void sdw_peripheral_release(struct device *dev)
{
	struct sdw_peripheral *peripheral = dev_to_sdw_dev(dev);

	kfree(peripheral);
}

struct device_type sdw_peripheral_type = {
	.name =		"sdw_peripheral",
	.release =	sdw_peripheral_release,
	.uevent =	sdw_peripheral_uevent,
};

int sdw_peripheral_add(struct sdw_bus *bus,
		       struct sdw_peripheral_id *id, struct fwnode_handle *fwnode)
{
	struct sdw_peripheral *peripheral;
	int ret;
	int i;

	peripheral = kzalloc(sizeof(*peripheral), GFP_KERNEL);
	if (!peripheral)
		return -ENOMEM;

	/* Initialize data structure */
	memcpy(&peripheral->id, id, sizeof(*id));
	peripheral->dev.parent = bus->dev;
	peripheral->dev.fwnode = fwnode;

	if (id->unique_id == SDW_IGNORED_UNIQUE_ID) {
		/* name shall be sdw:link:mfg:part:class */
		dev_set_name(&peripheral->dev, "sdw:%x:%x:%x:%x",
			     bus->link_id, id->mfg_id, id->part_id,
			     id->class_id);
	} else {
		/* name shall be sdw:link:mfg:part:class:unique */
		dev_set_name(&peripheral->dev, "sdw:%x:%x:%x:%x:%x",
			     bus->link_id, id->mfg_id, id->part_id,
			     id->class_id, id->unique_id);
	}

	peripheral->dev.bus = &sdw_bus_type;
	peripheral->dev.of_node = of_node_get(to_of_node(fwnode));
	peripheral->dev.type = &sdw_peripheral_type;
	peripheral->dev.groups = sdw_peripheral_status_attr_groups;
	peripheral->bus = bus;
	peripheral->status = SDW_PERIPHERAL_UNATTACHED;
	init_completion(&peripheral->enumeration_complete);
	init_completion(&peripheral->initialization_complete);
	peripheral->dev_num = 0;
	init_completion(&peripheral->probe_complete);
	peripheral->probed = false;
	peripheral->first_interrupt_done = false;

	for (i = 0; i < SDW_MAX_PORTS; i++)
		init_completion(&peripheral->port_ready[i]);

	ret = device_register(&peripheral->dev);
	if (ret) {
		dev_err(bus->dev, "Failed to add peripheral: ret %d\n", ret);

		/*
		 * On err, don't free but drop ref as this will be freed
		 * when release method is invoked.
		 */
		put_device(&peripheral->dev);

		return ret;
	}

	mutex_lock(&bus->bus_lock);
	list_add_tail(&peripheral->node, &bus->peripherals);
	mutex_unlock(&bus->bus_lock);

	sdw_peripheral_debugfs_init(peripheral);

	return ret;
}
EXPORT_SYMBOL(sdw_peripheral_add);

#if IS_ENABLED(CONFIG_ACPI)

static bool find_peripheral(struct sdw_bus *bus,
			    struct acpi_device *adev,
			    struct sdw_peripheral_id *id)
{
	u64 addr;
	unsigned int link_id;
	acpi_status status;

	status = acpi_evaluate_integer(adev->handle,
				       METHOD_NAME__ADR, NULL, &addr);

	if (ACPI_FAILURE(status)) {
		dev_err(bus->dev, "_ADR resolution failed: %x\n",
			status);
		return false;
	}

	if (bus->ops->override_adr)
		addr = bus->ops->override_adr(bus, addr);

	if (!addr)
		return false;

	/* Extract link id from ADR, Bit 51 to 48 (included) */
	link_id = SDW_DISCO_LINK_ID(addr);

	/* Check for link_id match */
	if (link_id != bus->link_id)
		return false;

	sdw_extract_peripheral_id(bus, addr, id);

	return true;
}

/*
 * sdw_acpi_find_peripherals() - Find Peripheral devices in Manager ACPI node
 * @bus: SDW bus instance
 *
 * Scans Manager ACPI node for SDW child Peripheral devices and registers it.
 */
int sdw_acpi_find_peripherals(struct sdw_bus *bus)
{
	struct acpi_device *adev, *parent;
	struct acpi_device *adev2, *parent2;

	parent = ACPI_COMPANION(bus->dev);
	if (!parent) {
		dev_err(bus->dev, "Can't find parent for acpi bind\n");
		return -ENODEV;
	}

	list_for_each_entry(adev, &parent->children, node) {
		struct sdw_peripheral_id id;
		struct sdw_peripheral_id id2;
		bool ignore_unique_id = true;

		if (!find_peripheral(bus, adev, &id))
			continue;

		/* brute-force O(N^2) search for duplicates */
		parent2 = parent;
		list_for_each_entry(adev2, &parent2->children, node) {

			if (adev == adev2)
				continue;

			if (!find_peripheral(bus, adev2, &id2))
				continue;

			if (id.sdw_version != id2.sdw_version ||
			    id.mfg_id != id2.mfg_id ||
			    id.part_id != id2.part_id ||
			    id.class_id != id2.class_id)
				continue;

			if (id.unique_id != id2.unique_id) {
				dev_dbg(bus->dev,
					"Valid unique IDs 0x%x 0x%x for Peripheral mfg_id 0x%04x, part_id 0x%04x\n",
					id.unique_id, id2.unique_id, id.mfg_id, id.part_id);
				ignore_unique_id = false;
			} else {
				dev_err(bus->dev,
					"Invalid unique IDs 0x%x 0x%x for Peripheral mfg_id 0x%04x, part_id 0x%04x\n",
					id.unique_id, id2.unique_id, id.mfg_id, id.part_id);
				return -ENODEV;
			}
		}

		if (ignore_unique_id)
			id.unique_id = SDW_IGNORED_UNIQUE_ID;

		/*
		 * don't error check for sdw_peripheral_add as we want to continue
		 * adding Peripherals
		 */
		sdw_peripheral_add(bus, &id, acpi_fwnode_handle(adev));
	}

	return 0;
}

#endif

/*
 * sdw_of_find_peripherals() - Find Peripheral devices in manager device tree node
 * @bus: SDW bus instance
 *
 * Scans Manager DT node for SDW child Peripheral devices and registers it.
 */
int sdw_of_find_peripherals(struct sdw_bus *bus)
{
	struct device *dev = bus->dev;
	struct device_node *node;

	for_each_child_of_node(bus->dev->of_node, node) {
		int link_id, ret, len;
		unsigned int sdw_version;
		const char *compat = NULL;
		struct sdw_peripheral_id id;
		const __be32 *addr;

		compat = of_get_property(node, "compatible", NULL);
		if (!compat)
			continue;

		ret = sscanf(compat, "sdw%01x%04hx%04hx%02hhx", &sdw_version,
			     &id.mfg_id, &id.part_id, &id.class_id);

		if (ret != 4) {
			dev_err(dev, "Invalid compatible string found %s\n",
				compat);
			continue;
		}

		addr = of_get_property(node, "reg", &len);
		if (!addr || (len < 2 * sizeof(u32))) {
			dev_err(dev, "Invalid Link and Instance ID\n");
			continue;
		}

		link_id = be32_to_cpup(addr++);
		id.unique_id = be32_to_cpup(addr);
		id.sdw_version = sdw_version;

		/* Check for link_id match */
		if (link_id != bus->link_id)
			continue;

		sdw_peripheral_add(bus, &id, of_fwnode_handle(node));
	}

	return 0;
}
