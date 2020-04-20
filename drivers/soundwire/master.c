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
	return;
}

struct device_type sdw_master_type = {
	.name =		"soundwire_master",
	.release =	sdw_master_device_release,
};

/**
 * sdw_master_device_add() - create a Linux Master Device representation.
 * @bus: SDW bus instance
 *
 * The link_ops argument can be NULL, it is only used when link-specific
 * initializations and power-management are required.
 */
int sdw_master_device_add(struct sdw_bus *bus)
{
	struct sdw_master_device *md = &bus->md;
	int ret;

	if (!md)
		return -EINVAL;

	md->dev.bus = &sdw_bus_type;
	md->dev.type = &sdw_master_type;
	md->dev.dma_mask = md->dev.parent->dma_mask;
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
		dev_err(md->dev.parent, "Failed to add master: ret %d\n", ret);
		/*
		 * On err, don't free but drop ref as this will be freed
		 * when release method is invoked.
		 */
		put_device(&md->dev);
		goto device_register_err;
	}
	md->bus = bus;
	bus->dev = &md->dev;

	if (bus->link_ops && bus->link_ops->add) {
		ret = bus->link_ops->add(bus, bus->pdata);
		if (ret < 0) {
			dev_err(&md->dev, "link_ops add callback failed: %d\n", ret);
			goto link_add_err;
		}
	}

	return ret;

link_add_err:
	device_unregister(&md->dev);
device_register_err:
	return ret;
}
EXPORT_SYMBOL_GPL(sdw_master_device_add);

/**
 * sdw_master_device_del() - delete a Linux Master Device representation.
 * @md: the master device
 *
 * This function is the dual of sdw_master_device_add(), itreleases
 * all link-specific resources and unregisters the device.
 */
int sdw_master_device_del(struct sdw_bus *bus)
{
	int ret = 0;

	if (bus && bus->link_ops && bus->link_ops->del) {
		ret = bus->link_ops->del(bus);
		if (ret < 0) {
			dev_err(bus->dev, "link_ops del callback failed: %d\n",
				ret);
			return ret;
		}
	}

	device_unregister(bus->dev);
	return ret;
}

/**
 * sdw_bus_master_startup() - startup hardware
 *
 * @md: Linux Soundwire master device
 */
int sdw_bus_master_startup(struct sdw_bus *bus)
{
	struct sdw_link_ops *link_ops;
	int ret = 0;

	if (IS_ERR_OR_NULL(bus))
		return -EINVAL;

	link_ops = bus->link_ops;

	if (link_ops && link_ops->startup)
		ret = link_ops->startup(bus);

	return ret;
}
EXPORT_SYMBOL_GPL(sdw_bus_master_startup);

/**
 * sdw_bus_master_process_wake_event() - handle external wake
 * event, e.g. handled at the PCI level
 *
 * @md: Linux Soundwire master device
 */
int sdw_bus_master_process_wake_event(struct sdw_bus *bus)
{
	struct sdw_link_ops *link_ops;
	int ret = 0;

	if (IS_ERR_OR_NULL(bus))
		return -EINVAL;

	link_ops = bus->link_ops;

	if (link_ops && link_ops->process_wake_event)
		ret = link_ops->process_wake_event(bus);

	return ret;
}
EXPORT_SYMBOL_GPL(sdw_bus_master_process_wake_event);
