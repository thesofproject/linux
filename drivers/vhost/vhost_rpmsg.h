/* SPDX-License-Identifier: (GPL-2.0) */
/*
 * Copyright(c) 2020 Intel Corporation. All rights reserved.
 *
 * Author: Guennadi Liakhovetski <guennadi.liakhovetski@linux.intel.com>
 */

#ifndef VHOST_RPMSG_H
#define VHOST_RPMSG_H

#include <linux/uio.h>
#include <linux/virtio_rpmsg.h>

#include "vhost.h"

/* RPMsg uses two VirtQueues: one for each direction */
enum {
	VIRTIO_RPMSG_RESPONSE,	/* RPMsg response (host->guest) buffers */
	VIRTIO_RPMSG_REQUEST,	/* RPMsg request (guest->host) buffers */
	/* Keep last */
	VIRTIO_RPMSG_NUM_OF_VQS,
};

struct vhost_rpmsg_ept;

struct vhost_rpmsg_iter {
	struct iov_iter iov_iter;
	struct rpmsg_hdr rhdr;
	struct vhost_virtqueue *vq;
	const struct vhost_rpmsg_ept *ept;
	int head;
	void *priv;
};

struct vhost_rpmsg {
	struct vhost_dev dev;
	struct vhost_virtqueue vq[VIRTIO_RPMSG_NUM_OF_VQS];
	struct vhost_virtqueue *vq_p[VIRTIO_RPMSG_NUM_OF_VQS];
	const struct vhost_rpmsg_ept *ept;
	unsigned int n_epts;
};

struct vhost_rpmsg_ept {
	ssize_t (*read)(struct vhost_rpmsg *, struct vhost_rpmsg_iter *);
	ssize_t (*write)(struct vhost_rpmsg *, struct vhost_rpmsg_iter *);
	int addr;
};

static inline size_t vhost_rpmsg_iter_len(const struct vhost_rpmsg_iter *iter)
{
	return iter->rhdr.len;
}

#define VHOST_RPMSG_ITER(_src, _dst) {	\
	.rhdr = {			\
			.src = _src,	\
			.dst = _dst,	\
		},			\
	}

void vhost_rpmsg_init(struct vhost_rpmsg *vr, const struct vhost_rpmsg_ept *ept,
		      unsigned int n_epts);
void vhost_rpmsg_destroy(struct vhost_rpmsg *vr);
int vhost_rpmsg_ns_announce(struct vhost_rpmsg *vr, const char *name,
			    unsigned int src);
int vhost_rpmsg_start_lock(struct vhost_rpmsg *vr,
			   struct vhost_rpmsg_iter *iter,
			   unsigned int qid, ssize_t len);
size_t vhost_rpmsg_copy(struct vhost_rpmsg *vr, struct vhost_rpmsg_iter *iter,
			void *data, size_t size);
int vhost_rpmsg_finish_unlock(struct vhost_rpmsg *vr,
			      struct vhost_rpmsg_iter *iter);

#endif
