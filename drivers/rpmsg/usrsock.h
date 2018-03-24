/* SPDX-License-Identifier: BSD-3-Clause */
/*
 *  Copyright (C) 2015, 2017 Haltian Ltd. All rights reserved.
 *  Copyright (C) 2018 Pinecone Inc. All rights reserved.
 *  Author: Jussi Kivilinna <jussi.kivilinna@haltian.com>
 */

#ifndef __USRSOCK_H
#define __USRSOCK_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <linux/types.h>
#include <linux/compiler.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The socket created by socket() has the indicated type, which specifies
 * the communication semantics.
 */

#define USRSOCK_SOCK_STREAM     0
#define USRSOCK_SOCK_DGRAM      1
#define USRSOCK_SOCK_SEQPACKET  2
#define USRSOCK_SOCK_RAW        3
#define USRSOCK_SOCK_RDM        4
#define USRSOCK_SOCK_PACKET     5

/* Protocol levels supported by get/setsockopt(): */

#define USRSOCK_SOL_SOCKET      0
#define USRSOCK_SOL_IP          1
#define USRSOCK_SOL_IPV6        2
#define USRSOCK_SOL_TCP         3
#define USRSOCK_SOL_UDP         4

/* Socket-level options */

#define USRSOCK_SO_ACCEPTCONN   0
#define USRSOCK_SO_BROADCAST    1
#define USRSOCK_SO_DEBUG        2
#define USRSOCK_SO_DONTROUTE    3
#define USRSOCK_SO_ERROR        4
#define USRSOCK_SO_KEEPALIVE    5
#define USRSOCK_SO_LINGER       6
#define USRSOCK_SO_OOBINLINE    7
#define USRSOCK_SO_RCVBUF       8
#define USRSOCK_SO_RCVLOWAT     9
#define USRSOCK_SO_RCVTIMEO    10
#define USRSOCK_SO_REUSEADDR   11
#define USRSOCK_SO_SNDBUF      12
#define USRSOCK_SO_SNDLOWAT    13
#define USRSOCK_SO_SNDTIMEO    14
#define USRSOCK_SO_TYPE        15

/* TCP-level options */

#define USRSOCK_TCP_NODELAY    16
#define USRSOCK_TCP_KEEPIDLE   17
#define USRSOCK_TCP_KEEPINTVL  18
#define USRSOCK_TCP_KEEPCNT    19

/* Event message flags */

#define USRSOCK_EVENT_ABORT          BIT(1)
#define USRSOCK_EVENT_SENDTO_READY   BIT(2)
#define USRSOCK_EVENT_RECVFROM_AVAIL BIT(3)
#define USRSOCK_EVENT_REMOTE_CLOSED  BIT(4)

/* Response message flags */

#define USRSOCK_MESSAGE_FLAG_REQ_IN_PROGRESS BIT(0)
#define USRSOCK_MESSAGE_FLAG_EVENT           BIT(1)

#define USRSOCK_MESSAGE_IS_EVENT(flags) \
			  (!!((flags) & USRSOCK_MESSAGE_FLAG_EVENT))
#define USRSOCK_MESSAGE_IS_REQ_RESPONSE(flags) \
			  (!USRSOCK_MESSAGE_IS_EVENT(flags))

#define USRSOCK_MESSAGE_REQ_IN_PROGRESS(flags) \
			  (!!((flags) & USRSOCK_MESSAGE_FLAG_REQ_IN_PROGRESS))
#define USRSOCK_MESSAGE_REQ_COMPLETED(flags) \
			  (!USRSOCK_MESSAGE_REQ_IN_PROGRESS(flags))

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Request types */

