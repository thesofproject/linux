// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2017 Pinecone Inc.
 *
 * redirect rtc API from remote to the kernel.
 */

#include <linux/alarmtimer.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/rpmsg.h>
#include <linux/slab.h>
#include <linux/timekeeping.h>
#include <linux/workqueue.h>

#define RPMSG_RTC_SET		0
#define RPMSG_RTC_GET		1
#define RPMSG_RTC_ALARM_SET	2
#define RPMSG_RTC_ALARM_CANCEL	3
#define RPMSG_RTC_ALARM_FIRE	4

struct rpmsg_rtc_header {
	u32			command;
	s32			result;
	u64			cookie;
} __packed;

struct rpmsg_rtc_set {
	struct rpmsg_rtc_header	header;
	s64			sec;
	s32			nsec;
} __packed;

#define rpmsg_rtc_get		rpmsg_rtc_set

struct rpmsg_rtc_alarm_set {
	struct rpmsg_rtc_header	header;
	s64			sec;
	s32			nsec;
	s32			id;
} __packed;

struct rpmsg_rtc_alarm_cancel {
	struct rpmsg_rtc_header	header;
	s32			id;
} __packed;

#define rpmsg_rtc_alarm_fire	rpmsg_rtc_alarm_cancel

struct rpmsg_rtc_alarm {
	struct alarm		alarm;
	struct work_struct	work;
	int			id;
};

struct rpmsg_rtc {
	struct mutex		lock; /* protect alarms field */
	struct idr		alarms;
};

static int rpmsg_rtc_set_handler(struct rpmsg_device *rpdev,
				 void *data, int len, void *priv, u32 src)
{
	struct rpmsg_rtc_set *msg = data;
	struct timespec64 time = {
		.tv_sec  = msg->sec,
		.tv_nsec = msg->nsec,
	};

	msg->header.result = do_settimeofday64(&time);
	return rpmsg_send(rpdev->ept, msg, sizeof(*msg));
}

static int rpmsg_rtc_get_handler(struct rpmsg_device *rpdev,
				 void *data, int len, void *priv, u32 src)
{
	struct rpmsg_rtc_get *msg = data;
	struct timespec64 time;

	getnstimeofday64(&time);

	msg->header.result = 0;
	msg->sec  = time.tv_sec;
	msg->nsec = time.tv_nsec;

	return rpmsg_send(rpdev->ept, msg, sizeof(*msg));
}

static void rpmsg_rtc_alarm_work(struct work_struct *work)
{
	struct rpmsg_rtc_alarm *alarm =
		container_of(work, struct rpmsg_rtc_alarm, work);
	struct rpmsg_device *rpdev = alarm->alarm.data;
	struct rpmsg_rtc_alarm_fire msg = {
		.header.command = RPMSG_RTC_ALARM_FIRE,
		.id = alarm->id,
	};

	rpmsg_send(rpdev->ept, &msg, sizeof(msg));
}

static enum alarmtimer_restart
rpmsg_rtc_alarm_func(struct alarm *alarm_, ktime_t now)
{
	struct rpmsg_rtc_alarm *alarm =
		container_of(alarm_, struct rpmsg_rtc_alarm, alarm);

	schedule_work(&alarm->work);
	return ALARMTIMER_NORESTART;
}

static int
rpmsg_rtc_alarm_set_handler(struct rpmsg_device *rpdev,
			    void *data, int len, void *priv_, u32 src)
{
	struct rpmsg_rtc *priv = dev_get_drvdata(&rpdev->dev);
	struct rpmsg_rtc_alarm_set *msg = data;
	struct rpmsg_rtc_alarm *alarm;

	mutex_lock(&priv->lock);
	alarm = idr_find(&priv->alarms, msg->id);
	if (!alarm) {
		alarm = kzalloc(sizeof(*alarm), GFP_KERNEL);
		if (alarm) {
			alarm->alarm.data = rpdev;
			INIT_WORK(&alarm->work, rpmsg_rtc_alarm_work);
			alarm_init(&alarm->alarm,
				   ALARM_REALTIME, rpmsg_rtc_alarm_func);
			alarm->id = idr_alloc(&priv->alarms, alarm,
					      msg->id, msg->id, GFP_KERNEL);
			if (alarm->id < 0) {
				kfree(alarm);
				alarm = NULL;
			}
		}
	}
	mutex_unlock(&priv->lock);

	if (alarm) {
		alarm_start(&alarm->alarm, ktime_set(msg->sec, msg->nsec));
		msg->header.result = 0;
	} else {
		msg->header.result = -ENOMEM;
	}

	return rpmsg_send(rpdev->ept, msg, sizeof(*msg));
}

