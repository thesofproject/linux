// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018 Pinecone Inc.
 *
 * redirect socket API from remote to the kernel.
 */

#include <linux/of.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/net.h>
#include <linux/rpmsg.h>
#include <linux/slab.h>
#include <linux/tcp.h>
#include <linux/workqueue.h>

#include <net/net_namespace.h>
#include <net/sock.h>

#include "usrsock.h"

struct rpmsg_usrsock_priv {
	struct mutex lock; /* protect socks field */
	struct idr socks;
	struct kmem_cache *cache;
};

struct rpmsg_usrsock_conn {
	struct rpmsg_device *rpdev;
	struct sock *sk;
	struct work_struct state_change;
	struct work_struct data_ready;
	struct work_struct write_space;
	struct work_struct error_report;
	void (*old_state_change)(struct sock *sk);
	void (*old_data_ready)(struct sock *sk);
	void (*old_write_space)(struct sock *sk);
	void (*old_error_report)(struct sock *sk);
	atomic_t xid_connecting;
	int id;
};

static int rpmsg_usrsock_convert_type(int type)
{
	switch (type) {
	case USRSOCK_SOCK_STREAM:
		return SOCK_STREAM;
	case USRSOCK_SOCK_DGRAM:
		return SOCK_DGRAM;
	case USRSOCK_SOCK_SEQPACKET:
		return SOCK_SEQPACKET;
	case USRSOCK_SOCK_RAW:
		return SOCK_RAW;
	case USRSOCK_SOCK_RDM:
		return SOCK_RDM;
	case USRSOCK_SOCK_PACKET:
		return SOCK_PACKET;
	}
	return type;
}

static int rpmsg_usrsock_convert_level(int level)
{
	switch (level) {
	case USRSOCK_SOL_SOCKET:
		return SOL_SOCKET;
	case USRSOCK_SOL_IP:
		return SOL_IP;
	case USRSOCK_SOL_IPV6:
		return SOL_IPV6;
	case USRSOCK_SOL_TCP:
		return SOL_TCP;
	case USRSOCK_SOL_UDP:
		return SOL_UDP;
	}
	return level;
}

static int rpmsg_usrsock_convert_option(int level, int option)
{
	switch (level) {
	case USRSOCK_SOL_SOCKET:
		switch (option) {
		case USRSOCK_SO_ACCEPTCONN:
			return SO_ACCEPTCONN;
		case USRSOCK_SO_BROADCAST:
			return SO_BROADCAST;
		case USRSOCK_SO_DEBUG:
			return SO_DEBUG;
		case USRSOCK_SO_DONTROUTE:
			return SO_DONTROUTE;
		case USRSOCK_SO_ERROR:
			return SO_ERROR;
		case USRSOCK_SO_KEEPALIVE:
			return SO_KEEPALIVE;
		case USRSOCK_SO_LINGER:
			return SO_LINGER;
		case USRSOCK_SO_OOBINLINE:
			return SO_OOBINLINE;
		case USRSOCK_SO_RCVBUF:
			return SO_RCVBUF;
		case USRSOCK_SO_RCVLOWAT:
			return SO_RCVLOWAT;
		case USRSOCK_SO_RCVTIMEO:
			return SO_RCVTIMEO;
		case USRSOCK_SO_REUSEADDR:
			return SO_REUSEADDR;
		case USRSOCK_SO_SNDBUF:
			return SO_SNDBUF;
		case USRSOCK_SO_SNDLOWAT:
			return SO_SNDLOWAT;
		case USRSOCK_SO_SNDTIMEO:
			return SO_SNDTIMEO;
		case USRSOCK_SO_TYPE:
			return SO_TYPE;
		}
		break;
	case USRSOCK_SOL_TCP:
		switch (option) {
		case USRSOCK_TCP_NODELAY:
			return TCP_NODELAY;
		case USRSOCK_TCP_KEEPIDLE:
			return TCP_KEEPIDLE;
		case USRSOCK_TCP_KEEPINTVL:
			return TCP_KEEPINTVL;
		case USRSOCK_TCP_KEEPCNT:
			return TCP_KEEPCNT;
		}
		break;
	}
	return option;
}

