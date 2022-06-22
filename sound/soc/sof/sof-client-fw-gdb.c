// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
//
// Copyright(c) 2022 Intel Corporation.
//
// Authors: Noah Klayman <noah.klayman@intel.com>
//	    Guennadi Liakhovetski <guennadi.liakhovetski@linux.intel.com>
//

#include <linux/auxiliary_bus.h>
#include <linux/completion.h>
#include <linux/debugfs.h>
#include <linux/ktime.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <sound/sof/header.h>
#include <sound/sof/ipc4/header.h>

#include "sof-client.h"
#include "sof-priv.h"
#include "ipc4-priv.h"
#include "ops.h"

#define SOF_IPC_CLIENT_SUSPEND_DELAY_MS	3000
#define SOF_FS_GDB_POLL_DELAY_MS 5

/*
 * We should use a free slot in the debug window, but on Tiger Lake all slots
 * are occupied. So we use the reserved space in the first page of the window.
 * That page only contains an array of descriptors in the beginning, which
 * currently occupies 180 bytes. We leave the first kilobyte reserved for future
 * compatibility but we should use a free slot on other platforms with larger
 * debug windows.
 */

union sof_ipc_gdb_message {
	struct {
		struct sof_ipc_cmd_hdr hdr;
		char cmd[];
	} ipc3;
	struct {
		struct sof_ipc4_msg msg;
		char cmd[];
	} ipc4;
} __packed;

struct sof_fw_gdb_priv {
	struct dentry *dfs_file;
	enum sof_ipc_type ipc_type;
	union sof_ipc_gdb_message *tx_buffer;
	struct delayed_work poll_work;
	wait_queue_head_t rxq;
	wait_queue_head_t txq;
	struct mutex mutex;			/* Protect the ring buffer */
	struct sof_client_dev *cdev;
};

#define DSP_CACHE_LINE_SIZE 64
/*
 * Must match the respective structure in the firmware. Ring buffer "head" and
 * "tail" pointers are placed in separate DSP cache lines to guarantee coherency.
 */
#define RING_SIZE (8 * DSP_CACHE_LINE_SIZE)
struct ring {
	u32 head;
	u8 fill1[DSP_CACHE_LINE_SIZE - sizeof(u32)];
	u32 tail;
	u8 fill2[DSP_CACHE_LINE_SIZE - sizeof(u32)];
	u8 data[RING_SIZE];
} __packed;

#define RING_TX_OFFSET 0
#define RING_RX_OFFSET sizeof(struct ring)

static int sof_fw_gdb_send_message(struct sof_client_dev *cdev)
{
	struct sof_fw_gdb_priv *priv = cdev->data;
	struct device *dev = &cdev->auxdev.dev;
	int ret;

	switch (priv->ipc_type) {
	case SOF_IPC_TYPE_3:
	{
		struct sof_ipc_cmd_hdr *hdr = &priv->tx_buffer->ipc3.hdr;

		hdr->cmd = SOF_IPC_GLB_GDB_DEBUG;
		hdr->size = sizeof(*hdr);
		break;
	}
	case SOF_IPC_TYPE_4:
	{
		struct sof_ipc4_msg *msg = &priv->tx_buffer->ipc4.msg;

		msg->primary = SOF_IPC4_MSG_TYPE_SET(SOF_IPC4_GLB_ENTER_GDB) |
			SOF_IPC4_MSG_DIR(SOF_IPC4_MSG_REQUEST) |
			SOF_IPC4_MSG_TARGET(SOF_IPC4_FW_GEN_MSG);
		break;
	}
	default:
		return -EINVAL;
	}

	/* send the message */
	ret = sof_client_ipc_tx_message_no_reply(cdev, priv->tx_buffer);
	if (ret < 0)
		dev_err(dev, "IPC message send failed: %d\n", ret);

	return ret;
}

static bool gdb_is_open;

