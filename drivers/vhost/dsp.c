// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
//
// This file is provided under a dual BSD/GPLv2 license.  When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2019 Intel Corporation. All rights reserved.
//
// Author: Guennadi Liakhovetski <guennadi.liakhovetski@linux.intel.com>
//
// vhost-SOF VirtIO interface

#include <linux/compat.h>
#include <linux/file.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/vhost.h>
#include <linux/workqueue.h>

#include <sound/sof/virtio.h>

#include "dsp.h"
#include "vhost.h"

#include "../sound/soc/sof/sof-priv.h"

#define VHOST_DSP_FEATURES VHOST_FEATURES
#define VHOST_DSP_BATCH 64
#define VHOST_DSP_WEIGHT 0x80000
#define VHOST_DSP_PKT_WEIGHT 256

static struct snd_sof_dev *vhost_dsp_sdev;
static DEFINE_MUTEX(vhost_dsp_lock);

static int vhost_dsp_set_features(struct vhost_dsp *dsp, u64 features)
{
	struct vhost_virtqueue *vq;
	unsigned int i;

	if (features & ~VHOST_DSP_FEATURES)
		return -EOPNOTSUPP;

	mutex_lock(&dsp->dev.mutex);
	if ((features & (1 << VHOST_F_LOG_ALL)) &&
	    !vhost_log_access_ok(&dsp->dev)) {
		mutex_unlock(&dsp->dev.mutex);
		return -EFAULT;
	}

	for (i = 0; i < SOF_VIRTIO_NUM_OF_VQS; i++) {
		vq = &dsp->vqs[i].vq;
		mutex_lock(&vq->mutex);
		vq->acked_features = features;
		mutex_unlock(&vq->mutex);
	}
	mutex_unlock(&dsp->dev.mutex);
	return 0;
}

static long vhost_dsp_ioctl(struct file *f, unsigned int ioctl,
			      unsigned long arg)
{
	struct vhost_dsp *dsp = f->private_data;
	void __user *argp = (void __user *)arg;
	u64 __user *featurep = argp;
	u64 features;
	long ret;

	pr_debug("%s(): %x\n", __func__, ioctl);

	switch (ioctl) {
	case VHOST_GET_FEATURES:
		features = VHOST_DSP_FEATURES;
		if (copy_to_user(featurep, &features, sizeof features))
			return -EFAULT;
		return 0;
	case VHOST_SET_FEATURES:
		if (copy_from_user(&features, featurep, sizeof features))
			return -EFAULT;
		pr_debug("%s(): features %llx\n", __func__, features);
		return vhost_dsp_set_features(dsp, features);
	case VHOST_GET_BACKEND_FEATURES:
		features = 0;
		if (copy_to_user(featurep, &features, sizeof(features)))
			return -EFAULT;
		return 0;
	case VHOST_SET_BACKEND_FEATURES:
		if (copy_from_user(&features, featurep, sizeof(features)))
			return -EFAULT;
		if (features)
			return -EOPNOTSUPP;
		return 0;
	case VHOST_RESET_OWNER:
		mutex_lock(&dsp->dev.mutex);
		ret = vhost_dev_check_owner(&dsp->dev);
		if (!ret) {
			struct vhost_umem *umem = vhost_dev_reset_owner_prepare();
			if (!umem) {
				ret = -ENOMEM;
			} else {
				vhost_dev_stop(&dsp->dev);
				vhost_dev_reset_owner(&dsp->dev, umem);
			}
		}
		mutex_unlock(&dsp->dev.mutex);
		return ret;
	case VHOST_SET_OWNER:
		mutex_lock(&dsp->dev.mutex);
		ret = vhost_dev_has_owner(&dsp->dev) ? -EBUSY :
			vhost_dev_set_owner(&dsp->dev);
		mutex_unlock(&dsp->dev.mutex);
		return ret;
	default:
		mutex_lock(&dsp->dev.mutex);
		ret = vhost_dev_ioctl(&dsp->dev, ioctl, argp);
		if (ret == -ENOIOCTLCMD)
			ret = vhost_vring_ioctl(&dsp->dev, ioctl, argp);
		mutex_unlock(&dsp->dev.mutex);
		return ret;
	}

	return 0;
}

