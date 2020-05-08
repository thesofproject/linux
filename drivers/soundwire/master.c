// SPDX-License-Identifier: GPL-2.0-only
// Copyright(c) 2019-2020 Intel Corporation.

#include <linux/device.h>
#include <linux/acpi.h>
#include <linux/soundwire/sdw.h>
#include <linux/soundwire/sdw_type.h>
#include "bus.h"

/*
 * The sysfs for properties reflects the MIPI description as given
 * in the MIPI DisCo spec
 *
 * Base file is:
 *	sdw-master-N
 *      |---- revision
 *      |---- clk_stop_modes
 *      |---- max_clk_freq
 *      |---- clk_freq
 *      |---- clk_gears
 *      |---- default_row
 *      |---- default_col
 *      |---- dynamic_shape
 *      |---- err_threshold
 */

#define sdw_master_attr(field, format_string)				\
static ssize_t field##_show(struct device *dev,				\
			    struct device_attribute *attr,		\
			    char *buf)					\
{									\
	struct sdw_master_device *md = dev_to_sdw_master_device(dev);	\
	return sprintf(buf, format_string, md->bus->prop.field);	\
}									\
static DEVICE_ATTR_RO(field)

sdw_master_attr(revision, "0x%x\n");
sdw_master_attr(clk_stop_modes, "0x%x\n");
sdw_master_attr(max_clk_freq, "%d\n");
sdw_master_attr(default_row, "%d\n");
sdw_master_attr(default_col, "%d\n");
sdw_master_attr(default_frame_rate, "%d\n");
sdw_master_attr(dynamic_frame, "%d\n");
sdw_master_attr(err_threshold, "%d\n");

static ssize_t clock_frequencies_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct sdw_master_device *md = dev_to_sdw_master_device(dev);
	ssize_t size = 0;
	int i;

	for (i = 0; i < md->bus->prop.num_clk_freq; i++)
		size += sprintf(buf + size, "%8d ",
				md->bus->prop.clk_freq[i]);
	size += sprintf(buf + size, "\n");

	return size;
}
static DEVICE_ATTR_RO(clock_frequencies);

static ssize_t clock_gears_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct sdw_master_device *md = dev_to_sdw_master_device(dev);
	ssize_t size = 0;
	int i;

	for (i = 0; i < md->bus->prop.num_clk_gears; i++)
		size += sprintf(buf + size, "%8d ",
				md->bus->prop.clk_gears[i]);
	size += sprintf(buf + size, "\n");

	return size;
}
static DEVICE_ATTR_RO(clock_gears);

static struct attribute *master_node_attrs[] = {
	&dev_attr_revision.attr,
	&dev_attr_clk_stop_modes.attr,
	&dev_attr_max_clk_freq.attr,
	&dev_attr_default_row.attr,
	&dev_attr_default_col.attr,
	&dev_attr_default_frame_rate.attr,
	&dev_attr_dynamic_frame.attr,
	&dev_attr_err_threshold.attr,
	&dev_attr_clock_frequencies.attr,
	&dev_attr_clock_gears.attr,
	NULL,
};
ATTRIBUTE_GROUPS(master_node);

static void sdw_master_device_release(struct device *dev)
{
	struct sdw_master_device *md = dev_to_sdw_master_device(dev);

	kfree(md);
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

	if (!parent)
		return -EINVAL;

	md = kzalloc(sizeof(*md), GFP_KERNEL);
	if (!md)
		return -ENOMEM;

	md->dev.bus = &sdw_bus_type;
	md->dev.type = &sdw_master_type;
	md->dev.parent = parent;
	md->dev.groups = master_node_groups;
	md->dev.of_node = parent->of_node;
	md->dev.fwnode = fwnode;
	md->dev.dma_mask = parent->dma_mask;

	md->link_id = bus->link_id;

	dev_set_name(&md->dev, "sdw-master-%d", md->link_id);

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

	if (bus->link_ops && bus->link_ops->add) {
		ret = bus->link_ops->add(bus, bus->pdata);
		if (ret < 0) {
			dev_err(&md->dev, "link_ops add callback failed: %d\n", ret);
			goto link_add_err;
		}
	}

	/* add shortcuts to improve code readability/compactness */
	md->bus = bus;
	bus->md = md;
	/* Now, Master device is created, point bus->dev to &md->dev */
	bus->dev = &md->dev;

	return ret;

link_add_err:
	device_unregister(&md->dev);
device_register_err:
	return ret;
}

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

	if (bus->link_ops && bus->link_ops->del) {
		ret = bus->link_ops->del(bus);
		if (ret < 0) {
			dev_err(bus->dev, "link_ops del callback failed: %d\n",
				ret);
			return ret;
		}
	}

	device_unregister(&bus->md->dev);

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

	link_ops = bus->link_ops;

	if (link_ops && link_ops->process_wake_event)
		ret = link_ops->process_wake_event(bus);

	return ret;
}
EXPORT_SYMBOL_GPL(sdw_bus_master_process_wake_event);
