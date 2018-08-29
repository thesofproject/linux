/*
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * Copyright(c) 2017 Intel Corporation. All rights reserved.
 *
 *  Author: Libin Yang <libin.yang@intel.com>
 *	  Liam Girdwood <liam.r.girdwood@linux.intel.com>
 */

#ifndef __SOUND_SOF_VIRTIO_MISCDEV_H
#define __SOUND_SOF_VIRTIO_MISCDEV_H

struct virtio_miscdev {
	struct device *dev;
	int (*open)(struct file *f, void *data);
	long (*ioctl)(struct file *f, void *data, unsigned int ioctl,
		      unsigned long arg);
	int (*release)(struct file *f, void *data);
	void *priv;
};

int snd_audio_virtio_miscdev_register(struct device *dev, void *data,
				      struct virtio_miscdev **va);
int snd_audio_virtio_miscdev_unregister(void);

#endif	/* __SOUND_SOF_VIRTIO_MISCDEV_H */
