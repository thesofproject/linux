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

static const struct ancillary_device_id *ancillary_match_id(const struct ancillary_device_id *id,
							    const struct ancillary_device *ancildev)
{

	while (id->name[0]) {
		const char *p = strrchr(dev_name(&ancildev->dev), '.');
		int match_size;

		if (!p)
			continue;
		match_size = p - dev_name(&ancildev->dev);

		/* use dev_name(&ancildev->dev) prefix before last '.' char to match to */
		if (!strncmp(dev_name(&ancildev->dev), id->name, match_size))
			return id;
		id++;
	}
	return NULL;
}

static int ancillary_match(struct device *dev, struct device_driver *drv)
{
	struct ancillary_device *ancildev = to_ancillary_dev(dev);
	struct ancillary_driver *ancildrv = to_ancillary_drv(drv);

	return !!ancillary_match_id(ancildrv->id_table, ancildev);
}

static int ancillary_uevent(struct device *dev, struct kobj_uevent_env *env)
{
	const char *name, *p;

	name = dev_name(dev);
	p = strrchr(name, '.');

	return add_uevent_var(env, "MODALIAS=%s%.*s", ANCILLARY_MODULE_PREFIX, (int)(p - name),
			      name);
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
 * ancillary_device_initialize - check ancillary_device and initialize
 * @ancildev: ancillary device struct
 */
int ancillary_device_initialize(struct ancillary_device *ancildev)
{
	struct device *dev = &ancildev->dev;

	dev->bus = &ancillary_bus_type;

	if (WARN_ON(!dev->parent) || WARN_ON(!ancildev->name) ||
	    WARN_ON(!(dev->type && dev->type->release) && !dev->release))
		return -EINVAL;

	device_initialize(&ancildev->dev);
	return 0;
}
EXPORT_SYMBOL_GPL(ancillary_device_initialize);

/**
 * __ancillary_device_add - add an ancillary bus device
 * @ancildev: ancillary bus device to add to the bus
 * @modname: name of the parent device's driver module
 */
int __ancillary_device_add(struct ancillary_device *ancildev, const char *modname)
{
	struct device *dev = &ancildev->dev;
	int ret;

	if (WARN_ON(!modname))
		return -EINVAL;

	ret = dev_set_name(dev, "%s.%s.%d", modname, ancildev->name, ancildev->id);
	if (ret) {
		dev_err(dev->parent, "dev_set_name failed for device: %d\n", ret);
		return ret;
	}

	ret = device_add(dev);
	if (ret)
		dev_err(dev, "adding device failed!: %d\n", ret);

	return ret;
}
EXPORT_SYMBOL_GPL(__ancillary_device_add);

static int ancillary_probe_driver(struct device *dev)
{
	struct ancillary_driver *ancildrv = to_ancillary_drv(dev->driver);
	struct ancillary_device *ancildev = to_ancillary_dev(dev);
	int ret;

	ret = dev_pm_domain_attach(dev, true);
	if (ret) {
		dev_warn(&ancildev->dev, "Failed to attach to PM Domain : %d\n", ret);
		return ret;
	}

	ret = ancildrv->probe(ancildev, ancillary_match_id(ancildrv->id_table, ancildev));
	if (ret)
		dev_pm_domain_detach(dev, true);

	return ret;
}

static int ancillary_remove_driver(struct device *dev)
{
	struct ancillary_driver *ancildrv = to_ancillary_drv(dev->driver);
	struct ancillary_device *ancildev = to_ancillary_dev(dev);
	int ret;

	ret = ancildrv->remove(ancildev);
	dev_pm_domain_detach(dev, true);

	return ret;
}

static void ancillary_shutdown_driver(struct device *dev)
{
	struct ancillary_driver *ancildrv = to_ancillary_drv(dev->driver);
	struct ancillary_device *ancildev = to_ancillary_dev(dev);

	ancildrv->shutdown(ancildev);
}

/**
 * __ancillary_driver_register - register a driver for ancillary bus devices
 * @ancildrv: ancillary_driver structure
 * @owner: owning module/driver
 */
int __ancillary_driver_register(struct ancillary_driver *ancildrv, struct module *owner)
{
	if (WARN_ON(!ancildrv->probe) || WARN_ON(!ancildrv->remove) ||
	    WARN_ON(!ancildrv->shutdown) || WARN_ON(!ancildrv->id_table))
		return -EINVAL;

	ancildrv->driver.owner = owner;
	ancildrv->driver.bus = &ancillary_bus_type;
	ancildrv->driver.probe = ancillary_probe_driver;
	ancildrv->driver.remove = ancillary_remove_driver;
	ancildrv->driver.shutdown = ancillary_shutdown_driver;

	return driver_register(&ancildrv->driver);
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