static bool rpmsg_usrsock_schedule_work(struct work_struct *work)
{
#ifdef CONFIG_PREEMPT
	if (preempt_count() == 0) {
		work->func(work);
		return true;
	}
#endif
	return schedule_work(work);
}

static struct socket *rpmsg_usrsock_get_sock(struct rpmsg_device *rpdev, int id)
{
	struct rpmsg_usrsock_priv *priv;
	struct socket *sock;

	priv = dev_get_drvdata(&rpdev->dev);
	mutex_lock(&priv->lock);
	sock = idr_find(&priv->socks, id);
	mutex_unlock(&priv->lock);

	return sock;
}

static int rpmsg_usrsock_send_ack(struct rpmsg_device *rpdev,
				  u8 xid, s32 result)
{
	struct usrsock_message_req_ack_s ack = {
		.head.msgid = USRSOCK_MESSAGE_RESPONSE_ACK,
		.head.flags = (result == -EINPROGRESS),
		.xid        = xid,
		.result     = result,
	};

	return rpmsg_send(rpdev->ept, &ack, sizeof(ack));
}

static struct usrsock_message_datareq_ack_s *
rpmsg_usrsock_get_datareq_ack(struct rpmsg_device *rpdev, int *len)
{
	struct rpmsg_usrsock_priv *priv = dev_get_drvdata(&rpdev->dev);

	if (priv->cache) {
		*len = rpmsg_get_max_bufsize(rpdev->ept);
		return kmem_cache_alloc(priv->cache, GFP_KERNEL);
	} else {
		return rpmsg_get_tx_payload_buffer(rpdev->ept,
						   (unsigned int *)len, true);
	}
}

static int
rpmsg_usrsock_send_data_ack(struct rpmsg_device *rpdev,
			    struct usrsock_message_datareq_ack_s *ack,
			    u8 xid, s32 result, u16 valuelen,
			    u16 valuelen_nontrunc)
{
	struct rpmsg_usrsock_priv *priv = dev_get_drvdata(&rpdev->dev);
	int ret = sizeof(*ack);

	ack->reqack.head.msgid = USRSOCK_MESSAGE_RESPONSE_DATA_ACK;
	ack->reqack.head.flags = 0;

	ack->reqack.xid    = xid;
	ack->reqack.result = result;

	if (result >= 0) {
		if (valuelen > valuelen_nontrunc)
			valuelen = valuelen_nontrunc;
		ret += valuelen + result;
	} else {
		valuelen_nontrunc = 0;
		valuelen = 0;
	}

	ack->valuelen          = valuelen;
	ack->valuelen_nontrunc = valuelen_nontrunc;

	if (priv->cache) {
		ret = rpmsg_send(rpdev->ept, ack, ret);
		kmem_cache_free(priv->cache, ack);
	} else {
		ret = rpmsg_send_nocopy(rpdev->ept, ack, ret);
	}

	return ret;
}

static int rpmsg_usrsock_send_event(struct rpmsg_device *rpdev,
				    s16 usockid, u16 events)
{
	struct usrsock_message_socket_event_s event = {
		.head.msgid = USRSOCK_MESSAGE_SOCKET_EVENT,
		.head.flags = USRSOCK_MESSAGE_FLAG_EVENT,
		.usockid    = usockid,
		.events     = events,
	};

	return rpmsg_send(rpdev->ept, &event, sizeof(event));
}

static void rpmsg_usrsock_send_connect_event(struct rpmsg_usrsock_conn *conn)
{
	int xid_connecting = atomic_xchg(&conn->xid_connecting, 0);
	int type = conn->sk->sk_type;

	if (xid_connecting != 0) {
		/* the successful connection finish asynchronously */
		rpmsg_usrsock_send_ack(conn->rpdev, xid_connecting, 0);
		if (sk_stream_is_writeable(conn->sk) &&
		    (type == SOCK_STREAM || type == SOCK_SEQPACKET)) {
			rpmsg_usrsock_send_event(conn->rpdev, conn->id,
						 USRSOCK_EVENT_SENDTO_READY);
		}
	}
}

