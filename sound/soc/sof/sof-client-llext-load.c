// SPDX-License-Identifier: GPL-2.0-only
//
// Copyright(c) 2024 Intel Corporation
//
// Author: Kai Vehmanen <kai.vehmanen@linux.intel.com>
//
// SOF client driver: interactive llext module load via debugfs.
//
// Provides /sys/kernel/debug/sof/llext_load — a write-only debugfs file that
// accepts a raw rimage-format library binary (e.g. mymodule.ri) piped from the
// host using:
//
//   cat mymodule.ri > /sys/kernel/debug/sof/llext_load
//
// The driver implements the host side of the 2-step handshake with the DSP
// shell command "sof llext_load <name> [lib_id]":
//
//   1. DSP shell sets ADSP debug window slot state = REQUESTING with lib_id.
//   2. Host write handler detects REQUESTING, sets DMA_ACTIVE, copies the
//      binary via the HDA code-loader DMA (IPC4 LOAD_LIBRARY sequence).
//   3. On completion, host sets DMA_DONE (or ERROR + result errno).
//   4. DSP shell wakes up and reports the result.

#include <linux/auxiliary_bus.h>
#include <linux/debugfs.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm_runtime.h>
#include <sound/sof/ipc4/header.h>

#include "ipc4-priv.h"
#include "sof-client.h"
#include "sof-priv.h"
#include "shell-llext-shm.h"

#define SOF_LLEXT_SUSPEND_DELAY_MS	3000
#define SOF_LLEXT_MAX_SIZE		(8 * 1024 * 1024)	/* 8 MB */

struct sof_llext_load_priv {
	struct dentry	*dfs_file;
	struct mutex	 lock;  /* serialise concurrent writes */
};

/* -------------------------------------------------------------------------
 * debugfs file operations
 * -------------------------------------------------------------------------
 */

static int sof_llext_dfs_open(struct inode *inode, struct file *file)
{
	int ret = debugfs_file_get(file->f_path.dentry);

	if (unlikely(ret))
		return ret;

	return simple_open(inode, file);
}

static int sof_llext_dfs_release(struct inode *inode, struct file *file)
{
	debugfs_file_put(file->f_path.dentry);
	return 0;
}

static ssize_t sof_llext_dfs_write(struct file *file,
				   const char __user *user_buf,
				   size_t count, loff_t *ppos)
{
	struct sof_client_dev *cdev = file->private_data;
	struct sof_llext_load_priv *priv = cdev->data;
	struct device *dev = &cdev->auxdev.dev;
	struct snd_sof_dev *sdev = sof_client_dev_to_sof_dev(cdev);
	struct sof_shell_llext_slot slot_hdr;
	ssize_t slot_offset;
	void *buf = NULL;
	u32 lib_id, state;
	int ret;

	if (*ppos) {
		dev_err_ratelimited(&cdev->auxdev.dev,
			"llext_load: partial write rejected (offset=%lld) — "
			"use 'dd if=<module.ri> of=/sys/kernel/debug/sof/llext_load bs=$(stat -c%%s <module.ri>) count=1'\n",
			*ppos);
		return -EINVAL;		/* no seek; single-shot write only */
	}

	if (!count || count > SOF_LLEXT_MAX_SIZE)
		return count ? -EFBIG : -EINVAL;

	if (!mutex_trylock(&priv->lock))
		return -EBUSY;

	/*
	 * Find the ADSP debug window slot populated by the DSP shell command.
	 * The slot is created dynamically by the DSP; it won't exist until the
	 * user has run 'sof llext_load' on the DSP shell.
	 */
	slot_offset = sof_client_ipc4_find_debug_slot_offset_by_type(
					cdev, SOF_IPC4_DEBUG_SLOT_LLEXT_LOAD);
	if (!slot_offset) {
		dev_err(dev, "llext_load: DSP slot not found — "
			"run 'sof llext_load <name> [lib_id]' on the DSP shell first\n");
		ret = -ENOENT;
		goto out_unlock;
	}

	/* Read and validate the slot header */
	sof_client_mailbox_read(cdev, (u32)slot_offset,
				&slot_hdr, sizeof(slot_hdr));

	if (slot_hdr.magic != SOF_SHELL_LLEXT_MAGIC) {
		dev_err(dev, "llext_load: bad slot magic 0x%08x\n",
			slot_hdr.magic);
		ret = -EINVAL;
		goto out_unlock;
	}

	state = slot_hdr.state;
	if (state != SOF_SHELL_LLEXT_REQUESTING) {
		dev_err(dev, "llext_load: DSP not waiting (state=%u); "
			"run 'sof llext_load' on DSP shell first\n", state);
		ret = (state == SOF_SHELL_LLEXT_IDLE) ? -ENOENT : -EBUSY;
		goto out_unlock;
	}

	lib_id = slot_hdr.lib_id;
	if (!lib_id) {
		dev_err(dev, "llext_load: invalid lib_id=0 in slot\n");
		ret = -EINVAL;
		goto out_unlock;
	}

	dev_dbg(dev, "llext_load: slot OK — lib_id=%u  name='%s'  size=%zu\n",
		lib_id, slot_hdr.name, count);

	/* Signal the DSP that the host is now performing the DMA */
	state = SOF_SHELL_LLEXT_DMA_ACTIVE;
	sof_client_mailbox_write(cdev,
		(u32)slot_offset + offsetof(struct sof_shell_llext_slot, state),
		&state, sizeof(state));

	/* Copy the library binary from userspace.
	 * Use devm_kmalloc so the buffer outlives this function: the pointer is
	 * stored inside the fw_lib that xa_insert() keeps alive until the next
	 * firmware reload.  The devm allocator on the sof device will free it
	 * on device removal.
	 */
	buf = devm_kmalloc(dev->parent, count, GFP_KERNEL);
	if (!buf) {
		ret = -ENOMEM;
		goto out_error;
	}
	if (copy_from_user(buf, user_buf, count)) {
		devm_kfree(dev->parent, buf);
		ret = -EFAULT;
		goto out_error;
	}

	/*
	 * Do NOT call sof_client_boot_dsp() here.  The DSP must already be
	 * awake: the shell command 'sof llext_load' is actively polling the
	 * debug-window slot, which requires the DSP cores to be running.
	 * Calling pm_runtime_resume_and_get(sdev->dev) when the SoF device
	 * unexpectedly entered D3 (SOF_FW_BOOT_COMPLETE not set) would trigger
	 * a full firmware reload, wiping the debug-window slot and causing the
	 * DSP to reject LOAD_LIBRARY_PREPARE with FW error 6.
	 *
	 * If D3 did occur between the slot arm and this write, the slot state
	 * will have been cleared to IDLE and the earlier state != REQUESTING
	 * check will have already returned -ENOENT before we reach this point.
	 */

	/*
	 * Perform the IPC4 LOAD_LIBRARY_PREPARE + HDA code-loader DMA +
	 * LOAD_LIBRARY sequence.  On success @buf is owned by the fw_lib
	 * inside the xa_array and must NOT be freed here.
	 */
	ret = snd_sof_ipc4_load_library_from_buf(sdev, lib_id, buf, count);
	if (ret)
		devm_kfree(dev->parent, buf); /* load failed: free the buffer */

	if (!ret) {
		u32 xfer = (u32)count;
		u32 done  = SOF_SHELL_LLEXT_DMA_DONE;

		sof_client_mailbox_write(cdev,
			(u32)slot_offset +
			offsetof(struct sof_shell_llext_slot, xfer_bytes),
			&xfer, sizeof(xfer));
		sof_client_mailbox_write(cdev,
			(u32)slot_offset +
			offsetof(struct sof_shell_llext_slot, state),
			&done, sizeof(done));

		dev_info(dev, "llext_load: lib_id=%u loaded (%zu bytes)\n",
			 lib_id, count);
		mutex_unlock(&priv->lock);
		return (ssize_t)count;
	}

out_error: {
		u32 err_state = SOF_SHELL_LLEXT_ERROR;
		s32 result    = (s32)ret;

		sof_client_mailbox_write(cdev,
			(u32)slot_offset +
			offsetof(struct sof_shell_llext_slot, result),
			&result, sizeof(result));
		sof_client_mailbox_write(cdev,
			(u32)slot_offset +
			offsetof(struct sof_shell_llext_slot, state),
			&err_state, sizeof(err_state));
	}

out_unlock:
	mutex_unlock(&priv->lock);
	return ret;
}

