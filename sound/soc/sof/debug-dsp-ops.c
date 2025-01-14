// SPDX-License-Identifier: GPL-2.0-only
//
// Copyright(c) 2024 Intel Corporation.
//

#include <linux/debugfs.h>
#include <sound/sof/debug.h>
#include "sof-priv.h"
#include "ops.h"

#define MAX_FW_STATE_STRING_LEN 128

#define SOF_DSP_PM_D0I0 0
#define SOF_DSP_PM_D0I3 1

/*
 * set dsp power state op by writing the requested power state.
 * ex: echo D3 > dsp_power_state
 */
static int sof_dsp_ops_set_power_state(struct snd_sof_dev *sdev, char *state)
{
	const struct sof_ipc_pm_ops *pm_ops = sof_ipc_get_ops(sdev, pm);
	struct sof_dsp_power_state target_state;
	int ret;

	/* Only D3/D0IO/D0I3 states supported */
	if (strcmp(state, "D3") && strcmp(state, "D0I3") && strcmp(state, "D0I0")) {
		dev_err(sdev->dev, "Unsupported DSP PM state %s\n", state);
		return -EINVAL;
	}

	/* put the DSP in D3 state */
	if (!strcmp(state, "D3")) {
		if (sdev->dsp_power_state.state == SOF_DSP_PM_D3)
			return 0;

		target_state.state = SOF_DSP_PM_D3;

		/* notify DSP of upcoming power down */
		if (pm_ops && pm_ops->ctx_save) {
			ret = pm_ops->ctx_save(sdev);
			if (ret < 0)
				return ret;
		}

		ret = snd_sof_dsp_set_power_state(sdev, &target_state);
		if (ret < 0)
			return ret;

		sdev->enabled_cores_mask = 0;
		sof_set_fw_state(sdev, SOF_FW_BOOT_NOT_STARTED);

		return 0;
	}

	/* handle D0I0/D0I3 transitions */
	target_state.state = SOF_DSP_PM_D0;
	if (!strcmp(state, "D0I3"))
		target_state.substate = SOF_DSP_PM_D0I3;
	else
		target_state.substate = SOF_DSP_PM_D0I0;

	return snd_sof_dsp_set_power_state(sdev, &target_state);
}

static int sof_dsp_ops_trace_init(struct snd_sof_dev *sdev, bool init)
{
	if (init)
		return sof_fw_trace_init(sdev);

	sof_fw_trace_free(sdev);
	return 0;
}

static void sof_dsp_ops_unload_firmware(struct snd_sof_dev *sdev)
{
	/* release the currently loaded firmware */
	if (sdev->basefw.fw)
		snd_sof_fw_unload(sdev);

	sof_set_fw_state(sdev, SOF_FW_BOOT_PREPARE);
}

static int sof_dsp_ops_boot_firmware(struct snd_sof_dev *sdev)
{
	const char *fw_filename = NULL;
	int ret;

	if (sdev->test_profile.fw_name && strlen(sdev->test_profile.fw_name)) {
		if (sdev->test_profile.fw_path && strlen(sdev->test_profile.fw_path))
			fw_filename = kasprintf(GFP_KERNEL, "%s/%s", sdev->test_profile.fw_path,
						sdev->test_profile.fw_name);
		else
			fw_filename = kasprintf(GFP_KERNEL, "%s", sdev->test_profile.fw_name);
	}

	/* If fw_filename is NULL the firmware from the default profile will be loaded */
	ret = snd_sof_load_firmware(sdev, fw_filename);
	kfree(fw_filename);
	if (ret < 0)
		return ret;

	/* boot firmware */
	sof_set_fw_state(sdev, SOF_FW_BOOT_IN_PROGRESS);

	ret = snd_sof_run_firmware(sdev);

	/* set first_boot to false so subsequent boots will be from IMR if supported */
	if (!ret)
		sdev->first_boot = false;

	return ret;
}