static void rpmsg_usrsock_send_close_event(struct rpmsg_usrsock_conn *conn)
{
	int xid_connecting = atomic_xchg(&conn->xid_connecting, 0);

	if (xid_connecting != 0) {
		int ret = -ETIMEDOUT;

		if (conn->sk->sk_err)
			ret = -conn->sk->sk_err;
		/* the unsuccessful connection finish asynchronously */
		rpmsg_usrsock_send_ack(conn->rpdev, xid_connecting, ret);
	} else {
		rpmsg_usrsock_send_event(conn->rpdev, conn->id,
					 USRSOCK_EVENT_REMOTE_CLOSED);
	}
}

static void rpmsg_usrsock_state_change_work(struct work_struct *work)
{
	struct rpmsg_usrsock_conn *conn =
		container_of(work, struct rpmsg_usrsock_conn, state_change);

	if (conn->sk->sk_err != 0) {
		rpmsg_usrsock_send_close_event(conn);
	} else {
		switch (conn->sk->sk_state) {
		case TCP_ESTABLISHED:
			rpmsg_usrsock_send_connect_event(conn);
			break;
		case TCP_CLOSE:
			rpmsg_usrsock_send_close_event(conn);
			break;
		}
	}
}

static void rpmsg_usrsock_state_change(struct sock *sk)
{
	struct rpmsg_usrsock_conn *conn = sk->sk_user_data;

	conn->old_state_change(sk);
	rpmsg_usrsock_schedule_work(&conn->state_change);
}

static void rpmsg_usrsock_data_ready_work(struct work_struct *work)
{
	struct rpmsg_usrsock_conn *conn =
		container_of(work, struct rpmsg_usrsock_conn, data_ready);

	rpmsg_usrsock_send_event(conn->rpdev, conn->id,
				 USRSOCK_EVENT_RECVFROM_AVAIL);
}

static void rpmsg_usrsock_data_ready(struct sock *sk)
{
	struct rpmsg_usrsock_conn *conn = sk->sk_user_data;

	if (sk->sk_socket) {
		conn->old_data_ready(sk);
		rpmsg_usrsock_schedule_work(&conn->data_ready);
	}
}

static void rpmsg_usrsock_write_space_work(struct work_struct *work)
{
	struct rpmsg_usrsock_conn *conn =
		container_of(work, struct rpmsg_usrsock_conn, write_space);

	rpmsg_usrsock_send_event(conn->rpdev, conn->id,
				 USRSOCK_EVENT_SENDTO_READY);
}

static void rpmsg_usrsock_write_space(struct sock *sk)
{
	struct rpmsg_usrsock_conn *conn = sk->sk_user_data;

	conn->old_write_space(sk);
	/* Do not wake up a writer until he can make "significant" progress. */
	if (sock_writeable(sk)) {
		clear_bit(SOCK_NOSPACE, &sk->sk_socket->flags);
		rpmsg_usrsock_schedule_work(&conn->write_space);
	}
}

static void rpmsg_usrsock_error_report_work(struct work_struct *work)
{
	struct rpmsg_usrsock_conn *conn =
		container_of(work, struct rpmsg_usrsock_conn, error_report);

	rpmsg_usrsock_send_close_event(conn);
}

static void rpmsg_usrsock_error_report(struct sock *sk)
{
	struct rpmsg_usrsock_conn *conn = sk->sk_user_data;

	conn->old_error_report(sk);
	rpmsg_usrsock_schedule_work(&conn->error_report);
}

static int rpmsg_usrsock_init_conn(struct rpmsg_device *rpdev,
				   struct socket *sock)
{
	struct rpmsg_usrsock_priv *priv = dev_get_drvdata(&rpdev->dev);
	struct rpmsg_usrsock_conn *conn;

	conn = kzalloc(sizeof(*conn), GFP_KERNEL);
	if (!conn)
		return -ENOMEM;

	conn->rpdev = rpdev;
	conn->sk = sock->sk;

	INIT_WORK(&conn->state_change, rpmsg_usrsock_state_change_work);
	INIT_WORK(&conn->data_ready, rpmsg_usrsock_data_ready_work);
	INIT_WORK(&conn->write_space, rpmsg_usrsock_write_space_work);
	INIT_WORK(&conn->error_report, rpmsg_usrsock_error_report_work);

	conn->old_state_change = sock->sk->sk_state_change;
	conn->old_data_ready = sock->sk->sk_data_ready;
	conn->old_write_space = sock->sk->sk_write_space;
	conn->old_error_report = sock->sk->sk_error_report;

