// SPDX-License-Identifier: GPL-2.0-only
//
// Copyright(c) 2021 Intel Corporation. All rights reserved.
//
// Authors: Ranjani Sridharan <ranjani.sridharan@linux.intel.com>
//	    Peter Ujfalusi <peter.ujfalusi@linux.intel.com>
//

#include <linux/debugfs.h>
#include <linux/errno.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include "ops.h"
#include "sof-client.h"
#include "sof-priv.h"

static void sof_client_auxdev_release(struct device *dev)
{
	struct auxiliary_device *auxdev = to_auxiliary_dev(dev);
	struct sof_client_dev *cdev = auxiliary_dev_to_sof_client_dev(auxdev);

	kfree(cdev->auxdev.dev.platform_data);
	kfree(cdev);
}

static int sof_client_dev_add_data(struct sof_client_dev *cdev, const void *data,
				   size_t size)
{
	void *d = NULL;

	if (data) {
		d = kmemdup(data, size, GFP_KERNEL);
		if (!d)
			return -ENOMEM;
	}

	cdev->auxdev.dev.platform_data = d;
	return 0;
}

int sof_register_clients(struct snd_sof_dev *sdev)
{
	if (sof_ops(sdev) && sof_ops(sdev)->register_ipc_clients)
		return sof_ops(sdev)->register_ipc_clients(sdev);

	return 0;
}

void sof_unregister_clients(struct snd_sof_dev *sdev)
{
	if (sof_ops(sdev) && sof_ops(sdev)->unregister_ipc_clients)
		sof_ops(sdev)->unregister_ipc_clients(sdev);
}

int sof_client_dev_register(struct snd_sof_dev *sdev, const char *name, u32 id,
			    const void *data, size_t size)
{
	struct auxiliary_device *auxdev;
	struct sof_client_dev *cdev;
	int ret;

	cdev = kzalloc(sizeof(*cdev), GFP_KERNEL);
	if (!cdev)
		return -ENOMEM;

	cdev->sdev = sdev;
	auxdev = &cdev->auxdev;
	auxdev->name = name;
	auxdev->dev.parent = sdev->dev;
	auxdev->dev.release = sof_client_auxdev_release;
	auxdev->id = id;

	ret = sof_client_dev_add_data(cdev, data, size);
	if (ret < 0) {
		kfree(cdev);
		return ret;
	}

	ret = auxiliary_device_init(auxdev);
	if (ret < 0) {
		dev_err(sdev->dev, "error: failed to initialize client dev %s\n", name);
		kfree(cdev->auxdev.dev.platform_data);
		kfree(cdev);
		return ret;
	}

	ret = auxiliary_device_add(&cdev->auxdev);
	if (ret < 0) {
		dev_err(sdev->dev, "error: failed to add client dev %s\n", name);
		/*
		 * sof_client_auxdev_release() will be invoked to free up memory
		 * allocations through put_device()
		 */
		auxiliary_device_uninit(&cdev->auxdev);
		return ret;
	}

	/* add to list of SOF client devices */
	mutex_lock(&sdev->ipc_client_mutex);
	list_add(&cdev->list, &sdev->ipc_client_list);
	mutex_unlock(&sdev->ipc_client_mutex);

	return 0;
}
EXPORT_SYMBOL_NS_GPL(sof_client_dev_register, SND_SOC_SOF_CLIENT);

void sof_client_dev_unregister(struct snd_sof_dev *sdev, const char *name, u32 id)
{
	struct sof_client_dev *cdev;

	mutex_lock(&sdev->ipc_client_mutex);

	/*
	 * sof_client_auxdev_release() will be invoked to free up memory
	 * allocations through put_device()
	 */
	list_for_each_entry(cdev, &sdev->ipc_client_list, list) {
		if (!strcmp(cdev->auxdev.name, name) && cdev->auxdev.id == id) {
			list_del(&cdev->list);
			auxiliary_device_delete(&cdev->auxdev);
			auxiliary_device_uninit(&cdev->auxdev);
			break;
		}
	}

	mutex_unlock(&sdev->ipc_client_mutex);
}
EXPORT_SYMBOL_NS_GPL(sof_client_dev_unregister, SND_SOC_SOF_CLIENT);

int sof_client_ipc_tx_message(struct sof_client_dev *cdev, void *ipc_msg,
			      void *reply_data, size_t reply_bytes)
{
	struct sof_ipc_cmd_hdr *hdr = ipc_msg;

	return sof_ipc_tx_message(cdev->sdev->ipc, hdr->cmd, ipc_msg, hdr->size,
				  reply_data, reply_bytes);
}
EXPORT_SYMBOL_NS_GPL(sof_client_ipc_tx_message, SND_SOC_SOF_CLIENT);

struct dentry *sof_client_get_debugfs_root(struct sof_client_dev *cdev)
{
	return cdev->sdev->debugfs_root;
}
EXPORT_SYMBOL_NS_GPL(sof_client_get_debugfs_root, SND_SOC_SOF_CLIENT);

/* DMA buffer allocation in client drivers must use the core SOF device */
struct device *sof_client_get_dma_dev(struct sof_client_dev *cdev)
{
	return cdev->sdev->dev;
}
EXPORT_SYMBOL_NS_GPL(sof_client_get_dma_dev, SND_SOC_SOF_CLIENT);