static int sof_fw_gdb_dfs_open(struct inode *inode, struct file *file)
{
	struct sof_client_dev *cdev = inode->i_private;
	struct device *dev = &cdev->auxdev.dev;
	struct sof_fw_gdb_priv *priv = cdev->data;
	int ret;

	if (sof_client_get_fw_state(cdev) == SOF_FW_CRASHED)
		return -ENODEV;

	if (gdb_is_open)
		return -EBUSY;

	ret = debugfs_file_get(file->f_path.dentry);
	if (ret < 0)
		return ret;

	ret = simple_open(inode, file);
	if (ret < 0) {
		dev_err(dev, "failed to open %d\n", ret);
		goto e_dbgfs;
	}

	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0) {
		dev_err(dev, "debugfs open failed to resume %d\n", ret);
		goto e_dbgfs;
	}

	ret = sof_client_boot_dsp(cdev);
	if (ret < 0) {
		dev_err(dev, "debugfs open failed to power on DSP %d\n", ret);
		goto e_pm;
	}

	/* When file is opened, send GDB init command to FW */
	if (!gdb_is_open) {
		ret = sof_fw_gdb_send_message(cdev);
		if (ret < 0) {
			dev_err(dev, "failed to send IPC %d\n", ret);
			goto e_pm;
		}

		gdb_is_open = true;
	}

	priv->cdev = cdev;

	schedule_delayed_work(&priv->poll_work, msecs_to_jiffies(SOF_FS_GDB_POLL_DELAY_MS));

	return ret;

e_pm:
	pm_runtime_put_autosuspend(dev);
e_dbgfs:
	debugfs_file_put(file->f_path.dentry);
	return ret;
}

static ssize_t sof_fw_gdb_get_ptrs(struct sof_client_dev *cdev,
				   u32 *head, u32 *tail, size_t offset)
{
	ssize_t slot_offset = sof_client_ipc4_find_debug_slot_offset_by_type(cdev,
								SOF_IPC4_DEBUG_SLOT_GDB_STUB);

	if (!slot_offset)
		return -ENODEV;

	slot_offset += offset;

	sof_client_mailbox_read(cdev, slot_offset, head, sizeof(*head));
	sof_client_mailbox_read(cdev, slot_offset + offsetof(struct ring, tail),
				tail, sizeof(*tail));

	return slot_offset;
}

static ssize_t sof_fw_gdb_get_rx_ptrs(struct sof_client_dev *cdev,
				      u32 *head, u32 *tail)
{
	return sof_fw_gdb_get_ptrs(cdev, head, tail, RING_RX_OFFSET);
}

static ssize_t sof_debug_read(struct sof_client_dev *cdev, char *buff, size_t bytes)
{
	size_t count = 0;
	u32 head, tail;
	ssize_t rx_ring_offset = sof_fw_gdb_get_rx_ptrs(cdev, &head, &tail);
	unsigned int i;

	if (rx_ring_offset < 0)
		return rx_ring_offset;

	if (tail >= RING_SIZE) {
		dev_err(&cdev->auxdev.dev, "%s: %#zx head %x beyond buffer limit!\n",
			__func__, rx_ring_offset, head);
		return -EIO;
	}

	for (i = 0; i < bytes; i++) {
		if (tail == head) {
			/* No Data */
			break;
		}
		sof_client_mailbox_read(cdev, rx_ring_offset + offsetof(struct ring, data) + tail,
					buff + i, sizeof(buff[i]));
		tail = (tail + 1) % RING_SIZE;
		count++;
	}

	/* Read out up to "bytes" bytes from "tail." Update "tail" to the new location */
	sof_client_mailbox_write(cdev, rx_ring_offset + offsetof(struct ring, tail), &tail,
				 sizeof(tail));

	return count;
}

