// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
/*
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * Copyright(c) 2017 Intel Corporation. All rights reserved.
 *
 *  Author: Libin Yang <libin.yang@intel.com>
 *	  Luo Xionghu <xionghu.luo@intel.com>
 *	  Liam Girdwood <liam.r.girdwood@linux.intel.com>
 */

#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/slab.h>

#include "sof-priv.h"
#include "virtio-miscdev.h"
#include <linux/vbs/vbs.h>

/*
 * This module registers a device node /dev/vbs_k_audio,
 * that handle the communication between Device Model and
 * the virtio backend service. The device model can
 * control the backend to : set the status, set the vq account and etc.
 * The config of the DM and VBS must be accordance.
 */

static struct virtio_miscdev *virtio_audio;

static struct virtio_miscdev *get_virtio_audio(void)
{
	return virtio_audio;
}

void *get_sof_dev(void)
{
	struct virtio_miscdev *vaudio = get_virtio_audio();

	if (vaudio)
		return vaudio->priv;

	return NULL;
}

/* To be implemented later for sof related */
static int sof_virtio_open(struct file *f, void *data)
{
	return 0;
}

/* To be implemented later for sof related */
static long sof_virtio_ioctl(struct file *f, void *data, unsigned int ioctl,
			     unsigned long arg)
{
	return 0;
}

/* To be implemented later for sof related */
static int sof_virtio_release(struct file *f, void *data)
{
	return 0;
}

int snd_sof_virtio_miscdev_register(struct snd_sof_dev *sdev)
{
	struct virtio_miscdev *vaudio;
	int ret;

	ret = snd_audio_virtio_miscdev_register(sdev->dev, sdev, &vaudio);
	if (ret)
		return ret;

	vaudio->open = sof_virtio_open;
	vaudio->ioctl = sof_virtio_ioctl;
	vaudio->release = sof_virtio_release;

	return 0;
}

int snd_sof_virtio_miscdev_unregister(void)
{
	return snd_audio_virtio_miscdev_unregister();
}

static int vbs_audio_open(struct inode *inode, struct file *f)
{
	struct virtio_miscdev *vaudio = get_virtio_audio();

	if (!vaudio)
		return -ENODEV;	/* This should never happen */

	dev_dbg(vaudio->dev, "virtio audio open\n");
	if (vaudio->open)
		return vaudio->open(f, virtio_audio->priv);

	return 0;
}

static long vbs_audio_ioctl(struct file *f, unsigned int ioctl,
			    unsigned long arg)
{
	struct virtio_miscdev *vaudio = get_virtio_audio();

	if (!vaudio)
		return -ENODEV;	/* This should never happen */

	dev_dbg(vaudio->dev, "virtio audio ioctl\n");
	if (vaudio->ioctl)
		return vaudio->ioctl(f, vaudio->priv, ioctl, arg);
	else
		return -ENXIO;
}

static int vbs_audio_release(struct inode *inode, struct file *f)
{
	struct virtio_miscdev *vaudio = get_virtio_audio();

	if (!vaudio)
		return -ENODEV;	/* This should never happen */

	dev_dbg(vaudio->dev, "release virtio audio\n");

	if (vaudio->release)
		vaudio->release(f, vaudio->priv);

	return 0;
}

static const struct file_operations vbs_audio_fops = {
	.owner          = THIS_MODULE,
	.release        = vbs_audio_release,
	.unlocked_ioctl = vbs_audio_ioctl,
	.open           = vbs_audio_open,
	.llseek		= noop_llseek,
};

static struct miscdevice vbs_audio_k = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "vbs_k_audio",
	.fops = &vbs_audio_fops,
};

/**
 * snd_audio_virtio_miscdev_register() - init the virtio be audio driver
 * @dev: the device the audio virtio belongs to
 * @data: the priv data of virtio_miscdev
 * @va: allocated virtio_miscdev, return to caller
 *
 * This function registers the misc device, which will be used
 * by the user space to communicate with the audio driver.
 *
 * Return: 0 for success or negative value for err
 */
int snd_audio_virtio_miscdev_register(struct device *dev, void *data,
				      struct virtio_miscdev **va)
{
	struct virtio_miscdev *vaudio;
	int ret;

	ret = misc_register(&vbs_audio_k);
	if (ret) {
		dev_err(dev, "misc device register failed %d\n", ret);
		return ret;
	}

	vaudio = kzalloc(sizeof(*vaudio), GFP_KERNEL);
	if (!vaudio) {
		misc_deregister(&vbs_audio_k);
		return -ENOMEM;
	}

	vaudio->priv = data;
	vaudio->dev = dev;
	virtio_audio = vaudio;
	*va = vaudio;

	return 0;
}
EXPORT_SYMBOL(snd_audio_virtio_miscdev_register);

/**
 * snd_audio_virtio_miscdev_unregisger() - release the virtio be audio driver
 *
 * This function deregisters the misc device, and free virtio_miscdev
 *
 */
int snd_audio_virtio_miscdev_unregister(void)
{
	if (virtio_audio) {
		misc_deregister(&vbs_audio_k);
		kfree(virtio_audio);
		virtio_audio = NULL;
	}

	return 0;
}
EXPORT_SYMBOL(snd_audio_virtio_miscdev_unregister);
