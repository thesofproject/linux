// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2017 Pinecone Inc.
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/rpmsg.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/workqueue.h>

#define RPMSG_TTY_WRITE		0
#define RPMSG_TTY_WAKEUP	1

struct rpmsg_tty_header {
	u32			command:31;
	u32			response:1;
	s32			result;
	u64			cookie;
} __packed;

struct rpmsg_tty_write {
	struct rpmsg_tty_header	header;
	u32			count;
	u32			resolved;
	char			data[0];
} __packed;

#define rpmsg_tty_wakeup	rpmsg_tty_header

struct rpmsg_tty_cookie {
	struct completion	done;
	s32			result;
};

struct tty_rpmsg_port {
	struct tty_port		port;
	struct work_struct	work;
	struct tty_driver	*driver;
	int			xmit_size;
};

static struct tty_rpmsg_port *to_tty_rpmsg_port(struct tty_port *p)
{
	return container_of(p, struct tty_rpmsg_port, port);
}

static int tty_rpmsg_port_activate(struct tty_port *p, struct tty_struct *tty)
{
	struct tty_rpmsg_port *port = to_tty_rpmsg_port(p);

	port->xmit_size = 0;
	return tty_port_alloc_xmit_buf(p);
}

static void tty_rpmsg_port_shutdown(struct tty_port *p)
{
	tty_port_free_xmit_buf(p);
}

static void tty_rpmsg_port_destruct(struct tty_port *p)
{
	/* don't need free manually since it come from devm_kzalloc */
}

static const struct tty_port_operations tty_rpmsg_port_ops = {
	.activate = tty_rpmsg_port_activate,
	.shutdown = tty_rpmsg_port_shutdown,
	.destruct = tty_rpmsg_port_destruct,
};

static int tty_rpmsg_open(struct tty_struct *tty, struct file *filp)
{
	tty_flip_buffer_push(tty->port);
	return tty_port_open(tty->port, tty, filp);
}

static void tty_rpmsg_close(struct tty_struct *tty, struct file *filp)
{
	tty_port_close(tty->port, tty, filp);
}

static int tty_rpmsg_write_room(struct tty_struct *tty)
{
	struct tty_port *p = tty->port;
	struct tty_rpmsg_port *port = to_tty_rpmsg_port(p);
	struct rpmsg_device *rpdev = dev_get_drvdata(tty->dev);
	int space = rpmsg_get_max_bufsize(rpdev->ept);

	space -= sizeof(struct rpmsg_tty_write);
	if (space > PAGE_SIZE)
		space = PAGE_SIZE;

	mutex_lock(&p->buf_mutex);
	space -= port->xmit_size;
	mutex_unlock(&p->buf_mutex);

	return space;
}

static void tty_rpmsg_memcpy(void *dst, const void *src, size_t len)
{
	const uintptr_t *srcw;
	uintptr_t *dstw;
	const u8 *srcb;
	u8 *dstb;

	dstw = dst;
	srcw = src;
	while (len >= sizeof(*srcw)) {
		*dstw++ = *srcw++;
		len -= sizeof(*srcw);
	}

	dstb = (u8 *)dstw;
	srcb = (u8 *)srcw;
	while (len--)
		*dstb++ = *srcb++;
}

static int tty_rpmsg_do_write(struct tty_struct *tty,
			      const unsigned char *buf, int count)
{
	struct rpmsg_device *rpdev = dev_get_drvdata(tty->dev);
	struct rpmsg_tty_cookie cookie;
	struct rpmsg_tty_write *msg;
	unsigned int max;
	int ret;

	msg = rpmsg_get_tx_payload_buffer(rpdev->ept, &max, true);
	if (IS_ERR(msg))
		return PTR_ERR(msg);

	if (sizeof(*msg) + count > max)
		count = max - sizeof(*msg);

	memset(msg, 0, sizeof(*msg));

	msg->header.command = RPMSG_TTY_WRITE;
	msg->header.result  = -ENXIO;
	msg->header.cookie  = (uintptr_t)&cookie;

	msg->count = count;
	tty_rpmsg_memcpy(msg->data, buf, count);

	memset(&cookie, 0, sizeof(cookie));
	init_completion(&cookie.done);

	ret = rpmsg_send_nocopy(rpdev->ept, msg, sizeof(*msg) + count);
	if (ret < 0)
		return ret;

	wait_for_completion(&cookie.done);
	return cookie.result;
}

