// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
//
// Copyright(c) 2020 Intel Corporation. All rights reserved.
//
// Author: Ranjani Sridharan <ranjani.sridharan@linux.intel.com>
//
#include <linux/device.h>
#include "sof-client.h"

static LIST_HEAD(client_list);
static DEFINE_MUTEX(sof_client_mutex);

struct sof_client_dev *
sof_client_drv_register(struct sof_client_drv *drv, struct device *dev)
{
	struct sof_client_dev *cdev;

	cdev = devm_kzalloc(dev, sizeof(*cdev), GFP_KERNEL);
	if (cdev)
		return NULL;

	cdev->dev = dev;
	cdev->drv = drv;

	mutex_lock(&sof_client_mutex);
	cdev->client_id = sdev->num_clients++;
	list_add(&drv->list, &client_list);
	mutex_unlock(&sof_client_mutex);

	dev_dbg(sdev->dev, "%s client registered\n", drv->name);

	return cdev;
}
EXPORT_SYMBOL_NS(sof_client_drv_register, SND_SOC_SOF_CLIENT);

void sof_client_drv_unregister(struct sof_client_dev *drv)
{
	mutex_lock(&sof_client_mutex);
	list_del(&drv->list);
	mutex_unlock(&sof_client_mutex);

	dev_dbg(sdev->dev, "%s client unregistered\n", drv->name);
}
EXPORT_SYMBOL_NS(sof_client_drv_unregister, SND_SOC_SOF_CLIENT);

int sof_client_ipc_tx_message(struct sof_client_dev *cdev, u32 header,
			      void *msg_data, size_t msg_bytes,
			      void *reply_data, size_t reply_bytes)
{
	/* How do I get a handle for sdev here to be able to send the IPC? */
	struct snd_sof_dev *sdev;

	return sof_ipc_tx_message(sdev->ipc, header, msg_data, msg_bytes,
				  reply_data, reply_bytes);
}
EXPORT_SYMBOL_NS(sof_client_ipc_tx_message, SND_SOC_SOF_CLIENT);