	sock->sk->sk_user_data = conn;
	sock->sk->sk_state_change = rpmsg_usrsock_state_change;
	sock->sk->sk_data_ready = rpmsg_usrsock_data_ready;
	sock->sk->sk_write_space = rpmsg_usrsock_write_space;
	sock->sk->sk_error_report = rpmsg_usrsock_error_report;

	mutex_lock(&priv->lock);
	conn->id = idr_alloc(&priv->socks, sock, 0, 0, GFP_KERNEL);
	mutex_unlock(&priv->lock);

	return conn->id; /* return as usockid */
}

static int
rpmsg_usrsock_socket_handler(struct rpmsg_device *rpdev,
			     void *data, int len, void *priv, u32 src)
{
	struct usrsock_request_socket_s *req = data;
	int type = rpmsg_usrsock_convert_type(req->type);
	struct socket *sock;
	int ret;

	ret = sock_create_kern(&init_net, req->domain,
			       type, req->protocol, &sock);
	if (ret >= 0) {
		ret = rpmsg_usrsock_init_conn(rpdev, sock);
		if (ret < 0)
			sock_release(sock);
	}

	rpmsg_usrsock_send_ack(rpdev, req->head.xid, ret);
	if (ret >= 0 && sk_stream_is_writeable(sock->sk) &&
	    type != SOCK_STREAM && type != SOCK_SEQPACKET) {
		rpmsg_usrsock_send_event(rpdev, ret,
					 USRSOCK_EVENT_SENDTO_READY);
	}

	return 0;
}

static void rpmsg_usrsock_release_sock(struct socket *sock)
{
	struct rpmsg_usrsock_conn *conn = sock->sk->sk_user_data;
	struct rpmsg_usrsock_priv *priv = dev_get_drvdata(&conn->rpdev->dev);

	if (conn->id >= 0) {
		mutex_lock(&priv->lock);
		idr_remove(&priv->socks, conn->id);
		mutex_unlock(&priv->lock);
	}

	sock->sk->sk_user_data = NULL;
	sock->sk->sk_state_change = conn->old_state_change;
	sock->sk->sk_data_ready = conn->old_data_ready;
	sock->sk->sk_write_space = conn->old_write_space;
	sock->sk->sk_error_report = conn->old_error_report;

	cancel_work_sync(&conn->state_change);
	cancel_work_sync(&conn->data_ready);
	cancel_work_sync(&conn->write_space);
	cancel_work_sync(&conn->error_report);

	kfree(conn);
	sock_release(sock);
}

static int rpmsg_usrsock_close_handler(struct rpmsg_device *rpdev,
				       void *data, int len, void *priv, u32 src)
{
	struct usrsock_request_close_s *req = data;
	struct socket *sock;
	int ret = -EBADF;

	sock = rpmsg_usrsock_get_sock(rpdev, req->usockid);
	if (sock) {
		rpmsg_usrsock_release_sock(sock);
		ret = 0;
	}

	return rpmsg_usrsock_send_ack(rpdev, req->head.xid, ret);
}

static int
rpmsg_usrsock_connect_handler(struct rpmsg_device *rpdev,
			      void *data, int len, void *priv, u32 src)
{
	struct usrsock_request_connect_s *req = data;
	struct rpmsg_usrsock_conn *conn = NULL;
	struct socket *sock;
	int ret = -EBADF;

	/* indicate the connecting is in the background */
	rpmsg_usrsock_send_ack(rpdev, req->head.xid, -EINPROGRESS);

	sock = rpmsg_usrsock_get_sock(rpdev, req->usockid);
	if (sock) {
		conn = sock->sk->sk_user_data;
		atomic_set(&conn->xid_connecting, req->head.xid);
		ret = kernel_connect(sock, (struct sockaddr *)(req + 1),
				     req->addrlen, O_NONBLOCK);
	}

	if (ret == -EINPROGRESS)
		return 0;

	/* the connection finish synchronously */
	atomic_set(&conn->xid_connecting, 0);
	rpmsg_usrsock_send_ack(rpdev, req->head.xid, ret);
	if (ret >= 0 && sk_stream_is_writeable(sock->sk) &&
	    (sock->type == SOCK_STREAM || sock->type == SOCK_SEQPACKET)) {
		rpmsg_usrsock_send_event(rpdev, req->usockid,
					 USRSOCK_EVENT_SENDTO_READY);
	}

	return 0;
}

