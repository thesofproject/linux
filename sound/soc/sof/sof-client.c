// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
//
// Copyright(c) 2019 Intel Corporation. All rights reserved.
//
// Author: Ranjani Sridharan <ranjani.sridharan@linux.intel.com>
//
#include <linux/device.h>
#include "sof-client.h"

void *sof_get_client_data(struct device *dev)
{
	struct snd_sof_client *client = dev_get_platdata(dev);

	return client->client_data;
}
EXPORT_SYMBOL(sof_get_client_data);

struct snd_sof_client *sof_client_dev_register(struct snd_sof_dev *sdev,
					       const char *name)
{
	struct snd_sof_client *client;

	client = devm_kzalloc(sdev->dev, sizeof(*client), GFP_KERNEL);
	if (!client)
		return NULL;

	/* register client platform device */
	client->pdev = platform_device_register_data(sdev->dev, name,
						     PLATFORM_DEVID_NONE,
						     client, sizeof(*client));
	if (IS_ERR(client->pdev)) {
		dev_err(sdev->dev, "error: Failed to register %s\n", name);
		return NULL;
	}

	dev_dbg(sdev->dev, "%s client registered\n", name);

	return client;
}
EXPORT_SYMBOL(sof_client_dev_register);

void sof_client_dev_unregister(struct snd_sof_client *client)
{
	if (!IS_ERR_OR_NULL(client->pdev))
		platform_device_unregister(client->pdev);
}
EXPORT_SYMBOL(sof_client_dev_unregister);
