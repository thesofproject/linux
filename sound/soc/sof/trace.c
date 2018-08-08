// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
/*
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * Copyright(c) 2017 Intel Corporation. All rights reserved.
 *
 * Author: Liam Girdwood <liam.r.girdwood@linux.intel.com>
 *         Yan Wang <yan.wan@linux.intel.com>
 */

#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/sched/signal.h>
#include <linux/time.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include <linux/firmware.h>
#include <linux/debugfs.h>
#include <uapi/sound/sof-ipc.h>
#include <uapi/sound/sof-fw.h>
#include "sof-priv.h"
#include "ops.h"

#define LEVEL_LEN	64

static int sof_set_trace_level(struct snd_sof_dev *sdev,
			       int comp_id, int level)
{
	struct sof_ipc_trace_level levels;
	struct sof_ipc_reply ipc_reply;
	int ret;

	if (!sdev->dtrace_is_enabled)
		return 0;

	/* set IPC parameters */
	levels.hdr.size = sizeof(levels);
	levels.hdr.cmd = SOF_IPC_GLB_TRACE_MSG | SOF_IPC_TRACE_LEVEL;
	levels.comp_id = comp_id;
	levels.level = level;

	/* send IPC to the DSP */
	ret = sof_ipc_tx_message(sdev->ipc,
				 levels.hdr.cmd, &levels, sizeof(levels),
				 &ipc_reply, sizeof(ipc_reply));
	if (ret < 0) {
		dev_err(sdev->dev,
			"error: can't set levels for DMA for trace %d\n", ret);
		goto level_err;
	}

	dev_dbg(sdev->dev, "update trace level: %d\n", level);

	return 0;

level_err:
	return ret;
}

static ssize_t sof_dfsentry_trace_level_read(struct file *file,
					     char __user *user_buf,
					     size_t count, loff_t *ppos)
{
	struct snd_sof_dfsentry_buf *dfse = file->private_data;
	struct snd_sof_dev *sdev = dfse->sdev;
	struct sof_ipc_trace_comp *icomp;
	char str_comp[32];
	char value_str[16];
	int size, total_size, i;
	char __user *pbuf = user_buf;

	if (*ppos < 0 || !count)
		return -EINVAL;

	total_size = 0;

	icomp = sdev->info_comp;
	if (!icomp)
		return -EINVAL;

	/* output trace level of non-component modules */
	for (i = 0; i < icomp->num_components; i++) {
		snprintf(value_str, 16, "0x%x\n", icomp->comp[i].level);
		strcpy(str_comp, icomp->comp[i].name);
		strcat(str_comp, " >> ");
		strcat(str_comp, value_str);

		size = strlen(str_comp);
		total_size += size;
		if (total_size <= *ppos)
			return 0;

		if (size > count) {
			dev_err(sdev->dev, "error: count is not enough\n");
			return -EINVAL;
		}

		if (copy_to_user(pbuf, str_comp, size))
			return -EFAULT;

		pbuf += size;
	}

	size = sizeof(icomp->level_info);
	copy_to_user(pbuf, icomp->level_info, size);

	total_size += size;
	*ppos += total_size;
	return total_size;
}

