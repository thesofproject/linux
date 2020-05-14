/* SPDX-License-Identifier: (GPL-2.0-only) */
/*
 * Copyright(c) 2020 Intel Corporation. All rights reserved.
 *
 * Author: Guennadi Liakhovetski <guennadi.liakhovetski@linux.intel.com>
 *
 * vhost-RPMsg VirtIO interface
 */

#include <linux/compat.h>
#include <linux/file.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/vhost.h>
#include <linux/virtio_rpmsg.h>
#include <uapi/linux/rpmsg.h>

#include "vhost.h"
#include "vhost_rpmsg.h"

/*
 * All virtio-rpmsg virtual queue kicks always come with just one buffer -
 * either input or output
 */
static int vhost_rpmsg_get_single(struct vhost_virtqueue *vq)
{
	struct vhost_rpmsg *vr = container_of(vq->dev, struct vhost_rpmsg, dev);
	unsigned int out, in;
	int head = vhost_get_vq_desc(vq, vq->iov, ARRAY_SIZE(vq->iov),
				     &out, &in, NULL, NULL);
	if (head < 0) {
		vq_err(vq, "%s(): error %d getting buffer\n",
		       __func__, head);
		return head;
	}

	/* Nothing new? */
	if (head == vq->num)
		return head;

	if (vq == &vr->vq[VIRTIO_RPMSG_RESPONSE] && (out || in != 1)) {
		vq_err(vq,
		       "%s(): invalid %d input and %d output in response queue\n",
		       __func__, in, out);
		goto return_buf;
	}

	if (vq == &vr->vq[VIRTIO_RPMSG_REQUEST] && (in || out != 1)) {
		vq_err(vq,
		       "%s(): invalid %d input and %d output in request queue\n",
		       __func__, in, out);
		goto return_buf;
	}

	return head;

return_buf:
	/*
	 * FIXME: might need to return the buffer using vhost_add_used()
	 * or vhost_discard_vq_desc(). vhost_discard_vq_desc() is
	 * described as "being useful for error handling," but it makes
	 * the thus discarded buffers "unseen," so next time we look we
	 * retrieve them again?
	 */
	return -EINVAL;
}

static const struct vhost_rpmsg_ept *vhost_rpmsg_ept_find(struct vhost_rpmsg *vr,
							  int addr)
{
	unsigned int i;

	for (i = 0; i < vr->n_epts; i++)
		if (vr->ept[i].addr == addr)
			return vr->ept + i;

	return NULL;
}

/*
 * if len < 0, then for reading a request, the complete virtual queue buffer
 * size is prepared, for sending a response, the length in the iterator is used
 */
