/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2017 Pinecone Inc.
 */

#include <linux/module.h>
#include <linux/rpmsg.h>
#include <linux/slab.h>

#define RPMSG_SYSLOG_TRANSFER		0
#define RPMSG_SYSLOG_TRANSFER_DONE	1
#define RPMSG_SYSLOG_SUSPEND		2
#define RPMSG_SYSLOG_RESUME		3

struct rpmsg_syslog_header {
	uint32_t			command;
	int32_t				result;
} __packed;

struct rpmsg_syslog_transfer {
	struct rpmsg_syslog_header	header;
	uint32_t			count;
	char				data[0];
} __packed;

#define rpmsg_syslog_suspend		rpmsg_syslog_header
#define rpmsg_syslog_resume		rpmsg_syslog_header
#define rpmsg_syslog_transfer_done	rpmsg_syslog_header

struct rpmsg_syslog {
	char				*tmpbuf;
	unsigned int			nextpos;
	unsigned int			alloced;
};

static int rpmsg_syslog_callback(struct rpmsg_device *rpdev,
		void *data, int len, void *priv_, u32 src)
{
	struct rpmsg_syslog *priv = dev_get_drvdata(&rpdev->dev);
	struct rpmsg_syslog_header *header = data;

	if (header->command == RPMSG_SYSLOG_TRANSFER) {
		struct rpmsg_syslog_transfer *msg = data;
		struct rpmsg_syslog_transfer_done done;
		unsigned int copied = msg->count;
		unsigned int printed = 0;
		const char *nl;

		nl = memrchr(msg->data, '\n', msg->count);
		if (nl != NULL) {
			printed = nl + 1 - msg->data;
			copied = msg->count - printed;

			if (priv->nextpos) {
				pr_info(KERN_TIME "%.*s%.*s", priv->nextpos,
					priv->tmpbuf, printed, msg->data);
				priv->nextpos = 0;
			} else
				pr_info(KERN_TIME "%.*s", printed, msg->data);
		}

		if (copied != 0) {
			unsigned int newsize = priv->nextpos + copied;
			if (newsize > priv->alloced) {
				char *newbuf = krealloc(priv->tmpbuf, newsize, GFP_KERNEL);
				if (newbuf != NULL) {
					priv->tmpbuf  = newbuf;
					priv->alloced = newsize;
				} else
					copied = priv->alloced - priv->nextpos;
			}
			memcpy(priv->tmpbuf + priv->nextpos, msg->data + printed, copied);
			priv->nextpos += copied;
		}

		done.command = RPMSG_SYSLOG_TRANSFER_DONE;
		done.result  = printed + copied;
		return rpmsg_send(rpdev->ept, &done, sizeof(done));
	}

	return -EINVAL;
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

	if (priv->nextpos)
		pr_info(KERN_TIME "%.*s\n", priv->nextpos, priv->tmpbuf);
	kfree(priv->tmpbuf);
}

#ifdef CONFIG_PM_SLEEP
static int rpmsg_syslog_dev_suspend(struct device *dev)
{
	struct rpmsg_device *rpdev = dev_get_drvdata(dev);
	struct rpmsg_syslog_suspend msg = {0};

	msg.command = RPMSG_SYSLOG_SUSPEND;
	return rpmsg_send(rpdev->ept, &msg, sizeof(msg));
}

static int rpmsg_syslog_dev_resume(struct device *dev)
{
	struct rpmsg_device *rpdev = dev_get_drvdata(dev);
	struct rpmsg_syslog_resume msg = {0};

	msg.command = RPMSG_SYSLOG_RESUME;
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

module_driver(rpmsg_syslog_driver,
		register_rpmsg_driver,
		unregister_rpmsg_driver);

MODULE_ALIAS("rpmsg:rpmsg_syslog");
MODULE_AUTHOR("Guiding Li <liguiding@pinecone.net>");
MODULE_DESCRIPTION("rpmsg syslog redirection driver");
MODULE_LICENSE("GPL v2");