static int rpmsg_usrsock_sendto(struct socket *sock, void *buf, int size,
				struct sockaddr *addr, int addrlen)
{
	struct msghdr msg = {
		.msg_name    = addrlen > 0 ? addr : NULL,
		.msg_namelen = addrlen,
		.msg_flags   = MSG_DONTWAIT | MSG_NOSIGNAL,
	};
	struct kvec iov = {
		.iov_base    = buf,
		.iov_len     = size,
	};

	return kernel_sendmsg(sock, &msg, &iov, 1, size);
}

static int
rpmsg_usrsock_sendto_handler(struct rpmsg_device *rpdev,
			     void *data, int len, void *priv, u32 src)
{
	struct usrsock_request_sendto_s *req = data;
	struct socket *sock;
	int ret = -EBADF;

	sock = rpmsg_usrsock_get_sock(rpdev, req->usockid);
	if (sock) {
		ret = rpmsg_usrsock_sendto(sock,
					   (void *)(req + 1) + req->addrlen,
					   req->buflen,
					   (struct sockaddr *)(req + 1),
					   req->addrlen);
	}

	rpmsg_usrsock_send_ack(rpdev, req->head.xid, ret);
	if (ret <= 0) {
		set_bit(SOCK_NOSPACE, &sock->flags);
	} else if (sk_stream_is_writeable(sock->sk) &&
		 (sock->type == SOCK_STREAM || sock->type == SOCK_SEQPACKET)) {
		rpmsg_usrsock_send_event(rpdev, req->usockid,
					 USRSOCK_EVENT_SENDTO_READY);
	}

	return 0;
}

static int rpmsg_usrsock_recvfrom(struct socket *sock, void *buf, int size,
				  struct sockaddr *addr, int *addrlen)
{
	struct msghdr msg = {
		.msg_name    = *addrlen ? addr : NULL,
		.msg_namelen = *addrlen,
	};
	struct kvec iov = {
		.iov_base    = buf,
		.iov_len     = size,
	};
	int ret;

	ret = kernel_recvmsg(sock, &msg, &iov, 1, size,
			     MSG_DONTWAIT | MSG_NOSIGNAL);
	if (ret >= 0)
		*addrlen = msg.msg_namelen;

	return ret;
}

static int
rpmsg_usrsock_recvfrom_handler(struct rpmsg_device *rpdev,
			       void *data, int len, void *priv, u32 src)
{
	struct usrsock_request_recvfrom_s *req = data;
	struct usrsock_message_datareq_ack_s *ack;
	int outaddrlen = req->max_addrlen;
	int inaddrlen = req->max_addrlen;
	int buflen = req->max_buflen;
	struct socket *sock;
	int ret = -EBADF;

	ack = rpmsg_usrsock_get_datareq_ack(rpdev, &len);
	if (!ack)
		return -ENOMEM;

	if (sizeof(*ack) + inaddrlen + buflen > len)
		buflen = len - sizeof(*ack) - inaddrlen;

	sock = rpmsg_usrsock_get_sock(rpdev, req->usockid);
	if (sock) {
		ret = rpmsg_usrsock_recvfrom(sock,
					     (void *)(ack + 1) + inaddrlen,
					     buflen,
					     (struct sockaddr *)(ack + 1),
					     &outaddrlen);
		if (ret > 0 && outaddrlen < inaddrlen) {
			memcpy((void *)(ack + 1) + outaddrlen,
			       (void *)(ack + 1) + inaddrlen, ret);
		}
	}

	return rpmsg_usrsock_send_data_ack(rpdev,
		ack, req->head.xid, ret, inaddrlen, outaddrlen);
}

static int
rpmsg_usrsock_setsockopt_handler(struct rpmsg_device *rpdev,
				 void *data, int len, void *priv, u32 src)
{
	struct usrsock_request_setsockopt_s *req = data;
	struct socket *sock;
	int ret = -EBADF;

	sock = rpmsg_usrsock_get_sock(rpdev, req->usockid);
	if (sock) {
		int level, option;

		level = rpmsg_usrsock_convert_level(req->level);
		option = rpmsg_usrsock_convert_option(req->level, req->option);
		ret = kernel_setsockopt(sock, level, option,
					(char *)(req + 1), req->valuelen);
	}

	return rpmsg_usrsock_send_ack(rpdev, req->head.xid, ret);
}

