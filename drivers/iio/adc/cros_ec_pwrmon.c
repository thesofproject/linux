// SPDX-License-Identifier: GPL-2.0-only
/*
 * ChromeOS EC Power Monitor Driver
 *
 * Copyright (c) 2026 Google Inc.
 */

#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_data/cros_ec_commands.h>
#include <linux/platform_data/cros_ec_proto.h>
#include <linux/platform_device.h>

#include "cros_ec_pwrmon.h"

#define PWRMON_MAX_CHANNELS 40
#define UWATT_PER_WATT 1000000

struct cros_ec_pwrmon_state {
	struct cros_ec_command *msg;
	struct cros_ec_dev *ec_dev;
	struct cros_ec_device *ec;
	struct mutex lock;
	u64 *accumulated_energy;
	char (*channel_names)[32];
	int num_channels;
	int rate;
};

/*
 * Expose ODPM results in the form of accumulated energy
 * through the energy_value sysfs node.
 * Format requirement:
 * t=<Measurement timestamp, ms>
 * CH<N>(T=<Duration, ms>)[<Schematic name>], <Accumulated Energy, uWs>
 */
static ssize_t energy_value_show(struct device *dev,
				 struct device_attribute *attr,
				 char *buf)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct cros_ec_pwrmon_state *st = iio_priv(indio_dev);
	struct pwrmon_channel_info *info;
	struct ec_params_pwrmon *params;
	u32 duration_ms;
	u64 energy_uws;
	ssize_t len = 0;
	int ret, i;

	mutex_lock(&st->lock);
	/* Latch all PACs before the reading sequence. */
	params = (struct ec_params_pwrmon *)st->msg->data;
	params->cmd = EC_PWRMON_LATCH;
	ret = cros_ec_cmd_xfer_status(st->ec, st->msg);
	if (ret < 0) {
		dev_warn(dev, "Failed to latch PAC accumulators: %d\n", ret);
		mutex_unlock(&st->lock);
		return ret;
	}

	/* Timestamp of snapshot taken. */
	len += scnprintf(buf + len, PAGE_SIZE - len, "t=%llu\n",
			 ktime_get_boottime_ns() / NSEC_PER_MSEC);

	for (i = 0; i < st->num_channels; i++) {
		params->cmd = EC_PWRMON_GET;
		params->get_channel_info.channel_id = i;

		ret = cros_ec_cmd_xfer_status(st->ec, st->msg);
		if (ret < 0) {
			dev_warn(dev, "Failed to read ODPM data for channel %d: %d\n", i, ret);
			continue;
		}

		info = &((struct ec_response_pwrmon *)st->msg->data)->channel_info;

		duration_ms = 0;
		energy_uws = 0;
		if (st->rate > 0) {
			/* Duration of sampling. */
			duration_ms = (info->samples * MSEC_PER_SEC) / st->rate;
			/*
			 * PAC result value is accumulated power [Watt-samples].
			 * Convert it to energy in uWs: uWatt-samples / sampling_freq.
			 */
			energy_uws = (info->value * UWATT_PER_WATT) / st->rate;
		}

		st->accumulated_energy[i] += energy_uws;

		if (len < PAGE_SIZE) {
			len += scnprintf(buf + len, PAGE_SIZE - len,
					 "CH%u(T=%u)[%s], %llu\n",
					 i,
					 duration_ms,
					 st->channel_names[i],
					 st->accumulated_energy[i]);
		}
	}
	mutex_unlock(&st->lock);

	return len;
}

/* Setup the IIO_DEVICE attribute for "energy_value" */
static IIO_DEVICE_ATTR(energy_value, 0444, energy_value_show, NULL, 0);

static struct attribute *cros_ec_pwrmon_attrs[] = {
	&iio_dev_attr_energy_value.dev_attr.attr,
	NULL
};

static const struct attribute_group cros_ec_pwrmon_attr_group = {
	.attrs = cros_ec_pwrmon_attrs,
};

static const struct iio_info cros_ec_pwrmon_info = {
	.attrs = &cros_ec_pwrmon_attr_group,
};

