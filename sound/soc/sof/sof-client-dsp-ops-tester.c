// SPDX-License-Identifier: GPL-2.0-only
//
// Copyright(c) 2023 Intel Corporation. All rights reserved.
//
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

#define SOF_DSP_OPS_TESTER_CLIENT_SUSPEND_DELAY_MS	3000

struct sof_dsp_ops_tester_priv {
	struct dentry *dfs_root;
};

static int sof_dsp_ops_tester_dfs_open(struct inode *inode, struct file *file)
{
	struct sof_client_dev *cdev = inode->i_private;
	int ret;

	if (sof_client_get_fw_state(cdev) == SOF_FW_CRASHED)
		return -ENODEV;

	ret = debugfs_file_get(file->f_path.dentry);
	if (unlikely(ret))
		return ret;

	ret = simple_open(inode, file);
	if (ret)
		debugfs_file_put(file->f_path.dentry);

	return ret;
}

/* ops are executed as "op_name,argument1,argument2...". For example, to set the DSP power state
 * to D3: echo "set_power_state,D3" > op_to_execute"
 */
static ssize_t sof_dsp_ops_tester_dfs_write(struct file *file, const char __user *buffer,
					    size_t count, loff_t *ppos)
{
	struct sof_client_dev *cdev = file->private_data;
	struct device *dev = &cdev->auxdev.dev;
	size_t size;
	const char *op_name;
	char *string;
	int ret;

	string = kzalloc(count + 1, GFP_KERNEL);
	if (!string)
		return -ENOMEM;

	size = simple_write_to_buffer(string, count, ppos, buffer, count);

	/* truncate the \n at the end */
	string[count - 1] = '\0';

	/* extract the name of the op to execute */
	op_name = strsep(&string, ",");
	if (!op_name)
		op_name = (const char *)string;

	if (!strcmp(op_name, "load_firmware")) {
		ret = sof_client_load_firmware(cdev);
		if (ret < 0)
			goto err;
	}

	if (!strcmp(op_name, "unload_firmware")) {
		sof_client_unload_firmware(cdev);
		dev_dbg(dev, "firmware unloaded\n");
	}

	if (!strcmp(op_name, "run_firmware")) {
		ret = sof_client_run_firmware(cdev);
		if (ret < 0)
			goto err;
	}

	if (!strcmp(op_name, "set_power_state")) {
		ret = sof_client_set_power_state(cdev, string);
		if (ret < 0)
			goto err;
	}
err:
	if (ret >= 0)
		ret = size;

	kfree(string);

	return ret;
}

static int sof_dsp_ops_tester_dfs_release(struct inode *inode, struct file *file)
{
	debugfs_file_put(file->f_path.dentry);

	return 0;
}

static const struct file_operations sof_dsp_ops_tester_fops = {
	.open = sof_dsp_ops_tester_dfs_open,
	.write = sof_dsp_ops_tester_dfs_write,
	.release = sof_dsp_ops_tester_dfs_release,

	.owner = THIS_MODULE,
};

static int sof_dsp_ops_tester_probe(struct auxiliary_device *auxdev,
				    const struct auxiliary_device_id *id)
{
	struct sof_client_dev *cdev = auxiliary_dev_to_sof_client_dev(auxdev);
	struct dentry *debugfs_root = sof_client_get_debugfs_root(cdev);
	struct device *dev = &auxdev->dev;
	struct sof_dsp_ops_tester_priv *priv;

	/* allocate memory for client data */
	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	cdev->data = priv;

	priv->dfs_root = debugfs_create_dir(dev_name(dev), debugfs_root);
	if (!IS_ERR_OR_NULL(priv->dfs_root)) {
		debugfs_create_file("op_to_execute", 0644, priv->dfs_root,
				    cdev, &sof_dsp_ops_tester_fops);
	}

	/* enable runtime PM */
	pm_runtime_set_autosuspend_delay(dev, SOF_DSP_OPS_TESTER_CLIENT_SUSPEND_DELAY_MS);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_enable(dev);
	pm_runtime_mark_last_busy(dev);
	pm_runtime_idle(dev);

	return 0;
}

static void sof_dsp_ops_tester_remove(struct auxiliary_device *auxdev)
{
	struct sof_client_dev *cdev = auxiliary_dev_to_sof_client_dev(auxdev);
	struct sof_dsp_ops_tester_priv *priv = cdev->data;

	pm_runtime_disable(&auxdev->dev);

	debugfs_remove_recursive(priv->dfs_root);
}

static const struct auxiliary_device_id sof_dsp_ops_tester_client_id_table[] = {
	{ .name = "snd_sof.dsp_ops_tester" },
	{},
};
MODULE_DEVICE_TABLE(auxiliary, sof_dsp_ops_tester_client_id_table);

/*
 * No need for driver pm_ops as the generic pm callbacks in the auxiliary bus
 * type are enough to ensure that the parent SOF device resumes to bring the DSP
 * back to D0.
 * Driver name will be set based on KBUILD_MODNAME.
 */
static struct auxiliary_driver sof_dsp_ops_tester_client_drv = {
	.probe = sof_dsp_ops_tester_probe,
	.remove = sof_dsp_ops_tester_remove,

	.id_table = sof_dsp_ops_tester_client_id_table,
};

module_auxiliary_driver(sof_dsp_ops_tester_client_drv);

MODULE_DESCRIPTION("SOF DSP Ops Tester Client Driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(SND_SOC_SOF_CLIENT);
