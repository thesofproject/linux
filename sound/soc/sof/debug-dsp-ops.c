// SPDX-License-Identifier: GPL-2.0-only
//
// Copyright(c) 2023 Intel Corporation. All rights reserved.
//

#include <linux/debugfs.h>
#include <sound/sof/debug.h>
#include "sof-priv.h"
#include "ops.h"

/*
 * set dsp power state op by writing the power state.
 * ex: echo set_power_state,D3 > dsp_test_op
 */
static int sof_dsp_ops_set_power_state(struct snd_sof_dev *sdev, char *state)
{
	/* only D3 supported for now */
	if (strcmp(state, "D3")) {
		dev_err(sdev->dev, "Unsupported state %s\n", state);
		return -EINVAL;
	}

	/* power off the DSP */
	if (sdev->dsp_power_state.state == SOF_DSP_PM_D0) {
		const struct sof_ipc_pm_ops *pm_ops = sof_ipc_get_ops(sdev, pm);
		pm_message_t pm_state;
		int ret;

		pm_state.event = SOF_DSP_PM_D3;

		/* suspend DMA trace */
		sof_fw_trace_suspend(sdev, pm_state);

		/* notify DSP of upcoming power down */
		if (pm_ops && pm_ops->ctx_save) {
			ret = pm_ops->ctx_save(sdev);
			if (ret < 0)
				return ret;
		}

		ret = snd_sof_dsp_runtime_suspend(sdev);
		if (ret < 0) {
			dev_err(sdev->dev, "failed to power off DSP\n");
			return ret;
		}

		sdev->enabled_cores_mask = 0;
		sof_set_fw_state(sdev, SOF_FW_BOOT_NOT_STARTED);
	}

	return 0;
}

/*
 * test firmware boot by passing the firmware file as the argument:
 * ex: echo boot_firmware,intel/avs/tgl/community/dsp_basefw.bin > dsp_test_op
 */
static int sof_dsp_ops_boot_firmware(struct snd_sof_dev *sdev, char *fw_filename)
{
	int ret;

	/* power off the DSP */
	ret = sof_dsp_ops_set_power_state(sdev, "D3");
	if (ret < 0)
		return ret;

	if (sdev->basefw.fw)
		snd_sof_fw_unload(sdev);

	ret = snd_sof_dsp_runtime_resume(sdev);
	if (ret < 0)
		return ret;

	sdev->first_boot = true;

	/* load and boot firmware */
	ret = snd_sof_load_firmware(sdev, (const char *)fw_filename);
	if (ret < 0)
		return ret;

	sof_set_fw_state(sdev, SOF_FW_BOOT_IN_PROGRESS);

	ret = snd_sof_run_firmware(sdev);
	if (ret < 0)
		return ret;

	/* resume DMA trace */
	return sof_fw_trace_resume(sdev);
}

/* ops are executed as "op_name,argument1,argument2...". For example, to set the DSP power state
 * to D3: echo "load_firmware,<PATH>/sof-tgl.ri" > dsp_test_op"
 */
static ssize_t sof_dsp_ops_tester_dfs_write(struct file *file, const char __user *buffer,
					    size_t count, loff_t *ppos)
{
	struct snd_sof_dfsentry *dfse = file->private_data;
	struct snd_sof_dev *sdev = dfse->sdev;
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

	if (!strcmp(op_name, "boot_firmware")) {
		ret = sof_dsp_ops_boot_firmware(sdev, string);
		if (ret < 0)
			goto err;
	}

	if (!strcmp(op_name, "set_power_state")) {
		ret = sof_dsp_ops_set_power_state(sdev, string);
		if (ret < 0)
			goto err;
	}

err:
	if (ret >= 0)
		ret = size;

	kfree(string);

	return ret;
}

static const struct file_operations sof_dsp_ops_tester_fops = {
	.open = simple_open,
	.write = sof_dsp_ops_tester_dfs_write,
};

int sof_dbg_dsp_ops_test_init(struct snd_sof_dev *sdev)
{
	struct snd_sof_dfsentry *dfse;

	dfse = devm_kzalloc(sdev->dev, sizeof(*dfse), GFP_KERNEL);
	if (!dfse)
		return -ENOMEM;

	/* no need to allocate dfse buffer */
	dfse->type = SOF_DFSENTRY_TYPE_BUF;
	dfse->sdev = sdev;

	debugfs_create_file("dsp_test_op", 0222, sdev->debugfs_root, dfse, &sof_dsp_ops_tester_fops);

	/* add to dfsentry list */
	list_add(&dfse->list, &sdev->dfsentry_list);
	return 0;
}
