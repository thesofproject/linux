/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Lightweight software bus
 *
 * Copyright (c) 2019-2020 Intel Corporation
 *
 * Please see Documentation/driver-api/ancillary_bus.rst for more information
 */

#ifndef _ANCILLARY_BUS_H_
#define _ANCILLARY_BUS_H_

#include <linux/device.h>

struct ancillary_device {
	struct device dev;
	const char *match_name;
	void (*release)(struct ancillary_device *adev);
	u32 id;
};

struct ancillary_driver {
	int (*probe)(struct ancillary_device *adev);
	int (*remove)(struct ancillary_device *adev);
	void (*shutdown)(struct ancillary_device *adev);
	int (*suspend)(struct ancillary_device *adev, pm_message_t);
	int (*resume)(struct ancillary_device *adev);
	struct device_driver driver;
	const struct ancillary_device_id *id_table;
};

static inline
struct ancillary_device *to_ancillary_dev(struct device *dev)
{
	return container_of(dev, struct ancillary_device, dev);
}

static inline
struct ancillary_driver *to_ancillary_drv(struct device_driver *drv)
{
	return container_of(drv, struct ancillary_driver, driver);
}

int ancillary_register_device(struct ancillary_device *adev);

int
__ancillary_register_driver(struct ancillary_driver *adrv, struct module *owner);

#define ancillary_register_driver(adrv) \
	__ancillary_register_driver(adrv, THIS_MODULE)

static inline void ancillary_unregister_device(struct ancillary_device *adev)
{
	device_unregister(&adev->dev);
}

static inline void ancillary_unregister_driver(struct ancillary_driver *adrv)
{
	driver_unregister(&adrv->driver);
}

#endif /* _ANCILLARY_BUS_H_ */
