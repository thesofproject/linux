/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2019-2020 Intel Corporation
 *
 * Please see Documentation/driver-api/ancillary_bus.rst for more information.
 */

#ifndef _ANCILLARY_BUS_H_
#define _ANCILLARY_BUS_H_

#include <linux/device.h>
#include <linux/slab.h>

struct ancillary_device {
	struct device dev;
	const char *name;
	const char *match_name;
	u32 id;
};

struct ancillary_driver {
	int (*probe)(struct ancillary_device *adev);
	int (*remove)(struct ancillary_device *adev);
	void (*shutdown)(struct ancillary_device *adev);
	int (*suspend)(struct ancillary_device *adev, pm_message_t state);
	int (*resume)(struct ancillary_device *adev);
	struct device_driver driver;
	const struct ancillary_device_id *id_table;
};

static inline struct ancillary_device *to_ancillary_dev(struct device *dev)
{
	return container_of(dev, struct ancillary_device, dev);
}

static inline struct ancillary_driver *to_ancillary_drv(struct device_driver *drv)
{
	return container_of(drv, struct ancillary_driver, driver);
}

int __ancillary_device_register(struct ancillary_device *adev, const char *modname);

#define ancillary_device_register(adev) __ancillary_device_register(adev, KBUILD_MODNAME)

static inline void ancillary_device_unregister(struct ancillary_device *adev)
{
	kfree(adev->match_name);
	device_unregister(&adev->dev);
}

int
__ancillary_driver_register(struct ancillary_driver *adrv, struct module *owner);

#define ancillary_driver_register(adrv) __ancillary_driver_register(adrv, THIS_MODULE)

static inline void ancillary_driver_unregister(struct ancillary_driver *adrv)
{
	driver_unregister(&adrv->driver);
}

#endif /* _ANCILLARY_BUS_H_ */