enum usrsock_request_types_e {
	USRSOCK_REQUEST_SOCKET = 0,
	USRSOCK_REQUEST_CLOSE,
	USRSOCK_REQUEST_CONNECT,
	USRSOCK_REQUEST_SENDTO,
	USRSOCK_REQUEST_RECVFROM,
	USRSOCK_REQUEST_SETSOCKOPT,
	USRSOCK_REQUEST_GETSOCKOPT,
	USRSOCK_REQUEST_GETSOCKNAME,
	USRSOCK_REQUEST_GETPEERNAME,
	USRSOCK_REQUEST_BIND,
	USRSOCK_REQUEST_LISTEN,
	USRSOCK_REQUEST_ACCEPT,
	USRSOCK_REQUEST_IOCTL,
	USRSOCK_REQUEST__MAX
};

/* Response/event message types */

enum usrsock_message_types_e {
	USRSOCK_MESSAGE_RESPONSE_ACK = 0,
	USRSOCK_MESSAGE_RESPONSE_DATA_ACK,
	USRSOCK_MESSAGE_SOCKET_EVENT,
};

/* Request structures (kernel => /dev/usrsock => daemon) */

struct usrsock_request_common_s {
	s8 reqid;
	u8 xid;
} __packed;

struct usrsock_request_socket_s {
	struct usrsock_request_common_s head;

	s16 domain;
	s16 type;
	s16 protocol;
} __packed;

struct usrsock_request_close_s {
	struct usrsock_request_common_s head;

	s16 usockid;
} __packed;

struct usrsock_request_bind_s {
	struct usrsock_request_common_s head;

	s16 usockid;
	u16 addrlen;
} __packed;

struct usrsock_request_connect_s {
	struct usrsock_request_common_s head;

	s16 usockid;
	u16 addrlen;
} __packed;

struct usrsock_request_listen_s {
	struct usrsock_request_common_s head;

	s16 usockid;
	u16 backlog;
} __packed;

struct usrsock_request_accept_s {
	struct usrsock_request_common_s head;

	s16 usockid;
	u16 max_addrlen;
} __packed;

struct usrsock_request_sendto_s {
	struct usrsock_request_common_s head;

	s16 usockid;
	u16 addrlen;
	u16 buflen;
} __packed;

struct usrsock_request_recvfrom_s {
	struct usrsock_request_common_s head;

	s16 usockid;
	u16 max_buflen;
	u16 max_addrlen;
} __packed;

struct usrsock_request_setsockopt_s {
	struct usrsock_request_common_s head;

	s16 usockid;
	s16 level;
	s16 option;
	u16 valuelen;
} __packed;

struct usrsock_request_getsockopt_s {
	struct usrsock_request_common_s head;

	s16 usockid;
	s16 level;
	s16 option;
	u16 max_valuelen;
} __packed;

struct usrsock_request_getsockname_s {
	struct usrsock_request_common_s head;

	s16 usockid;
	u16 max_addrlen;
} __packed;

struct usrsock_request_getpeername_s {
	struct usrsock_request_common_s head;

	s16 usockid;
	u16 max_addrlen;
} __packed;

struct usrsock_request_ioctl_s {
	struct usrsock_request_common_s head;

	s16 usockid;
	s32 cmd;
	u16 arglen;
} __packed;

/* Response/event message structures (kernel <= /dev/usrsock <= daemon) */

struct usrsock_message_common_s {
	s8 msgid;
	s8 flags;
} __packed;

/* Request acknowledgment/completion message */

struct usrsock_message_req_ack_s {
	struct usrsock_message_common_s head;

	u8 xid;
	s32 result;
} __packed;

/* Request acknowledgment/completion message */

struct usrsock_message_datareq_ack_s {
	struct usrsock_message_req_ack_s reqack;

	/* head.result => positive buflen, negative error-code. */

	u16 valuelen;          /* length of value returned after buffer */
	u16 valuelen_nontrunc; /* actual non-truncated length of value */
} __packed;

/* Socket event message */

struct usrsock_message_socket_event_s {
	struct usrsock_message_common_s head;

	s16 usockid;
	u16 events;
} __packed;

#endif /* __USRSOCK_H */
