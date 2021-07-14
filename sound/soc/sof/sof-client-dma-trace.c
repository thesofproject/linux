// SPDX-License-Identifier: GPL-2.0-only
//
// This file is provided under a dual BSD/GPLv2 license.  When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2018-2021 Intel Corporation. All rights reserved.
// Author: Liam Girdwood <liam.r.girdwood@linux.intel.com>
//
// SOF client version:
//  Peter Ujfalusi <peter.ujfalusi@linux.intel.com>
//

#include <asm/unaligned.h>
#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/sched/signal.h>
#include <sound/sof/header.h>
#include <sound/sof/info.h>
#include <sound/sof/trace.h>
#include <sound/pcm.h>
#include "sof-client.h"
#include "sof-client-dma-trace.h"

/* DMA buffer size for trace */
#define SOF_DTRACE_BUF_SIZE			(PAGE_SIZE * 16)
#define SOF_DTRACE_SUSPEND_DELAY_MS		3000

#define TRACE_FILTER_ELEMENTS_PER_ENTRY		4
#define TRACE_FILTER_MAX_CONFIG_STRING_LENGTH	1024

struct sof_dtrace_priv {
	struct device *dev;
	struct dentry *dfs_trace;
	struct dentry *dfs_filter;

	struct snd_dma_buffer dmatb;
	struct snd_dma_buffer dmatp;
	int dtrace_pages;
	wait_queue_head_t dtrace_sleep;
	u32 host_offset;
	bool dtrace_is_enabled;
	bool dtrace_error;
	bool dtrace_draining;

	const struct sof_dma_trace_host_ops *host_ops;
};

/*
 * SHOULD NOT BE HERE!!!!!
 * copy of snd_sof_create_page_table from utils.c
 *
 * Generic buffer page table creation.
 * Take the each physical page address and drop the least significant unused
 * bits from each (based on PAGE_SIZE). Then pack valid page address bits
 * into compressed page table.
 */

static int create_page_table(struct device *dev, struct snd_dma_buffer *dmab,
			     unsigned char *page_table, size_t size)
{
	int i, pages;

	pages = snd_sgbuf_aligned_pages(size);

	dev_dbg(dev, "generating page table for %p size 0x%zx pages %d\n",
		dmab->area, size, pages);

	for (i = 0; i < pages; i++) {
		/*
		 * The number of valid address bits for each page is 20.
		 * idx determines the byte position within page_table
		 * where the current page's address is stored
		 * in the compressed page_table.
		 * This can be calculated by multiplying the page number by 2.5.
		 */
		u32 idx = (5 * i) >> 1;
		u32 pfn = snd_sgbuf_get_addr(dmab, i * PAGE_SIZE) >> PAGE_SHIFT;
		u8 *pg_table;

		dev_vdbg(dev, "pfn i %i idx %d pfn %x\n", i, idx, pfn);

		pg_table = (u8 *)(page_table + idx);

		/*
		 * pagetable compression:
		 * byte 0     byte 1     byte 2     byte 3     byte 4     byte 5
		 * ___________pfn 0__________ __________pfn 1___________  _pfn 2...
		 * .... ....  .... ....  .... ....  .... ....  .... ....  ....
		 * It is created by:
		 * 1. set current location to 0, PFN index i to 0
		 * 2. put pfn[i] at current location in Little Endian byte order
		 * 3. calculate an intermediate value as
		 *    x = (pfn[i+1] << 4) | (pfn[i] & 0xf)
		 * 4. put x at offset (current location + 2) in LE byte order
		 * 5. increment current location by 5 bytes, increment i by 2
		 * 6. continue to (2)
		 */
		if (i & 1)
			put_unaligned_le32((pg_table[0] & 0xf) | pfn << 4,
					   pg_table);
		else
			put_unaligned_le32(pfn, pg_table);
	}

	return pages;
}

