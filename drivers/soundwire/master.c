// SPDX-License-Identifier: (GPL-2.0)
// Copyright(c) 2019-2020 Intel Corporation.

#include <linux/device.h>
#include <linux/acpi.h>
#include <linux/soundwire/sdw.h>
#include <linux/soundwire/sdw_type.h>
#include "bus.h"

static void sdw_master_device_release(struct device *dev)
{
	struct sdw_master_device *md = dev_to_sdw_master_device(dev);

	kfree(md);
}

struct device_type sdw_master_type = {
	.name =		"soundwire_master",
	.release =	sdw_master_device_release,
};

struct sdw_master_device
*sdw_master_device_add(const char *master_name,
		       struct device *parent,
		       struct fwnode_handle *fwnode,
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
	md->master_name = master_name;

	init_completion(&md->probe_complete);

	md->dev.parent = parent;
	md->dev.fwnode = fwnode;
	md->dev.bus = &sdw_bus_type;
	md->dev.type = &sdw_master_type;
	md->dev.dma_mask = md->dev.parent->dma_mask;
	dev_set_name(&md->dev, "sdw-master-%d", md->link_id);

	ret = device_register(&md->dev);
	if (ret) {
		dev_err(parent, "Failed to add master: ret %d\n", ret);
		/*
		 * On err, don't free but drop ref as this will be freed
		 * when release method is invoked.
		 */
		put_device(&md->dev);
		return ERR_PTR(-ENOMEM);
	}

	return md;
}
EXPORT_SYMBOL_GPL(sdw_master_device_add);

int sdw_master_device_startup(struct sdw_master_device *md)
{
	struct sdw_master_driver *mdrv;
	struct device *dev;
	int ret = 0;

	if (IS_ERR_OR_NULL(md))
		return -EINVAL;

	dev = &md->dev;
	mdrv = drv_to_sdw_master_driver(dev->driver);

	if (mdrv && mdrv->startup)
		ret = mdrv->startup(md);

	return ret;
}
EXPORT_SYMBOL_GPL(sdw_master_device_startup);

int sdw_master_device_process_wake_event(struct sdw_master_device *md)
{
	struct sdw_master_driver *mdrv;
	struct device *dev;
	int ret = 0;

	if (IS_ERR_OR_NULL(md))
		return -EINVAL;

	dev = &md->dev;
	mdrv = drv_to_sdw_master_driver(dev->driver);

	if (mdrv && mdrv->process_wake_event)
		ret = mdrv->process_wake_event(md);

	return ret;
}
EXPORT_SYMBOL_GPL(sdw_master_device_process_wake_event);
