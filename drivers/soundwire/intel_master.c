// SPDX-License-Identifier: GPL-2.0-only
// Copyright(c) 2020 Intel Corporation.

#include <linux/device.h>
#include <linux/acpi.h>
#include <linux/soundwire/sdw.h>
#include <linux/soundwire/sdw_type.h>
#if !IS_ENABLED(CONFIG_VIRTUAL_BUS)
#include <linux/platform_device.h>
#else
#include <linux/virtual_bus.h>
#endif
#include "cadence_master.h"
#include "intel_master.h"
#include "intel.h"
#include "bus.h"

/*
 * this driver is an abstraction of the virtbus_device or
 * platform_device, with additional data structures needed to help
 * manage the Intel SoundWire IP and completion structure to force a
 * synchronous behavior so that the bus enumeration is complete before
 * the parent can continue its initialization. This is needed e.g. to
 * detect the platform configuration and identify the relevant ASoC
 * machine driver.
 */
#if IS_ENABLED(CONFIG_VIRTUAL_BUS)

static void sdw_intel_master_release(struct virtbus_device *_dev)
{
	struct sdw_intel_master_dev *master_dev =
		to_sdw_intel_master_dev(_dev);

	kfree(master_dev);
}

#else

static void sdw_intel_master_release(struct device *_dev)
{
	struct platform_device *pdev = container_of(_dev, struct platform_device, dev);
	struct sdw_intel_master_dev *master_dev =
		to_sdw_intel_master_dev(pdev);

	kfree(master_dev);
}

#endif

struct sdw_intel_master_dev
*sdw_intel_master_register(struct device *parent,
			   struct fwnode_handle *fwnode,
			   char *name,
			   int link_id,
			   struct sdw_intel_link_res *link_res)
{
#if IS_ENABLED(CONFIG_VIRTUAL_BUS)
	struct virtbus_device *vdev;
#else
	struct platform_device *pdev;
#endif
	struct sdw_intel_master_dev *master_dev;

	unsigned long time, timeout;
	int ret;

	master_dev = kzalloc(sizeof(*master_dev), GFP_KERNEL);
	if (!master_dev)
		return ERR_PTR(-ENOMEM);

	master_dev->link_id = link_id;
	master_dev->link_res = link_res;
	master_dev->fwnode = fwnode;

	init_completion(&master_dev->probe_complete);

#if IS_ENABLED(CONFIG_VIRTUAL_BUS)
	vdev = &master_dev->vdev;
	vdev->name = devm_kstrdup(parent, name, GFP_KERNEL);
	if (!vdev->name)
		return ERR_PTR(-ENOMEM);
	vdev->release = sdw_intel_master_release;
	vdev->dev.parent = parent;
	vdev->dev.fwnode = fwnode;
	vdev->dev.dma_mask = parent->dma_mask;

	ret = virtbus_register_device(vdev);
	if (ret < 0)
		return ERR_PTR(ret);
#else
	/*
	 * we don't use platform_device_alloc() since we want the
	 * platform device to be encapsulated in the
	 * sdw_intel_master_device structure.
	 */
	pdev = &master_dev->pdev;
	pdev->name = devm_kstrdup(parent, name, GFP_KERNEL);
	if (!pdev->name)
		return ERR_PTR(-ENOMEM);
	pdev->id = link_id;
	device_initialize(&pdev->dev);
	pdev->dev.release = sdw_intel_master_release;
	pdev->dev.parent = parent;
	pdev->dev.fwnode = fwnode;
	pdev->dev.dma_mask = parent->dma_mask;

	ret = platform_device_add(pdev);
	if (ret) {
		platform_device_put(pdev);
		goto err_unregister;
	}
#endif

	/* make sure the probe is complete before returning */
	timeout = msecs_to_jiffies(SDW_INTEL_MASTER_PROBE_TIMEOUT_MS);
	time = wait_for_completion_timeout(&master_dev->probe_complete,
					   timeout);
	if (!time) {
		dev_err(parent, "error: probe of %s timed out\n",
			name);
		ret = -ETIMEDOUT;
		goto err_unregister;
	}

	return master_dev;

err_unregister:
#if IS_ENABLED(CONFIG_VIRTUAL_BUS)
	virtbus_unregister_device(vdev);
#else
	platform_device_unregister(pdev);
#endif
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(sdw_intel_master_register);

void sdw_intel_master_unregister(struct sdw_intel_master_dev *master_dev)
{
#if IS_ENABLED(CONFIG_VIRTUAL_BUS)
	virtbus_unregister_device(&master_dev->vdev);
#else
	platform_device_unregister(&master_dev->pdev);
#endif
}
EXPORT_SYMBOL_GPL(sdw_intel_master_unregister);

int sdw_intel_master_startup(struct sdw_intel_master_dev *master_dev)
{
	struct sdw_intel_master_drv *master_drv;
	struct device *dev;
	struct device_driver *driver;
#if IS_ENABLED(CONFIG_VIRTUAL_BUS)
	struct virtbus_device *vdev;
	struct virtbus_driver *vdrv;
#else
	struct platform_device *pdev;
	struct platform_driver *pdrv;
#endif

	/* paranoid sanity check */
	if (!master_dev)
		return -EINVAL;

#if IS_ENABLED(CONFIG_VIRTUAL_BUS)
	vdev = &master_dev->vdev;
	dev = &vdev->dev;
	driver = dev->driver;

	/* even more paranoid sanity check */
	if (!driver)
		return -EINVAL;

	vdrv = to_virtbus_drv(driver);

	master_drv = container_of(vdrv,
				  struct sdw_intel_master_drv,
				  virtbus_drv);
#else
	pdev = &master_dev->pdev;
	dev = &pdev->dev;
	driver = dev->driver;

	/* even more paranoid sanity check */
	if (!driver)
		return -EINVAL;

	pdrv = container_of(driver, struct platform_driver, driver);

	master_drv = container_of(pdrv,
				  struct sdw_intel_master_drv,
				  platform_drv);
#endif

	if (master_drv->link_ops->startup)
		return master_drv->link_ops->startup(master_dev);

	return -EINVAL;
}
EXPORT_SYMBOL_GPL(sdw_intel_master_startup);

MODULE_LICENSE("GPL v2");
MODULE_ALIAS("sdw:intel-master-core");
MODULE_DESCRIPTION("Intel Soundwire Master core Driver");