static ssize_t sof_dsp_ops_fw_state_read(struct snd_sof_dev *sdev, char __user *buffer,
					 size_t count, loff_t *ppos)
{
	char string[MAX_FW_STATE_STRING_LEN];
	size_t size_ret;

	switch (sdev->fw_state) {
	case SOF_FW_BOOT_NOT_STARTED:
		snprintf(string, MAX_FW_STATE_STRING_LEN,
			 "%d: NOT STARTED\n", sdev->fw_state);
		break;
	case SOF_DSPLESS_MODE:
		snprintf(string, MAX_FW_STATE_STRING_LEN,
			 "%d: DSPLESS MODE\n", sdev->fw_state);
		break;
	case SOF_FW_BOOT_PREPARE:
		snprintf(string, MAX_FW_STATE_STRING_LEN,
			 "%d: PREPARE\n", sdev->fw_state);
		break;
	case SOF_FW_BOOT_IN_PROGRESS:
		snprintf(string, MAX_FW_STATE_STRING_LEN,
			 "%d: BOOT IN PROGRESS\n", sdev->fw_state);
		break;
	case SOF_FW_BOOT_FAILED:
		snprintf(string, MAX_FW_STATE_STRING_LEN,
			 "%d: FAILED\n", sdev->fw_state);
		break;
	case SOF_FW_BOOT_READY_FAILED:
		snprintf(string, MAX_FW_STATE_STRING_LEN,
			 "%d: READY FAILED\n", sdev->fw_state);
		break;
	case SOF_FW_BOOT_READY_OK:
		snprintf(string, MAX_FW_STATE_STRING_LEN,
			 "%d: READY OK\n", sdev->fw_state);
		break;
	case SOF_FW_BOOT_COMPLETE:
		snprintf(string, MAX_FW_STATE_STRING_LEN,
			 "%d: COMPLETE\n", sdev->fw_state);
		break;
	case SOF_FW_CRASHED:
		snprintf(string, MAX_FW_STATE_STRING_LEN,
			 "%d: CRASHED\n", sdev->fw_state);
		break;
	default:
		break;
	}

	if (*ppos)
		return 0;

	count = min_t(size_t, count, strlen(string));
	size_ret = copy_to_user(buffer, string, count);
	if (size_ret)
		return -EFAULT;

	*ppos += count;

	return count;
}

static ssize_t sof_dsp_ops_tester_dfs_read(struct file *file, char __user *buffer,
					   size_t count, loff_t *ppos)
{
	struct snd_sof_dfsentry *dfse = file->private_data;
	struct snd_sof_dev *sdev = dfse->sdev;
	const char *string = NULL;
	struct dentry *dentry;
	size_t size_ret;

	/* return the FW filename or path */
	dentry = file->f_path.dentry;
	if (!strcmp(dentry->d_name.name, "fw_filename")) {
		string = sdev->test_profile.fw_name;
	} else if (!strcmp(dentry->d_name.name, "fw_path")) {
		string = sdev->test_profile.fw_path;
	} else if (!strcmp(dentry->d_name.name, "dsp_power_state")) {
		switch (sdev->dsp_power_state.state) {
		case SOF_DSP_PM_D0:
			switch (sdev->dsp_power_state.substate) {
			case SOF_DSP_PM_D0I0:
				string = "D0I0\n";
				break;
			case SOF_DSP_PM_D0I3:
				string = "D0I3\n";
				break;
			default:
				break;
			}
			break;
		case SOF_DSP_PM_D3:
			string = "D3\n";
			break;
		default:
			break;
		}
	} else if (!strcmp(dentry->d_name.name, "fw_state")) {
		return sof_dsp_ops_fw_state_read(sdev, buffer, count, ppos);
	} else {
		return 0;
	}

	if (*ppos || !string)
		return 0;

	count = min_t(size_t, count, strlen(string));
	size_ret = copy_to_user(buffer, string, count);
	if (size_ret)
		return -EFAULT;

	*ppos += count;

	return count;
}