static int cros_ec_pwrmon_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct cros_ec_dev *ec_dev = dev_get_drvdata(dev->parent);
	struct cros_ec_pwrmon_state *st;
	struct ec_params_pwrmon *params;
	struct pwrmon_dump_info *dump;
	struct iio_dev *indio_dev;
	size_t msg_size;
	int ret, i;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*st));
	if (!indio_dev)
		return -ENOMEM;

	st = iio_priv(indio_dev);
	st->ec_dev = ec_dev;
	st->ec = ec_dev->ec_dev;

	ret = devm_mutex_init(dev, &st->lock);
	if (ret)
		return ret;

	msg_size = sizeof(*st->msg) + max(sizeof(struct ec_params_pwrmon),
				      sizeof(struct ec_response_pwrmon));
	st->msg = devm_kzalloc(dev, msg_size, GFP_KERNEL);
	if (!st->msg)
		return -ENOMEM;

	st->msg->version = 0;
	st->msg->command = EC_CMD_PWRMON  + st->ec_dev->cmd_offset;
	params = (struct ec_params_pwrmon *)st->msg->data;
	st->msg->outsize = sizeof(*params);
	st->msg->insize = sizeof(struct ec_response_pwrmon);

	/* Start PACs */
	params->cmd = EC_PWRMON_START;
	ret = cros_ec_cmd_xfer_status(st->ec, st->msg);
	if (ret < 0) {
		dev_warn(dev, "Failed to start PACs: %d\n", ret);
		return ret;
	}

	/* Get Channel Count */
	params->cmd = EC_PWRMON_GET_CHANNEL_COUNT;
	ret = cros_ec_cmd_xfer_status(st->ec, st->msg);
	if (ret < 0) {
		dev_warn(dev, "Failed to get channel count: %d\n", ret);
		return ret < 0 ? ret : -EIO;
	}
	st->num_channels = ((struct ec_response_pwrmon *)st->msg->data)->channel_count.count;
	if (st->num_channels > PWRMON_MAX_CHANNELS)
		st->num_channels = PWRMON_MAX_CHANNELS;

	st->accumulated_energy = devm_kcalloc(dev, st->num_channels, sizeof(*st->accumulated_energy), GFP_KERNEL);
	st->channel_names = devm_kcalloc(dev, st->num_channels, sizeof(*st->channel_names), GFP_KERNEL);
	if (!st->accumulated_energy || !st->channel_names)
		return -ENOMEM;

	/* Get Rate (PAC sampling frequency) */
	params->cmd = EC_PWRMON_GET_RATE;
	ret = cros_ec_cmd_xfer_status(st->ec, st->msg);
	if (ret < 0) {
		dev_warn(dev, "Failed to get rate: %d\n", ret);
		return ret;
	}
	st->rate = ((struct ec_response_pwrmon *)st->msg->data)->get_rate.rate;

	/* Get Dump Info (Names) */
	for (i = 0; i < st->num_channels; i++) {
		params->cmd = EC_PWRMON_DUMP_INFO;
		params->get_dump_info.channel_id = i;
		ret = cros_ec_cmd_xfer_status(st->ec, st->msg);
		if (ret < 0) {
			dev_warn(dev, "Failed to get dump info for channel %d: %d\n", i, ret);
			continue;
		}
		dump = &((struct ec_response_pwrmon *)st->msg->data)->dump_info;
		strscpy(st->channel_names[i], dump->channel_name, sizeof(st->channel_names[i]));
	}

	indio_dev->name = dev_name(dev);
	indio_dev->info = &cros_ec_pwrmon_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	platform_set_drvdata(pdev, indio_dev);

	return devm_iio_device_register(dev, indio_dev);
}

static void cros_ec_pwrmon_remove(struct platform_device *pdev)
{
	struct iio_dev *indio_dev = platform_get_drvdata(pdev);
	struct cros_ec_pwrmon_state *st = iio_priv(indio_dev);
	struct ec_params_pwrmon *params;
	int ret;

	/* Stop PACs */
	params = (struct ec_params_pwrmon *)st->msg->data;
	params->cmd = EC_PWRMON_STOP;
	ret = cros_ec_cmd_xfer_status(st->ec, st->msg);
	if (ret < 0)
		dev_warn(&pdev->dev, "Failed to stop PACs: %d\n", ret);
}

static const struct platform_device_id cros_ec_pwrmon_id[] = {
	{ "cros-ec-pwrmon", 0 },
	{}
};

static struct platform_driver cros_ec_pwrmon_driver = {
	.driver = {
		.name = "cros-ec-pwrmon",
	},
	.probe = cros_ec_pwrmon_probe,
	.remove = cros_ec_pwrmon_remove,
	.id_table = cros_ec_pwrmon_id,
};
module_platform_driver(cros_ec_pwrmon_driver);

MODULE_DEVICE_TABLE(platform, cros_ec_pwrmon_id);
MODULE_AUTHOR("Daniel Balint <dbalint@google.com>");
MODULE_DESCRIPTION("ChromeOS EC Power Monitor Driver");
MODULE_LICENSE("GPL");
