// SPDX-License-Identifier: GPL-2.0-only
/*
 * Software based bus for Ancillary devices
 *
 * Copyright (c) 2019-2020 Intel Corporation
 *
 * Please see Documentation/driver-api/ancillary_bus.rst for more information.
 */

#include <linux/device.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/string.h>
#include <linux/ancillary_bus.h>

static bool ancillary_match_id(const struct ancillary_device_id *id,
			       const struct ancillary_device *adev)
{
	while (id->name[0]) {
		if (!strcmp(adev->match_name, id->name))
			return true;
		id++;
	}
	return false;
}

static int ancillary_match(struct device *dev, struct device_driver *drv)
{
	struct ancillary_driver *adrv = to_ancillary_drv(drv);
	struct ancillary_device *adev = to_ancillary_dev(dev);

	return ancillary_match_id(adrv->id_table, adev);
}

static int ancillary_uevent(struct device *dev, struct kobj_uevent_env *env)
{
	struct ancillary_device *adev = to_ancillary_dev(dev);

	if (add_uevent_var(env, "MODALIAS=%s%s", ANCILLARY_MODULE_PREFIX, adev->match_name))
		return -ENOMEM;

	return 0;
}

static const struct dev_pm_ops ancillary_dev_pm_ops = {
	SET_RUNTIME_PM_OPS(pm_generic_runtime_suspend, pm_generic_runtime_resume, NULL)
	SET_SYSTEM_SLEEP_PM_OPS(pm_generic_suspend, pm_generic_resume)
};

struct bus_type ancillary_bus_type = {
	.name = "ancillary",
	.match = ancillary_match,
	.uevent = ancillary_uevent,
	.pm = &ancillary_dev_pm_ops,
};

/**
 * ancillary_device_register - add an ancillary bus device
 * @adev: ancillary bus device to add to the bus
 * @modname: name of the parent devices driver module
 */
int __ancillary_device_register(struct ancillary_device *adev, const char *modname)
{
	struct device *dev = &adev->dev;

	int ret;

	if (!(dev->type && dev->type->release) && !dev->release) {
		dev_err(dev, "release callback not set for ancillary's device!\n");
		return -EINVAL;
	}

	if (!dev->parent) {
		dev_err(dev, "parent device not set for ancillary's dev!\n");
		return -EINVAL;
	}

	/* All error paths out of this function after the device_initialize must perform a
	 * put_device() so that the .release() callback is called for an error condition.
	 */
	device_initialize(dev);
	dev->bus = &ancillary_bus_type;

	adev->match_name = kasprintf(GFP_KERNEL, "%s.%s", modname, adev->name);

	ret = dev_set_name(dev, "%s.%s.%d", modname, adev->match_name, adev->id);
	if (ret) {
		dev_err(dev, "dev_set_name failed for device\n");
		goto err;
	}

	dev_dbg(dev, "Registering ancillary device '%s'\n", dev_name(dev));

	/* Having a non-unique adev.match->name + adev->id will cause device_add to fail */
	ret = device_add(dev);
	if (!ret)
		return 0;

	dev_err(dev, "Add device to ancillary device failed!: %d\n", ret);

err:
	put_device(dev);

	return ret;
}
EXPORT_SYMBOL_GPL(__ancillary_device_register);

static int ancillary_probe_driver(struct device *dev)
{
	struct ancillary_driver *adrv = to_ancillary_drv(dev->driver);
	struct ancillary_device *adev = to_ancillary_dev(dev);
	int ret;

	ret = dev_pm_domain_attach(dev, true);
	if (ret) {
		dev_warn(&adev->dev, "Failed to attach to PM Domain : %d\n", ret);
		return ret;
	}

	ret = adrv->probe(adev);
	if (ret) {
		dev_err(&adev->dev, "Probe returned error %d\n", ret);
		dev_pm_domain_detach(dev, true);
	}

	return ret;
}

static int ancillary_remove_driver(struct device *dev)
{
	struct ancillary_driver *adrv = to_ancillary_drv(dev->driver);
	struct ancillary_device *adev = to_ancillary_dev(dev);
	int ret;

	ret = adrv->remove(adev);
	dev_pm_domain_detach(dev, true);

	return ret;
}

static void ancillary_shutdown_driver(struct device *dev)
{
	struct ancillary_driver *adrv = to_ancillary_drv(dev->driver);
	struct ancillary_device *adev = to_ancillary_dev(dev);

	adrv->shutdown(adev);
}

/**
 * __ancillary_driver_register - register a driver for ancillary bus devices
 * @adrv: ancillary_driver structure
 * @owner: owning module/driver
 */
int __ancillary_driver_register(struct ancillary_driver *adrv, struct module *owner)
{
	if (!adrv->probe || !adrv->remove || !adrv->shutdown || !adrv->id_table)
		return -EINVAL;

	adrv->driver.owner = owner;
	adrv->driver.bus = &ancillary_bus_type;
	adrv->driver.probe = ancillary_probe_driver;
	adrv->driver.remove = ancillary_remove_driver;
	adrv->driver.shutdown = ancillary_shutdown_driver;

	return driver_register(&adrv->driver);
}
EXPORT_SYMBOL_GPL(__ancillary_driver_register);

static int __init ancillary_bus_init(void)
{
	return bus_register(&ancillary_bus_type);
}

static void __exit ancillary_bus_exit(void)
{
	bus_unregister(&ancillary_bus_type);
}

module_init(ancillary_bus_init);
module_exit(ancillary_bus_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Ancillary Bus");
MODULE_AUTHOR("David Ertman <david.m.ertman@intel.com>");
MODULE_AUTHOR("Kiran Patil <kiran.patil@intel.com>");