static int trace_filter_append_elem(u32 key, u32 value,
				    struct sof_ipc_trace_filter_elem *elem_list,
				    int capacity, int *counter)
{
	if (*counter >= capacity)
		return -ENOMEM;

	elem_list[*counter].key = key;
	elem_list[*counter].value = value;
	++*counter;

	return 0;
}

static int trace_filter_parse_entry(struct sof_client_dev *cdev, const char *line,
				    struct sof_ipc_trace_filter_elem *elem,
				    int capacity, int *counter)
{
	int log_level, pipe_id, comp_id, read, ret;
	struct sof_dtrace_priv *priv = cdev->data;
	int len = strlen(line);
	int cnt = *counter;
	u32 uuid_id;

	/* ignore empty content */
	ret = sscanf(line, " %n", &read);
	if (!ret && read == len)
		return len;

	ret = sscanf(line, " %d %x %d %d %n", &log_level, &uuid_id, &pipe_id, &comp_id, &read);
	if (ret != TRACE_FILTER_ELEMENTS_PER_ENTRY || read != len) {
		dev_err(priv->dev, "invalid trace filter entry '%s'\n", line);
		return -EINVAL;
	}

	if (uuid_id > 0) {
		ret = trace_filter_append_elem(SOF_IPC_TRACE_FILTER_ELEM_BY_UUID,
					       uuid_id, elem, capacity, &cnt);
		if (ret)
			return ret;
	}
	if (pipe_id >= 0) {
		ret = trace_filter_append_elem(SOF_IPC_TRACE_FILTER_ELEM_BY_PIPE,
					       pipe_id, elem, capacity, &cnt);
		if (ret)
			return ret;
	}
	if (comp_id >= 0) {
		ret = trace_filter_append_elem(SOF_IPC_TRACE_FILTER_ELEM_BY_COMP,
					       comp_id, elem, capacity, &cnt);
		if (ret)
			return ret;
	}

	ret = trace_filter_append_elem(SOF_IPC_TRACE_FILTER_ELEM_SET_LEVEL |
				       SOF_IPC_TRACE_FILTER_ELEM_FIN,
				       log_level, elem, capacity, &cnt);
	if (ret)
		return ret;

	/* update counter only when parsing whole entry passed */
	*counter = cnt;

	return len;
}

static int trace_filter_parse(struct sof_client_dev *cdev, char *string,
			      int *out_elem_cnt,
			      struct sof_ipc_trace_filter_elem **out)
{
	struct sof_dtrace_priv *priv = cdev->data;
	static const char entry_delimiter[] = ";";
	char *entry = string;
	int capacity = 0;
	int entry_len;
	int cnt = 0;

	/*
	 * Each entry contains at least 1, up to TRACE_FILTER_ELEMENTS_PER_ENTRY
	 * IPC elements, depending on content. Calculate IPC elements capacity
	 * for the input string where each element is set.
	 */
	while (entry) {
		capacity += TRACE_FILTER_ELEMENTS_PER_ENTRY;
		entry = strchr(entry + 1, entry_delimiter[0]);
	}
	*out = kmalloc(capacity * sizeof(**out), GFP_KERNEL);
	if (!*out)
		return -ENOMEM;

	/* split input string by ';', and parse each entry separately in trace_filter_parse_entry */
	while ((entry = strsep(&string, entry_delimiter))) {
		entry_len = trace_filter_parse_entry(cdev, entry, *out, capacity, &cnt);
		if (entry_len < 0) {
			dev_err(priv->dev, "%s failed for '%s', %d\n", __func__, entry,
				entry_len);
			return -EINVAL;
		}
	}

	*out_elem_cnt = cnt;

	return 0;
}

static int sof_ipc_trace_update_filter(struct sof_client_dev *cdev, int num_elems,
				       struct sof_ipc_trace_filter_elem *elems)
{
	struct sof_ipc_trace_filter *msg;
	struct sof_ipc_reply reply;
	size_t size;
	int ret;

	size = struct_size(msg, elems, num_elems);
	if (size > SOF_IPC_MSG_MAX_SIZE)
		return -EINVAL;

