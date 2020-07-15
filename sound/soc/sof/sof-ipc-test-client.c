// SPDX-License-Identifier: GPL-2.0-only
//
// Copyright(c) 2020 Intel Corporation. All rights reserved.
//
// Author: Ranjani Sridharan <ranjani.sridharan@linux.intel.com>
//

#include <linux/auxiliary_bus.h>
#include <linux/completion.h>
#include <linux/debugfs.h>
#include <linux/ktime.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <sound/sof/header.h>
#include "sof-client.h"

#define MAX_IPC_FLOOD_DURATION_MS 1000
#define MAX_IPC_FLOOD_COUNT 10000
#define IPC_FLOOD_TEST_RESULT_LEN 512
#define SOF_IPC_CLIENT_SUSPEND_DELAY_MS 3000

struct sof_ipc_client_data {
	struct dentry *dfs_root;
	char *buf;
};

/*
 * helper function to perform the flood test. Only one of the two params, ipc_duration_ms
 * or ipc_count, will be non-zero and will determine the type of test
 */
static int sof_debug_ipc_flood_test(struct sof_client_dev *cdev, unsigned long ipc_duration_ms,
				    unsigned long ipc_count)
{
	struct sof_ipc_client_data *ipc_client_data = cdev->data;
	struct device *dev = &cdev->auxdev.dev;
	struct sof_ipc_cmd_hdr hdr;
	struct sof_ipc_reply reply;
	u64 min_response_time = U64_MAX;
	u64 avg_response_time = 0;
	u64 max_response_time = 0;
	ktime_t cur;
	ktime_t test_end;
	int i = 0;
	int ret = 0;
	bool end_test = false;

	/* configure test IPC */
	hdr.cmd = SOF_IPC_GLB_TEST_MSG | SOF_IPC_TEST_IPC_FLOOD;
	hdr.size = sizeof(hdr);

	/* set test end time for duration flood test */
	test_end = ktime_get_ns() + ipc_duration_ms * NSEC_PER_MSEC;

	/* send test IPC's */
	do {
		ktime_t start;
		u64 ipc_response_time;

		start = ktime_get();
		ret = sof_client_ipc_tx_message(cdev, hdr.cmd, &hdr, hdr.size, &reply,
						sizeof(reply));
		if (ret < 0)
			break;
		cur = ktime_get();

		i++;

		/* compute min and max response times */
		ipc_response_time = ktime_to_ns(ktime_sub(cur, start));
		min_response_time = min(min_response_time, ipc_response_time);
		max_response_time = max(max_response_time, ipc_response_time);

		/* sum up response times */
		avg_response_time += ipc_response_time;

		/* end test? */
		if (ipc_count && i == ipc_count)
			end_test = true;
		else if (ipc_duration_ms && (ktime_to_ns(cur) >= test_end))
			end_test = true;

	} while (!end_test);

	if (ret < 0)
		return ret;

	/* return if the first IPC fails */
	if (!i)
		return ret;

	/* compute average response time */
	DIV_ROUND_CLOSEST(avg_response_time, i);

	/* clear previous test output */
	memset(ipc_client_data->buf, 0, IPC_FLOOD_TEST_RESULT_LEN);

	if (!ipc_count) {
		dev_dbg(dev, "IPC Flood test duration: %lums\n", ipc_duration_ms);
		snprintf(ipc_client_data->buf, IPC_FLOOD_TEST_RESULT_LEN,
			 "IPC Flood test duration: %lums\n", ipc_duration_ms);
	}

	dev_dbg(dev,
		"IPC Flood count: %d, Avg response time: %lluns\n", i, avg_response_time);
	dev_dbg(dev, "Max response time: %lluns\n", max_response_time);
	dev_dbg(dev, "Min response time: %lluns\n", min_response_time);

	/* format output string and save test results */
	snprintf(ipc_client_data->buf + strlen(ipc_client_data->buf),
		 IPC_FLOOD_TEST_RESULT_LEN - strlen(ipc_client_data->buf),
		 "IPC Flood count: %d\nAvg response time: %lluns\n", i, avg_response_time);

	snprintf(ipc_client_data->buf + strlen(ipc_client_data->buf),
		 IPC_FLOOD_TEST_RESULT_LEN - strlen(ipc_client_data->buf),
		 "Max response time: %lluns\nMin response time: %lluns\n",
		 max_response_time, min_response_time);

	return ret;
}

/*
 * Writing to the debugfs entry initiates the IPC flood test based on
 * the IPC count or the duration specified by the user.
 */
static ssize_t sof_ipc_dfsentry_write(struct file *file, const char __user *buffer,
				      size_t count, loff_t *ppos)
{
	struct dentry *dentry = file->f_path.dentry;
	struct sof_client_dev *cdev = file->private_data;
	struct device *dev = &cdev->auxdev.dev;
	unsigned long ipc_duration_ms = 0;
	bool flood_duration_test;
	unsigned long ipc_count = 0;
	char *string;
	size_t size;
	int err;
	int ret;

	string = kzalloc(count, GFP_KERNEL);
	if (!string)
		return -ENOMEM;

	size = simple_write_to_buffer(string, count, ppos, buffer, count);

	flood_duration_test = !strcmp(dentry->d_name.name, "ipc_flood_duration_ms");

	/* limit max duration/ipc count for flood test */
	if (flood_duration_test) {
		ret = kstrtoul(string, 0, &ipc_duration_ms);
		if (ret < 0)
			goto out;

		if (!ipc_duration_ms) {
			ret = size;
			goto out;
		}

		ipc_duration_ms = min_t(unsigned long, ipc_duration_ms, MAX_IPC_FLOOD_DURATION_MS);
	} else {
		ret = kstrtoul(string, 0, &ipc_count);
		if (ret < 0)
			goto out;

		if (!ipc_count) {
			ret = size;
			goto out;
		}

		ipc_count = min_t(unsigned long, ipc_count, MAX_IPC_FLOOD_COUNT);
	}

	ret = pm_runtime_get_sync(dev);
	if (ret < 0 && ret != -EACCES) {
		dev_err_ratelimited(dev, "error: debugfs write failed to resume %d\n", ret);
		pm_runtime_put_noidle(dev);
		goto out;
	}

	ret = sof_debug_ipc_flood_test(cdev, ipc_duration_ms, ipc_count);

	pm_runtime_mark_last_busy(dev);
	err = pm_runtime_put_autosuspend(dev);
	if (err < 0) {
		ret = err;
		goto out;
	}

	/* return size if test is successful */
	if (ret >= 0)
		ret = size;
out:
	kfree(string);
	return ret;
}