static int tty_rpmsg_write(struct tty_struct *tty,
			   const unsigned char *buf, int count)
{
	struct tty_port *p = tty->port;
	struct tty_rpmsg_port *port = to_tty_rpmsg_port(p);
	int space, written;

	space = tty_rpmsg_write_room(tty);
	if (count > space)
		count = space;

	written = count;

	mutex_lock(&p->buf_mutex);
	if (port->xmit_size) {
		memcpy(p->xmit_buf + port->xmit_size, buf, count);
		port->xmit_size += count;

		buf = p->xmit_buf;
		count = port->xmit_size;
	}

	if (count) {
		int ret = tty_rpmsg_do_write(tty, buf, count);

		if (ret > 0) {
			memmove(p->xmit_buf, buf + ret, count - ret);
			port->xmit_size = count - ret;
		} else if (p->xmit_buf != buf) {
			memcpy(p->xmit_buf, buf, count);
			port->xmit_size = count;
		}
	}
	mutex_unlock(&p->buf_mutex);

	return written;
}

static void tty_rpmsg_write_work(struct work_struct *work)
{
	struct tty_rpmsg_port *port =
		container_of(work, struct tty_rpmsg_port, work);
	struct tty_struct *tty = tty_port_tty_get(&port->port);

	if (tty) {
		tty_rpmsg_write(tty, NULL, 0);
		tty_kref_put(tty);
	}

	tty_port_tty_wakeup(&port->port);
}

static void tty_rpmsg_hangup(struct tty_struct *tty)
{
	tty_port_hangup(tty->port);
}

static const struct tty_operations tty_rpmsg_ops = {
	.open = tty_rpmsg_open,
	.close = tty_rpmsg_close,
	.write = tty_rpmsg_write,
	.write_room = tty_rpmsg_write_room,
	.hangup = tty_rpmsg_hangup,
};

static int rpmsg_tty_probe(struct rpmsg_device *rpdev)
{
	struct tty_rpmsg_port *port;
	struct device *dev;
	u32 max_size;
	int ret;

	port = devm_kzalloc(&rpdev->dev, sizeof(*port), GFP_KERNEL);
	if (!port)
		return -ENOMEM;
	dev_set_drvdata(&rpdev->dev, &port->port);

	tty_port_init(&port->port);
	port->port.ops = &tty_rpmsg_port_ops;
	INIT_WORK(&port->work, tty_rpmsg_write_work);

	/*
	 * don't limit receive buffer size by default since:
	 * 1.tty core don't notify us when somebody read the data from buffer
	 * 2.it's hard to send RPMSG_TTY_WAKEUP once some space is available
	 */
	if (of_property_read_u32(rpdev->dev.of_node,
				 "max-size", &max_size) < 0) {
		max_size = INT_MAX;
	}
	tty_buffer_set_limit(&port->port, max_size);

	port->driver = tty_alloc_driver(1, TTY_DRIVER_REAL_RAW |
		TTY_DRIVER_DYNAMIC_DEV | TTY_DRIVER_UNNUMBERED_NODE);
	if (IS_ERR(port->driver)) {
		ret = PTR_ERR(port->driver);
		goto put_port;
	}

	port->driver->driver_name  = rpdev->id.name;
	port->driver->name         = rpdev->id.name + 6; /* skip "rpmsg-" */
	port->driver->type         = TTY_DRIVER_TYPE_SERIAL |
				     TTY_DRIVER_REAL_RAW;
	port->driver->subtype      = SERIAL_TYPE_NORMAL;

	port->driver->init_termios = tty_std_termios;
	port->driver->init_termios.c_iflag = 0;
	port->driver->init_termios.c_oflag = 0;
	port->driver->init_termios.c_lflag = 0;

	tty_set_operations(port->driver, &tty_rpmsg_ops);
	ret = tty_register_driver(port->driver);
	if (ret < 0)
		goto put_tty;

	dev = tty_port_register_device_attr(&port->port, port->driver,
					    0, &rpdev->dev, rpdev, NULL);
	if (IS_ERR(dev)) {
		ret = PTR_ERR(dev);
		goto tty_unregister;
	}

	return 0;

tty_unregister:
	tty_unregister_driver(port->driver);
put_tty:
	put_tty_driver(port->driver);
put_port:
	tty_port_put(&port->port);
	return ret;
}