static const struct file_operations sof_llext_load_fops = {
	.open    = sof_llext_dfs_open,
	.write   = sof_llext_dfs_write,
	.release = sof_llext_dfs_release,
	.llseek  = noop_llseek,	/* allow lseek(0) from dd/tools; write must start at pos=0 */
	.owner   = THIS_MODULE,
};

/* -------------------------------------------------------------------------
 * Auxiliary driver probe / remove
 * -------------------------------------------------------------------------
 */

static int sof_llext_load_probe(struct auxiliary_device *auxdev,
				const struct auxiliary_device_id *id)
{
	struct sof_client_dev *cdev = auxiliary_dev_to_sof_client_dev(auxdev);
	struct dentry *debugfs_root = sof_client_get_debugfs_root(cdev);
	struct sof_llext_load_priv *priv;

	priv = devm_kzalloc(&auxdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	mutex_init(&priv->lock);
	cdev->data = priv;

	/* Write-only: the host pushes data in; the DSP pulls it via DMA. */
	priv->dfs_file = debugfs_create_file("llext_load", 0200,
					     debugfs_root, cdev,
					     &sof_llext_load_fops);

	pm_runtime_set_autosuspend_delay(&auxdev->dev, SOF_LLEXT_SUSPEND_DELAY_MS);
	pm_runtime_use_autosuspend(&auxdev->dev);
	pm_runtime_enable(&auxdev->dev);
	pm_runtime_mark_last_busy(&auxdev->dev);
	pm_runtime_idle(&auxdev->dev);

	dev_dbg(&auxdev->dev, "llext_load: debugfs entry created\n");
	return 0;
}

static void sof_llext_load_remove(struct auxiliary_device *auxdev)
{
	struct sof_client_dev *cdev = auxiliary_dev_to_sof_client_dev(auxdev);
	struct sof_llext_load_priv *priv = cdev->data;

	pm_runtime_disable(&auxdev->dev);
	debugfs_remove(priv->dfs_file);
}

static const struct auxiliary_device_id sof_llext_load_id_table[] = {
	{ .name = "snd_sof.llext-load" },
	{ },
};
MODULE_DEVICE_TABLE(auxiliary, sof_llext_load_id_table);

static struct auxiliary_driver sof_llext_load_drv = {
	.name     = "snd_sof_llext_load",
	.probe    = sof_llext_load_probe,
	.remove   = sof_llext_load_remove,
	.id_table = sof_llext_load_id_table,
};

module_auxiliary_driver(sof_llext_load_drv);

MODULE_DESCRIPTION("SOF Shell LLEXT Load Client");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_IMPORT_NS("SND_SOC_SOF_CLIENT");
MODULE_IMPORT_NS("SND_SOC_SOF");