#ifdef CONFIG_COMPAT
static long vhost_dsp_compat_ioctl(struct file *f, unsigned int ioctl,
				     unsigned long arg)
{
	return vhost_dsp_ioctl(f, ioctl, (unsigned long)compat_ptr(arg));
}
#endif

static ssize_t vhost_dsp_chr_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	struct file *file = iocb->ki_filp;
	struct vhost_dsp *dsp = file->private_data;
	struct vhost_dev *dev = &dsp->dev;
	int noblock = file->f_flags & O_NONBLOCK;

	return vhost_chr_read_iter(dev, to, noblock);
}

static ssize_t vhost_dsp_chr_write_iter(struct kiocb *iocb,
					struct iov_iter *from)
{
	struct file *file = iocb->ki_filp;
	struct vhost_dsp *dsp = file->private_data;
	struct vhost_dev *dev = &dsp->dev;

	return vhost_chr_write_iter(dev, from);
}

static __poll_t vhost_dsp_chr_poll(struct file *file, poll_table *wait)
{
	struct vhost_dsp *dsp = file->private_data;
	struct vhost_dev *dev = &dsp->dev;

	return vhost_chr_poll(file, dev, wait);
}

static void handle_ipc_cmd_kick(struct vhost_work *work)
{
	struct vhost_virtqueue *vq = container_of(work, struct vhost_virtqueue,
						  poll.work);
	struct vhost_dsp *dsp = container_of(vq->dev, struct vhost_dsp, dev);
	int vq_idx = SOF_VIRTIO_IPC_CMD_VQ;
	size_t total_len = 0;

	/* IPC message from the guest */
	mutex_lock(&vq->mutex);

	vhost_disable_notify(&dsp->dev, vq);

	for (;;) {
		struct iov_iter iov_iter;
		size_t len, nbytes;
		unsigned int out, in, i;
		size_t iov_offset, iov_count;
		/* IPC command from FE to DSP */
		int head = vhost_get_vq_desc(vq, vq->iov, ARRAY_SIZE(vq->iov),
					     &out, &in, NULL, NULL);
		if (head < 0)
			break;

		/* Nothing new?  Wait for eventfd to tell us they refilled. */
		if (head == vq->num) {
			if (unlikely(vhost_enable_notify(&dsp->dev, vq))) {
				vhost_disable_notify(&dsp->dev, vq);
				continue;
			}
			break;
		}

		if (in != out)
			/* We expect in == out and usually == 1 */
			continue;

		iov_offset = out;
		iov_count = out;

		for (i = 0; i < iov_count; i++) {
			struct sof_ipc_reply *rhdr = (struct sof_ipc_reply *)dsp->reply_buf;
			size_t to_copy;

			len = vq->iov[i].iov_len;

			if (len > sizeof(dsp->ipc_buf)) {
				dev_err(vhost_dsp_sdev->dev,
					"%s(): head %d out %d in %d len %zd\n",
					__func__, head, out, in, len);
				continue;
			}

			total_len += len;

			iov_iter_init(&iov_iter, WRITE, vq->iov + i, 1, len);

			nbytes = copy_from_iter(dsp->ipc_buf, len, &iov_iter);
			if (nbytes != len) {
				vq_err(vq, "Expected %zu bytes for IPC, got %zu bytes\n",
				       len, nbytes);
				continue;
			}

			dsp_sof_ipc_fwd(dsp, vq_idx, dsp->ipc_buf, dsp->reply_buf,
					len, vq->iov[iov_offset + i].iov_len);

			to_copy = min_t(size_t, sizeof(dsp->reply_buf),
					rhdr->hdr.size);

			iov_iter_init(&iov_iter, READ, vq->iov + iov_offset + i,
				      1, to_copy);
			if (copy_to_iter(dsp->reply_buf, to_copy, &iov_iter) > 0)
				vhost_add_used_and_signal(vq->dev, vq, head, to_copy);
		}
	}

	mutex_unlock(&vq->mutex);
}

/*
 * This function will be called when there is a poistion update requirement
 * from vBE. Return true if posn buffer is filled successfully
 */
