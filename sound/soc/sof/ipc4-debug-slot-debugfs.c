// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
//
// This file is provided under a dual BSD/GPLv2 license.  When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2024 Intel Corporation.
//

#include <linux/debugfs.h>
#include <linux/io.h>
#include <linux/pm_runtime.h>
#include <sound/sof/debug.h>
#include <sound/sof/ipc4/header.h>
#include "sof-priv.h"
#include "ops.h"
#include "ipc4-priv.h"

struct debug_slot_fs_ud {
	struct snd_sof_dfsentry dfse;
	u32 slot_type;
	size_t data_offset;
};

static ssize_t sof_debug_slot_debugfs_entry_read(struct file *file, char __user *buffer,
						 size_t count, loff_t *ppos)
{
	struct debug_slot_fs_ud *ud = file->private_data;
	struct snd_sof_dfsentry *dfse = &ud->dfse;
	struct snd_sof_dev *sdev = dfse->sdev;
	size_t doffset = ud->data_offset;
	u32 type = ud->slot_type;
	loff_t pos = *ppos;
	size_t size_ret;
	u32 offset;
	u8 *buf;

	if (pos < 0)
		return -EINVAL;
	if (pos + doffset >= SOF_IPC4_DEBUG_SLOT_SIZE || !count)
		return 0;
	if (count > SOF_IPC4_DEBUG_SLOT_SIZE - pos - doffset)
		count = SOF_IPC4_DEBUG_SLOT_SIZE - pos - doffset;

	offset = sof_ipc4_find_debug_slot_offset_by_type(sdev, type);
	if (!offset)
		return -EFAULT;

	buf = kzalloc(SOF_IPC4_DEBUG_SLOT_SIZE - doffset, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	sof_mailbox_read(sdev, offset + doffset, buf, SOF_IPC4_DEBUG_SLOT_SIZE - doffset);
	size_ret = copy_to_user(buffer, buf + pos, count);
	if (size_ret) {
		kfree(buf);
		return -EFAULT;
	}

	*ppos = pos + count;
	kfree(buf);

	return count;
}

static const struct file_operations sof_debug_stream_fops = {
	.open = simple_open,
	.read = sof_debug_slot_debugfs_entry_read,
	.llseek = default_llseek,
};

void sof_ipc4_create_debug_slot_debugfs_node(struct snd_sof_dev *sdev, u32 slot_type,
					     size_t data_offset, const char *name)
{
	struct debug_slot_fs_ud *ud;

	ud = devm_kzalloc(sdev->dev, sizeof(*ud), GFP_KERNEL);
	if (!ud)
		return;

	ud->dfse.type = SOF_DFSENTRY_TYPE_IOMEM;
	ud->dfse.size = SOF_IPC4_DEBUG_SLOT_SIZE;
	ud->dfse.access_type = SOF_DEBUGFS_ACCESS_ALWAYS;
	ud->dfse.sdev = sdev;

	ud->slot_type = slot_type;
	ud->data_offset = data_offset;

	list_add(&ud->dfse.list, &sdev->dfsentry_list);

	debugfs_create_file(name, 0444, sdev->debugfs_root, ud, &sof_debug_stream_fops);
}