static int
rpmsg_usrsock_getsockopt_handler(struct rpmsg_device *rpdev,
				 void *data, int len, void *priv, u32 src)
{
	struct usrsock_request_getsockopt_s *req = data;
	struct usrsock_message_datareq_ack_s *ack;
	int optlen = req->max_valuelen;
	struct socket *sock;
	int ret = -EBADF;

	ack = rpmsg_usrsock_get_datareq_ack(rpdev, &len);
	if (!ack)
		return -ENOMEM;

	sock = rpmsg_usrsock_get_sock(rpdev, req->usockid);
	if (sock) {
		int level, option;

		level = rpmsg_usrsock_convert_level(req->level);
		option = rpmsg_usrsock_convert_option(req->level, req->option);
		ret = kernel_getsockopt(sock, level, option,
					(char *)(ack + 1), &optlen);
	}

	return rpmsg_usrsock_send_data_ack(rpdev,
		ack, req->head.xid, ret, optlen, optlen);
}

static int
rpmsg_usrsock_getsockname_handler(struct rpmsg_device *rpdev,
				  void *data, int len, void *priv, u32 src)
{
	struct usrsock_request_getsockname_s *req = data;
	struct usrsock_message_datareq_ack_s *ack;
	int outaddrlen = req->max_addrlen;
	int inaddrlen = req->max_addrlen;
	struct socket *sock;
	int ret = -EBADF;

	ack = rpmsg_usrsock_get_datareq_ack(rpdev, &len);
	if (!ack)
		return -ENOMEM;

	sock = rpmsg_usrsock_get_sock(rpdev, req->usockid);
	if (sock) {
		ret = kernel_getsockname(sock, (struct sockaddr *)(ack + 1));
		if (ret >= 0) {
			outaddrlen = ret;
			ret = 0;
		}
	}

	return rpmsg_usrsock_send_data_ack(rpdev,
		ack, req->head.xid, ret, inaddrlen, outaddrlen);
}

static int
rpmsg_usrsock_getpeername_handler(struct rpmsg_device *rpdev,
				  void *data, int len, void *priv, u32 src)
{
	struct usrsock_request_getpeername_s *req = data;
	struct usrsock_message_datareq_ack_s *ack;
	int outaddrlen = req->max_addrlen;
	int inaddrlen = req->max_addrlen;
	struct socket *sock;
	int ret = -EBADF;

	ack = rpmsg_usrsock_get_datareq_ack(rpdev, &len);
	if (!ack)
		return -ENOMEM;

	sock = rpmsg_usrsock_get_sock(rpdev, req->usockid);
	if (sock) {
		ret = kernel_getpeername(sock, (struct sockaddr *)(ack + 1));
		if (ret >= 0) {
			outaddrlen = ret;
			ret = 0;
		}
	}

	return rpmsg_usrsock_send_data_ack(rpdev,
		ack, req->head.xid, ret, inaddrlen, outaddrlen);
}

static int rpmsg_usrsock_bind_handler(struct rpmsg_device *rpdev,
				      void *data, int len, void *priv, u32 src)
{
	struct usrsock_request_bind_s *req = data;
	struct socket *sock;
	int ret = -EBADF;

	sock = rpmsg_usrsock_get_sock(rpdev, req->usockid);
	if (sock) {
		ret = kernel_bind(sock,
				  (struct sockaddr *)(req + 1),
				  req->addrlen);
	}

	return rpmsg_usrsock_send_ack(rpdev, req->head.xid, ret);
}

static int
rpmsg_usrsock_listen_handler(struct rpmsg_device *rpdev,
			     void *data, int len, void *priv, u32 src)
{
	struct usrsock_request_listen_s *req = data;
	struct socket *sock;
	int ret = -EBADF;

	sock = rpmsg_usrsock_get_sock(rpdev, req->usockid);
	if (sock)
		ret = kernel_listen(sock, req->backlog);

	return rpmsg_usrsock_send_ack(rpdev, req->head.xid, ret);
}