static ssize_t sof_dsp_ops_tester_dfs_write(struct file *file, const char __user *buffer,
					    size_t count, loff_t *ppos)
{
	struct snd_sof_dfsentry *dfse = file->private_data;
	struct dentry *dentry = file->f_path.dentry;
	struct snd_sof_dev *sdev = dfse->sdev;
	size_t size;
	char *string;


	if (!strcmp(dentry->d_name.name, "init_trace"))
		return sof_dsp_ops_trace_init(sdev, true);

	if (!strcmp(dentry->d_name.name, "free_trace"))
		return sof_dsp_ops_trace_init(sdev, false);

	if (!strcmp(dentry->d_name.name, "unload_fw")) {
		string = kzalloc(count + 1, GFP_KERNEL);
		if (!string)
			return -ENOMEM;

		size = simple_write_to_buffer(string, count, ppos, buffer, count);
		kfree(string);
		sof_dsp_ops_unload_firmware(sdev);
		return size;
	}

	if (!strcmp(dentry->d_name.name, "boot_fw")) {
		int ret;
		string = kzalloc(count + 1, GFP_KERNEL);
		if (!string)
			return -ENOMEM;

		size = simple_write_to_buffer(string, count, ppos, buffer, count);
		kfree(string);

		ret = sof_dsp_ops_boot_firmware(sdev);
		if (ret < 0)
			return ret;
		return size;
	}

	/* set DSP power state */
	if (!strcmp(dentry->d_name.name, "dsp_power_state")) {
		int ret;

		string = kzalloc(count + 1, GFP_KERNEL);
		if (!string)
			return -ENOMEM;

		size = simple_write_to_buffer(string, count, ppos, buffer, count);

		/* truncate the \n at the end */
		string[count - 1] = '\0';
		ret = sof_dsp_ops_set_power_state(sdev, string);
		kfree(string);
		if (ret < 0)
			return ret;

		return size;
	}

	if (strcmp(dentry->d_name.name, "fw_filename") &&
	    strcmp(dentry->d_name.name, "fw_path"))
		return 0;

	string = devm_kzalloc(sdev->dev, count + 1, GFP_KERNEL);
	if (!string)
		return -ENOMEM;

	size = simple_write_to_buffer(string, count, ppos, buffer, count);

	/* truncate the \n at the end */
	string[count - 1] = '\0';

	if (!strcmp(dentry->d_name.name, "fw_filename")) {
		if (sdev->test_profile.fw_name)
			devm_kfree(sdev->dev, sdev->test_profile.fw_name);
		sdev->test_profile.fw_name = string;

		return size;
	}

	if (sdev->test_profile.fw_path)
		devm_kfree(sdev->dev, sdev->test_profile.fw_path);
	sdev->test_profile.fw_path = string;

	return size;
}

static const struct file_operations sof_dsp_ops_tester_fops = {
	.open = simple_open,
	.write = sof_dsp_ops_tester_dfs_write,
	.read = sof_dsp_ops_tester_dfs_read,
};

static int sof_dsp_dsp_ops_create_dfse(struct snd_sof_dev *sdev, const char *name,
				       struct dentry *parent, umode_t mode)
{
	struct snd_sof_dfsentry *dfse;

	/* create debugfs entry for FW filename */
	dfse = devm_kzalloc(sdev->dev, sizeof(*dfse), GFP_KERNEL);
	if (!dfse)
		return -ENOMEM;

	dfse->type = SOF_DFSENTRY_TYPE_BUF;
	dfse->sdev = sdev;
	debugfs_create_file(name, mode, parent, dfse, &sof_dsp_ops_tester_fops);
	list_add(&dfse->list, &sdev->dfsentry_list);

	return 0;
}

int sof_dbg_dsp_ops_test_init(struct snd_sof_dev *sdev)
{
	struct dentry *dsp_ops_debugfs;
	int ret;

	/* debugfs root directory for DSP ops debug */
	dsp_ops_debugfs = debugfs_create_dir("fw_debug_ops", sdev->debugfs_root);

	/* create debugfs entry for FW filename */
	ret = sof_dsp_dsp_ops_create_dfse(sdev, "fw_filename", dsp_ops_debugfs, 0666);
	if (ret < 0)
		return ret;

	/* create debugfs entry for FW path */
	ret = sof_dsp_dsp_ops_create_dfse(sdev, "fw_path", dsp_ops_debugfs, 0666);
	if (ret < 0)
		return ret;

	ret = sof_dsp_dsp_ops_create_dfse(sdev, "boot_fw", dsp_ops_debugfs, 0222);
	if (ret < 0)
		return ret;

	ret = sof_dsp_dsp_ops_create_dfse(sdev, "dsp_power_state", dsp_ops_debugfs, 0666);
	if (ret < 0)
		return ret;

	ret = sof_dsp_dsp_ops_create_dfse(sdev, "fw_state", dsp_ops_debugfs, 0444);
	if (ret < 0)
		return ret;

	ret = sof_dsp_dsp_ops_create_dfse(sdev, "unload_fw", dsp_ops_debugfs, 0222);
	if (ret < 0)
		return ret;

	ret = sof_dsp_dsp_ops_create_dfse(sdev, "init_trace", dsp_ops_debugfs, 0222);
	if (ret < 0)
		return ret;

	return sof_dsp_dsp_ops_create_dfse(sdev, "free_trace", dsp_ops_debugfs, 0222);
}