static bool sbe_fill_posn_vqbuf(struct vhost_dsp *dsp)
{
	struct vhost_virtqueue *vq = &dsp->vqs[SOF_VIRTIO_IPC_PSN_VQ].vq;
	struct device *dev = dsp->sdev->dev;
	struct iov_iter iov_iter;
	struct vhost_dsp_iovec *buf;
	struct vhost_dsp_posn *entry;
	unsigned long flags;
	unsigned int out, in;
	int head;

	spin_lock_irqsave(&dsp->posn_lock, flags);

	if (list_empty(&dsp->posn_list)) {
		spin_unlock_irqrestore(&dsp->posn_lock, flags);
		return false;
	}

	entry = list_first_entry(&dsp->posn_list, struct vhost_dsp_posn, list);
	list_del(&entry->list);

	if (list_empty(&dsp->posn_buf_list)) {
		dev_warn(dev, "%s(): no vq descriptors\n",
			 __func__);
		buf = NULL;
	} else {
		buf = list_first_entry(&dsp->posn_buf_list, struct vhost_dsp_iovec,
				       list);
		list_del(&buf->list);
	}

	spin_unlock_irqrestore(&dsp->posn_lock, flags);

	if (buf) {
		head = buf->head;
		kfree(buf);
		out = 0;
	} else {
		/*
		 * FIXME: we should just bail out here. When a buffer arrives,
		 * this function will be called again from the kick handler, no
		 * need to double-check here.
		 */
		vhost_disable_notify(&dsp->dev, vq);
		head = vhost_get_vq_desc(vq, vq->iov, ARRAY_SIZE(vq->iov),
					 &out, &in, NULL, NULL);
		vhost_enable_notify(&dsp->dev, vq);

		if (head < 0 || head == vq->num) {
			spin_lock_irqsave(&dsp->posn_lock, flags);
			list_add(&entry->list, &dsp->posn_list);
			spin_unlock_irqrestore(&dsp->posn_lock, flags);

			dev_warn(dev, "%s(): no vq descriptors: %d\n",
				 __func__, head);
			return false;
		}

		if (unlikely(out))
			dev_warn(dev,
				 "%s(): position update has %d outgoing buffers!\n",
				 __func__, out);

		if (unlikely(vq->iov[out].iov_len != sizeof(entry->posn)))
			dev_warn(dev,
				 "%s(): position update has wrong size %d!\n",
				 __func__, out);

		if (!in) {
			/* This queue should only contain "in" buffers */
			dev_warn(dev, "%s(): no input buffers!\n", __func__);
			kfree(entry);
			return false;
		}
	}

	iov_iter_init(&iov_iter, READ, vq->iov + out, 1, sizeof(entry->posn));
	if (copy_to_iter(&entry->posn, sizeof(entry->posn), &iov_iter) > 0)
		/*
		 * Actually the last parameter for vhost_add_used_and_signal()
		 * should be "sizeof(*posn)," but that breaks the VirtIO
		 */
		vhost_add_used_and_signal(vq->dev, vq, head, 0);

	kfree(entry);

	return true;
}

