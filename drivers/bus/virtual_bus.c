// SPDX-License-Identifier: GPL-2.0-only
/*
 * Lightweight software based bus for virtual devices
 *
 * Copyright (c) 2019-2020 Intel Corporation
 *
 * Please see Documentation/driver-api/virtual_bus.rst for
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
#include <linux/virtual_bus.h>

static DEFINE_IDA(virtbus_dev_ida);
#define VIRTBUS_INVALID_ID	~0U

static const
struct virtbus_device_id *virtbus_match_id(const struct virtbus_device_id *id,
					   struct virtbus_device *vdev)
{
	while (id->name[0]) {
		if (!strcmp(vdev->match_name, id->name))
			return id;
		id++;
	}
	return NULL;
}

static int virtbus_match(struct device *dev, struct device_driver *drv)
{
	struct virtbus_driver *vdrv = to_virtbus_drv(drv);
	struct virtbus_device *vdev = to_virtbus_dev(dev);

	return !!virtbus_match_id(vdrv->id_table, vdev);
}

static int virtbus_uevent(struct device *dev, struct kobj_uevent_env *env)
{
	struct virtbus_device *vdev = to_virtbus_dev(dev);

	if (add_uevent_var(env, "MODALIAS=%s%s", VIRTBUS_MODULE_PREFIX,
			   vdev->match_name))
		return -ENOMEM;

	return 0;
}

static const struct dev_pm_ops virtbus_dev_pm_ops = {
	SET_RUNTIME_PM_OPS(pm_generic_runtime_suspend,
			   pm_generic_runtime_resume, NULL)
	SET_SYSTEM_SLEEP_PM_OPS(pm_generic_suspend, pm_generic_resume)
};

struct bus_type virtual_bus_type = {
	.name = "virtbus",
	.match = virtbus_match,
	.uevent = virtbus_uevent,
	.pm = &virtbus_dev_pm_ops,
};

/**
 * virtbus_release_device - Destroy a virtbus device
 * @_dev: device to release
 */
static void virtbus_release_device(struct device *_dev)
{
	struct virtbus_device *vdev = to_virtbus_dev(_dev);
	u32 ida = vdev->id;

	vdev->release(vdev);
	if (ida != VIRTBUS_INVALID_ID)
		ida_simple_remove(&virtbus_dev_ida, ida);
}

/**
 * virtbus_register_device - add a virtual bus device
 * @vdev: virtual bus device to add
 */
int virtbus_register_device(struct virtbus_device *vdev)
{
	int ret;

	if (!vdev->release) {
		dev_err(&vdev->dev, "release callback not set for vdev!\n");
		return -EINVAL;
	}

	/* All error paths out of this function after the device_initialize
	 * must perform a put_device() so that the .release() callback is
	 * called for an error condition.
	 */
	device_initialize(&vdev->dev);

	vdev->dev.bus = &virtual_bus_type;
	vdev->dev.release = virtbus_release_device;

	/* All device IDs are automatically allocated */
	ret = ida_simple_get(&virtbus_dev_ida, 0, 0, GFP_KERNEL);
	if (ret < 0) {
		vdev->id = VIRTBUS_INVALID_ID;
		dev_err(&vdev->dev, "get IDA idx for virtbus device failed!\n");
		goto err;
	}

	vdev->id = ret;

	ret = dev_set_name(&vdev->dev, "%s.%d", vdev->match_name, vdev->id);
	if (ret) {
		dev_err(&vdev->dev, "dev_set_name failed for device\n");
		goto err;
	}

	dev_dbg(&vdev->dev, "Registering virtbus device '%s'\n",
		dev_name(&vdev->dev));

	ret = device_add(&vdev->dev);
	if (!ret)
		return ret;

	dev_err(&vdev->dev, "Add device to virtbus failed!: %d\n", ret);

err:
	put_device(&vdev->dev);

	return ret;
}
EXPORT_SYMBOL_GPL(virtbus_register_device);

static int virtbus_probe_driver(struct device *_dev)
{
	struct virtbus_driver *vdrv = to_virtbus_drv(_dev->driver);
	struct virtbus_device *vdev = to_virtbus_dev(_dev);
	int ret;

	ret = dev_pm_domain_attach(_dev, true);
	if (ret) {
		dev_warn(_dev, "Failed to attach to PM Domain : %d\n", ret);
		return ret;
	}

	ret = vdrv->probe(vdev);
	if (ret) {
		dev_err(&vdev->dev, "Probe returned error\n");
		dev_pm_domain_detach(_dev, true);
	}

	return ret;
}

static int virtbus_remove_driver(struct device *_dev)
{
	struct virtbus_driver *vdrv = to_virtbus_drv(_dev->driver);
	struct virtbus_device *vdev = to_virtbus_dev(_dev);
	int ret;

	ret = vdrv->remove(vdev);
	dev_pm_domain_detach(_dev, true);

	return ret;
}

static void virtbus_shutdown_driver(struct device *_dev)
{
	struct virtbus_driver *vdrv = to_virtbus_drv(_dev->driver);
	struct virtbus_device *vdev = to_virtbus_dev(_dev);

	vdrv->shutdown(vdev);
}

/**
 * __virtbus_register_driver - register a driver for virtual bus devices
 * @vdrv: virtbus_driver structure
 * @owner: owning module/driver
 */
int __virtbus_register_driver(struct virtbus_driver *vdrv, struct module *owner)
{
	if (!vdrv->probe || !vdrv->remove || !vdrv->shutdown || !vdrv->id_table)
		return -EINVAL;

	vdrv->driver.owner = owner;
	vdrv->driver.bus = &virtual_bus_type;
	vdrv->driver.probe = virtbus_probe_driver;
	vdrv->driver.remove = virtbus_remove_driver;
	vdrv->driver.shutdown = virtbus_shutdown_driver;

	return driver_register(&vdrv->driver);
}
EXPORT_SYMBOL_GPL(__virtbus_register_driver);

static int __init virtual_bus_init(void)
{
	return bus_register(&virtual_bus_type);
}

static void __exit virtual_bus_exit(void)
{
	bus_unregister(&virtual_bus_type);
	ida_destroy(&virtbus_dev_ida);
}

module_init(virtual_bus_init);
module_exit(virtual_bus_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Virtual Bus");
MODULE_AUTHOR("David Ertman <david.m.ertman@intel.com>");
MODULE_AUTHOR("Kiran Patil <kiran.patil@intel.com>");