static int rpmsg_usrsock_accept(struct socket *sock, struct socket **newsock,
				struct sockaddr *addr, int *addrlen)
{
	int ret;

	ret = kernel_accept(sock, newsock, O_NONBLOCK);
	if (ret >= 0 && *addrlen) {
		ret = kernel_getpeername(*newsock, addr);
		if (ret >= 0) {
			*addrlen = ret;
			ret = 0;
		} else {
			sock_release(*newsock);
		}
	}

	return ret;
}

static int
rpmsg_usrsock_accept_handler(struct rpmsg_device *rpdev,
			     void *data, int len, void *priv, u32 src)
{
	struct usrsock_request_accept_s *req = data;
	struct usrsock_message_datareq_ack_s *ack;
	int outaddrlen = req->max_addrlen;
	int inaddrlen = req->max_addrlen;
	struct socket *newsock;
	struct socket *sock;
	int ret = -EBADF;
	int newid = -1;

	ack = rpmsg_usrsock_get_datareq_ack(rpdev, &len);
	if (!ack)
		return -ENOMEM;

	sock = rpmsg_usrsock_get_sock(rpdev, req->usockid);
	if (sock) {
		ret = rpmsg_usrsock_accept(sock, &newsock,
					   (struct sockaddr *)(ack + 1),
					   &outaddrlen);
		if (ret >= 0) {
			ret = rpmsg_usrsock_init_conn(rpdev, newsock);
			if (ret >= 0) {
				s16 *loc;

				/* append index as usockid to the payload */
				if (outaddrlen <= inaddrlen)
					loc = (void *)(ack + 1) + outaddrlen;
				else
					loc = (void *)(ack + 1) + inaddrlen;
				newid = *loc = ret; /* save the new usockid */
				ret = sizeof(s16); /* return usockid size */
			} else {
				sock_release(newsock);
			}
		}
	}

	rpmsg_usrsock_send_data_ack(rpdev, ack, req->head.xid,
				    ret, inaddrlen, outaddrlen);
	if (newid >= 0) {
		if (sk_stream_is_writeable(newsock->sk)) {
			rpmsg_usrsock_send_event(rpdev, newid,
						 USRSOCK_EVENT_SENDTO_READY);
		}
		rpmsg_usrsock_send_event(rpdev, newid,
					 USRSOCK_EVENT_RECVFROM_AVAIL);
	}

	return 0;
}

static int rpmsg_usrsock_ioctl(struct socket *sock, int cmd, unsigned long arg)
{
	mm_segment_t oldfs = get_fs();
	int ret;

	set_fs(KERNEL_DS);
	ret = sock->ops->ioctl(sock, cmd, arg);
	set_fs(oldfs);

	return ret;
}

static int rpmsg_usrsock_ioctl_handler(struct rpmsg_device *rpdev,
				       void *data, int len, void *priv, u32 src)
{
	struct usrsock_request_ioctl_s *req = data;
	struct usrsock_message_datareq_ack_s *ack;
	struct socket *sock;
	int ret = -EBADF;

	ack = rpmsg_usrsock_get_datareq_ack(rpdev, &len);
	if (!ack)
		return -ENOMEM;

	sock = rpmsg_usrsock_get_sock(rpdev, req->usockid);
	if (sock) {
		memcpy(ack + 1, req + 1, req->arglen);
		ret = rpmsg_usrsock_ioctl(sock, req->cmd,
					  (unsigned long)(ack + 1));
	}

	return rpmsg_usrsock_send_data_ack(rpdev,
		ack, req->head.xid, ret, req->arglen, req->arglen);
}