static void handle_data_kick(struct vhost_work *work)
{
	struct vhost_virtqueue *vq = container_of(work, struct vhost_virtqueue,
						  poll.work);
	struct vhost_dsp *dsp = container_of(vq->dev, struct vhost_dsp, dev);
	struct snd_sof_dev *sdev = dsp->sdev;

	mutex_lock(&vq->mutex);

	vhost_disable_notify(&dsp->dev, vq);

	for (;;) {
		struct iov_iter iov_iter;
		unsigned int out, in, i;
		int head = vhost_get_vq_desc(vq, vq->iov, ARRAY_SIZE(vq->iov),
					     &out, &in, NULL, NULL);
		if (head < 0)
			break;

		/* Nothing new?  Wait for eventfd to tell us they refilled. */
		if (head == vq->num) {
			if (unlikely(vhost_enable_notify(&dsp->dev, vq))) {
				vhost_disable_notify(&dsp->dev, vq);
				continue;
			}
			break;
		}

		if (in != out)
			/* We expect in == out and usually == 1 */
			continue;

		for (i = 0; i < out; i++) {
			u8 _req[HDR_SIZE_REQ];
			u8 _resp[HDR_SIZE_RESP];
			struct dsp_sof_data_resp *resp;
			struct dsp_sof_data_req *req;
			size_t to_copy, nbytes, len = vq->iov[i].iov_len;
			int ret;

			if (len > sizeof(dsp->data_req) || len < HDR_SIZE_REQ) {
				dev_err(vhost_dsp_sdev->dev,
					"%s(): head %d out %d in %d len %zd\n",
					__func__, head, out, in, len);
				continue;
			}

			iov_iter_init(&iov_iter, WRITE, vq->iov + i, 1, len);

			if (len > HDR_SIZE_REQ) {
				/* playback */
				req = &dsp->data_req;
				resp = (struct dsp_sof_data_resp *)_resp;
			} else {
				/* capture */
				req = (struct dsp_sof_data_req *)_req;
				resp = &dsp->data_resp;
			}

			nbytes = copy_from_iter(req, len, &iov_iter);
			if (nbytes != len) {
				vq_err(vq, "Expected %zu bytes for IPC, got %zu bytes\n",
				       len, nbytes);
				continue;
			}

			ret = dsp_sof_ipc_stream_data(sdev, req, resp);
			if (ret < 0) {
				vq_err(vq, "Error %d copying data\n", ret);
				continue;
			}

			to_copy = resp->size + HDR_SIZE_RESP;

			iov_iter_init(&iov_iter, READ, vq->iov + out + i,
				      1, to_copy);
			if (copy_to_iter(resp, to_copy, &iov_iter) > 0)
				vhost_add_used_and_signal(vq->dev, vq, head, to_copy);
		}
	}

	mutex_unlock(&vq->mutex);
}

static void handle_ipc_psn_kick(struct vhost_work *work)
{
	struct vhost_virtqueue *vq = container_of(work, struct vhost_virtqueue,
						  poll.work);
	struct vhost_dsp *dsp = container_of(vq->dev, struct vhost_dsp, dev);
	struct device *dev = dsp->sdev->dev;
	struct vhost_dsp_iovec *buf;
	unsigned int out, in;
	unsigned long flags;

	/* A new VQ buffer from a guest */

	if (!list_empty(&dsp->posn_list)) {
		/* We have a position update waiting, send immediately */
		sbe_fill_posn_vqbuf(dsp);
		return;
	}

	/* Queue the buffer for future position updates from the DSP */
	buf = kmalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf)
		return;

	vhost_disable_notify(&dsp->dev, vq);
	buf->head = vhost_get_vq_desc(vq, vq->iov, ARRAY_SIZE(vq->iov),
				      &out, &in, NULL, NULL);
	vhost_enable_notify(&dsp->dev, vq);

	if (buf->head < 0) {
		dev_warn(dev, "%s(): no vq descriptors: %d\n",
			 __func__, buf->head);
		kfree(buf);

		return;
	}

	if (unlikely(out))
		dev_warn(dev,
			 "%s(): position update has %d outgoing buffers!\n",
			 __func__, out);

	if (unlikely(vq->iov[out].iov_len !=
		     sizeof(struct sof_ipc_stream_posn)))
		dev_warn(dev, "%s(): position update has wrong size %d!\n",
			 __func__, out);

	if (!in) {
		/* This queue should only contain "in" buffers */
		dev_warn(dev, "%s(): no input buffers!\n", __func__);
		kfree(buf);

		return;
	}

	spin_lock_irqsave(&dsp->posn_lock, flags);
	list_add_tail(&buf->list, &dsp->posn_buf_list);
	spin_unlock_irqrestore(&dsp->posn_lock, flags);
}

static void vhost_dsp_posn_work(struct vhost_work *work)
{
	struct vhost_dsp *dsp = container_of(work, struct vhost_dsp, work);

	/*
	 * let's try to get a notification RX vq available buffer
	 * If there is an available buffer, let's notify immediately
	 */
	sbe_fill_posn_vqbuf(dsp);
}