static void rpmsg_tty_remove(struct rpmsg_device *rpdev)
{
	struct tty_port *p = dev_get_drvdata(&rpdev->dev);
	struct tty_rpmsg_port *port = to_tty_rpmsg_port(p);

	cancel_work_sync(&port->work);
	tty_unregister_device(port->driver, 0);
	tty_unregister_driver(port->driver);
	put_tty_driver(port->driver);
	tty_port_put(p);
}

static int rpmsg_tty_write_handler(struct rpmsg_device *rpdev,
				   void *data, int len, void *priv, u32 src)
{
	struct tty_port *p = dev_get_drvdata(&rpdev->dev);
	struct rpmsg_tty_write *msg = data;
	int ret;

	ret = tty_insert_flip_string(p, msg->data, msg->count);
	if (ret > 0)
		tty_flip_buffer_push(p);

	msg->header.response = 1;
	msg->header.result = ret;

	return rpmsg_send(rpdev->ept, msg, sizeof(*msg));
}

static int rpmsg_tty_wakeup_handler(struct rpmsg_device *rpdev,
				    void *data, int len, void *priv, u32 src)
{
	struct tty_port *p = dev_get_drvdata(&rpdev->dev);
	struct tty_rpmsg_port *port = to_tty_rpmsg_port(p);

	schedule_work(&port->work);
	return 0;
}

static const rpmsg_rx_cb_t rpmsg_tty_handler[] = {
	[RPMSG_TTY_WRITE]  = rpmsg_tty_write_handler,
	[RPMSG_TTY_WAKEUP] = rpmsg_tty_wakeup_handler,
};

static int rpmsg_tty_callback(struct rpmsg_device *rpdev,
			      void *data, int len, void *priv, u32 src)
{
	struct rpmsg_tty_header *hdr = data;
	u32 cmd = hdr->command;
	int ret = -EINVAL;

	if (hdr->response) {
		struct rpmsg_tty_cookie *cookie =
			(struct rpmsg_tty_cookie *)hdr->cookie;

		cookie->result = hdr->result;
		complete(&cookie->done);
	} else if (cmd < ARRAY_SIZE(rpmsg_tty_handler)) {
		ret = rpmsg_tty_handler[cmd](rpdev, data, len, priv, src);
	} else {
		dev_err(&rpdev->dev, "invalid command %u\n", cmd);
	}

	return ret;
}

static int rpmsg_tty_match(struct rpmsg_device *dev, struct rpmsg_driver *drv)
{
	const char *devname = dev->id.name;
	const char *drvname = "rpmsg-tty";

	/* match all device start with rpmsg-tty */
	return strncmp(devname, drvname, strlen(drvname)) == 0;
}

static const struct rpmsg_device_id rpmsg_tty_id_table[] = {
	{ .name = "rpmsg-tty" },
	{ },
};
MODULE_DEVICE_TABLE(rpmsg, rpmsg_tty_id_table);

static struct rpmsg_driver rpmsg_tty_driver = {
	.drv = {
		.name	= "rpmsg_tty",
		.owner	= THIS_MODULE,
	},
	.id_table	= rpmsg_tty_id_table,
	.probe		= rpmsg_tty_probe,
	.remove		= rpmsg_tty_remove,
	.callback	= rpmsg_tty_callback,
	.match		= rpmsg_tty_match,
};

module_rpmsg_driver(rpmsg_tty_driver);

MODULE_ALIAS("rpmsg:rpmsg_tty");
MODULE_AUTHOR("Xiang Xiao <xiaoxiang@pinecone.net>");
MODULE_DESCRIPTION("TTY over the rpmsg channel");
MODULE_LICENSE("GPL v2");
