// SPDX-License-Identifier: GPL-2.0-only
//
// Copyright(c) 2020 Intel Corporation. All rights reserved.
//
// Author: Ranjani Sridharan <ranjani.sridharan@linux.intel.com>
//

#include <linux/debugfs.h>
#include <linux/errno.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include "sof-client.h"
#include "sof-priv.h"

static void sof_client_auxdev_release(struct device *dev)
{
	struct auxiliary_device *auxdev = to_auxiliary_dev(dev);
	struct sof_client_dev *cdev = auxiliary_dev_to_sof_client_dev(auxdev);

	kfree(cdev);
}

static struct sof_client_dev *sof_client_dev_alloc(struct snd_sof_dev *sdev, const char *name,
						   u32 id)
{
	struct sof_client_dev *cdev;
	struct auxiliary_device *auxdev;
	int ret;

	cdev = kzalloc(sizeof(*cdev), GFP_KERNEL);
	if (!cdev)
		return ERR_PTR(-ENOMEM);

	cdev->sdev = sdev;
	auxdev = &cdev->auxdev;
	auxdev->name = name;
	auxdev->dev.parent = sdev->dev;
	auxdev->dev.release = sof_client_auxdev_release;
	auxdev->id = id;

	ret = auxiliary_device_init(auxdev);
	if (ret < 0) {
		dev_err(sdev->dev, "error: failed to initialize client dev %s\n", name);
		goto err_free;
	}

	return cdev;

err_free:
	kfree(cdev);
	return NULL;
}

int sof_client_dev_register(struct snd_sof_dev *sdev, const char *name, u32 id)
{
	struct sof_client_dev *cdev;
	int ret;

	cdev = sof_client_dev_alloc(sdev, name, id);
	if (IS_ERR_OR_NULL(cdev))
		return PTR_ERR(cdev);

	ret = auxiliary_device_add(&cdev->auxdev);
	if (ret < 0) {
		dev_err(sdev->dev, "error: failed to add client dev %s\n", name);
		/* cdev will be freed when the release callback is invoked through put_device() */
		auxiliary_device_uninit(&cdev->auxdev);
		return ret;
	}

	/* add to list of SOF client devices */
	mutex_lock(&sdev->client_mutex);
	list_add(&cdev->list, &sdev->client_list);
	mutex_unlock(&sdev->client_mutex);

	return 0;
}
EXPORT_SYMBOL_NS_GPL(sof_client_dev_register, SND_SOC_SOF_CLIENT);

void sof_client_dev_unregister(struct snd_sof_dev *sdev, const char *name, u32 id)
{
	struct sof_client_dev *cdev, *_cdev;

	mutex_lock(&sdev->client_mutex);

	/* cdev will be freed when the release callback for the auxiliary device is invoked */
	list_for_each_entry_safe(cdev, _cdev, &sdev->client_list, list) {
		if (!strcmp(cdev->auxdev.name, name) && cdev->auxdev.id == id) {
			auxiliary_device_delete(&cdev->auxdev);
			auxiliary_device_uninit(&cdev->auxdev);
			break;
		}
	}

	mutex_unlock(&sdev->client_mutex);
}
EXPORT_SYMBOL_NS_GPL(sof_client_dev_unregister, SND_SOC_SOF_CLIENT);

int sof_client_ipc_tx_message(struct sof_client_dev *cdev, u32 header, void *msg_data,
			      size_t msg_bytes, void *reply_data, size_t reply_bytes)
{
	return sof_ipc_tx_message(cdev->sdev->ipc, header, msg_data, msg_bytes,
				  reply_data, reply_bytes);
}
EXPORT_SYMBOL_NS_GPL(sof_client_ipc_tx_message, SND_SOC_SOF_CLIENT);

struct dentry *sof_client_get_debugfs_root(struct sof_client_dev *cdev)
{
	return cdev->sdev->debugfs_root;
}
EXPORT_SYMBOL_NS_GPL(sof_client_get_debugfs_root, SND_SOC_SOF_CLIENT);

MODULE_LICENSE("GPL");
