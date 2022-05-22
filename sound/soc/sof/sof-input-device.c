// SPDX-License-Identifier: GPL-2.0-only
//
// Copyright(c) 2022 Intel Corporation. All rights reserved.
//
// Author: Jyri Sarha <jyri.sarha@intel.com>
//

#include "sof-priv.h"
#include "sof-client.h"
#include <sound/sof/input-event.h>
#include <linux/input.h>

struct sof_input_device {
	struct input_dev *input_dev;
	struct device *dev;
};

static
void sof_input_device_event(struct sof_client_dev *cdev, void *msg_buf)
{
	struct sof_ipc_input_event *event = msg_buf;
	struct sof_input_device *sid = cdev->data;

	printk("%s: Input event code %u key %d\n", __func__, event->code,
	       event->value);

	input_report_key(sid->input_dev, event->code, event->value);
	input_sync(sid->input_dev);
}

static int sof_input_device_client_probe(struct auxiliary_device *auxdev,
					 const struct auxiliary_device_id *id)
{
	struct sof_client_dev *cdev = auxiliary_dev_to_sof_client_dev(auxdev);
	struct device *dev = &auxdev->dev;
	struct sof_input_device *sid;
	int ret;

	if (sof_client_get_ipc_type(cdev) != SOF_IPC)
		return -ENOTSUPP;

	sid = devm_kzalloc(dev, sizeof(*sid), GFP_KERNEL);
	if (!sid)
		return -ENOMEM;

	sid->input_dev = devm_input_allocate_device(dev);
	if (!sid->input_dev) {
		dev_err(dev, "%s:%d: Not enough memory\n", __func__,
			__LINE__);
		return -ENOMEM;
	}

	sid->input_dev->name = "SOF Input Device";
	sid->input_dev->evbit[0] = BIT_MASK(EV_KEY);
	sid->input_dev->keybit[BIT_WORD(BTN_0)] = BIT_MASK(BTN_0) |
		BIT_MASK(BTN_1);

	ret = input_register_device(sid->input_dev);
	if (ret) {
		dev_err(dev, "input_register_device() failed %d\n", ret);
		return ret;
	}

	sid->dev = dev;
	cdev->data = sid;

	ret = sof_client_register_ipc_rx_handler(cdev, SOF_IPC_GLB_INPUT_EVENT,
						 sof_input_device_event);
	if (ret) {
		dev_err(dev, "sof_client_register_ipc_rx_handler() failed %d\n",
			ret);
		input_unregister_device(sid->input_dev);
		return ret;
	}

	return 0;
}

static void sof_input_device_client_remove(struct auxiliary_device *auxdev)
{
	struct sof_client_dev *cdev = auxiliary_dev_to_sof_client_dev(auxdev);
	struct sof_input_device *sid = cdev->data;

	sof_client_unregister_ipc_rx_handler(cdev, SOF_IPC_GLB_INPUT_EVENT);

	input_unregister_device(sid->input_dev);
}

static const struct auxiliary_device_id sof_input_device_client_id_table[] = {
	{ .name = "snd_sof.input_device", },
	{},
};
MODULE_DEVICE_TABLE(auxiliary, sof_input_device_client_id_table);

/* driver name will be set based on KBUILD_MODNAME */
static struct auxiliary_driver sof_input_device_client_drv = {
	.probe = sof_input_device_client_probe,
	.remove = sof_input_device_client_remove,

	.id_table = sof_input_device_client_id_table,
};

module_auxiliary_driver(sof_input_device_client_drv);

MODULE_DESCRIPTION("SOF Input Device Client Driver");
MODULE_LICENSE("GPL v2");
MODULE_IMPORT_NS(SND_SOC_SOF_CLIENT);
