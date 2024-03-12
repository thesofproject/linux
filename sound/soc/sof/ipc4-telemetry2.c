// SPDX-License-Identifier: GPL-2.0-only
//
// Copyright(c) 2024 Intel Corporation.

#include <linux/debugfs.h>
#include <linux/io.h>
#include <linux/pm_runtime.h>
#include <sound/sof/debug.h>
#include <sound/sof/ipc4/header.h>
#include "sof-priv.h"
#include "ops.h"
#include "ipc4-telemetry2.h"
#include "ipc4-priv.h"

static ssize_t sof_telemetry2_entry_read(struct file *file, char __user *buffer,
					size_t count, loff_t *ppos)
{
	struct snd_sof_dfsentry *dfse = file->private_data;
	struct snd_sof_dev *sdev = dfse->sdev;
	u32 type = SOF_IPC4_DEBUG_SLOT_TELEMETRY2;
	loff_t pos = *ppos;
	size_t size_ret;
	u32 offset;
	u8 *buf;

	if (pos < 0)
		return -EINVAL;
	if (pos >= SOF_IPC4_DEBUG_SLOT_SIZE || !count)
		return 0;
	if (count > SOF_IPC4_DEBUG_SLOT_SIZE - pos)
		count = SOF_IPC4_DEBUG_SLOT_SIZE - pos;

	offset = sof_ipc4_find_debug_slot_offset_by_type(sdev, type);
	if (!offset)
		return -EFAULT;

	buf = kzalloc(SOF_IPC4_DEBUG_SLOT_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	sof_mailbox_read(sdev, offset, buf, SOF_IPC4_DEBUG_SLOT_SIZE);
	size_ret = copy_to_user(buffer, buf + pos, count);
	if (size_ret) {
		kfree(buf);
		return -EFAULT;
	}

	*ppos = pos + count;
	kfree(buf);

	return count;
}

static const struct file_operations sof_telemetry2_fops = {
	.open = simple_open,
	.read = sof_telemetry2_entry_read,
	.llseek = default_llseek,
};

void sof_ipc4_create_telemetry2_debugfs_node(struct snd_sof_dev *sdev)
{
	struct snd_sof_dfsentry *dfse;

	dfse = devm_kzalloc(sdev->dev, sizeof(*dfse), GFP_KERNEL);
	if (!dfse)
		return;

	dfse->type = SOF_DFSENTRY_TYPE_IOMEM;
	dfse->size = SOF_IPC4_DEBUG_SLOT_SIZE;
	dfse->access_type = SOF_DEBUGFS_ACCESS_ALWAYS;
	dfse->sdev = sdev;

	list_add(&dfse->list, &sdev->dfsentry_list);

	debugfs_create_file("telemetry2", 0444, sdev->debugfs_root, dfse, &sof_telemetry2_fops);
}