static ssize_t sof_fw_gdb_dfs_read(struct file *file, char __user *buffer,
				   size_t count, loff_t *ppos)
{
	struct sof_client_dev *cdev = file->private_data;
	struct sof_fw_gdb_priv *priv = cdev->data;
	char *kbuff = kzalloc(count, GFP_KERNEL);

	if (!kbuff)
		return -ENOMEM;

	mutex_lock(&priv->mutex);

	ssize_t count_read = sof_debug_read(cdev, kbuff, count);

	mutex_unlock(&priv->mutex);

	unsigned long remain = copy_to_user(buffer, kbuff, count_read);

	kfree(kbuff);

	if (remain)
		return -EFAULT;

	/*
	 * Since the DSP runs asynchronously to the kernel, we have to wait a
	 * while for a response. If we return no data, gdb will exit. This keeps
	 * it listening for future data.
	 */
	if (count_read == 0) {
		if (copy_to_user(buffer, "\0", 1))
			return -EFAULT;

		return 1;
	}

	return count_read;
}

static ssize_t sof_fw_gdb_get_tx_ptrs(struct sof_client_dev *cdev,
				      u32 *head, u32 *tail)
{
	return sof_fw_gdb_get_ptrs(cdev, head, tail, RING_TX_OFFSET);
}

static ssize_t sof_debug_write(struct sof_client_dev *cdev, char *message, size_t bytes)
{
	size_t count = 0;
	u32 head, tail;
	ssize_t tx_ring_offset = sof_fw_gdb_get_tx_ptrs(cdev, &head, &tail);
	unsigned int i;

	if (tx_ring_offset < 0)
		return tx_ring_offset;

	if (head >= RING_SIZE) {
		dev_err(&cdev->auxdev.dev, "%s: %#zx head %x beyond buffer limit!\n",
			__func__, tx_ring_offset, head);
		return -EIO;
	}

	for (i = 0; i < bytes; i++) {
		if ((head + 1) % RING_SIZE == tail)
			break;

		sof_client_mailbox_write(cdev, tx_ring_offset + offsetof(struct ring, data) + head,
					 message + i, sizeof(message[i]));
		head = (head + 1) % RING_SIZE;
		count++;
	}

	sof_client_mailbox_write(cdev, tx_ring_offset, &head, sizeof(head));

	return count;
}

static ssize_t sof_fw_gdb_dfs_write(struct file *file, const char __user *buffer,
				    size_t count, loff_t *ppos)
{
	struct sof_client_dev *cdev = file->private_data;
	struct sof_fw_gdb_priv *priv = cdev->data;
	char *kbuff = kzalloc(count + 1, GFP_KERNEL);

	if (!kbuff)
		return -ENOMEM;

	unsigned long num_failed = copy_from_user(kbuff, buffer, count);

	if (num_failed == count) {
		kfree(kbuff);
		return -EFAULT;
	}

	mutex_lock(&priv->mutex);

	ssize_t num_written = sof_debug_write(cdev, kbuff, count - num_failed);

	mutex_unlock(&priv->mutex);

	kfree(kbuff);

	return num_written;
};

static int sof_fw_gdb_dfs_release(struct inode *inode, struct file *file)
{
	struct sof_client_dev *cdev = inode->i_private;
	struct device *dev = &cdev->auxdev.dev;
	struct sof_fw_gdb_priv *priv = cdev->data;

	cancel_delayed_work_sync(&priv->poll_work);
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);
	debugfs_file_put(file->f_path.dentry);
	gdb_is_open = false;

	return 0;
}

static __poll_t sof_fw_gdb_dfs_poll(struct file *file, struct poll_table_struct *wait)
{
	struct sof_client_dev *cdev = file->private_data;
	struct sof_fw_gdb_priv *priv = cdev->data;
	__poll_t mask = 0;

	mutex_lock(&priv->mutex);
	u32 head, tail;

	poll_wait(file, &priv->rxq, wait);
	poll_wait(file, &priv->txq, wait);

	if (sof_fw_gdb_get_rx_ptrs(cdev, &head, &tail) >= 0 && tail != head)
		mask |= POLLIN | POLLRDNORM;
	/* readable */

	if (sof_fw_gdb_get_tx_ptrs(cdev, &head, &tail) >= 0 && (head + 1) % RING_SIZE != tail)
		mask |= POLLOUT | POLLWRNORM;
	/* writable */

	mutex_unlock(&priv->mutex);

	return mask;
}

