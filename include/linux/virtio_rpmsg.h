// SPDX-License-Identifier: GPL-2.0

#ifndef _LINUX_VIRTIO_RPMSG_H
#define _LINUX_VIRTIO_RPMSG_H

#include <linux/mod_devicetable.h>

/* Address 53 is reserved for advertising remote services */
#define RPMSG_NS_ADDR			(53)

/*
 * We're allocating buffers of 512 bytes each for communications. The
 * number of buffers will be computed from the number of buffers supported
 * by the vring, upto a maximum of 512 buffers (256 in each direction).
 *
 * Each buffer will have 16 bytes for the msg header and 496 bytes for
 * the payload.
 *
 * This will utilize a maximum total space of 256KB for the buffers.
 *
 * We might also want to add support for user-provided buffers in time.
 * This will allow bigger buffer size flexibility, and can also be used
 * to achieve zero-copy messaging.
 *
 * Note that these numbers are purely a decision of this driver - we
 * can change this without changing anything in the firmware of the remote
 * processor.
 */
#define MAX_RPMSG_NUM_BUFS	512
#define MAX_RPMSG_BUF_SIZE	512

/**
 * struct rpmsg_hdr - common header for all rpmsg messages
 * @src: source address
 * @dst: destination address
 * @reserved: reserved for future use
 * @len: length of payload (in bytes)
 * @flags: message flags
 * @data: @len bytes of message payload data
 *
 * Every message sent(/received) on the rpmsg bus begins with this header.
 */
struct rpmsg_hdr {
	u32 src;
	u32 dst;
	u32 reserved;
	u16 len;
	u16 flags;
	u8 data[0];
} __packed;

/**
 * struct rpmsg_ns_msg - dynamic name service announcement message
 * @name: name of remote service that is published
 * @addr: address of remote service that is published
 * @flags: indicates whether service is created or destroyed
 *
 * This message is sent across to publish a new service, or announce
 * about its removal. When we receive these messages, an appropriate
 * rpmsg channel (i.e device) is created/destroyed. In turn, the ->probe()
 * or ->remove() handler of the appropriate rpmsg driver will be invoked
 * (if/as-soon-as one is registered).
 */
struct rpmsg_ns_msg {
	char name[RPMSG_NAME_SIZE];
	u32 addr;
	u32 flags;
} __packed;

/**
 * enum rpmsg_ns_flags - dynamic name service announcement flags
 *
 * @RPMSG_NS_CREATE: a new remote service was just created
 * @RPMSG_NS_DESTROY: a known remote service was just destroyed
 */
enum rpmsg_ns_flags {
	RPMSG_NS_CREATE		= 0,
	RPMSG_NS_DESTROY	= 1,
};

#endif
