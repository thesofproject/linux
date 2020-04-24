// SPDX-License-Identifier: GPL-2.0-only
// Copyright(c) 2019-2020 Intel Corporation.

#include <linux/device.h>
#include <linux/acpi.h>
#include <linux/soundwire/sdw.h>
#include <linux/soundwire/sdw_type.h>
#include "bus.h"

/* nothing to free but this function is mandatory */
static void sdw_master_device_release(struct device *dev)
{
}

struct device_type sdw_master_type = {
	.name =		"soundwire_master",
	.release =	sdw_master_device_release,
};

/**
 * sdw_master_device_add() - create a Linux Master Device representation.
 * @bus: SDW bus instance
 * @parent: parent device
 * @fwnode: firmware node handle
 */
int sdw_master_device_add(struct sdw_bus *bus, struct device *parent,
			  struct fwnode_handle *fwnode)
{
	struct sdw_master_device *md;
	int ret;

	if (!bus)
		return -EINVAL;

	/*
	 * Unlike traditional devices, there's no allocation here since the
	 * sdw_master_device is embedded in the bus structure.
	 */
	md = &bus->md;
	md->dev.bus = &sdw_bus_type;
	md->dev.type = &sdw_master_type;
	md->dev.parent = parent;
	md->dev.of_node = parent->of_node;
	md->dev.fwnode = fwnode;
	md->dev.dma_mask = parent->dma_mask;

	dev_set_name(&md->dev, "sdw-master-%d", bus->link_id);

	if (bus->link_ops && bus->link_ops->driver) {
		/*
		 * A driver is only needed for ASoC integration (need
		 * driver->name) and for link-specific power management
		 * w/ a pm_dev_ops structure.
		 *
		 * The driver needs to be registered by the parent
		 */
		md->dev.driver = bus->link_ops->driver;
	}

	ret = device_register(&md->dev);
	if (ret) {
		dev_err(parent, "Failed to add master: ret %d\n", ret);
		/*
		 * On err, don't free but drop ref as this will be freed
		 * when release method is invoked.
		 */
		put_device(&md->dev);
		goto device_register_err;
	}

	/* add shortcuts to improve code readability/compactness */
	md->bus = bus;
	bus->dev = &md->dev;

	if (bus->link_ops && bus->link_ops->add) {
		ret = bus->link_ops->add(bus, bus->pdata);
		if (ret < 0) {
			dev_err(&md->dev,
				"link_ops add callback failed: %d\n", ret);
			goto link_add_err;
		}
	}

	return ret;

link_add_err:
	device_unregister(&md->dev);
device_register_err:
	return ret;
}

/**
 * sdw_master_device_del() - delete a Linux Master Device representation.
 * @bus: bus handle
 *
 * This function is the dual of sdw_master_device_add()
 */
int sdw_master_device_del(struct sdw_bus *bus)
{
	int ret;

	if (!bus)
		return -EINVAL;

	if (bus->link_ops && bus->link_ops->del) {
		ret = bus->link_ops->del(bus);
		if (ret < 0)
			dev_err(bus->dev,
				"link_ops del callback failed: %d\n",
				ret);
	}

	device_unregister(bus->dev);

	return 0;
}

/**
 * sdw_bus_master_startup() - startup hardware
 * @bus: bus handle
 */
int sdw_bus_master_startup(struct sdw_bus *bus)
{
	if (!bus)
		return -EINVAL;

	if (bus->link_ops && bus->link_ops->startup)
		return bus->link_ops->startup(bus);

	return 0;
}
EXPORT_SYMBOL_GPL(sdw_bus_master_startup);

/**
 * sdw_bus_master_process_wake_event() - handle external wake
 * event, e.g. handled at the PCI level
 * @bus: bus handle
 */
int sdw_bus_master_process_wake_event(struct sdw_bus *bus)
{
	if (!bus)
		return -EINVAL;

	if (bus->link_ops && bus->link_ops->process_wake_event)
		return bus->link_ops->process_wake_event(bus);

	return 0;
}
EXPORT_SYMBOL_GPL(sdw_bus_master_process_wake_event);