static const rpmsg_rx_cb_t rpmsg_usrsock_handler[] = {
	[USRSOCK_REQUEST_SOCKET]      = rpmsg_usrsock_socket_handler,
	[USRSOCK_REQUEST_CLOSE]       = rpmsg_usrsock_close_handler,
	[USRSOCK_REQUEST_CONNECT]     = rpmsg_usrsock_connect_handler,
	[USRSOCK_REQUEST_SENDTO]      = rpmsg_usrsock_sendto_handler,
	[USRSOCK_REQUEST_RECVFROM]    = rpmsg_usrsock_recvfrom_handler,
	[USRSOCK_REQUEST_SETSOCKOPT]  = rpmsg_usrsock_setsockopt_handler,
	[USRSOCK_REQUEST_GETSOCKOPT]  = rpmsg_usrsock_getsockopt_handler,
	[USRSOCK_REQUEST_GETSOCKNAME] = rpmsg_usrsock_getsockname_handler,
	[USRSOCK_REQUEST_GETPEERNAME] = rpmsg_usrsock_getpeername_handler,
	[USRSOCK_REQUEST_BIND]        = rpmsg_usrsock_bind_handler,
	[USRSOCK_REQUEST_LISTEN]      = rpmsg_usrsock_listen_handler,
	[USRSOCK_REQUEST_ACCEPT]      = rpmsg_usrsock_accept_handler,
	[USRSOCK_REQUEST_IOCTL]       = rpmsg_usrsock_ioctl_handler,
};

static int rpmsg_usrsock_callback(struct rpmsg_device *rpdev,
				  void *data, int len, void *priv_, u32 src)
{
	struct rpmsg_usrsock_priv *priv = dev_get_drvdata(&rpdev->dev);
	struct usrsock_request_common_s *common = data;
	s8 id = common->reqid;
	int ret = -EINVAL;

	if (priv->cache) {
		void *tmp;

		tmp = kmem_cache_alloc(priv->cache, GFP_KERNEL);
		if (!tmp)
			return -ENOMEM;
		memcpy(tmp, data, len);
		data = tmp;
	}

	if (id >= 0 && id < ARRAY_SIZE(rpmsg_usrsock_handler)) {
		ret = rpmsg_usrsock_handler[id](rpdev, data, len, priv, src);
		if (ret < 0)
			dev_err(&rpdev->dev, "request handle error %d\n", id);
	}

	if (priv->cache)
		kmem_cache_free(priv->cache, data);

	return ret;
}

static int rpmsg_usrsock_probe(struct rpmsg_device *rpdev)
{
	struct device_node *np = rpdev->dev.of_node;
	struct rpmsg_usrsock_priv *priv;
	bool aligned;

	priv = devm_kzalloc(&rpdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	aligned = of_property_read_bool(np, "memory-aligned-access");
	if (!aligned) {
		/* try the parent node */
		np = of_get_parent(np);
		aligned = of_property_read_bool(np, "memory-aligned-access");
		of_node_put(np);
	}

	if (aligned) {
		int size = rpmsg_get_max_bufsize(rpdev->ept);

		priv->cache = kmem_cache_create(dev_name(&rpdev->dev),
						size, 8, 0, NULL);
		if (!priv->cache)
			return -ENOMEM;
	}

	mutex_init(&priv->lock);
	idr_init(&priv->socks);
	dev_set_drvdata(&rpdev->dev, priv);

	return 0;
}

static void rpmsg_usrsock_remove(struct rpmsg_device *rpdev)
{
	struct rpmsg_usrsock_priv *priv = dev_get_drvdata(&rpdev->dev);
	struct socket *sock;
	int id;

	idr_for_each_entry(&priv->socks, sock, id)
		rpmsg_usrsock_release_sock(sock);

	kmem_cache_destroy(priv->cache);
	mutex_destroy(&priv->lock);
	idr_destroy(&priv->socks);
}

static const struct rpmsg_device_id rpmsg_usrsock_id_table[] = {
	{ .name = "rpmsg-usrsock" },
	{ },
};
MODULE_DEVICE_TABLE(rpmsg, rpmsg_usrsock_id_table);

static struct rpmsg_driver rpmsg_usrsock_driver = {
	.drv = {
		.name  = "rpmsg_usrsock",
		.owner = THIS_MODULE,
	},

	.id_table = rpmsg_usrsock_id_table,
	.probe    = rpmsg_usrsock_probe,
	.callback = rpmsg_usrsock_callback,
	.remove   = rpmsg_usrsock_remove,
};

module_rpmsg_driver(rpmsg_usrsock_driver);

MODULE_ALIAS("rpmsg:rpmsg_usrsock");
MODULE_AUTHOR("Xiang Xiao <xiaoxiang@pinecone.net>");
MODULE_DESCRIPTION("rpmsg socket API redirection driver");
MODULE_LICENSE("GPL v2");