static ssize_t sof_dfsentry_trace_level_write(struct file *file,
					      const char __user *user_buf,
					      size_t count, loff_t *ppos)
{
	struct snd_sof_dfsentry_buf *dfse = file->private_data;
	struct snd_sof_dev *sdev = dfse->sdev;
	struct sof_ipc_trace_comp *icomp;
	char *buf, *start;
	char *type_str = NULL;
	char *value_str = NULL;
	int i, j, ret;
	unsigned long value;

	icomp = sdev->info_comp;
	if (!icomp)
		return -EINVAL;

	if (count > LEVEL_LEN)
		return -EINVAL;

	buf = kzalloc(LEVEL_LEN, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	start = buf;

	ret = simple_write_to_buffer(buf, LEVEL_LEN, ppos, user_buf, count);
	if (ret < 0)
		goto err_free;

	type_str = strsep(&start, " ");
	if (!type_str)
		goto err_free;

	dev_dbg(sdev->dev, "trace level type: %s\n", type_str);

	value_str = strsep(&start, " ");
	if (!value_str)
		goto err_free;

	dev_dbg(sdev->dev, "trace level value: %s\n", value_str);

	/* find corresponding trace level type for non-component */
	for (i = 0; i < icomp->num_components; i++) {
		if (!strcmp(type_str, icomp->comp[i].name)) {
			dev_dbg(sdev->dev, "trace level type is found\n");
			break;
		}
	}

	/* non-compoent trace level setting is found */
	if (i == icomp->num_components)
		goto err_free;

	/* find corresponding trace level value */
	ret = kstrtoul(value_str, 16, &value);
	if (ret) {
		dev_err(sdev->dev,
			"error: trace level value is not available: %s\n",
			value_str);
		goto err_free;
	}

	icomp->comp[i].level = value;

	/* propaget the value to each component when all are set*/
	if (i == icomp->num_components - 1) {
		for (j = 0; j < icomp->num_components - 1; j++)
			icomp->comp[j].level = value;
	}

	ret = sof_set_trace_level(sdev, i, value);
	if (ret < 0) {
		dev_err(sdev->dev,
			"error: fail to set trace level: %d\n", ret);
		goto err_free;
	}

	return count;

err_free:
	kfree(buf);
	return -EFAULT;
}

static const struct file_operations sof_dfs_trace_level_fops = {
	.open = simple_open,
	.read = sof_dfsentry_trace_level_read,
	.write = sof_dfsentry_trace_level_write,
	.llseek = default_llseek,
};

static size_t sof_wait_trace_avail(struct snd_sof_dev *sdev,
				   loff_t pos, size_t buffer_size)
{
	wait_queue_entry_t wait;

	/*
	 * If host offset is less than local pos, it means write pointer of
	 * host DMA buffer has been wrapped. We should output the trace data
	 * at the end of host DMA buffer at first.
	 */
	if (sdev->host_offset < pos)
		return buffer_size - pos;

	/* If there is available trace data now, it is unnecessary to wait. */
	if (sdev->host_offset > pos)
		return sdev->host_offset - pos;

	/* wait for available trace data from FW */
	init_waitqueue_entry(&wait, current);
	set_current_state(TASK_INTERRUPTIBLE);
	add_wait_queue(&sdev->trace_sleep, &wait);

	if (signal_pending(current)) {
		remove_wait_queue(&sdev->trace_sleep, &wait);
		goto out;
	}

	/* set timeout to max value, no error code */
	schedule_timeout(MAX_SCHEDULE_TIMEOUT);
	remove_wait_queue(&sdev->trace_sleep, &wait);

out:
	/* return bytes available for copy */
	if (sdev->host_offset < pos)
		return buffer_size - pos;
	else
		return sdev->host_offset - pos;
}

static ssize_t sof_dfsentry_trace_read(struct file *file, char __user *buffer,
				       size_t count, loff_t *ppos)
{
	struct snd_sof_dfsentry_buf *dfse = file->private_data;
	struct snd_sof_dev *sdev = dfse->sdev;
	unsigned long rem;
	loff_t lpos = *ppos;
	size_t avail, buffer_size = dfse->size;
	u64 lpos_64;

	/* make sure we know about any failures on the DSP side */
	sdev->dtrace_error = false;

	/* check pos and count */
	if (lpos < 0)
		return -EINVAL;
	if (!count)
		return 0;

	/* check for buffer wrap and count overflow */
	lpos_64 = lpos;
	lpos = do_div(lpos_64, buffer_size);

	if (count > buffer_size - lpos)
		count = buffer_size - lpos;

	/* get available count based on current host offset */
	avail = sof_wait_trace_avail(sdev, lpos, buffer_size);
	if (sdev->dtrace_error) {
		dev_err(sdev->dev, "error: trace IO error\n");
		return -EIO;
	}

	/* make sure count is <= avail */
	count = avail > count ? count : avail;

	/* copy available trace data to debugfs */
	rem = copy_to_user(buffer, dfse->buf + lpos, count);
	if (rem == count)
		return -EFAULT;

	*ppos += count;

	/* move debugfs reading position */
	return count;
}

static const struct file_operations sof_dfs_trace_fops = {
	.open = simple_open,
	.read = sof_dfsentry_trace_read,
	.llseek = default_llseek,
};

static int trace_debugfs_create(struct snd_sof_dev *sdev)
{
	struct snd_sof_dfsentry_buf *dfse;

	if (!sdev)
		return -EINVAL;

	dfse = kzalloc(sizeof(*dfse), GFP_KERNEL);
	if (!dfse)
		return -ENOMEM;

	dfse->buf = sdev->dmatb.area;
	dfse->size = sdev->dmatb.bytes;
	dfse->sdev = sdev;

	dfse->dfsentry = debugfs_create_file("trace", 0444, sdev->debugfs_root,
					     dfse, &sof_dfs_trace_fops);
	if (!dfse->dfsentry) {
		dev_err(sdev->dev,
			"error: cannot create debugfs entry for trace\n");
		kfree(dfse);
		return -ENODEV;
	}

	sdev->tracefs = dfse;

	/* Create debugfs for trace level */
	dfse = kzalloc(sizeof(*dfse), GFP_KERNEL);
	if (!dfse)
		return -ENOMEM;

	dfse->buf = NULL;
	dfse->size = 0;
	dfse->sdev = sdev;

	dfse->dfsentry = debugfs_create_file("trace_level", 0644,
					     sdev->debugfs_root,
					     dfse, &sof_dfs_trace_level_fops);
	if (!dfse->dfsentry) {
		dev_err(sdev->dev,
			"error: cannot create debugfs entry for trace level\n");
		kfree(dfse);
		return -ENODEV;
	}

	sdev->trace_levelfs = dfse;

	return 0;
}

int snd_sof_init_trace(struct snd_sof_dev *sdev)
{
	struct sof_ipc_dma_trace_params params;
	struct sof_ipc_reply ipc_reply;
	int ret;

	/* set false before start initialization */
	sdev->dtrace_is_enabled = false;

	/* allocate trace page table buffer */
	ret = snd_dma_alloc_pages(SNDRV_DMA_TYPE_DEV, sdev->parent,
				  PAGE_SIZE, &sdev->dmatp);
	if (ret < 0) {
		dev_err(sdev->dev,
			"error: can't alloc page table for trace %d\n", ret);
		return ret;
	}

	/* allocate trace data buffer */
	ret = snd_dma_alloc_pages(SNDRV_DMA_TYPE_DEV_SG, sdev->parent,
				  DMA_BUF_SIZE_FOR_TRACE, &sdev->dmatb);
	if (ret < 0) {
		dev_err(sdev->dev,
			"error: can't alloc buffer for trace %d\n", ret);
		goto page_err;
	}

	/* create compressed page table for audio firmware */
	ret = snd_sof_create_page_table(sdev, &sdev->dmatb, sdev->dmatp.area,
					sdev->dmatb.bytes);
	if (ret < 0)
		goto table_err;

	sdev->dma_trace_pages = ret;
	dev_dbg(sdev->dev, "dma_trace_pages: %d\n", sdev->dma_trace_pages);

	ret = trace_debugfs_create(sdev);
	if (ret < 0)
		goto table_err;

	/* set IPC parameters */
	params.hdr.size = sizeof(params);
	params.hdr.cmd = SOF_IPC_GLB_TRACE_MSG | SOF_IPC_TRACE_DMA_PARAMS;
	params.buffer.phy_addr = sdev->dmatp.addr;
	params.buffer.size = sdev->dmatb.bytes;
	params.buffer.offset = 0;
	params.buffer.pages = sdev->dma_trace_pages;

	init_waitqueue_head(&sdev->trace_sleep);
	sdev->host_offset = 0;

	ret = snd_sof_dma_trace_init(sdev, &params.stream_tag);
	if (ret < 0) {
		dev_err(sdev->dev,
			"error: fail in snd_sof_dma_trace_init %d\n", ret);
		goto table_err;
	}
	dev_dbg(sdev->dev, "stream_tag: %d\n", params.stream_tag);

	/* send IPC to the DSP */
	ret = sof_ipc_tx_message(sdev->ipc,
				 params.hdr.cmd, &params, sizeof(params),
				 &ipc_reply, sizeof(ipc_reply));
	if (ret < 0) {
		dev_err(sdev->dev,
			"error: can't set params for DMA for trace %d\n", ret);
		goto table_err;
	}

	ret = snd_sof_dma_trace_trigger(sdev, SNDRV_PCM_TRIGGER_START);
	if (ret < 0) {
		dev_err(sdev->dev,
			"error: snd_sof_dma_trace_trigger: start: %d\n", ret);
		goto table_err;
	}

	sdev->dtrace_is_enabled = true;
	return 0;

table_err:
	snd_dma_free_pages(&sdev->dmatb);
page_err:
	snd_dma_free_pages(&sdev->dmatp);
	return ret;
}
EXPORT_SYMBOL(snd_sof_init_trace);

int snd_sof_trace_update_pos(struct snd_sof_dev *sdev,
			     struct sof_ipc_dma_trace_posn *posn)
{
	if (sdev->dtrace_is_enabled && sdev->host_offset != posn->host_offset) {
		sdev->host_offset = posn->host_offset;
		wake_up(&sdev->trace_sleep);
	}

	if (posn->overflow != 0)
		dev_err(sdev->dev,
			"error: DSP trace buffer overflow %u bytes. Total messages %d\n",
			posn->overflow, posn->messages);

	return 0;
}

/* an error has occurred within the DSP that prevents further trace */
void snd_sof_trace_notify_for_error(struct snd_sof_dev *sdev)
{
	if (sdev->dtrace_is_enabled) {
		dev_err(sdev->dev, "error: waking up any trace sleepers\n");
		sdev->dtrace_error = true;
		wake_up(&sdev->trace_sleep);
	}
}
EXPORT_SYMBOL(snd_sof_trace_notify_for_error);

void snd_sof_release_trace(struct snd_sof_dev *sdev)
{
	int ret;

	if (!sdev->dtrace_is_enabled)
		return;

	ret = snd_sof_dma_trace_trigger(sdev, SNDRV_PCM_TRIGGER_STOP);
	if (ret < 0)
		dev_err(sdev->dev,
			"error: snd_sof_dma_trace_trigger: stop: %d\n", ret);

	ret = snd_sof_dma_trace_release(sdev);
	if (ret < 0)
		dev_err(sdev->dev,
			"error: fail in snd_sof_dma_trace_release %d\n", ret);

	snd_dma_free_pages(&sdev->dmatb);
	snd_dma_free_pages(&sdev->dmatp);

	kfree(sdev->tracefs);
	kfree(sdev->trace_levelfs);
}
EXPORT_SYMBOL(snd_sof_release_trace);
