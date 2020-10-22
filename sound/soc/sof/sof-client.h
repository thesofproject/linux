/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __SOC_SOF_CLIENT_H
#define __SOC_SOF_CLIENT_H

#include <linux/auxiliary_bus.h>
#include <linux/device.h>
#include <linux/list.h>

struct snd_sof_dev;
struct dentry;

/* SOF client device */
struct sof_client_dev {
	struct auxiliary_device auxdev;
	struct snd_sof_dev *sdev;
	struct list_head list;	/* item in SOF core client dev list */
	void *data;
};

#define sof_client_dev_to_sof_dev(cdev)		(cdev->sdev)

#define auxiliary_dev_to_sof_client_dev(auxiliary_dev) \
	container_of(auxiliary_dev, struct sof_client_dev, auxdev)

int sof_client_ipc_tx_message(struct sof_client_dev *cdev, void *ipc_msg,
			      void *reply_data, size_t reply_bytes);

struct dentry *sof_client_get_debugfs_root(struct sof_client_dev *cdev);
struct device *sof_client_get_dma_dev(struct sof_client_dev *cdev);

#if IS_ENABLED(CONFIG_SND_SOC_SOF_CLIENT)
int sof_client_dev_register(struct snd_sof_dev *sdev, const char *name, u32 id);
void sof_client_dev_unregister(struct snd_sof_dev *sdev, const char *name, u32 id);
#else
static inline int sof_client_dev_register(struct snd_sof_dev *sdev,
					  const char *name, u32 id)
{
	return 0;
}

static inline void sof_client_dev_unregister(struct snd_sof_dev *sdev,
					     const char *name, u32 id)
{
}
#endif

#endif /* __SOC_SOF_CLIENT_H */