	msg = kmalloc(size, GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	msg->hdr.size = size;
	msg->hdr.cmd = SOF_IPC_GLB_TRACE_MSG | SOF_IPC_TRACE_FILTER_UPDATE;
	msg->elem_cnt = num_elems;
	memcpy(&msg->elems[0], elems, num_elems * sizeof(*elems));

	ret = sof_client_ipc_tx_message(cdev, msg, &reply, sizeof(reply));

	kfree(msg);

	return ret ? ret : reply.error;
}

static ssize_t sof_dfsentry_trace_filter_write(struct file *file, const char __user *from,
					       size_t count, loff_t *ppos)
{
	struct sof_client_dev *cdev = file->private_data;
	struct sof_ipc_trace_filter_elem *elems = NULL;
	struct sof_dtrace_priv *priv = cdev->data;
	int num_elems, ret;
	loff_t pos = 0;
	char *string;

	if (!priv->dtrace_is_enabled) {
		dev_err(priv->dev, "filter can not be updated while suspended\n");
		return -EBUSY;
	}

	if (count > TRACE_FILTER_MAX_CONFIG_STRING_LENGTH) {
		dev_err(priv->dev, "%s too long input, %zu > %d\n", __func__, count,
			TRACE_FILTER_MAX_CONFIG_STRING_LENGTH);
		return -EINVAL;
	}

	string = kmalloc(count + 1, GFP_KERNEL);
	if (!string)
		return -ENOMEM;

	/* assert null termination */
	string[count] = 0;
	ret = simple_write_to_buffer(string, count, &pos, from, count);
	if (ret < 0)
		goto error;

	ret = trace_filter_parse(cdev, string, &num_elems, &elems);
	if (ret < 0)
		goto error;

	if (num_elems) {
		ret = sof_ipc_trace_update_filter(cdev, num_elems, elems);
		if (ret < 0) {
			dev_err(priv->dev, "filter update failed: %d\n", ret);
			goto error;
		}
	}
	ret = count;

error:
	kfree(string);
	kfree(elems);
	return ret;
}

static const struct file_operations sof_dtrace_filter_fops = {
	.open = simple_open,
	.write = sof_dfsentry_trace_filter_write,
	.llseek = default_llseek,
};

static size_t sof_dtrace_avail(struct sof_client_dev *cdev,
			       loff_t pos, size_t buffer_size)
{
	struct sof_dtrace_priv *priv = cdev->data;
	loff_t host_offset = READ_ONCE(priv->host_offset);

	/*
	 * If host offset is less than local pos, it means write pointer of
	 * host DMA buffer has been wrapped. We should output the trace data
	 * at the end of host DMA buffer at first.
	 */
	if (host_offset < pos)
		return buffer_size - pos;

	/* If there is available trace data now, it is unnecessary to wait. */
	if (host_offset > pos)
		return host_offset - pos;

	return 0;
}

static size_t sof_wait_trace_avail(struct sof_client_dev *cdev,
				   loff_t pos, size_t buffer_size)
{
	size_t ret = sof_dtrace_avail(cdev, pos, buffer_size);
	struct sof_dtrace_priv *priv = cdev->data;
	wait_queue_entry_t wait;

	/* data immediately available */
	if (ret)
		return ret;

	if (!priv->dtrace_is_enabled && priv->dtrace_draining) {
		/*
		 * tracing has ended and all traces have been
		 * read by client, return EOF
		 */
		priv->dtrace_draining = false;
		return 0;
	}

	/* wait for available trace data from FW */
	init_waitqueue_entry(&wait, current);
	set_current_state(TASK_INTERRUPTIBLE);
	add_wait_queue(&priv->dtrace_sleep, &wait);

	if (!signal_pending(current)) {
		/* set timeout to max value, no error code */
		schedule_timeout(MAX_SCHEDULE_TIMEOUT);
	}
	remove_wait_queue(&priv->dtrace_sleep, &wait);

	return sof_dtrace_avail(cdev, pos, buffer_size);
}

static ssize_t sof_dfsentry_trace_read(struct file *file, char __user *buffer,
				       size_t count, loff_t *ppos)
{
	struct sof_client_dev *cdev = file->private_data;
	struct sof_dtrace_priv *priv = cdev->data;
	size_t buffer_size = priv->dmatb.bytes;
	unsigned long rem;
	loff_t lpos = *ppos;
	size_t avail;
	u64 lpos_64;

	/* make sure we know about any failures on the DSP side */
	priv->dtrace_error = false;

	/* check pos and count */
	if (lpos < 0)
		return -EINVAL;
	if (!count)
		return 0;

	/* check for buffer wrap and count overflow */
	lpos_64 = lpos;
	lpos = do_div(lpos_64, buffer_size);

	if (count > buffer_size - lpos) /* min() not used to avoid sparse warnings */
		count = buffer_size - lpos;

	/* get available count based on current host offset */
	avail = sof_wait_trace_avail(cdev, lpos, buffer_size);
	if (priv->dtrace_error) {
		dev_err(priv->dev, "trace IO error\n");
		return -EIO;
	}

	/* make sure count is <= avail */
	count = avail > count ? count : avail;

	/* copy available trace data to debugfs */
	rem = copy_to_user(buffer, ((u8 *)(priv->dmatb.area) + lpos), count);
	if (rem)
		return -EFAULT;

	*ppos += count;

	/* move debugfs reading position */
	return count;
}

static int sof_dfsentry_trace_release(struct inode *inode, struct file *file)
{
	struct sof_client_dev *cdev = inode->i_private;
	struct sof_dtrace_priv *priv = cdev->data;

	/* avoid duplicate traces at next open */
	if (!priv->dtrace_is_enabled)
		priv->host_offset = 0;

	return 0;
}

static const struct file_operations sof_dtrace_trace_fops = {
	.open = simple_open,
	.read = sof_dfsentry_trace_read,
	.llseek = default_llseek,
	.release = sof_dfsentry_trace_release,
};

static void snd_sof_dtrace_update_pos(struct sof_client_dev *cdev, void *full_msg)
{
	struct sof_ipc_dma_trace_posn *posn = full_msg;
	u32 msg_type = posn->rhdr.hdr.cmd & SOF_CMD_TYPE_MASK;
	struct sof_dtrace_priv *priv = cdev->data;

	if (msg_type != SOF_IPC_TRACE_DMA_POSITION)
		dev_info(priv->dev, "unhandled trace message %#x\n", msg_type);

	if (priv->dtrace_is_enabled && priv->host_offset != posn->host_offset) {
		priv->host_offset = posn->host_offset;
		wake_up(&priv->dtrace_sleep);
	}

	if (posn->overflow != 0)
		dev_err(priv->dev,
			"DSP trace buffer overflow %u bytes. Total messages %d\n",
			posn->overflow, posn->messages);
}

/* an error has occurred within the DSP that prevents further trace */
static void sof_dtrace_dsp_panic(struct sof_client_dev *cdev)
{
	struct sof_dtrace_priv *priv = cdev->data;

	if (priv->dtrace_is_enabled) {
		priv->dtrace_error = true;
		wake_up(&priv->dtrace_sleep);
	}
}

static void sof_dtrace_release(struct sof_client_dev *cdev)
{
	struct sof_dtrace_priv *priv = cdev->data;
	const struct sof_dma_trace_host_ops *ops = priv->host_ops;
	int ret;

	if (!priv->dtrace_is_enabled)
		return;

	ret = ops->stop(cdev);
	if (ret < 0)
		dev_err(priv->dev, "host stop failed: %d\n", ret);

	ret = ops->release(cdev);
	if (ret < 0)
		dev_err(priv->dev, "host release failed: %d\n", ret);

	priv->dtrace_is_enabled = false;
	priv->dtrace_draining = true;
	wake_up(&priv->dtrace_sleep);
}

static int sof_dtrace_init_ipc(struct sof_client_dev *cdev)
{
	const struct sof_ipc_fw_version *v = sof_client_get_fw_version(cdev);
	struct sof_dtrace_priv *priv = cdev->data;
	const struct sof_dma_trace_host_ops *ops = priv->host_ops;
	struct sof_ipc_dma_trace_params_ext params;
	struct sof_ipc_reply ipc_reply;
	int ret;

	if (priv->dtrace_is_enabled)
		return 0;

	/* set IPC parameters */
	params.hdr.cmd = SOF_IPC_GLB_TRACE_MSG;
	/* PARAMS_EXT is only supported from ABI 3.7.0 onwards */
	if (v->abi_version >= SOF_ABI_VER(3, 7, 0)) {
		params.hdr.size = sizeof(struct sof_ipc_dma_trace_params_ext);
		params.hdr.cmd |= SOF_IPC_TRACE_DMA_PARAMS_EXT;
		params.timestamp_ns = ktime_get(); /* in nanosecond */
	} else {
		params.hdr.size = sizeof(struct sof_ipc_dma_trace_params);
		params.hdr.cmd |= SOF_IPC_TRACE_DMA_PARAMS;
	}
	params.buffer.phy_addr = priv->dmatp.addr;
	params.buffer.size = priv->dmatb.bytes;
	params.buffer.pages = priv->dtrace_pages;
	params.stream_tag = 0;

	priv->host_offset = 0;
	priv->dtrace_draining = false;

	ret = ops->init(cdev, &priv->dmatb, &params.stream_tag);
	if (ret < 0) {
		dev_err(priv->dev, "host init failed: %d\n", ret);
		return ret;
	}
	dev_dbg(priv->dev, "stream_tag: %d\n", params.stream_tag);

	/* send IPC to the DSP */
	ret = sof_client_ipc_tx_message(cdev, &params, &ipc_reply, sizeof(ipc_reply));
	if (ret < 0) {
		dev_err(priv->dev, "can't set params for DMA for trace %d\n", ret);
		goto trace_release;
	}

	ret = ops->start(cdev);
	if (ret < 0) {
		dev_err(priv->dev, "host start failed: %d\n", ret);
		goto trace_release;
	}

	priv->dtrace_is_enabled = true;

	return 0;

trace_release:
	ops->release(cdev);
	return ret;
}

static int sof_dtrace_client_probe(struct auxiliary_device *auxdev,
				   const struct auxiliary_device_id *id)
{
	struct sof_client_dev *cdev = auxiliary_dev_to_sof_client_dev(auxdev);
	struct dentry *dfsroot = sof_client_get_debugfs_root(cdev);
	struct device *dma_dev =  sof_client_get_dma_dev(cdev);
	struct sof_dma_trace_host_ops *ops;
	struct device *dev = &auxdev->dev;
	struct sof_dtrace_priv *priv;
	int ret;

	if (!dev->platform_data) {
		dev_err(dev, "missing platform data\n");
		return -ENODEV;
	}

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	ops = dev->platform_data;

	if (!ops->init || !ops->release || !ops->start || !ops->stop) {
		dev_err(dev, "missing platform callback(s)\n");
		return -ENODEV;
	}

	/*
	 * dma-trace is power managed via auxdev suspend/resume callbacks by
	 * SOF core
	 */
	pm_runtime_no_callbacks(dev);

	priv->host_ops = ops;
	priv->dev = dev;
	cdev->data = priv;

	/* allocate trace page table buffer */
	ret = snd_dma_alloc_pages(SNDRV_DMA_TYPE_DEV, dma_dev, PAGE_SIZE,
				  &priv->dmatp);
	if (ret < 0) {
		dev_err(dev, "can't alloc page table for trace %d\n", ret);
		return ret;
	}

	/* allocate trace data buffer */
	ret = snd_dma_alloc_pages(SNDRV_DMA_TYPE_DEV_SG, dma_dev,
				  SOF_DTRACE_BUF_SIZE, &priv->dmatb);
	if (ret < 0) {
		dev_err(dev, "can't alloc buffer for trace %d\n", ret);
		goto page_err;
	}

	/* create compressed page table for audio firmware */
	ret = create_page_table(dma_dev, &priv->dmatb, priv->dmatp.area,
				priv->dmatb.bytes);
	if (ret < 0)
		goto table_err;

	priv->dtrace_pages = ret;
	dev_dbg(dev, "dtrace_pages: %d\n", priv->dtrace_pages);

	priv->dfs_trace = debugfs_create_file("trace", 0444, dfsroot, cdev,
					      &sof_dtrace_trace_fops);
	priv->dfs_filter = debugfs_create_file("filter", 0200, dfsroot, cdev,
					       &sof_dtrace_filter_fops);

	init_waitqueue_head(&priv->dtrace_sleep);

	ret = sof_client_register_ipc_rx_handler(cdev, SOF_IPC_GLB_TRACE_MSG,
						 snd_sof_dtrace_update_pos);
	if (ret)
		goto register_rx_err;

	ret = sof_client_register_dsp_panic_handler(cdev, sof_dtrace_dsp_panic);
	if (ret)
		goto register_panic_err;

	ret = sof_dtrace_init_ipc(cdev);
	if (ret < 0) {
		sof_client_unregister_ipc_rx_handler(cdev, SOF_IPC_GLB_TRACE_MSG);
		goto ipc_err;
	}

	return 0;

ipc_err:
	sof_client_unregister_dsp_panic_handler(cdev);
register_panic_err:
	sof_client_unregister_ipc_rx_handler(cdev, SOF_IPC_GLB_TRACE_MSG);
register_rx_err:
	debugfs_remove(priv->dfs_trace);
	debugfs_remove(priv->dfs_filter);
table_err:
	snd_dma_free_pages(&priv->dmatb);
page_err:
	snd_dma_free_pages(&priv->dmatp);

	return ret;
}

static void sof_dtrace_client_remove(struct auxiliary_device *auxdev)
{
	struct sof_client_dev *cdev = auxiliary_dev_to_sof_client_dev(auxdev);
	struct sof_dtrace_priv *priv = cdev->data;

	sof_client_unregister_dsp_panic_handler(cdev);
	sof_client_unregister_ipc_rx_handler(cdev, SOF_IPC_GLB_TRACE_MSG);
	sof_dtrace_release(cdev);

	debugfs_remove(priv->dfs_trace);
	debugfs_remove(priv->dfs_filter);

	snd_dma_free_pages(&priv->dmatb);
	snd_dma_free_pages(&priv->dmatp);
}

static int sof_dtrace_client_resume(struct auxiliary_device *auxdev)
{
	struct sof_client_dev *cdev = auxiliary_dev_to_sof_client_dev(auxdev);

	sof_dtrace_init_ipc(cdev);

	return 0;
}

static int sof_dtrace_client_suspend(struct auxiliary_device *auxdev,
				     pm_message_t state)
{
	struct sof_client_dev *cdev = auxiliary_dev_to_sof_client_dev(auxdev);

	sof_dtrace_release(cdev);

	return 0;
}

static const struct auxiliary_device_id sof_dtrace_client_id_table[] = {
	{ .name = "snd_sof.hda-dma-trace", },
	{},
};
MODULE_DEVICE_TABLE(auxiliary, sof_dtrace_client_id_table);

/* driver name will be set based on KBUILD_MODNAME */
static struct auxiliary_driver sof_dtrace_client_drv = {
	.probe = sof_dtrace_client_probe,
	.remove = sof_dtrace_client_remove,
	.suspend = sof_dtrace_client_suspend,
	.resume = sof_dtrace_client_resume,

	.id_table = sof_dtrace_client_id_table,
};

module_auxiliary_driver(sof_dtrace_client_drv);

MODULE_DESCRIPTION("SOF DMA Trace Client Driver");
MODULE_LICENSE("GPL v2");
MODULE_IMPORT_NS(SND_SOC_SOF_CLIENT);
