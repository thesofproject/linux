// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
// Copyright(c) 2015-17 Intel Corporation.

#include <linux/acpi.h>
#include <linux/dmi.h>
#include <linux/of.h>
#include <linux/soundwire/sdw.h>
#include <linux/soundwire/sdw_type.h>
#include "bus.h"
#include "sysfs_local.h"

static void sdw_slave_release(struct device *dev)
{
	struct sdw_slave *slave = dev_to_sdw_dev(dev);

	kfree(slave);
}

struct device_type sdw_slave_type = {
	.name =		"sdw_slave",
	.release =	sdw_slave_release,
	.uevent =	sdw_slave_uevent,
};

int sdw_slave_add(struct sdw_bus *bus,
		  struct sdw_slave_id *id, struct fwnode_handle *fwnode)
{
	struct sdw_slave *slave;
	int ret;
	int i;

	slave = kzalloc(sizeof(*slave), GFP_KERNEL);
	if (!slave)
		return -ENOMEM;

	/* Initialize data structure */
	memcpy(&slave->id, id, sizeof(*id));
	slave->dev.parent = bus->dev;
	slave->dev.fwnode = fwnode;

	if (id->unique_id == SDW_IGNORED_UNIQUE_ID) {
		/* name shall be sdw:link:mfg:part:class */
		dev_set_name(&slave->dev, "sdw:%x:%x:%x:%x",
			     bus->link_id, id->mfg_id, id->part_id,
			     id->class_id);
	} else {
		/* name shall be sdw:link:mfg:part:class:unique */
		dev_set_name(&slave->dev, "sdw:%x:%x:%x:%x:%x",
			     bus->link_id, id->mfg_id, id->part_id,
			     id->class_id, id->unique_id);
	}

	slave->dev.bus = &sdw_bus_type;
	slave->dev.of_node = of_node_get(to_of_node(fwnode));
	slave->dev.type = &sdw_slave_type;
	slave->dev.groups = sdw_slave_status_attr_groups;
	slave->bus = bus;
	slave->status = SDW_SLAVE_UNATTACHED;
	init_completion(&slave->enumeration_complete);
	init_completion(&slave->initialization_complete);
	slave->dev_num = 0;
	init_completion(&slave->probe_complete);
	slave->probed = false;
	slave->first_interrupt_done = false;

	for (i = 0; i < SDW_MAX_PORTS; i++)
		init_completion(&slave->port_ready[i]);

	mutex_lock(&bus->bus_lock);
	list_add_tail(&slave->node, &bus->slaves);
	mutex_unlock(&bus->bus_lock);

	ret = device_register(&slave->dev);
	if (ret) {
		dev_err(bus->dev, "Failed to add slave: ret %d\n", ret);

		/*
		 * On err, don't free but drop ref as this will be freed
		 * when release method is invoked.
		 */
		mutex_lock(&bus->bus_lock);
		list_del(&slave->node);
		mutex_unlock(&bus->bus_lock);
		put_device(&slave->dev);

		return ret;
	}
	sdw_slave_debugfs_init(slave);

	return ret;
}

#if IS_ENABLED(CONFIG_ACPI)

struct adr_remap {
	u64 adr;
	u64 remapped_adr;
};

/*
 * HP Spectre 360 Convertible devices do not expose the correct _ADR
 * in the DSDT.
 * Remap the bad _ADR values to the ones reported by hardware
 */
static const struct adr_remap hp_spectre_360[] = {
	{
		0x000010025D070100,
		0x000020025D071100
	},
	{
		0x000110025d070100,
		0x000120025D130800
	},
	{}
};

/*
 * The initial version of the Dell SKU 0A3E did not expose the devices on the correct links.
 */
static const struct adr_remap dell_sku_0A3E[] = {
	/* rt715 on link0 */
	{
		0x00020025d071100,
		0x00021025d071500
	},
	/* rt711 on link1 */
	{
		0x000120025d130800,
		0x000120025d071100,
	},
	/* rt1308 on link2 */
	{
		0x000220025d071500,
		0x000220025d130800
	},
	{}
};

static const struct dmi_system_id adr_remap_quirk_table[] = {
	{
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "HP"),
			DMI_MATCH(DMI_PRODUCT_NAME, "HP Spectre x360 Convertible"),
		},
		.driver_data = (void *)hp_spectre_360,
	},
	{
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc"),
			DMI_EXACT_MATCH(DMI_PRODUCT_SKU, "0A3E")
		},
		.driver_data = (void *)dell_sku_0A3E,
	},
	{}
};