int vhost_rpmsg_start_lock(struct vhost_rpmsg *vr,
			   struct vhost_rpmsg_iter *iter,
			   unsigned int qid, ssize_t len)
	__acquires(vq->mutex)
{
	struct vhost_virtqueue *vq = vr->vq + qid;
	size_t tmp;

	if (qid >= VIRTIO_RPMSG_NUM_OF_VQS)
		return -EINVAL;

	iter->vq = vq;

	mutex_lock(&vq->mutex);
	vhost_disable_notify(&vr->dev, vq);

	iter->head = vhost_rpmsg_get_single(vq);
	if (iter->head == vq->num)
		iter->head = -EAGAIN;

	if (iter->head < 0)
		goto unlock;

	tmp = vq->iov[0].iov_len;
	if (tmp < sizeof(iter->rhdr)) {
		vq_err(vq, "%s(): size %zu too small\n", __func__, tmp);
		iter->head = -ENOBUFS;
		goto return_buf;
	}

	switch (qid) {
	case VIRTIO_RPMSG_REQUEST:
		if (len < 0) {
			len = tmp - sizeof(iter->rhdr);
		} else if (tmp < sizeof(iter->rhdr) + len) {
			iter->head = -ENOBUFS;
			goto return_buf;
		}

		/* len is now the size of the payload */
		iov_iter_init(&iter->iov_iter, WRITE,
			      vq->iov, 1, sizeof(iter->rhdr) + len);

		/* Read the RPMSG header with endpoint addresses */
		tmp = copy_from_iter(&iter->rhdr, sizeof(iter->rhdr),
				     &iter->iov_iter);
		if (tmp != sizeof(iter->rhdr)) {
			vq_err(vq, "%s(): got %zu instead of %zu\n", __func__,
			       tmp, sizeof(iter->rhdr));
			iter->head = -EIO;
			goto return_buf;
		}

		iter->ept = vhost_rpmsg_ept_find(vr, iter->rhdr.dst);
		if (!iter->ept) {
			vq_err(vq, "%s(): no endpoint with address %d\n",
			       __func__, iter->rhdr.dst);
			iter->head = -ENOENT;
			goto return_buf;
		}

		/* Let the endpoint read the payload */
		if (iter->ept->read) {
			ssize_t ret = iter->ept->read(vr, iter);
			if (ret < 0) {
				iter->head = ret;
				goto return_buf;
			}

			iter->rhdr.len = ret;
		} else {
			iter->rhdr.len = 0;
		}

		/* Prepare for the response phase */
		iter->rhdr.dst = iter->rhdr.src;
		iter->rhdr.src = iter->ept->addr;

		break;
	case VIRTIO_RPMSG_RESPONSE:
		if (!iter->ept && iter->rhdr.dst != RPMSG_NS_ADDR) {
			/*
			 * Usually the iterator is configured when processing a
			 * message on the request queue, but it's also possible
			 * to send a message on the response queue without a
			 * preceding request, in that case the iterator must
			 * contain source and destination addresses.
			 */
			iter->ept = vhost_rpmsg_ept_find(vr, iter->rhdr.src);
			if (!iter->ept) {
				iter->head = -ENOENT;
				goto return_buf;
			}
		}

		if (len < 0) {
			len = tmp - sizeof(iter->rhdr);
		} else if (tmp < sizeof(iter->rhdr) + len) {
			iter->head = -ENOBUFS;
			goto return_buf;
		} else {
			iter->rhdr.len = len;
		}

		/* len is now the size of the payload */
		iov_iter_init(&iter->iov_iter, READ,
			      vq->iov, 1, sizeof(iter->rhdr) + len);

		/* Write the RPMSG header with endpoint addresses */
		tmp = copy_to_iter(&iter->rhdr, sizeof(iter->rhdr),
				   &iter->iov_iter);
		if (tmp != sizeof(iter->rhdr)) {
			iter->head = -EIO;
			goto return_buf;
		}

		/* Let the endpoint write the payload */
		if (iter->ept && iter->ept->write) {
			ssize_t ret = iter->ept->write(vr, iter);
			if (ret < 0) {
				iter->head = ret;
				goto return_buf;
			}
		}

		break;
	}

	return 0;

return_buf:
	/*
	 * FIXME: vhost_discard_vq_desc() or vhost_add_used(), see comment in
	 * vhost_rpmsg_get_single()
	 */
unlock:
	vhost_enable_notify(&vr->dev, vq);
	mutex_unlock(&vq->mutex);

	return iter->head;
}
EXPORT_SYMBOL_GPL(vhost_rpmsg_start_lock);

size_t vhost_rpmsg_copy(struct vhost_rpmsg *vr, struct vhost_rpmsg_iter *iter,
			void *data, size_t size)
{
	/*
	 * We could check for excess data, but copy_{to,from}_iter() don't do
	 * that either
	 */
	if (iter->vq == vr->vq + VIRTIO_RPMSG_RESPONSE)
		return copy_to_iter(data, size, &iter->iov_iter);

	return copy_from_iter(data, size, &iter->iov_iter);
}
EXPORT_SYMBOL_GPL(vhost_rpmsg_copy);

int vhost_rpmsg_finish_unlock(struct vhost_rpmsg *vr,
			      struct vhost_rpmsg_iter *iter)
	__releases(vq->mutex)
{
	if (iter->head >= 0)
		vhost_add_used_and_signal(iter->vq->dev, iter->vq, iter->head,
					  iter->rhdr.len + sizeof(iter->rhdr));

	vhost_enable_notify(&vr->dev, iter->vq);
	mutex_unlock(&iter->vq->mutex);

	return iter->head;
}
EXPORT_SYMBOL_GPL(vhost_rpmsg_finish_unlock);

/*
 * Return false to terminate the external loop only if we fail to obtain either
 * a request or a response buffer
 */