static int vhost_dsp_open(struct inode *inode, struct file *f)
{
	struct vhost_dsp *dsp;
	int i;

	dsp = kvmalloc(sizeof(*dsp), GFP_KERNEL | __GFP_RETRY_MAYFAIL);
	if (!dsp)
		return -ENOMEM;

	for (i = 0; i < ARRAY_SIZE(dsp->vq_p); i++)
		dsp->vq_p[i] = &dsp->vqs[i].vq;

	dsp->vqs[SOF_VIRTIO_IPC_CMD_VQ].vq.handle_kick = handle_ipc_cmd_kick;
	dsp->vqs[SOF_VIRTIO_IPC_PSN_VQ].vq.handle_kick = handle_ipc_psn_kick;
	dsp->vqs[SOF_VIRTIO_DATA_VQ].vq.handle_kick = handle_data_kick;
	/*
	 * TODO: do we ever want to support multiple guest machines per DSP, if
	 * not, we might as well perform all allocations when registering the
	 * misc device.
	 */
	INIT_LIST_HEAD(&dsp->pipe_conn);
	INIT_LIST_HEAD(&dsp->posn_list);
	INIT_LIST_HEAD(&dsp->posn_buf_list);
	spin_lock_init(&dsp->posn_lock);
	vhost_work_init(&dsp->work, vhost_dsp_posn_work);

	vhost_dev_init(&dsp->dev, dsp->vq_p, SOF_VIRTIO_NUM_OF_VQS,
		       UIO_MAXIOV + VHOST_DSP_BATCH,
		       VHOST_DSP_PKT_WEIGHT, VHOST_DSP_WEIGHT);

	/*
	 * link to sdev->vbe_list
	 * Maybe virtio_miscdev managing the list is more reasonable.
	 * Let's use sdev to manage the FE audios now.
	 */
	list_add(&dsp->list, &vhost_dsp_sdev->vbe_list);

	dsp->sdev = vhost_dsp_sdev;
	f->private_data = dsp;

	return 0;
}

static int vhost_dsp_release(struct inode *inode, struct file *f)
{
	struct vhost_dsp *dsp = f->private_data;

	list_del(&dsp->list);
	vhost_work_flush(&dsp->dev, &dsp->work);
	vhost_dev_cleanup(&dsp->dev);
	vhost_poll_flush(&dsp->vqs[SOF_VIRTIO_IPC_PSN_VQ].vq.poll);
	vhost_poll_flush(&dsp->vqs[SOF_VIRTIO_IPC_CMD_VQ].vq.poll);
	vhost_poll_flush(&dsp->vqs[SOF_VIRTIO_DATA_VQ].vq.poll);
	kvfree(dsp);

	return 0;
}

static const struct file_operations vhost_dsp_fops = {
	.owner          = THIS_MODULE,
	.release        = vhost_dsp_release,
	.read_iter      = vhost_dsp_chr_read_iter,
	.write_iter     = vhost_dsp_chr_write_iter,
	.poll           = vhost_dsp_chr_poll,
	.unlocked_ioctl = vhost_dsp_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl   = vhost_dsp_compat_ioctl,
#endif
	.open           = vhost_dsp_open,
	.llseek		= noop_llseek,
};

static struct miscdevice vhost_dsp_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "vhost-sound",
	.fops = &vhost_dsp_fops,
};

int dsp_sof_virtio_miscdev_register(struct snd_sof_dev *sdev)
{
	int ret;

	mutex_lock(&vhost_dsp_lock);
	/* Could make it a list if needed */
	if (vhost_dsp_sdev) {
		mutex_unlock(&vhost_dsp_lock);
		return -EBUSY;
	}

	ret = misc_register(&vhost_dsp_misc);
	if (!ret)
		vhost_dsp_sdev = sdev;

	mutex_unlock(&vhost_dsp_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(dsp_sof_virtio_miscdev_register);

int dsp_sof_virtio_miscdev_unregister(void)
{
	mutex_lock(&vhost_dsp_lock);
	if (!vhost_dsp_sdev) {
		mutex_unlock(&vhost_dsp_lock);
		return -ENODEV;
	}

	misc_deregister(&vhost_dsp_misc);
	vhost_dsp_sdev = NULL;
	mutex_unlock(&vhost_dsp_lock);

	return 0;
}
EXPORT_SYMBOL_GPL(dsp_sof_virtio_miscdev_unregister);

MODULE_VERSION("0.5");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Guennadi Liakhovetski");
MODULE_DESCRIPTION("Host kernel accelerator for virtio sound");