static int
rpmsg_rtc_alarm_cancel_handler(struct rpmsg_device *rpdev,
			       void *data, int len, void *priv_, u32 src)
{
	struct rpmsg_rtc *priv = dev_get_drvdata(&rpdev->dev);
	struct rpmsg_rtc_alarm_cancel *msg = data;
	struct rpmsg_rtc_alarm *alarm;

	mutex_lock(&priv->lock);
	alarm = idr_find(&priv->alarms, msg->id);
	if (alarm) {
		idr_remove(&priv->alarms, msg->id);
		alarm_cancel(&alarm->alarm);
		cancel_work_sync(&alarm->work);
		kfree(alarm);
	}
	mutex_unlock(&priv->lock);

	msg->header.result = 0;
	return rpmsg_send(rpdev->ept, msg, sizeof(*msg));
}

static const rpmsg_rx_cb_t rpmsg_rtc_handler[] = {
	[RPMSG_RTC_SET]          = rpmsg_rtc_set_handler,
	[RPMSG_RTC_GET]          = rpmsg_rtc_get_handler,
	[RPMSG_RTC_ALARM_SET]    = rpmsg_rtc_alarm_set_handler,
	[RPMSG_RTC_ALARM_CANCEL] = rpmsg_rtc_alarm_cancel_handler,
};

static int rpmsg_rtc_callback(struct rpmsg_device *rpdev,
			      void *data, int len, void *priv, u32 src)
{
	struct rpmsg_rtc_header *header = data;
	u32 cmd = header->command;
	int ret = -EINVAL;

	if (cmd < ARRAY_SIZE(rpmsg_rtc_handler)) {
		ret = rpmsg_rtc_handler[cmd](rpdev, data, len, priv, src);
		if (ret < 0)
			dev_err(&rpdev->dev, "command handle error %d\n", cmd);
	}

	return ret;
}

static int rpmsg_rtc_probe(struct rpmsg_device *rpdev)
{
	struct rpmsg_rtc *priv;

	priv = devm_kzalloc(&rpdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	mutex_init(&priv->lock);
	idr_init(&priv->alarms);
	dev_set_drvdata(&rpdev->dev, priv);

	return 0;
}

static void rpmsg_rtc_remove(struct rpmsg_device *rpdev)
{
	struct rpmsg_rtc *priv = dev_get_drvdata(&rpdev->dev);
	struct rpmsg_rtc_alarm *alarm;
	int id;

	idr_for_each_entry(&priv->alarms, alarm, id) {
		alarm_cancel(&alarm->alarm);
		cancel_work_sync(&alarm->work);
		kfree(alarm);
	}

	mutex_destroy(&priv->lock);
	idr_destroy(&priv->alarms);
}

static const struct rpmsg_device_id rpmsg_rtc_id_table[] = {
	{ .name = "rpmsg-rtc" },
	{ },
};
MODULE_DEVICE_TABLE(rpmsg, rpmsg_rtc_id_table);

static struct rpmsg_driver rpmsg_rtc_driver = {
	.drv = {
		.name	= "rpmsg_rtc",
		.owner	= THIS_MODULE,
	},

	.id_table	= rpmsg_rtc_id_table,
	.probe		= rpmsg_rtc_probe,
	.callback	= rpmsg_rtc_callback,
	.remove		= rpmsg_rtc_remove,
};

module_rpmsg_driver(rpmsg_rtc_driver);

MODULE_ALIAS("rpmsg:rpmsg_rtc");
MODULE_AUTHOR("Guiding Li <liguiding@pinecone.net>");
MODULE_DESCRIPTION("rpmsg rtc API redirection driver");
MODULE_LICENSE("GPL v2");
