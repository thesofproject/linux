/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2019-2020 Intel Corporation
 *
 * Please see Documentation/driver-api/ancillary_bus.rst for more information.
 */

#ifndef _ANCILLARY_BUS_H_
#define _ANCILLARY_BUS_H_

#include <linux/device.h>
#include <linux/mod_devicetable.h>
#include <linux/slab.h>

struct ancillary_device {
	struct device dev;
	const char *name;
	u32 id;
};

struct ancillary_driver {
	int (*probe)(struct ancillary_device *ancildev, const struct ancillary_device_id *id);
	int (*remove)(struct ancillary_device *ancildev);
	void (*shutdown)(struct ancillary_device *ancildev);
	int (*suspend)(struct ancillary_device *ancildev, pm_message_t state);
	int (*resume)(struct ancillary_device *ancildev);
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

int ancillary_device_initialize(struct ancillary_device *ancildev);
int __ancillary_device_add(struct ancillary_device *ancildev, const char *modname);
#define ancillary_device_add(ancildev) __ancillary_device_add(ancildev, KBUILD_MODNAME)

static inline void ancillary_device_unregister(struct ancillary_device *ancildev)
{
	device_unregister(&ancildev->dev);
}

int __ancillary_driver_register(struct ancillary_driver *ancildrv, struct module *owner);
#define ancillary_driver_register(ancildrv) __ancillary_driver_register(ancildrv, THIS_MODULE)

static inline void ancillary_driver_unregister(struct ancillary_driver *ancildrv)
{
	driver_unregister(&ancildrv->driver);
}

/**
 * module_ancillary_driver() - Helper macro for registering an ancillary driver
 * @__ancillary_driver: ancillary driver struct
 *
 * Helper macro for ancillary drivers which do not do anything special in
 * module init/exit. This eliminates a lot of boilerplate. Each module may only
 * use this macro once, and calling it replaces module_init() and module_exit()
 */
#define module_ancillary_driver(__ancillary_driver) \
	module_driver(__ancillary_driver, ancillary_driver_register, ancillary_driver_unregister)
#endif /* _ANCILLARY_BUS_H_ */
