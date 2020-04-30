/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright(c) 2020 Intel Corporation. */

#ifndef __SDW_INTEL_MASTER_H
#define __SDW_INTEL_MASTER_H

#if IS_ENABLED(CONFIG_VIRTUAL_BUS)
#include <linux/virtual_bus.h>
#else
#include <linux/platform_device.h>
#endif

struct sdw_intel_master_dev {
#if IS_ENABLED(CONFIG_VIRTUAL_BUS)
	struct virtbus_device vdev;
#else
	struct platform_device pdev;
#endif
	struct completion probe_complete;
	int link_id;
	struct sdw_intel_link_res *link_res;
	struct fwnode_handle *fwnode;
	void *pdata;
};

/*
 * Intel-specific extensions needed to deal with SoundWire IP
 * integration in HDAudio controller
 */

struct sdw_intel_link_ops {
	int (*startup)(struct sdw_intel_master_dev *master_dev);
	int (*process_wake_event)(struct sdw_intel_master_dev *master_dev);
};

struct sdw_intel_master_drv {
	const struct sdw_intel_link_ops *link_ops;
#if IS_ENABLED(CONFIG_VIRTUAL_BUS)
	struct virtbus_driver virtbus_drv;
#else
	struct platform_driver platform_drv;
#endif
};

#if IS_ENABLED(CONFIG_VIRTUAL_BUS)

static inline
struct sdw_intel_master_dev
*to_sdw_intel_master_dev(struct virtbus_device *_vdev)
{
	return container_of(_vdev, struct sdw_intel_master_dev, vdev);
}

static inline
struct sdw_intel_master_dev
*dev_to_sdw_intel_master_dev(struct device *_dev)
{
	struct virtbus_device *_vdev = to_virtbus_dev(_dev);

	return to_sdw_intel_master_dev(_vdev);
}

static inline
struct device *sdw_intel_master_to_dev(struct sdw_intel_master_dev *master_dev)
{
	return &master_dev->vdev.dev;
}

static inline
int sdw_intel_master_drv_register(struct sdw_intel_master_drv *drv)
{
	return virtbus_register_driver(&drv->virtbus_drv);
}

static inline
void sdw_intel_master_drv_unregister(struct sdw_intel_master_drv *drv)
{
	virtbus_unregister_driver(&drv->virtbus_drv);
}

#else

static inline struct sdw_intel_master_dev
*to_sdw_intel_master_dev(struct platform_device *_pdev)
{
	return container_of(_pdev, struct sdw_intel_master_dev, pdev);
}

static inline
struct sdw_intel_master_dev
*dev_to_sdw_intel_master_dev(struct device *_dev)
{
	struct platform_device *_pdev = container_of(_dev, struct platform_device, dev);

	return to_sdw_intel_master_dev(_pdev);
}

static inline
struct device *sdw_intel_master_to_dev(struct sdw_intel_master_dev *master_dev)
{
	return &master_dev->pdev.dev;
}

static inline
int sdw_intel_master_drv_register(struct sdw_intel_master_drv *drv)
{
	return platform_driver_register(&drv->platform_drv);
}

static inline
void sdw_intel_master_drv_unregister(struct sdw_intel_master_drv *drv)
{
	platform_driver_unregister(&drv->platform_drv);
}

#endif

/**
 * module_sdw_intel_master_driver() - Helper macro for registering an SOF Client
 * driver
 * @__sdw_intel_master_driver: SOF client driver struct
 *
 * Helper macro for SOF client drivers which do not do anything special in
 * module init/exit. This eliminates a lot of boilerplate. Each module may only
 * use this macro once, and calling it replaces module_init() and module_exit()
 */
#define module_sdw_intel_master_driver(__sdw_intel_master_driver)	\
	module_driver(__sdw_intel_master_driver, sdw_intel_master_drv_register, \
		      sdw_intel_master_drv_unregister)

struct sdw_intel_master_dev
*sdw_intel_master_register(struct device *parent,
			   struct fwnode_handle *fwnode,
			   char *name,
			   int link_id,
			   struct sdw_intel_link_res *link_res);

void sdw_intel_master_unregister(struct sdw_intel_master_dev *master_dev);

int sdw_intel_master_startup(struct sdw_intel_master_dev *master_dev);

int sdw_intel_master_process_wake(struct sdw_intel_master_dev *master_dev);

#define SDW_INTEL_MASTER_PROBE_TIMEOUT_MS 3000

#endif /* __SDW_INTEL_LOCAL_H */