static bool find_slave(struct sdw_bus *bus,
		       struct acpi_device *adev,
		       struct sdw_slave_id *id)
{
	const struct dmi_system_id *dmi_id;
	unsigned long long addr;
	unsigned int link_id;
	acpi_status status;

	status = acpi_evaluate_integer(adev->handle,
				       METHOD_NAME__ADR, NULL, &addr);

	if (ACPI_FAILURE(status)) {
		dev_err(bus->dev, "_ADR resolution failed: %x\n",
			status);
		return false;
	}

	/* check if any address remap quirk applies */
	dmi_id = dmi_first_match(adr_remap_quirk_table);
	if (dmi_id) {
		struct adr_remap *map = dmi_id->driver_data;

		for (map = dmi_id->driver_data; map->adr; map++) {
			if (map->adr == addr) {
				dev_dbg(bus->dev, "remapped _ADR 0x%llx as 0x%llx\n",
					addr, map->remapped_adr);
				addr = map->remapped_adr;
				break;
			}
		}
	}

	/* Extract link id from ADR, Bit 51 to 48 (included) */
	link_id = SDW_DISCO_LINK_ID(addr);

	/* Check for link_id match */
	if (link_id != bus->link_id)
		return false;

	sdw_extract_slave_id(bus, addr, id);

	return true;
}

/*
 * sdw_acpi_find_slaves() - Find Slave devices in Master ACPI node
 * @bus: SDW bus instance
 *
 * Scans Master ACPI node for SDW child Slave devices and registers it.
 */
int sdw_acpi_find_slaves(struct sdw_bus *bus)
{
	struct acpi_device *adev, *parent;
	struct acpi_device *adev2, *parent2;

	parent = ACPI_COMPANION(bus->dev);
	if (!parent) {
		dev_err(bus->dev, "Can't find parent for acpi bind\n");
		return -ENODEV;
	}

	list_for_each_entry(adev, &parent->children, node) {
		struct sdw_slave_id id;
		struct sdw_slave_id id2;
		bool ignore_unique_id = true;

		if (!find_slave(bus, adev, &id))
			continue;

		/* brute-force O(N^2) search for duplicates */
		parent2 = parent;
		list_for_each_entry(adev2, &parent2->children, node) {

			if (adev == adev2)
				continue;

			if (!find_slave(bus, adev2, &id2))
				continue;

			if (id.sdw_version != id2.sdw_version ||
			    id.mfg_id != id2.mfg_id ||
			    id.part_id != id2.part_id ||
			    id.class_id != id2.class_id)
				continue;

			if (id.unique_id != id2.unique_id) {
				dev_dbg(bus->dev,
					"Valid unique IDs 0x%x 0x%x for Slave mfg_id 0x%04x, part_id 0x%04x\n",
					id.unique_id, id2.unique_id, id.mfg_id, id.part_id);
				ignore_unique_id = false;
			} else {
				dev_err(bus->dev,
					"Invalid unique IDs 0x%x 0x%x for Slave mfg_id 0x%04x, part_id 0x%04x\n",
					id.unique_id, id2.unique_id, id.mfg_id, id.part_id);
				return -ENODEV;
			}
		}

		if (ignore_unique_id)
			id.unique_id = SDW_IGNORED_UNIQUE_ID;

		/*
		 * don't error check for sdw_slave_add as we want to continue
		 * adding Slaves
		 */
		sdw_slave_add(bus, &id, acpi_fwnode_handle(adev));
	}

	return 0;
}

#endif

/*
 * sdw_of_find_slaves() - Find Slave devices in master device tree node
 * @bus: SDW bus instance
 *
 * Scans Master DT node for SDW child Slave devices and registers it.
 */
int sdw_of_find_slaves(struct sdw_bus *bus)
{
	struct device *dev = bus->dev;
	struct device_node *node;

	for_each_child_of_node(bus->dev->of_node, node) {
		int link_id, ret, len;
		unsigned int sdw_version;
		const char *compat = NULL;
		struct sdw_slave_id id;
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

		sdw_slave_add(bus, &id, of_fwnode_handle(node));
	}

	return 0;
}