/* return the result of the last IPC flood test */
static ssize_t sof_ipc_dfsentry_read(struct file *file, char __user *buffer,
				     size_t count, loff_t *ppos)
{
	struct sof_client_dev *cdev = file->private_data;
	struct sof_ipc_client_data *ipc_client_data = cdev->data;
	size_t size_ret;

	if (*ppos)
		return 0;

	/* return results of the last IPC test */
	count = min_t(size_t, count, strlen(ipc_client_data->buf));
	size_ret = copy_to_user(buffer, ipc_client_data->buf, count);
	if (size_ret)
		return -EFAULT;

	*ppos += count;
	return count;
}

static const struct file_operations sof_ipc_dfs_fops = {
	.open = simple_open,
	.read = sof_ipc_dfsentry_read,
	.llseek = default_llseek,
	.write = sof_ipc_dfsentry_write,
};

/*
 * The IPC test client creates a couple of debugfs entries that will be used
 * flood tests. Users can write to these entries to execute the IPC flood test
 * by specifying either the number of IPCs to flood the DSP with or the duration
 * (in ms) for which the DSP should be flooded with test IPCs. At the
 * end of each test, the average, min and max response times are reported back.
 * The results of the last flood test can be accessed by reading the debugfs
 * entries.
 */
static int sof_ipc_test_probe(struct auxiliary_device *auxdev,
			      const struct auxiliary_device_id *id)
{
	struct sof_client_dev *cdev = auxiliary_dev_to_sof_client_dev(auxdev);
	struct sof_ipc_client_data *ipc_client_data;

	/* allocate memory for client data */
	ipc_client_data = devm_kzalloc(&auxdev->dev, sizeof(*ipc_client_data), GFP_KERNEL);
	if (!ipc_client_data)
		return -ENOMEM;

	ipc_client_data->buf = devm_kzalloc(&auxdev->dev, IPC_FLOOD_TEST_RESULT_LEN, GFP_KERNEL);
	if (!ipc_client_data->buf)
		return -ENOMEM;

	cdev->data = ipc_client_data;

	/* create debugfs root folder with device name under parent SOF dir */
	ipc_client_data->dfs_root = debugfs_create_dir(dev_name(&auxdev->dev),
						       sof_client_get_debugfs_root(cdev));

	/* create read-write ipc_flood_count debugfs entry */
	debugfs_create_file("ipc_flood_count", 0644, ipc_client_data->dfs_root,
			    cdev, &sof_ipc_dfs_fops);

	/* create read-write ipc_flood_duration_ms debugfs entry */
	debugfs_create_file("ipc_flood_duration_ms", 0644, ipc_client_data->dfs_root,
			    cdev, &sof_ipc_dfs_fops);

	/* enable runtime PM */
	pm_runtime_set_autosuspend_delay(&auxdev->dev, SOF_IPC_CLIENT_SUSPEND_DELAY_MS);
	pm_runtime_use_autosuspend(&auxdev->dev);
	pm_runtime_enable(&auxdev->dev);
	pm_runtime_mark_last_busy(&auxdev->dev);
	pm_runtime_idle(&auxdev->dev);

	return 0;
}

static int sof_ipc_test_cleanup(struct auxiliary_device *auxdev)
{
	struct sof_client_dev *cdev = auxiliary_dev_to_sof_client_dev(auxdev);
	struct sof_ipc_client_data *ipc_client_data = cdev->data;

	pm_runtime_disable(&auxdev->dev);

	debugfs_remove_recursive(ipc_client_data->dfs_root);

	return 0;
}

static int sof_ipc_test_remove(struct auxiliary_device *auxdev)
{
	return sof_ipc_test_cleanup(auxdev);
}

static void sof_ipc_test_shutdown(struct auxiliary_device *auxdev)
{
	sof_ipc_test_cleanup(auxdev);
}

static const struct auxiliary_device_id sof_ipc_auxbus_id_table[] = {
	{ .name = "snd_sof_client.ipc_test" },
	{},
};
MODULE_DEVICE_TABLE(auxiliary, sof_ipc_auxbus_id_table);

/*
 * No need for driver pm_ops as the generic pm callbacks in the auxiliary bus type are enough to
 * ensure that the parent SOF device resumes to bring the DSP back to D0.
 * driver name will be set based on KBUILD_MODNAME.
 */
static struct sof_client_drv sof_ipc_test_client_drv = {
	.auxiliary_drv = {
		.id_table = sof_ipc_auxbus_id_table,
		.probe = sof_ipc_test_probe,
		.remove = sof_ipc_test_remove,
		.shutdown = sof_ipc_test_shutdown,
	},
};

module_sof_client_driver(sof_ipc_test_client_drv);

MODULE_DESCRIPTION("SOF IPC Test Client Driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(SND_SOC_SOF_CLIENT);
