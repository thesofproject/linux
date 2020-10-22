/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __SOUND_SOC_SOF_CLIENT_H
#define __SOUND_SOC_SOF_CLIENT_H

#include <linux/auxiliary_bus.h>
#include <linux/device.h>
#include <linux/idr.h>
#include <linux/list.h>

#define SOF_CLIENT_PROBE_TIMEOUT_MS 2000

struct snd_sof_dev;

/* SOF client device */
struct sof_client_dev {
	struct auxiliary_device auxdev;
	struct snd_sof_dev *sdev;
	struct list_head list;	/* item in SOF core client dev list */
	void *data;
};

/* client-specific ops, all optional */
struct sof_client_ops {
	int (*client_ipc_rx)(struct sof_client_dev *cdev, u32 msg_cmd);
};

struct sof_client_drv {
	const struct sof_client_ops ops;
	struct auxiliary_driver auxiliary_drv;
};

#define auxiliary_dev_to_sof_client_dev(auxiliary_dev) \
	container_of(auxiliary_dev, struct sof_client_dev, auxdev)

static inline int sof_client_drv_register(struct sof_client_drv *drv)
{
	return auxiliary_driver_register(&drv->auxiliary_drv);
}

static inline void sof_client_drv_unregister(struct sof_client_drv *drv)
{
	auxiliary_driver_unregister(&drv->auxiliary_drv);
}

int sof_client_dev_register(struct snd_sof_dev *sdev, const char *name, u32 id);
void sof_client_dev_unregister(struct snd_sof_dev *sdev, const char *name, u32 id);

int sof_client_ipc_tx_message(struct sof_client_dev *cdev, u32 header, void *msg_data,
			      size_t msg_bytes, void *reply_data, size_t reply_bytes);

struct dentry *sof_client_get_debugfs_root(struct sof_client_dev *cdev);

/**
 * module_sof_client_driver() - Helper macro for registering an SOF Client
 * driver
 * @__sof_client_driver: SOF client driver struct
 *
 * Helper macro for SOF client drivers which do not do anything special in
 * module init/exit. This eliminates a lot of boilerplate. Each module may only
 * use this macro once, and calling it replaces module_init() and module_exit()
 */
#define module_sof_client_driver(__sof_client_driver) \
	module_driver(__sof_client_driver, sof_client_drv_register, sof_client_drv_unregister)

#endif
