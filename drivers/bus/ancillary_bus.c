// SPDX-License-Identifier: GPL-2.0-only
/*
 * Lightweight software based bus for Ancillary devices
 *
 * Copyright (c) 2019-2020 Intel Corporation
 *
 * Please see Documentation/driver-api/ancillary_bus.rst for
 * more information
 */

#include <linux/acpi.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/of_irq.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/string.h>
#include <linux/ancillary_bus.h>

static DEFINE_IDA(ancillary_dev_ida);
#define ANCILLARY_INVALID_ID	~0U

static const
struct ancillary_device_id *ancillary_match_id(const struct ancillary_device_id *id,
					   struct ancillary_device *adev)
{
	while (id->name[0]) {
		if (!strcmp(adev->match_name, id->name))
			return id;
		id++;
	}
	return NULL;
}

static int ancillary_match(struct device *dev, struct device_driver *drv)
{
	struct ancillary_driver *adrv = to_ancillary_drv(drv);
	struct ancillary_device *adev = to_ancillary_dev(dev);

	return !!ancillary_match_id(adrv->id_table, adev);
}

static int ancillary_uevent(struct device *dev, struct kobj_uevent_env *env)
{
	struct ancillary_device *adev = to_ancillary_dev(dev);

	if (add_uevent_var(env, "MODALIAS=%s%s", ANCILLARY_MODULE_PREFIX,
			   adev->match_name))
		return -ENOMEM;

	return 0;
}

static const struct dev_pm_ops ancillary_dev_pm_ops = {
	SET_RUNTIME_PM_OPS(pm_generic_runtime_suspend,
			   pm_generic_runtime_resume, NULL)
	SET_SYSTEM_SLEEP_PM_OPS(pm_generic_suspend, pm_generic_resume)
};

struct bus_type ancillary_bus_type = {
	.name = "ancillary",
	.match = ancillary_match,
	.uevent = ancillary_uevent,
	.pm = &ancillary_dev_pm_ops,
};

/**
 * ancillary_release_device - Destroy a ancillary device
 * @_dev: device to release
 */
static void ancillary_release_device(struct device *_dev)
{
	struct ancillary_device *adev = to_ancillary_dev(_dev);
	u32 ida = adev->id;

	adev->release(adev);
	if (ida != ANCILLARY_INVALID_ID)
		ida_simple_remove(&ancillary_dev_ida, ida);
}

/**
 * ancillary_register_device - add a ancillary bus device
 * @adev: ancillary bus device to add
 */
int ancillary_register_device(struct ancillary_device *adev)
{
	int ret;

	if (!adev->release) {
		dev_err(&adev->dev, "release callback not set for adev!\n");
		return -EINVAL;
	}

	/* All error paths out of this function after the device_initialize
	 * must perform a put_device() so that the .release() callback is
	 * called for an error condition.
	 */
	device_initialize(&adev->dev);

	adev->dev.bus = &ancillary_bus_type;
	adev->dev.release = ancillary_release_device;

	/* All device IDs are automatically allocated */
	ret = ida_simple_get(&ancillary_dev_ida, 0, 0, GFP_KERNEL);
	if (ret < 0) {
		adev->id = ANCILLARY_INVALID_ID;
		dev_err(&adev->dev, "get IDA idx for ancillary device failed!\n");
		goto err;
	}

	adev->id = ret;

	ret = dev_set_name(&adev->dev, "%s.%d", adev->match_name, adev->id);
	if (ret) {
		dev_err(&adev->dev, "dev_set_name failed for device\n");
		goto err;
	}

	dev_dbg(&adev->dev, "Registering ancillary device '%s'\n",
		dev_name(&adev->dev));

	ret = device_add(&adev->dev);
	if (!ret)
		return ret;

	dev_err(&adev->dev, "Add device to ancillary device failed!: %d\n", ret);

err:
	put_device(&adev->dev);

	return ret;
}
EXPORT_SYMBOL_GPL(ancillary_register_device);

static int ancillary_probe_driver(struct device *_dev)
{
	struct ancillary_driver *adrv = to_ancillary_drv(_dev->driver);
	struct ancillary_device *adev = to_ancillary_dev(_dev);
	int ret;

	ret = dev_pm_domain_attach(_dev, true);
	if (ret) {
		dev_warn(_dev, "Failed to attach to PM Domain : %d\n", ret);
		return ret;
	}

	ret = adrv->probe(adev);
	if (ret) {
		dev_err(&adev->dev, "Probe returned error\n");
		dev_pm_domain_detach(_dev, true);
	}

	return ret;
}

static int ancillary_remove_driver(struct device *_dev)
{
	struct ancillary_driver *adrv = to_ancillary_drv(_dev->driver);
	struct ancillary_device *adev = to_ancillary_dev(_dev);
	int ret;

	ret = adrv->remove(adev);
	dev_pm_domain_detach(_dev, true);

	return ret;
}

static void ancillary_shutdown_driver(struct device *_dev)
{
	struct ancillary_driver *adrv = to_ancillary_drv(_dev->driver);
	struct ancillary_device *adev = to_ancillary_dev(_dev);

	adrv->shutdown(adev);
}

/**
 * __ancillary_register_driver - register a driver for ancillary bus devices
 * @adrv: ancillary_driver structure
 * @owner: owning module/driver
 */
int __ancillary_register_driver(struct ancillary_driver *adrv, struct module *owner)
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
EXPORT_SYMBOL_GPL(__ancillary_register_driver);

static int __init ancillary_bus_init(void)
{
	return bus_register(&ancillary_bus_type);
}

static void __exit ancillary_bus_exit(void)
{
	bus_unregister(&ancillary_bus_type);
	ida_destroy(&ancillary_dev_ida);
}

module_init(ancillary_bus_init);
module_exit(ancillary_bus_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Ancillary Bus");
MODULE_AUTHOR("David Ertman <david.m.ertman@intel.com>");
MODULE_AUTHOR("Kiran Patil <kiran.patil@intel.com>");
