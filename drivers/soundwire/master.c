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
 * @parent: the parent Linux device (e.g. a PCI device)
 * @fwnode: the parent fwnode (e.g. an ACPI companion device to the parent)
 * @link_ops: link-specific ops (optional)
 * @link_id: link index as defined by MIPI DisCo specification
 * @pdata: private data (e.g. register base, offsets, platform quirks, etc).
 *
 * The link_ops argument can be NULL, it is only used when link-specific
 * initializations and power-management are required.
 */
struct sdw_master_device
*sdw_master_device_add(struct device *parent,
		       struct fwnode_handle *fwnode,
		       struct sdw_link_ops *link_ops,
		       int link_id,
		       void *pdata)
{
	struct sdw_master_device *md;
	int ret;

	md = kzalloc(sizeof(*md), GFP_KERNEL);
	if (!md)
		return ERR_PTR(-ENOMEM);

	md->link_id = link_id;
	md->pdata = pdata;
	md->link_ops = link_ops;

	md->dev.parent = parent;
	md->dev.groups = master_node_groups;
	md->dev.of_node = parent->of_node;
	md->dev.fwnode = fwnode;
	md->dev.bus = &sdw_bus_type;
	md->dev.type = &sdw_master_type;
	md->dev.dma_mask = md->dev.parent->dma_mask;
	dev_set_name(&md->dev, "sdw-master-%d", md->link_id);

	if (link_ops && link_ops->driver) {
		/*
		 * A driver is only needed for ASoC integration (need
		 * driver->name) and for link-specific power management
		 * w/ a pm_dev_ops structure.
		 *
		 * The driver needs to be registered by the parent
		 */
		md->dev.driver = link_ops->driver;
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

	if (link_ops && link_ops->add) {
		ret = link_ops->add(md, pdata);
		if (ret < 0) {
			dev_err(&md->dev, "link_ops add callback failed: %d\n", ret);
			goto link_add_err;
		}
	}

	return md;

link_add_err:
	device_unregister(&md->dev);
device_register_err:
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(sdw_master_device_add);

/**
 * sdw_master_device_del() - delete a Linux Master Device representation.
 * @md: the master device
 *
 * This function is the dual of sdw_master_device_add(), itreleases
 * all link-specific resources and unregisters the device.
 */
int sdw_master_device_del(struct sdw_master_device *md)
{
	int ret = 0;

	if (md && md->link_ops && md->link_ops->del) {
		ret = md->link_ops->del(md);
		if (ret < 0) {
			dev_err(&md->dev, "link_ops del callback failed: %d\n",
				ret);
			return ret;
		}
	}

	device_unregister(&md->dev);

	return ret;
}
EXPORT_SYMBOL_GPL(sdw_master_device_del);

/**
 * sdw_master_device_startup() - startup hardware
 *
 * @md: Linux Soundwire master device
 */
int sdw_master_device_startup(struct sdw_master_device *md)
{
	struct sdw_link_ops *link_ops;
	int ret = 0;

	if (IS_ERR_OR_NULL(md))
		return -EINVAL;

	link_ops = md->link_ops;

	if (link_ops && link_ops->startup)
		ret = link_ops->startup(md);

	return ret;
}
EXPORT_SYMBOL_GPL(sdw_master_device_startup);

/**
 * sdw_master_device_process_wake_event() - handle external wake
 * event, e.g. handled at the PCI level
 *
 * @md: Linux Soundwire master device
 */
int sdw_master_device_process_wake_event(struct sdw_master_device *md)
{
	struct sdw_link_ops *link_ops;
	int ret = 0;

	if (IS_ERR_OR_NULL(md))
		return -EINVAL;

	link_ops = md->link_ops;

	if (link_ops && link_ops->process_wake_event)
		ret = link_ops->process_wake_event(md);

	return ret;
}
EXPORT_SYMBOL_GPL(sdw_master_device_process_wake_event);