static const struct file_operations sof_fw_gdb_fops = {
	.open = sof_fw_gdb_dfs_open,
	.read = sof_fw_gdb_dfs_read,
	.write = sof_fw_gdb_dfs_write,
	.poll = sof_fw_gdb_dfs_poll,
	.llseek = default_llseek,
	.release = sof_fw_gdb_dfs_release,

	.owner = THIS_MODULE,
};

static void sof_fw_gdb_poll_work(struct work_struct *work)
{
	struct sof_fw_gdb_priv *priv = container_of(work, struct sof_fw_gdb_priv,
						    poll_work.work);
	u32 head, tail;

	mutex_lock(&priv->mutex);

	if (sof_fw_gdb_get_rx_ptrs(priv->cdev, &head, &tail) >= 0 && head != tail)
		wake_up_interruptible(&priv->rxq);

	if (sof_fw_gdb_get_tx_ptrs(priv->cdev, &head, &tail) >= 0 && (head + 1) % RING_SIZE != tail)
		wake_up_interruptible(&priv->txq);

	mutex_unlock(&priv->mutex);

	schedule_delayed_work(&priv->poll_work, msecs_to_jiffies(SOF_FS_GDB_POLL_DELAY_MS));
}

static int sof_fw_gdb_probe(struct auxiliary_device *auxdev,
			    const struct auxiliary_device_id *id)
{
	struct sof_client_dev *cdev = auxiliary_dev_to_sof_client_dev(auxdev);
	struct dentry *debugfs_root = sof_client_get_debugfs_root(cdev);
	static const struct file_operations *fops;
	struct device *dev = &auxdev->dev;
	struct sof_fw_gdb_priv *priv;
	size_t alloc_size;

	/* allocate memory for client data */
	priv = devm_kzalloc(&auxdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->ipc_type = sof_client_get_ipc_type(cdev);
	alloc_size = sizeof(*priv->tx_buffer);
	INIT_DELAYED_WORK(&priv->poll_work, sof_fw_gdb_poll_work);

	priv->tx_buffer = devm_kmalloc(dev, alloc_size, GFP_KERNEL);
	if (!priv->tx_buffer)
		return -ENOMEM;

	fops = &sof_fw_gdb_fops;

	cdev->data = priv;
	mutex_init(&priv->mutex);
	init_waitqueue_head(&priv->rxq);
	init_waitqueue_head(&priv->txq);

	priv->dfs_file = debugfs_create_file("fw_gdb", 0644, debugfs_root,
					     cdev, fops);

	/* enable runtime PM */
	pm_runtime_set_autosuspend_delay(dev, SOF_IPC_CLIENT_SUSPEND_DELAY_MS);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_enable(dev);
	pm_runtime_mark_last_busy(dev);
	pm_runtime_idle(dev);

	return 0;
}

static void sof_fw_gdb_remove(struct auxiliary_device *auxdev)
{
	struct sof_client_dev *cdev = auxiliary_dev_to_sof_client_dev(auxdev);
	struct sof_fw_gdb_priv *priv = cdev->data;

	pm_runtime_disable(&auxdev->dev);

	debugfs_remove(priv->dfs_file);
}

static const struct auxiliary_device_id sof_fw_gdb_client_id_table[] = {
	{ .name = "snd_sof.fw_gdb" },
	{},
};
MODULE_DEVICE_TABLE(auxiliary, sof_fw_gdb_client_id_table);

/*
 * No need for driver pm_ops as the generic pm callbacks in the auxiliary bus
 * type are enough to ensure that the parent SOF device resumes to bring the DSP
 * back to D0.
 * Driver name will be set based on KBUILD_MODNAME.
 */
static struct auxiliary_driver sof_gdb_fw_client_drv = {
	.probe = sof_fw_gdb_probe,
	.remove = sof_fw_gdb_remove,

	.id_table = sof_fw_gdb_client_id_table,
};

module_auxiliary_driver(sof_gdb_fw_client_drv);

MODULE_DESCRIPTION("SOF IPC FW GDB Client Driver");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_IMPORT_NS("SND_SOC_SOF_CLIENT");