static bool handle_rpmsg_req_single(struct vhost_rpmsg *vr,
				    struct vhost_virtqueue *vq)
{
	struct vhost_rpmsg_iter iter;
	int ret = vhost_rpmsg_start_lock(vr, &iter, VIRTIO_RPMSG_REQUEST,
					 -EINVAL);
	if (!ret)
		ret = vhost_rpmsg_finish_unlock(vr, &iter);
	if (ret < 0) {
		if (ret != -EAGAIN)
			vq_err(vq, "%s(): RPMSG processing failed %d\n",
			       __func__, ret);
		return false;
	}

	if (!iter.ept->write)
		return true;

	ret = vhost_rpmsg_start_lock(vr, &iter, VIRTIO_RPMSG_RESPONSE,
				     -EINVAL);
	if (!ret)
		ret = vhost_rpmsg_finish_unlock(vr, &iter);
	if (ret < 0) {
		vq_err(vq, "%s(): RPMSG finalising failed %d\n", __func__, ret);
		return false;
	}

	return true;
}

static void handle_rpmsg_req_kick(struct vhost_work *work)
{
	struct vhost_virtqueue *vq = container_of(work, struct vhost_virtqueue,
						  poll.work);
	struct vhost_rpmsg *vr = container_of(vq->dev, struct vhost_rpmsg, dev);

	while (handle_rpmsg_req_single(vr, vq))
		;
}

/*
 * initialise two virtqueues with an array of endpoints,
 * request and response callbacks
 */
void vhost_rpmsg_init(struct vhost_rpmsg *vr, const struct vhost_rpmsg_ept *ept,
		      unsigned int n_epts)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(vr->vq); i++)
		vr->vq_p[i] = &vr->vq[i];

	/* vq[0]: host -> guest, vq[1]: host <- guest */
	vr->vq[VIRTIO_RPMSG_REQUEST].handle_kick = handle_rpmsg_req_kick;
	vr->vq[VIRTIO_RPMSG_RESPONSE].handle_kick = NULL;

	vr->ept = ept;
	vr->n_epts = n_epts;

	vhost_dev_init(&vr->dev, vr->vq_p, VIRTIO_RPMSG_NUM_OF_VQS,
		       UIO_MAXIOV, 0, 0, NULL);
}
EXPORT_SYMBOL_GPL(vhost_rpmsg_init);

void vhost_rpmsg_destroy(struct vhost_rpmsg *vr)
{
	if (vhost_dev_has_owner(&vr->dev))
		vhost_poll_flush(&vr->vq[VIRTIO_RPMSG_REQUEST].poll);

	vhost_dev_cleanup(&vr->dev);
}
EXPORT_SYMBOL_GPL(vhost_rpmsg_destroy);

/* send namespace */
int vhost_rpmsg_ns_announce(struct vhost_rpmsg *vr, const char *name,
			    unsigned int src)
{
	struct vhost_rpmsg_iter iter = {
		.rhdr = {
			.src = 0,
			.dst = RPMSG_NS_ADDR,
			.flags = RPMSG_NS_CREATE, /* rpmsg_recv_single() */
		},
	};
	struct rpmsg_ns_msg ns = {
		.addr = src,
		.flags = RPMSG_NS_CREATE, /* for rpmsg_ns_cb() */
	};
	int ret = vhost_rpmsg_start_lock(vr, &iter, VIRTIO_RPMSG_RESPONSE,
					 sizeof(ns));

	if (ret < 0)
		return ret;

	strlcpy(ns.name, name, sizeof(ns.name));

	ret = vhost_rpmsg_copy(vr, &iter, &ns, sizeof(ns));
	if (ret != sizeof(ns))
		vq_err(iter.vq, "%s(): added %d instead of %zu bytes\n",
		       __func__, ret, sizeof(ns));

	ret = vhost_rpmsg_finish_unlock(vr, &iter);
	if (ret < 0)
		vq_err(iter.vq, "%s(): namespace announcement failed: %d\n",
		       __func__, ret);

	return ret;
}
EXPORT_SYMBOL_GPL(vhost_rpmsg_ns_announce);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Intel, Inc.");
MODULE_DESCRIPTION("Vhost RPMsg API");
