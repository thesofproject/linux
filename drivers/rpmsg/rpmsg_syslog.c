// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2017 Pinecone Inc.
 *
 * redirect syslog/printf from remote to the kernel.
 */

#include <linux/module.h>
#include <linux/rpmsg.h>
#include <linux/slab.h>

#define RPMSG_SYSLOG_TRANSFER		0
#define RPMSG_SYSLOG_TRANSFER_DONE	1
#define RPMSG_SYSLOG_SUSPEND		2
#define RPMSG_SYSLOG_RESUME		3

struct rpmsg_syslog_header {
	u32				command;
	s32				result;
} __packed;

struct rpmsg_syslog_transfer {
	struct rpmsg_syslog_header	header;
	u32				count;
	char				data[0];
} __packed;

#define rpmsg_syslog_suspend		rpmsg_syslog_header
#define rpmsg_syslog_resume		rpmsg_syslog_header
#define rpmsg_syslog_transfer_done	rpmsg_syslog_header

struct rpmsg_syslog {
	char				*buf;
	unsigned int			next;
	unsigned int			size;
};

static int rpmsg_syslog_callback(struct rpmsg_device *rpdev,
				 void *data, int len, void *priv_, u32 src)
{
	struct rpmsg_syslog *priv = dev_get_drvdata(&rpdev->dev);
	struct rpmsg_syslog_transfer *msg = data;
	struct rpmsg_syslog_transfer_done done;
	unsigned int copied = msg->count;
	unsigned int printed = 0;
	const char *nl;

	if (msg->header.command != RPMSG_SYSLOG_TRANSFER)
		return -EINVAL;

	/* output the message before '\n' to the kernel log */
	nl = memrchr(msg->data, '\n', msg->count);
	if (nl) {
		printed = nl + 1 - msg->data;
		copied = msg->count - printed;

		if (priv->next) {
			pr_info(KERN_NOTIME "%.*s%.*s", priv->next,
				priv->buf, printed, msg->data);
			priv->next = 0;
		} else {
			pr_info(KERN_NOTIME "%.*s", printed, msg->data);
		}
	}

	/* append the message after '\n' to the buffer */
	if (copied != 0) {
		unsigned int newsize = priv->next + copied;

		if (newsize > priv->size) {
			char *newbuf;

			newbuf = krealloc(priv->buf, newsize, GFP_KERNEL);
			if (newbuf) {
				priv->buf  = newbuf;
				priv->size = newsize;
			} else {
				copied = priv->size - priv->next;
			}
		}

		strncpy(priv->buf + priv->next, msg->data + printed, copied);
		priv->next += copied;
	}

	done.command = RPMSG_SYSLOG_TRANSFER_DONE;
	done.result  = printed + copied;
	return rpmsg_send(rpdev->ept, &done, sizeof(done));
}

static int rpmsg_syslog_probe(struct rpmsg_device *rpdev)
{
	struct rpmsg_syslog *priv;

	priv = devm_kzalloc(&rpdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	dev_set_drvdata(&rpdev->dev, priv);
	return 0;
}

static void rpmsg_syslog_remove(struct rpmsg_device *rpdev)
{
	struct rpmsg_syslog *priv = dev_get_drvdata(&rpdev->dev);

	/* flush the buffered log if need */
	if (priv->next)
		pr_info(KERN_NOTIME "%.*s\n", priv->next, priv->buf);
	kfree(priv->buf);
}

#ifdef CONFIG_PM_SLEEP
static int rpmsg_syslog_dev_suspend(struct device *dev)
{
	struct rpmsg_device *rpdev = dev_get_drvdata(dev);
	struct rpmsg_syslog_suspend msg = {
		.command = RPMSG_SYSLOG_SUSPEND,
	};

	return rpmsg_send(rpdev->ept, &msg, sizeof(msg));
}

static int rpmsg_syslog_dev_resume(struct device *dev)
{
	struct rpmsg_device *rpdev = dev_get_drvdata(dev);
	struct rpmsg_syslog_resume msg = {
		.command = RPMSG_SYSLOG_RESUME,
	};

	return rpmsg_send(rpdev->ept, &msg, sizeof(msg));
}
#endif

static SIMPLE_DEV_PM_OPS(rpmsg_syslog_pm,
			rpmsg_syslog_dev_suspend,
			rpmsg_syslog_dev_resume);

static const struct rpmsg_device_id rpmsg_syslog_id_table[] = {
	{ .name = "rpmsg-syslog" },
	{ },
};
MODULE_DEVICE_TABLE(rpmsg, rpmsg_syslog_id_table);

static struct rpmsg_driver rpmsg_syslog_driver = {
	.drv = {
		.name  = "rpmsg_syslog",
		.owner = THIS_MODULE,
		.pm    = &rpmsg_syslog_pm,
	},

	.id_table = rpmsg_syslog_id_table,
	.probe    = rpmsg_syslog_probe,
	.callback = rpmsg_syslog_callback,
	.remove   = rpmsg_syslog_remove,
};

module_rpmsg_driver(rpmsg_syslog_driver);

MODULE_ALIAS("rpmsg:rpmsg_syslog");
MODULE_AUTHOR("Guiding Li <liguiding@pinecone.net>");
MODULE_DESCRIPTION("rpmsg syslog redirection driver");
MODULE_LICENSE("GPL v2");
