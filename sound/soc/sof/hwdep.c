// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
//
// This file is provided under a dual BSD/GPLv2 license.  When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2019 Intel Corporation. All rights reserved.
//
// Author: Ranjani Sridharan <ranjani.sridharan@linux.intel.com>
//

#include "sof-priv.h"

#define SOF_IOCTL_WIDGET_NAME 0

static int sof_hwdep_ioctl(struct snd_hwdep *hwdep, struct file *file,
                           unsigned int cmd, unsigned long arg)
{
	struct snd_sof_widget *swidget =
		(struct snd_sof_widget *)hwdep->private_data;
        void __user *argp = (void __user *)arg;

        switch (cmd) {
        case SOF_IOCTL_WIDGET_NAME:
		/*
		 * return the name of the widget that the hwdep dev
		 * is associated with
		 */
		if (copy_to_user(argp, swidget->widget->name,
				 sizeof(swidget->widget->name)))
			return -EFAULT;
		return 0;
	default:
		break;
        }
        return -ENOIOCTLCMD;
}

static long sof_hwdep_read(struct snd_hwdep *hwdep, char __user *buf,  long count,
			   loff_t *offset)
{
	struct snd_sof_widget *swidget =
		(struct snd_sof_widget *)hwdep->private_data;
	struct snd_sof_dev *sdev = swidget->sdev;

	/* TODO: send ipc and get data from DSP */
	dev_dbg(sdev->dev,"reading hwdep to userspace\n");
        return 0;
}

static long sof_hwdep_write(struct snd_hwdep *hwdep, const char __user *data, long count,
			    loff_t *offset)
{
	struct snd_sof_widget *swidget =
		(struct snd_sof_widget *)hwdep->private_data;
	struct snd_sof_dev *sdev = swidget->sdev;
	struct sof_ipc_ctrl_data *cdata;
	struct sof_abi_hdr *hdr;
	size_t msg_bytes, hdr_bytes, elems;
	unsigned int size;
	int ret, err;

	if (copy_from_user(&size, data, sizeof(unsigned int)))
		return -EFAULT;

	cdata = kzalloc(size + sizeof(struct sof_ipc_ctrl_data), GFP_KERNEL);
	if (!cdata)
		return -ENOMEM;

	/* copy data */
	if (copy_from_user(cdata->data, data + sizeof(unsigned int), size)) {
		ret = -EFAULT;
		goto out;
	}

	hdr = (struct sof_abi_hdr *)cdata->data;

	/* check ABI compatibility */
	if (hdr->magic != SOF_ABI_MAGIC) {
		dev_err_ratelimited(sdev->dev,
				    "error: Wrong ABI magic 0x%08x.\n",
				    hdr->magic);
		return -EINVAL;
	}

	if (SOF_ABI_VERSION_INCOMPATIBLE(SOF_ABI_VERSION, hdr->abi)) {
		dev_err_ratelimited(sdev->dev, "error: Incompatible ABI version 0x%08x.\n",
				    hdr->abi);
		ret = -EINVAL;
		goto out;
	}

	ret = pm_runtime_get_sync(sdev->dev);
	if (ret < 0) {
		dev_err_ratelimited(sdev->dev,
				    "error: bytes_ext put failed to resume %d\n",
				    ret);
		pm_runtime_put_noidle(sdev->dev);
		goto out;
	}

	/* config ipc */
	cdata->rhdr.hdr.cmd = SOF_IPC_GLB_COMP_MSG | SOF_IPC_COMP_SET_DATA;
	cdata->cmd = SOF_CTRL_CMD_BINARY;
	cdata->type = SOF_CTRL_TYPE_DATA_SET;
	cdata->comp_id = swidget->comp_id;
	cdata->msg_index = 0;

	msg_bytes = cdata->data->size;
	hdr_bytes = sizeof(struct sof_ipc_ctrl_data) +
		    sizeof(struct sof_abi_hdr);
	elems = cdata->data->size;

	/* send IPC */
	ret = snd_sof_ipc_get_set_data(sdev->ipc, cdata, msg_bytes, hdr_bytes,
				       elems, true);

	pm_runtime_mark_last_busy(sdev->dev);
	err = pm_runtime_put_autosuspend(sdev->dev);
	if (err < 0)
		dev_err_ratelimited(sdev->dev,
				    "error: bytes_ext put failed to idle %d\n",
				    err);

out:
	kfree(cdata);
	return ret;
}

static struct snd_hwdep_ops sof_hwdep_ops = {
	.read           = sof_hwdep_read,
	.write          = sof_hwdep_write,
	.ioctl          = sof_hwdep_ioctl,
};

int snd_sof_hwdep_create(struct snd_card *card, struct snd_sof_widget *swidget)
{
	struct snd_sof_dev *sdev = swidget->sdev;
	struct snd_hwdep *hwdep;
	int ret;

	ret = snd_hwdep_new(card, swidget->widget->name, swidget->comp_id,
			    &hwdep);
	if (ret < 0) {
		dev_err(sdev->dev,"error: creating hwdep dev for widget %s\n",
			swidget->widget->name);
		return ret;
	}

	hwdep->private_data = swidget;
	hwdep->ops = sof_hwdep_ops;
	hwdep->exclusive = 1;

	return 0;
}
EXPORT_SYMBOL(snd_sof_hwdep_create);
