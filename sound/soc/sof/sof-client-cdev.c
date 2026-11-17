// SPDX-License-Identifier: GPL-2.0-only
//
// Copyright(c) 2026 Intel Corporation. All rights reserved.
//
// SOF auxiliary client character driver (/dev/sof-dspN) exposing DSP
// firmware debug, telemetry, IPC, memory window, LLEXT module loading,
// winstream shell/GDB, and probe operations to host userspace.

#include <linux/auxiliary_bus.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/kfifo.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <sound/soc.h>
#include <sound/sof/ext_manifest4.h>
#include <sound/sof/header.h>
#include <sound/sof/ipc4/header.h>
#include <uapi/sound/sof/cdev.h>

#include "ipc4-priv.h"
#include "ops.h"
#include "sof-audio.h"
#include "sof-client.h"
#include "sof-priv.h"

#define SOF_CDEV_SUSPEND_DELAY_MS	3000
#define SOF_CDEV_EVENT_FIFO_LEN		64
#define SOF_CDEV_MAX_LARGE_CFG_BYTES	(256 * 1024)
#define SOF_CDEV_MAX_LLEXT_BYTES	(8 * 1024 * 1024)

/* Zephyr winstream header layout */
#define SOF_WINSTREAM_HDR_SIZE		16
#define SOF_WINSTREAM_DATA_MAX(wsz)	((wsz) - SOF_WINSTREAM_HDR_SIZE)

struct sof_winstream_hdr {
	__le32 len;
	__le32 start;
	__le32 end;
	__le32 seq;
	u8 data[];
} __packed;

/* Shared mailbox layout for DSP shell "sof llext_load" command */
#define SOF_SHELL_LLEXT_MAGIC		0x4C454C44U /* 'LELD' */

enum sof_shell_llext_state {
	SOF_SHELL_LLEXT_IDLE       = 0,
	SOF_SHELL_LLEXT_REQUESTING = 1,
	SOF_SHELL_LLEXT_DMA_ACTIVE = 2,
	SOF_SHELL_LLEXT_DMA_DONE   = 3,
	SOF_SHELL_LLEXT_ERROR      = 4,
};

struct sof_shell_llext_slot {
	__u32 magic;
	__u32 state;
	__u32 lib_id;
	__u32 xfer_bytes;
	__s32 result;
	__u32 reserved[3];
	char  name[64];
} __packed;

/* IPC4 Probe constants matching sof-client-probes-ipc4.c */
enum sof_ipc4_dma_type {
	SOF_IPC4_DMA_HDA_HOST_OUTPUT = 0,
	SOF_IPC4_DMA_HDA_HOST_INPUT = 1,
};

enum sof_ipc4_probe_runtime_param {
	SOF_IPC4_PROBE_INJECTION_DMA = 1,
	SOF_IPC4_PROBE_INJECTION_DMA_DETACH,
	SOF_IPC4_PROBE_POINTS,
	SOF_IPC4_PROBE_POINTS_DISCONNECT,
	SOF_IPC4_PROBE_POINTS_AVAILABLE,
};

struct sof_ipc4_probe_gtw_cfg {
	u32 node_id;
	u32 dma_buffer_size;
} __packed __aligned(4);

#define SOF_IPC4_PROBE_NODE_ID_INDEX(x)		((x) & GENMASK(7, 0))
#define SOF_IPC4_PROBE_NODE_ID_TYPE(x)		(((x) << 8) & GENMASK(12, 8))

struct sof_ipc4_probe_cfg {
	struct sof_ipc4_base_module_cfg base;
	struct sof_ipc4_probe_gtw_cfg gtw_cfg;
} __packed __aligned(4);

struct sof_ipc4_probe_point {
	u32 point_id;
	u32 purpose;
	u32 stream_tag;
} __packed __aligned(4);

struct sof_ipc4_probe_info {
	unsigned int num_elems;
	DECLARE_FLEX_ARRAY(struct sof_ipc4_probe_point, points);
} __packed;

#define SOF_PROBE_INVALID_PIPELINE_ID	0xFF

struct sof_cdev_priv {
	struct sof_client_dev *cdev;
	struct miscdevice miscdev;
	char dev_name[24];
	struct mutex io_lock;
	struct mutex fh_lock;
	struct list_head fh_list;
	struct sof_man4_module *probe_mentry;
};

struct sof_cdev_fh {
	struct sof_cdev_priv *priv;
	struct list_head list;
	wait_queue_head_t wait;
	spinlock_t fifo_lock;
	DECLARE_KFIFO(events, struct sof_cdev_event, SOF_CDEV_EVENT_FIFO_LEN);
	bool holding_d0;
};

/* -------------------------------------------------------------------------
 * DSP Power State (D3 / D0i3 -> D0i0) & Window Helpers
 * -------------------------------------------------------------------------
 */

static bool sof_cdev_is_dsp_d0i0(struct snd_sof_dev *sdev)
{
	return pm_runtime_active(sdev->dev) &&
	       sdev->fw_state == SOF_FW_BOOT_COMPLETE &&
	       sdev->dsp_power_state.state == SOF_DSP_PM_D0 &&
	       sdev->dsp_power_state.substate == 0;
}

static int sof_cdev_wake_dsp(struct sof_client_dev *cdev)
{
	struct snd_sof_dev *sdev = sof_client_dev_to_sof_dev(cdev);
	struct device *dev = &cdev->auxdev.dev;
	const struct sof_dsp_power_state target_d0i0 = {
		.state = SOF_DSP_PM_D0,
		.substate = 0,
	};
	int ret;

	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0 && ret != -EACCES)
		return ret;

	ret = sof_client_boot_dsp(cdev);
	if (ret < 0) {
		pm_runtime_put_autosuspend(dev);
		return ret;
	}

	ret = snd_sof_dsp_set_power_state(sdev, &target_d0i0);
	if (ret < 0) {
		pm_runtime_put_autosuspend(dev);
		return ret;
	}

	return 0;
}

static void sof_cdev_idle_dsp(struct sof_client_dev *cdev)
{
	struct device *dev = &cdev->auxdev.dev;

	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);
}

static u32 sof_cdev_get_window_size(struct snd_sof_dev *sdev, u32 win_id)
{
	switch (win_id) {
	case SOF_CDEV_WIN_FW_REGS:
	case SOF_CDEV_WIN_OUTBOX:
	case SOF_CDEV_WIN_PERF:
		return 0x2000; /* 8 KB mapped in DMWLO0/1/3 */
	case SOF_CDEV_WIN_DEBUG:
		return 0x3000; /* 12 KB mapped in DMWLO2 on ACE/cAVS */
	default:
		return 0x2000;
	}
}

static void sof_cdev_fill_windows_and_slots(struct snd_sof_dev *sdev,
					    struct sof_cdev_win_desc *wins,
					    struct sof_cdev_slot_desc *slots)
{
	u32 win2_size = sof_cdev_get_window_size(sdev, SOF_CDEV_WIN_DEBUG);
	int pm_ref;
	int i;

	memset(wins, 0, sizeof(*wins) * SOF_CDEV_MAX_WINDOWS);
	memset(slots, 0, sizeof(*slots) * SOF_CDEV_MAX_DEBUG_SLOTS);

	for (i = 0; i < SOF_CDEV_MAX_WINDOWS; i++) {
		int off = snd_sof_dsp_get_window_offset(sdev, i);

		wins[i].win_id = i;
		if (off >= 0 && sdev->bar[sdev->mailbox_bar]) {
			wins[i].bar_offset = (u32)off;
			wins[i].size = sof_cdev_get_window_size(sdev, i);
			wins[i].flags = 1;
		}
	}

	if (!wins[SOF_CDEV_WIN_DEBUG].flags)
		return;

	pm_ref = pm_runtime_get_if_active(sdev->dev);
	if (pm_ref <= 0)
		return;

	if (!sof_cdev_is_dsp_d0i0(sdev)) {
		pm_runtime_put_noidle(sdev->dev);
		return;
	}

	for (i = 0; i < SOF_CDEV_MAX_DEBUG_SLOTS; i++) {
		u32 desc_off = sdev->debug_box.offset +
			       i * SOF_IPC4_DEBUG_DESCRIPTOR_SIZE;
		u32 raw[3] = { 0 };
		u32 win_offset, slot_sz;

		if (i == SOF_CDEV_MAX_DEBUG_SLOTS - 1) {
			win_offset = SOF_IPC4_DEBUG_PAGE0_SLOT_OFFSET;
			slot_sz = SOF_IPC4_DEBUG_SLOT_SIZE -
				  SOF_IPC4_DEBUG_PAGE0_SLOT_OFFSET;
		} else {
			win_offset = (i + 1) * SOF_IPC4_DEBUG_SLOT_SIZE;
			slot_sz = SOF_IPC4_DEBUG_SLOT_SIZE;
		}

		sof_mailbox_read(sdev, desc_off, raw, sizeof(raw));

		slots[i].slot_index = i;
		if (raw[1] != 0 && raw[1] != 0xffffffffU &&
		    win_offset + slot_sz <= win2_size) {
			slots[i].res_id = raw[0];
			slots[i].type = raw[1];
			slots[i].vma = raw[2];
		}
		slots[i].win_offset = win_offset;
		slots[i].size = slot_sz;
		slots[i].bar_offset = sdev->debug_box.offset + win_offset;
	}

	pm_runtime_put_noidle(sdev->dev);
}

static int sof_cdev_resolve_slot(struct snd_sof_dev *sdev, u32 slot_type,
				 u32 match_idx, u32 *bar_offset, u32 *slot_size)
{
	u32 win2_size = sof_cdev_get_window_size(sdev, SOF_CDEV_WIN_DEBUG);
	u32 found = 0;
	int i;

	if (!slot_type || slot_type == 0xffffffffU)
		return -EINVAL;

	for (i = 0; i < SOF_CDEV_MAX_DEBUG_SLOTS; i++) {
		u32 desc_type_off = sdev->debug_box.offset +
				    i * SOF_IPC4_DEBUG_DESCRIPTOR_SIZE +
				    sizeof(u32);
		u32 type = 0;
		u32 win_off, sz;

		if (i == SOF_CDEV_MAX_DEBUG_SLOTS - 1) {
			win_off = SOF_IPC4_DEBUG_PAGE0_SLOT_OFFSET;
			sz = SOF_IPC4_DEBUG_SLOT_SIZE -
			     SOF_IPC4_DEBUG_PAGE0_SLOT_OFFSET;
		} else {
			win_off = (i + 1) * SOF_IPC4_DEBUG_SLOT_SIZE;
			sz = SOF_IPC4_DEBUG_SLOT_SIZE;
		}

		if (win_off + sz > win2_size)
			continue;

		sof_mailbox_read(sdev, desc_type_off, &type, sizeof(type));
		if (!type || type == 0xffffffffU)
			continue;

		if (type == slot_type ||
		    ((slot_type == SOF_CDEV_SLOT_CRITICAL_LOG ||
		      slot_type == SOF_CDEV_SLOT_DEBUG_LOG) &&
		     (type & 0xffffff00U) == slot_type)) {
			if (found == match_idx) {
				*bar_offset = sdev->debug_box.offset + win_off;
				*slot_size = sz;
				return 0;
			}
			found++;
		}
	}

	return -ENOENT;
}

/* -------------------------------------------------------------------------
 * Zephyr Winstream Helpers
 * -------------------------------------------------------------------------
 */

static u32 sof_ws_idx_mod(u32 idx, u32 len)
{
	return idx >= len ? idx - len : idx;
}

static u32 sof_ws_idx_sub(u32 a, u32 b, u32 len)
{
	return sof_ws_idx_mod(a + (len - b), len);
}

static void sof_ws_write(struct sof_winstream_hdr *ws, size_t win_size,
			 const u8 *data, u32 len0)
{
	u32 expected = SOF_WINSTREAM_DATA_MAX(win_size);
	u32 ws_len = le32_to_cpu(ws->len);
	u32 start, end, seq, len, suffix;

	if (ws_len != expected) {
		ws->len = cpu_to_le32(expected);
		ws->start = 0;
		ws->end = 0;
		ws->seq = 0;
		ws_len = expected;
	}

	start = le32_to_cpu(ws->start);
	end = le32_to_cpu(ws->end);
	seq = le32_to_cpu(ws->seq);
	len = len0;

	if (!ws_len)
		return;

	if (len > ws_len - 1) {
		start = end;
		len = ws_len - 1;
	}

	len = min(len, ws_len);
	if (seq != 0) {
		u32 avail = (ws_len - 1) - sof_ws_idx_sub(end, start, ws_len);

		if (len > avail)
			start = sof_ws_idx_mod(start + (len - avail), ws_len);
	}

	if (len < len0) {
		start = end;
		data += len0 - len;
	}

	suffix = min(len, ws_len - end);
	memcpy(&ws->data[end], data, suffix);
	if (len > suffix)
		memcpy(&ws->data[0], data + suffix, len - suffix);

	end = sof_ws_idx_mod(end + len, ws_len);

	ws->start = cpu_to_le32(start);
	ws->end = cpu_to_le32(end);
	ws->seq = cpu_to_le32(seq + len0);
}

static u32 sof_ws_read(struct sof_winstream_hdr *ws, size_t win_size,
		       u32 *seq, u8 *buf, u32 buflen)
{
	u32 expected = SOF_WINSTREAM_DATA_MAX(win_size);
	u32 ws_len = le32_to_cpu(ws->len);
	u32 ws_start = le32_to_cpu(ws->start);
	u32 ws_end = le32_to_cpu(ws->end);
	u32 ws_seq = le32_to_cpu(ws->seq);
	u32 seq0 = *seq;
	u32 len, behind, copy, suffix;

	if (ws_len != expected || !ws_len || !buflen)
		return 0;

	if (*seq == 0) {
		/* First read: start from current available window contents */
		u32 buffered = sof_ws_idx_sub(ws_end, ws_start, ws_len);

		*seq = (ws_seq >= buffered) ? (ws_seq - buffered) : ws_seq;
		seq0 = *seq;
	}

	if (*seq == ws_seq || ws_start == ws_end) {
		*seq = ws_seq;
		return 0;
	}

	behind = ws_seq - *seq;
	if (behind > sof_ws_idx_sub(ws_end, ws_start, ws_len)) {
		u32 buffered = sof_ws_idx_sub(ws_end, ws_start, ws_len);

		*seq = ws_seq - buffered;
		seq0 = *seq;
		behind = buffered;
	}

	copy = sof_ws_idx_sub(ws_end, behind, ws_len);
	len = min(buflen, behind);
	suffix = min(len, ws_len - copy);

	memcpy(buf, &ws->data[copy], suffix);
	if (len > suffix)
		memcpy(buf + suffix, &ws->data[0], len - suffix);

	*seq = seq0 + len;
	return len;
}

/* -------------------------------------------------------------------------
 * IOCTL Handlers
 * -------------------------------------------------------------------------
 */

static int sof_cdev_ioc_get_info(struct sof_cdev_priv *priv, void __user *argp)
{
	struct snd_sof_dev *sdev = sof_client_dev_to_sof_dev(priv->cdev);
	const struct sof_ipc_fw_version *ver = sof_client_get_fw_version(priv->cdev);
	struct sof_cdev_info *info;
	int ret = 0;

	info = kzalloc_obj(*info);
	if (!info)
		return -ENOMEM;

	info->abi_version = SOF_CDEV_ABI_VERSION;
	info->ipc_type = sof_client_get_ipc_type(priv->cdev);
	info->fw_state = sof_client_get_fw_state(priv->cdev);
	info->dsp_power_state = sdev->dsp_power_state.state;
	info->pm_runtime_active = pm_runtime_active(sdev->dev) ? 1 : 0;
	info->num_cores = sdev->num_cores ? sdev->num_cores : 1;
	info->enabled_cores_mask = sdev->enabled_cores_mask;
	info->max_payload_size = sof_client_get_ipc_max_payload_size(priv->cdev);

	if (sdev->fw_version.major || sdev->fw_version.minor) {
		info->fw_ver_major = sdev->fw_version.major;
		info->fw_ver_minor = sdev->fw_version.minor;
		info->fw_ver_micro = sdev->fw_version.micro;
		info->fw_ver_build = sdev->fw_version.build;
	} else if (ver) {
		info->fw_ver_major = ver->major;
		info->fw_ver_minor = ver->minor;
		info->fw_ver_micro = ver->micro;
		info->fw_ver_build = ver->build;
	}

	if (sdev->pdata) {
		if (sdev->pdata->name)
			strscpy(info->platform, sdev->pdata->name,
				sizeof(info->platform));
		else if (sdev->pdata->platform)
			strscpy(info->platform, sdev->pdata->platform,
				sizeof(info->platform));

		if (sdev->pdata->fw_filename)
			scnprintf(info->fw_filename, sizeof(info->fw_filename),
				  "%s/%s",
				  sdev->pdata->fw_filename_prefix ?
				  sdev->pdata->fw_filename_prefix : "",
				  sdev->pdata->fw_filename);
	}

	mutex_lock(&priv->io_lock);
	sof_cdev_fill_windows_and_slots(sdev, info->windows, info->debug_slots);
	mutex_unlock(&priv->io_lock);

	if (copy_to_user(argp, info, sizeof(*info)))
		ret = -EFAULT;

	kfree(info);
	return ret;
}

static int sof_cdev_ioc_pm_ctl(struct sof_cdev_fh *fh, void __user *argp)
{
	struct sof_cdev_priv *priv = fh->priv;
	struct snd_sof_dev *sdev = sof_client_dev_to_sof_dev(priv->cdev);
	struct sof_cdev_pm_ctl ctl;
	int ret = 0;

	if (copy_from_user(&ctl, argp, sizeof(ctl)))
		return -EFAULT;

	mutex_lock(&priv->io_lock);

	switch (ctl.op) {
	case SOF_PM_OP_QUERY:
		break;
	case SOF_PM_OP_WAKE_HOLD:
		if (!fh->holding_d0) {
			ret = sof_client_core_module_get(priv->cdev);
			if (ret < 0)
				break;
			ret = sof_cdev_wake_dsp(priv->cdev);
			if (ret < 0) {
				sof_client_core_module_put(priv->cdev);
				break;
			}
			fh->holding_d0 = true;
		}
		break;
	case SOF_PM_OP_RELEASE_HOLD:
		if (fh->holding_d0) {
			sof_cdev_idle_dsp(priv->cdev);
			sof_client_core_module_put(priv->cdev);
			fh->holding_d0 = false;
		}
		break;
	case SOF_PM_OP_BOOT_DSP:
		ret = sof_cdev_wake_dsp(priv->cdev);
		if (ret >= 0)
			sof_cdev_idle_dsp(priv->cdev);
		break;
	case SOF_PM_OP_CORE_GET:
		if (ctl.core_id >= sdev->num_cores) {
			ret = -EINVAL;
			break;
		}
		ret = snd_sof_dsp_core_get(sdev, ctl.core_id);
		break;
	case SOF_PM_OP_CORE_PUT:
		if (ctl.core_id >= sdev->num_cores) {
			ret = -EINVAL;
			break;
		}
		ret = snd_sof_dsp_core_put(sdev, ctl.core_id);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	ctl.fw_state = sof_client_get_fw_state(priv->cdev);
	ctl.dsp_power_state = sdev->dsp_power_state.state;
	ctl.pm_runtime_active = pm_runtime_active(sdev->dev) ? 1 : 0;
	ctl.enabled_cores_mask = sdev->enabled_cores_mask;
	ctl.fd_holding_d0 = fh->holding_d0 ? 1 : 0;

	mutex_unlock(&priv->io_lock);

	if (ret < 0)
		return ret;

	if (copy_to_user(argp, &ctl, sizeof(ctl)))
		return -EFAULT;

	return 0;
}

static int sof_cdev_ioc_ipc_tx(struct sof_cdev_priv *priv, void __user *argp)
{
	struct sof_client_dev *cdev = priv->cdev;
	size_t max_payload = sof_client_get_ipc_max_payload_size(cdev);
	struct sof_cdev_ipc_msg req;
	struct sof_ipc4_msg msg4 = {{ 0 }};
	struct sof_ipc4_msg reply4 = {{ 0 }};
	void *tx_buf = NULL;
	void *rx_buf = NULL;
	bool woke_dsp = false;
	int ret;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	if (req.tx_size > max_payload || req.rx_size > max_payload)
		return -EINVAL;

	if (req.tx_size) {
		if (!req.tx_data_ptr)
			return -EINVAL;
		tx_buf = memdup_user(u64_to_user_ptr(req.tx_data_ptr), req.tx_size);
		if (IS_ERR(tx_buf))
			return PTR_ERR(tx_buf);
	}

	if (req.rx_size && !(req.flags & SOF_CDEV_IPC_FL_NO_REPLY)) {
		if (!req.rx_data_ptr) {
			kfree(tx_buf);
			return -EINVAL;
		}
		rx_buf = kzalloc(req.rx_size, GFP_KERNEL);
		if (!rx_buf) {
			kfree(tx_buf);
			return -ENOMEM;
		}
	}

	mutex_lock(&priv->io_lock);

	ret = sof_cdev_wake_dsp(cdev);
	if (ret < 0)
		goto out_unlock;
	woke_dsp = true;

	if (sof_client_get_ipc_type(cdev) == SOF_IPC_TYPE_4) {
		msg4.primary = req.primary;
		msg4.extension = req.extension;
		msg4.data_size = req.tx_size;
		msg4.data_ptr = tx_buf;

		reply4.data_size = req.rx_size;
		reply4.data_ptr = rx_buf;

		if (req.flags & SOF_CDEV_IPC_FL_NO_REPLY)
			ret = sof_client_ipc_tx_message_no_reply(cdev, &msg4);
		else
			ret = sof_client_ipc_tx_message(cdev, &msg4, &reply4,
							req.rx_size);

		req.reply_primary = reply4.primary;
		req.reply_extension = reply4.extension;
		req.ipc_status = (s32)(reply4.primary & SOF_IPC4_REPLY_STATUS);
		req.rx_size = (u32)reply4.data_size;
	} else {
		if (!tx_buf || req.tx_size < sizeof(struct sof_ipc_cmd_hdr)) {
			ret = -EINVAL;
			goto out_unlock;
		}
		ret = sof_client_ipc_tx_message(cdev, tx_buf, rx_buf, req.rx_size);
		req.ipc_status = ret;
	}

out_unlock:
	if (woke_dsp)
		sof_cdev_idle_dsp(cdev);
	mutex_unlock(&priv->io_lock);

	if (!ret && rx_buf && req.rx_size > 0) {
		if (copy_to_user(u64_to_user_ptr(req.rx_data_ptr), rx_buf,
				 req.rx_size))
			ret = -EFAULT;
	}

	if (copy_to_user(argp, &req, sizeof(req)))
		ret = ret ? ret : -EFAULT;

	kfree(rx_buf);
	kfree(tx_buf);
	return ret;
}

static int sof_cdev_ioc_ipc_large_cfg(struct sof_cdev_priv *priv, void __user *argp)
{
	struct sof_client_dev *cdev = priv->cdev;
	struct sof_cdev_large_cfg cfg;
	struct sof_ipc4_msg msg = {{ 0 }};
	void *kbuf = NULL;
	bool is_set;
	int ret;

	if (sof_client_get_ipc_type(cdev) != SOF_IPC_TYPE_4)
		return -EOPNOTSUPP;

	if (copy_from_user(&cfg, argp, sizeof(cfg)))
		return -EFAULT;

	if (!cfg.data_size || cfg.data_size > SOF_CDEV_MAX_LARGE_CFG_BYTES ||
	    !cfg.data_ptr)
		return -EINVAL;

	is_set = (cfg.set_get == SOF_CDEV_LARGE_CFG_SET);

	if (is_set) {
		kbuf = vmemdup_user(u64_to_user_ptr(cfg.data_ptr), cfg.data_size);
		if (IS_ERR(kbuf))
			return PTR_ERR(kbuf);
	} else {
		kbuf = kvzalloc(cfg.data_size, GFP_KERNEL);
		if (!kbuf)
			return -ENOMEM;
	}

	msg.primary = SOF_IPC4_MSG_TARGET(SOF_IPC4_MODULE_MSG) |
		      SOF_IPC4_MSG_DIR(SOF_IPC4_MSG_REQUEST) |
		      SOF_IPC4_MOD_ID(cfg.module_id) |
		      SOF_IPC4_MOD_INSTANCE(cfg.instance_id);
	msg.extension = SOF_IPC4_MOD_EXT_MSG_PARAM_ID(cfg.param_id);
	msg.data_size = cfg.data_size;
	msg.data_ptr = kbuf;

	mutex_lock(&priv->io_lock);

	ret = sof_cdev_wake_dsp(cdev);
	if (!ret) {
		ret = sof_client_ipc_set_get_data(cdev, &msg, is_set);
		sof_cdev_idle_dsp(cdev);
	}

	mutex_unlock(&priv->io_lock);

	if (!ret) {
		cfg.data_size = (u32)msg.data_size;
		if (!is_set && cfg.data_size > 0) {
			if (copy_to_user(u64_to_user_ptr(cfg.data_ptr), kbuf,
					 cfg.data_size))
				ret = -EFAULT;
		}
		if (!ret && copy_to_user(argp, &cfg, sizeof(cfg)))
			ret = -EFAULT;
	}

	kvfree(kbuf);
	return ret;
}

static int sof_cdev_ioc_win_info(struct sof_cdev_priv *priv, void __user *argp)
{
	struct snd_sof_dev *sdev = sof_client_dev_to_sof_dev(priv->cdev);
	struct sof_cdev_win_info *winfo;
	int ret = 0;

	winfo = kzalloc_obj(*winfo);
	if (!winfo)
		return -ENOMEM;

	sof_cdev_fill_windows_and_slots(sdev, winfo->windows, winfo->debug_slots);

	if (copy_to_user(argp, winfo, sizeof(*winfo)))
		ret = -EFAULT;

	kfree(winfo);
	return ret;
}

static int sof_cdev_ioc_win_io(struct sof_cdev_priv *priv, void __user *argp,
			       bool is_write)
{
	struct sof_client_dev *cdev = priv->cdev;
	struct snd_sof_dev *sdev = sof_client_dev_to_sof_dev(cdev);
	struct sof_cdev_win_io wio;
	u32 base_off = 0, max_size = 0;
	void *kbuf = NULL;
	bool woke_dsp = false;
	bool got_active_ref = false;
	int ret = 0;

	if (copy_from_user(&wio, argp, sizeof(wio)))
		return -EFAULT;

	if (!wio.size || !wio.data_ptr)
		return -EINVAL;

	mutex_lock(&priv->io_lock);

	if (wio.flags & SOF_CDEV_WIN_FL_WAKE_DSP) {
		ret = sof_cdev_wake_dsp(cdev);
		if (ret < 0)
			goto out_unlock;
		woke_dsp = true;
	} else {
		if (pm_runtime_get_if_active(sdev->dev) <= 0) {
			ret = -EBUSY;
			goto out_unlock;
		}
		got_active_ref = true;
		if (!sof_cdev_is_dsp_d0i0(sdev)) {
			ret = -EBUSY;
			goto out_unlock;
		}
	}

	if (wio.win_id == SOF_CDEV_WIN_BY_SLOT_TYPE) {
		ret = sof_cdev_resolve_slot(sdev, wio.slot_type, wio.slot_index,
					    &base_off, &max_size);
		if (ret < 0)
			goto out_unlock;
	} else if (wio.win_id < SOF_CDEV_MAX_WINDOWS) {
		int off = snd_sof_dsp_get_window_offset(sdev, wio.win_id);

		if (off < 0) {
			ret = -EOPNOTSUPP;
			goto out_unlock;
		}
		base_off = (u32)off;
		max_size = sof_cdev_get_window_size(sdev, wio.win_id);
	} else {
		ret = -EINVAL;
		goto out_unlock;
	}

	if (wio.offset >= max_size || wio.size > max_size - wio.offset) {
		ret = -EINVAL;
		goto out_unlock;
	}

	if (is_write) {
		kbuf = memdup_user(u64_to_user_ptr(wio.data_ptr), wio.size);
		if (IS_ERR(kbuf)) {
			ret = PTR_ERR(kbuf);
			kbuf = NULL;
			goto out_unlock;
		}
		sof_mailbox_write(sdev, base_off + wio.offset, kbuf, wio.size);
	} else {
		kbuf = kzalloc(wio.size, GFP_KERNEL);
		if (!kbuf) {
			ret = -ENOMEM;
			goto out_unlock;
		}
		sof_mailbox_read(sdev, base_off + wio.offset, kbuf, wio.size);
		if (copy_to_user(u64_to_user_ptr(wio.data_ptr), kbuf, wio.size))
			ret = -EFAULT;
	}

out_unlock:
	if (got_active_ref)
		pm_runtime_put_noidle(sdev->dev);
	if (woke_dsp)
		sof_cdev_idle_dsp(cdev);
	mutex_unlock(&priv->io_lock);
	kfree(kbuf);
	return ret;
}

static int sof_cdev_ioc_winstream_io(struct sof_cdev_priv *priv, void __user *argp)
{
	struct sof_client_dev *cdev = priv->cdev;
	struct snd_sof_dev *sdev = sof_client_dev_to_sof_dev(cdev);
	struct sof_cdev_winstream_io ws_io;
	u32 slot_type, rx_win_size, tx_win_size;
	u32 slot_off = 0, slot_sz = 0;
	u8 *slot_buf = NULL;
	u8 *user_tx = NULL;
	u8 *user_rx = NULL;
	bool woke_dsp = false;
	int ret = 0;

	if (copy_from_user(&ws_io, argp, sizeof(ws_io)))
		return -EFAULT;

	slot_type = ws_io.slot_type ? ws_io.slot_type : SOF_CDEV_SLOT_SHELL;
	rx_win_size = ws_io.rx_win_size ? ws_io.rx_win_size : 256;

	if (rx_win_size < 64 || rx_win_size > 2048)
		return -EINVAL;

	if (ws_io.tx_len > 0) {
		if (!ws_io.tx_ptr || ws_io.tx_len > rx_win_size)
			return -EINVAL;
		user_tx = memdup_user(u64_to_user_ptr(ws_io.tx_ptr), ws_io.tx_len);
		if (IS_ERR(user_tx))
			return PTR_ERR(user_tx);
	}

	if (ws_io.rx_buf_len > 0) {
		if (!ws_io.rx_ptr || ws_io.rx_buf_len > 16384) {
			kfree(user_tx);
			return -EINVAL;
		}
		user_rx = kzalloc(ws_io.rx_buf_len, GFP_KERNEL);
		if (!user_rx) {
			kfree(user_tx);
			return -ENOMEM;
		}
	}

	slot_buf = kzalloc(SOF_IPC4_DEBUG_SLOT_SIZE, GFP_KERNEL);
	if (!slot_buf) {
		kfree(user_rx);
		kfree(user_tx);
		return -ENOMEM;
	}

	mutex_lock(&priv->io_lock);

	ret = sof_cdev_wake_dsp(cdev);
	if (ret < 0)
		goto out_unlock;
	woke_dsp = true;

	ret = sof_cdev_resolve_slot(sdev, slot_type, 0, &slot_off, &slot_sz);
	if (ret < 0)
		goto out_unlock;

	if (slot_sz <= rx_win_size + SOF_WINSTREAM_HDR_SIZE) {
		ret = -EINVAL;
		goto out_unlock;
	}
	tx_win_size = slot_sz - rx_win_size;

	if (user_tx && ws_io.tx_len > 0) {
		sof_mailbox_read(sdev, slot_off, slot_buf, rx_win_size);
		sof_ws_write((struct sof_winstream_hdr *)slot_buf, rx_win_size,
			     user_tx, ws_io.tx_len);
		sof_mailbox_write(sdev, slot_off, slot_buf, rx_win_size);
	}

	ws_io.rx_bytes_read = 0;
	if (user_rx && ws_io.rx_buf_len > 0) {
		sof_mailbox_read(sdev, slot_off + rx_win_size, slot_buf, tx_win_size);
		ws_io.rx_bytes_read = sof_ws_read((struct sof_winstream_hdr *)slot_buf,
						  tx_win_size, &ws_io.seq_in_out,
						  user_rx, ws_io.rx_buf_len);
	}

out_unlock:
	if (woke_dsp)
		sof_cdev_idle_dsp(cdev);
	mutex_unlock(&priv->io_lock);

	if (!ret && ws_io.rx_bytes_read > 0) {
		if (copy_to_user(u64_to_user_ptr(ws_io.rx_ptr), user_rx,
				 ws_io.rx_bytes_read))
			ret = -EFAULT;
	}

	if (!ret && copy_to_user(argp, &ws_io, sizeof(ws_io)))
		ret = -EFAULT;

	kfree(slot_buf);
	kfree(user_rx);
	kfree(user_tx);
	return ret;
}

static int sof_cdev_ioc_llext_load(struct sof_cdev_priv *priv, void __user *argp)
{
	struct sof_client_dev *cdev = priv->cdev;
	struct snd_sof_dev *sdev = sof_client_dev_to_sof_dev(cdev);
	struct sof_ipc4_fw_data *ipc4_data = sdev->private;
	struct sof_cdev_llext_load req;
	struct sof_shell_llext_slot slot;
	u32 slot_off = 0, slot_sz = 0;
	u32 lib_id;
	void *buf;
	bool woke_dsp = false;
	int ret;

	if (sof_client_get_ipc_type(cdev) != SOF_IPC_TYPE_4 || !ipc4_data)
		return -EOPNOTSUPP;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	if (!req.data_size || req.data_size > SOF_CDEV_MAX_LLEXT_BYTES ||
	    !req.data_ptr)
		return -EINVAL;

	mutex_lock(&priv->io_lock);

	ret = sof_cdev_wake_dsp(cdev);
	if (ret < 0)
		goto out_unlock;
	woke_dsp = true;

	if (req.mode == SOF_LLEXT_MODE_SHELL_SHM) {
		ret = sof_cdev_resolve_slot(sdev, SOF_CDEV_SLOT_LLEXT_LOAD, 0,
					    &slot_off, &slot_sz);
		if (ret < 0)
			goto out_unlock;

		sof_mailbox_read(sdev, slot_off, &slot, sizeof(slot));
		if (slot.magic != SOF_SHELL_LLEXT_MAGIC) {
			ret = -EPROTO;
			goto out_unlock;
		}
		if (slot.state != SOF_SHELL_LLEXT_REQUESTING) {
			req.fw_state = slot.state;
			ret = -EBUSY;
			goto out_unlock;
		}

		lib_id = slot.lib_id;
		if (!lib_id || lib_id > 15) {
			ret = -EINVAL;
			goto out_shm_err;
		}

		{
			u32 active = SOF_SHELL_LLEXT_DMA_ACTIVE;

			sof_mailbox_write(sdev,
					  slot_off + offsetof(struct sof_shell_llext_slot, state),
					  &active, sizeof(active));
		}

		buf = devm_kmalloc(sdev->dev, req.data_size, GFP_KERNEL);
		if (!buf) {
			ret = -ENOMEM;
			goto out_shm_err;
		}
		if (copy_from_user(buf, u64_to_user_ptr(req.data_ptr), req.data_size)) {
			devm_kfree(sdev->dev, buf);
			ret = -EFAULT;
			goto out_shm_err;
		}

		ret = sof_client_ipc4_load_library_buf(cdev, lib_id, buf, req.data_size);
		if (ret < 0) {
			devm_kfree(sdev->dev, buf);
			goto out_shm_err;
		}

		{
			u32 xfer = req.data_size;
			u32 done = SOF_SHELL_LLEXT_DMA_DONE;

			sof_mailbox_write(sdev,
					  slot_off + offsetof(struct sof_shell_llext_slot, xfer_bytes),
					  &xfer, sizeof(xfer));
			sof_mailbox_write(sdev,
					  slot_off + offsetof(struct sof_shell_llext_slot, state),
					  &done, sizeof(done));
		}

		req.lib_id = lib_id;
		req.fw_state = SOF_SHELL_LLEXT_DMA_DONE;
		goto out_copy;

out_shm_err:
		{
			u32 err_state = SOF_SHELL_LLEXT_ERROR;
			s32 result = (s32)ret;

			sof_mailbox_write(sdev,
					  slot_off + offsetof(struct sof_shell_llext_slot, result),
					  &result, sizeof(result));
			sof_mailbox_write(sdev,
					  slot_off + offsetof(struct sof_shell_llext_slot, state),
					  &err_state, sizeof(err_state));
			req.fw_state = SOF_SHELL_LLEXT_ERROR;
		}
		goto out_unlock;
	} else if (req.mode == SOF_LLEXT_MODE_IPC4_DIRECT) {
		lib_id = req.lib_id;
		if (!lib_id) {
			/* Find first free library slot >= 1 */
			u32 idx;

			for (idx = 1; idx < ipc4_data->max_libs_count; idx++) {
				if (!xa_load(&ipc4_data->fw_lib_xa, idx)) {
					lib_id = idx;
					break;
				}
			}
			if (!lib_id)
				lib_id = 1;
		}

		buf = devm_kmalloc(sdev->dev, req.data_size, GFP_KERNEL);
		if (!buf) {
			ret = -ENOMEM;
			goto out_unlock;
		}
		if (copy_from_user(buf, u64_to_user_ptr(req.data_ptr), req.data_size)) {
			devm_kfree(sdev->dev, buf);
			ret = -EFAULT;
			goto out_unlock;
		}

		ret = sof_client_ipc4_load_library_buf(cdev, lib_id, buf, req.data_size);
		if (ret < 0)
			devm_kfree(sdev->dev, buf);

		req.lib_id = lib_id;
		req.fw_state = ret;
	} else {
		ret = -EINVAL;
		goto out_unlock;
	}

out_copy:
	if (copy_to_user(argp, &req, sizeof(req)))
		ret = ret ? ret : -EFAULT;

out_unlock:
	if (woke_dsp)
		sof_cdev_idle_dsp(cdev);
	mutex_unlock(&priv->io_lock);
	return ret;
}

static struct sof_man4_module *sof_cdev_get_probe_module(struct sof_cdev_priv *priv)
{
	static const guid_t probe_uuid =
		GUID_INIT(0x7CAD0808, 0xAB10, 0xCD23,
			  0xEF, 0x45, 0x12, 0xAB, 0x34, 0xCD, 0x56, 0xEF);

	if (!priv->probe_mentry) {
		struct sof_ipc4_fw_module *fw_module =
			sof_client_ipc4_find_module(priv->cdev, &probe_uuid);

		if (!fw_module)
			return NULL;
		priv->probe_mentry = &fw_module->man4_module_entry;
	}

	return priv->probe_mentry;
}

static int sof_cdev_ioc_probe_ctl(struct sof_cdev_priv *priv, void __user *argp)
{
	struct sof_client_dev *cdev = priv->cdev;
	struct sof_cdev_probe_ctl pctl;
	struct sof_man4_module *mentry;
	struct sof_ipc4_msg msg = {{ 0 }};
	bool woke_dsp = false;
	int i, ret = 0;

	if (sof_client_get_ipc_type(cdev) != SOF_IPC_TYPE_4)
		return -EOPNOTSUPP;

	if (copy_from_user(&pctl, argp, sizeof(pctl)))
		return -EFAULT;

	mutex_lock(&priv->io_lock);

	ret = sof_cdev_wake_dsp(cdev);
	if (ret < 0)
		goto out_unlock;
	woke_dsp = true;

	mentry = sof_cdev_get_probe_module(priv);
	if (!mentry) {
		ret = -ENODEV;
		goto out_unlock;
	}

	switch (pctl.op) {
	case SOF_PROBE_OP_INIT: {
		struct sof_ipc4_probe_cfg cfg = {{ 0 }};

		if (!pctl.stream_tag) {
			ret = -EINVAL;
			break;
		}
		cfg.gtw_cfg.node_id = SOF_IPC4_PROBE_NODE_ID_INDEX(pctl.stream_tag - 1) |
				      SOF_IPC4_PROBE_NODE_ID_TYPE(SOF_IPC4_DMA_HDA_HOST_INPUT);
		cfg.gtw_cfg.dma_buffer_size = pctl.dma_buffer_size;

		msg.primary = mentry->id |
			      SOF_IPC4_MSG_TYPE_SET(SOF_IPC4_MOD_INIT_INSTANCE) |
			      SOF_IPC4_MSG_DIR(SOF_IPC4_MSG_REQUEST) |
			      SOF_IPC4_MSG_TARGET(SOF_IPC4_MODULE_MSG);
		msg.extension = SOF_IPC4_MOD_EXT_DST_MOD_INSTANCE(SOF_PROBE_INVALID_PIPELINE_ID) |
				SOF_IPC4_MOD_EXT_CORE_ID(0) |
				SOF_IPC4_MOD_EXT_PARAM_SIZE(sizeof(cfg) / sizeof(u32));
		msg.data_size = sizeof(cfg);
		msg.data_ptr = &cfg;

		ret = sof_client_ipc_tx_message_no_reply(cdev, &msg);
		break;
	}
	case SOF_PROBE_OP_DEINIT:
		msg.primary = mentry->id |
			      SOF_IPC4_MSG_TYPE_SET(SOF_IPC4_MOD_DELETE_INSTANCE) |
			      SOF_IPC4_MSG_DIR(SOF_IPC4_MSG_REQUEST) |
			      SOF_IPC4_MSG_TARGET(SOF_IPC4_MODULE_MSG);
		msg.extension = SOF_IPC4_MOD_EXT_DST_MOD_INSTANCE(SOF_PROBE_INVALID_PIPELINE_ID) |
				SOF_IPC4_MOD_EXT_CORE_ID(0);
		ret = sof_client_ipc_tx_message_no_reply(cdev, &msg);
		break;

	case SOF_PROBE_OP_INFO_ACTIVE:
	case SOF_PROBE_OP_INFO_AVAILABLE: {
		u32 param_id = (pctl.op == SOF_PROBE_OP_INFO_ACTIVE) ?
			       SOF_IPC4_PROBE_POINTS : SOF_IPC4_PROBE_POINTS_AVAILABLE;
		struct sof_ipc4_probe_info *info;
		struct sof_cdev_probe_point *upoints = NULL;
		u32 count, to_copy;

		msg.primary = mentry->id |
			      SOF_IPC4_MSG_DIR(SOF_IPC4_MSG_REQUEST) |
			      SOF_IPC4_MSG_TARGET(SOF_IPC4_MODULE_MSG);
		msg.extension = SOF_IPC4_MOD_EXT_MSG_PARAM_ID(param_id);
		msg.data_size = sof_client_get_ipc_max_payload_size(cdev);
		msg.data_ptr = kzalloc(msg.data_size, GFP_KERNEL);
		if (!msg.data_ptr) {
			ret = -ENOMEM;
			break;
		}

		ret = sof_client_ipc_set_get_data(cdev, &msg, false);
		if (ret < 0) {
			kfree(msg.data_ptr);
			break;
		}

		info = msg.data_ptr;
		if (msg.data_size < sizeof(*info) ||
		    info->num_elems > (msg.data_size - sizeof(*info)) /
				      sizeof(info->points[0])) {
			kfree(msg.data_ptr);
			ret = -EINVAL;
			break;
		}

		count = info->num_elems;
		to_copy = min(count, pctl.num_points);
		pctl.num_points = count;

		if (to_copy > 0 && pctl.points_ptr) {
			upoints = kcalloc(to_copy, sizeof(*upoints), GFP_KERNEL);
			if (!upoints) {
				kfree(msg.data_ptr);
				ret = -ENOMEM;
				break;
			}
			for (i = 0; i < to_copy; i++) {
				u32 bid = info->points[i].point_id;
				struct snd_sof_widget *swidget;

				upoints[i].buffer_id = bid;
				upoints[i].purpose = info->points[i].purpose;
				upoints[i].stream_tag = info->points[i].stream_tag;

				swidget = sof_client_ipc4_find_swidget_by_id(
					cdev,
					SOF_IPC4_MOD_ID_GET(bid),
					SOF_IPC4_MOD_INSTANCE_GET(bid));
				if (swidget && swidget->widget && swidget->widget->name)
					strscpy(upoints[i].widget_name,
						swidget->widget->name,
						sizeof(upoints[i].widget_name));
				else
					strscpy(upoints[i].widget_name, "<unknown>",
						sizeof(upoints[i].widget_name));
			}

			if (copy_to_user(u64_to_user_ptr(pctl.points_ptr),
					 upoints, to_copy * sizeof(*upoints)))
				ret = -EFAULT;
			kfree(upoints);
		}
		kfree(msg.data_ptr);
		break;
	}
	case SOF_PROBE_OP_POINTS_ADD: {
		struct sof_cdev_probe_point *upoints;
		struct sof_ipc4_probe_point *kpoints;

		if (!pctl.num_points || pctl.num_points > 64 || !pctl.points_ptr) {
			ret = -EINVAL;
			break;
		}
		upoints = memdup_user(u64_to_user_ptr(pctl.points_ptr),
				      pctl.num_points * sizeof(*upoints));
		if (IS_ERR(upoints)) {
			ret = PTR_ERR(upoints);
			break;
		}
		kpoints = kcalloc(pctl.num_points, sizeof(*kpoints), GFP_KERNEL);
		if (!kpoints) {
			kfree(upoints);
			ret = -ENOMEM;
			break;
		}
		for (i = 0; i < pctl.num_points; i++) {
			kpoints[i].point_id = upoints[i].buffer_id;
			kpoints[i].purpose = upoints[i].purpose;
			kpoints[i].stream_tag = upoints[i].stream_tag;
		}

		msg.primary = mentry->id |
			      SOF_IPC4_MSG_DIR(SOF_IPC4_MSG_REQUEST) |
			      SOF_IPC4_MSG_TARGET(SOF_IPC4_MODULE_MSG);
		msg.extension = SOF_IPC4_MOD_EXT_MSG_PARAM_ID(SOF_IPC4_PROBE_POINTS);
		msg.data_size = pctl.num_points * sizeof(*kpoints);
		msg.data_ptr = kpoints;

		ret = sof_client_ipc_set_get_data(cdev, &msg, true);
		kfree(kpoints);
		kfree(upoints);
		break;
	}
	case SOF_PROBE_OP_POINTS_REMOVE: {
		struct sof_cdev_probe_point *upoints;
		u32 *ids;

		if (!pctl.num_points || pctl.num_points > 64 || !pctl.points_ptr) {
			ret = -EINVAL;
			break;
		}
		upoints = memdup_user(u64_to_user_ptr(pctl.points_ptr),
				      pctl.num_points * sizeof(*upoints));
		if (IS_ERR(upoints)) {
			ret = PTR_ERR(upoints);
			break;
		}
		ids = kcalloc(pctl.num_points, sizeof(*ids), GFP_KERNEL);
		if (!ids) {
			kfree(upoints);
			ret = -ENOMEM;
			break;
		}
		for (i = 0; i < pctl.num_points; i++)
			ids[i] = upoints[i].buffer_id;

		msg.primary = mentry->id |
			      SOF_IPC4_MSG_DIR(SOF_IPC4_MSG_REQUEST) |
			      SOF_IPC4_MSG_TARGET(SOF_IPC4_MODULE_MSG);
		msg.extension = SOF_IPC4_MOD_EXT_MSG_PARAM_ID(SOF_IPC4_PROBE_POINTS_DISCONNECT);
		msg.data_size = pctl.num_points * sizeof(*ids);
		msg.data_ptr = ids;

		ret = sof_client_ipc_set_get_data(cdev, &msg, true);
		kfree(ids);
		kfree(upoints);
		break;
	}
	default:
		ret = -EINVAL;
		break;
	}

out_unlock:
	if (woke_dsp)
		sof_cdev_idle_dsp(cdev);
	mutex_unlock(&priv->io_lock);

	if (!ret && copy_to_user(argp, &pctl, sizeof(pctl)))
		ret = -EFAULT;

	return ret;
}

/* -------------------------------------------------------------------------
 * Character Device File Operations
 * -------------------------------------------------------------------------
 */

static int sof_cdev_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct sof_cdev_priv *priv = container_of(misc, struct sof_cdev_priv, miscdev);
	struct sof_cdev_fh *fh;

	fh = kzalloc_obj(*fh);
	if (!fh)
		return -ENOMEM;

	fh->priv = priv;
	init_waitqueue_head(&fh->wait);
	spin_lock_init(&fh->fifo_lock);
	INIT_KFIFO(fh->events);

	mutex_lock(&priv->fh_lock);
	list_add(&fh->list, &priv->fh_list);
	mutex_unlock(&priv->fh_lock);

	file->private_data = fh;
	return 0;
}

static int sof_cdev_release(struct inode *inode, struct file *file)
{
	struct sof_cdev_fh *fh = file->private_data;
	struct sof_cdev_priv *priv = fh->priv;

	mutex_lock(&priv->fh_lock);
	list_del(&fh->list);
	mutex_unlock(&priv->fh_lock);

	mutex_lock(&priv->io_lock);
	if (fh->holding_d0) {
		sof_cdev_idle_dsp(priv->cdev);
		sof_client_core_module_put(priv->cdev);
		fh->holding_d0 = false;
	}
	mutex_unlock(&priv->io_lock);

	kfree(fh);
	return 0;
}

static long sof_cdev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct sof_cdev_fh *fh = file->private_data;
	struct sof_cdev_priv *priv = fh->priv;
	void __user *argp = (void __user *)arg;

	switch (cmd) {
	case SOF_IOC_GET_INFO:
		return sof_cdev_ioc_get_info(priv, argp);
	case SOF_IOC_PM_CTL:
		return sof_cdev_ioc_pm_ctl(fh, argp);
	case SOF_IOC_IPC_TX:
		return sof_cdev_ioc_ipc_tx(priv, argp);
	case SOF_IOC_IPC_LARGE_CFG:
		return sof_cdev_ioc_ipc_large_cfg(priv, argp);
	case SOF_IOC_WIN_INFO:
		return sof_cdev_ioc_win_info(priv, argp);
	case SOF_IOC_WIN_READ:
		return sof_cdev_ioc_win_io(priv, argp, false);
	case SOF_IOC_WIN_WRITE:
		return sof_cdev_ioc_win_io(priv, argp, true);
	case SOF_IOC_WINSTREAM_IO:
		return sof_cdev_ioc_winstream_io(priv, argp);
	case SOF_IOC_LLEXT_LOAD:
		return sof_cdev_ioc_llext_load(priv, argp);
	case SOF_IOC_PROBE_CTL:
		return sof_cdev_ioc_probe_ctl(priv, argp);
	default:
		return -ENOTTY;
	}
}

static int sof_cdev_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct sof_cdev_fh *fh = file->private_data;
	struct sof_cdev_priv *priv = fh->priv;
	struct snd_sof_dev *sdev = sof_client_dev_to_sof_dev(priv->cdev);
	unsigned long size = vma->vm_end - vma->vm_start;
	u32 win_id = (vma->vm_pgoff >> (20 - PAGE_SHIFT)) & 0x7;
	u32 sub_pgoff = vma->vm_pgoff & ((1UL << (20 - PAGE_SHIFT)) - 1);
	u32 max_size;
	int win_off;
	phys_addr_t bar_phys, win_phys;

	if (!dev_is_pci(sdev->dev))
		return -EOPNOTSUPP;

	if (win_id >= SOF_CDEV_MAX_WINDOWS)
		return -EINVAL;

	win_off = snd_sof_dsp_get_window_offset(sdev, win_id);
	if (win_off < 0)
		return -EOPNOTSUPP;

	max_size = sof_cdev_get_window_size(sdev, win_id);

	if ((sub_pgoff << PAGE_SHIFT) >= max_size ||
	    size > max_size - (sub_pgoff << PAGE_SHIFT))
		return -EINVAL;

	bar_phys = pci_resource_start(to_pci_dev(sdev->dev), sdev->mailbox_bar);
	if (!bar_phys)
		return -ENXIO;

	win_phys = bar_phys + (u32)win_off + (sub_pgoff << PAGE_SHIFT);
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

	return io_remap_pfn_range(vma, vma->vm_start, win_phys >> PAGE_SHIFT,
				  size, vma->vm_page_prot);
}

static __poll_t sof_cdev_poll(struct file *file, poll_table *wait)
{
	struct sof_cdev_fh *fh = file->private_data;
	__poll_t mask = 0;

	poll_wait(file, &fh->wait, wait);

	if (!kfifo_is_empty(&fh->events))
		mask |= EPOLLIN | EPOLLRDNORM;

	return mask;
}

static ssize_t sof_cdev_read(struct file *file, char __user *buf, size_t count,
			     loff_t *ppos)
{
	struct sof_cdev_fh *fh = file->private_data;
	struct sof_cdev_event evt;
	size_t copied = 0;
	int ret;

	if (count < sizeof(evt))
		return -EINVAL;

	if (kfifo_is_empty(&fh->events)) {
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		ret = wait_event_interruptible(fh->wait,
					       !kfifo_is_empty(&fh->events));
		if (ret)
			return ret;
	}

	while (copied + sizeof(evt) <= count &&
	       kfifo_out_spinlocked(&fh->events, &evt, 1, &fh->fifo_lock)) {
		if (copy_to_user(buf + copied, &evt, sizeof(evt)))
			return copied ? (ssize_t)copied : -EFAULT;
		copied += sizeof(evt);
	}

	return copied;
}

static const struct file_operations sof_cdev_fops = {
	.owner		= THIS_MODULE,
	.open		= sof_cdev_open,
	.release	= sof_cdev_release,
	.unlocked_ioctl	= sof_cdev_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
	.mmap		= sof_cdev_mmap,
	.poll		= sof_cdev_poll,
	.read		= sof_cdev_read,
	.llseek		= noop_llseek,
};

/* -------------------------------------------------------------------------
 * Async FW State & Notification Callbacks
 * -------------------------------------------------------------------------
 */

static void sof_cdev_broadcast_event(struct sof_cdev_priv *priv,
				     const struct sof_cdev_event *evt)
{
	struct sof_cdev_fh *fh;

	mutex_lock(&priv->fh_lock);
	list_for_each_entry(fh, &priv->fh_list, list) {
		unsigned long flags;

		spin_lock_irqsave(&fh->fifo_lock, flags);
		if (kfifo_is_full(&fh->events)) {
			struct sof_cdev_event dummy;
			int __maybe_unused n = kfifo_out(&fh->events, &dummy, 1);
		}
		kfifo_in(&fh->events, evt, 1);
		spin_unlock_irqrestore(&fh->fifo_lock, flags);

		wake_up_interruptible(&fh->wait);
	}
	mutex_unlock(&priv->fh_lock);
}

static void sof_cdev_fw_state_cb(struct sof_client_dev *cdev,
				 enum sof_fw_state state)
{
	struct sof_cdev_priv *priv = cdev->data;
	struct sof_cdev_event evt = {
		.timestamp_ns = ktime_get_ns(),
		.event_type   = SOF_CDEV_EVT_FW_STATE,
		.fw_state     = state,
	};

	if (state == SOF_FW_CRASHED || state == SOF_FW_BOOT_NOT_STARTED)
		priv->probe_mentry = NULL;

	sof_cdev_broadcast_event(priv, &evt);
}

static void sof_cdev_ipc_notif_cb(struct sof_client_dev *cdev, void *msg_buf)
{
	struct sof_cdev_priv *priv = cdev->data;
	struct sof_ipc4_msg *msg = msg_buf;
	struct sof_cdev_event evt = {
		.timestamp_ns = ktime_get_ns(),
		.event_type   = SOF_CDEV_EVT_IPC_NOTIFICATION,
		.fw_state     = sof_client_get_fw_state(cdev),
		.primary      = msg ? msg->primary : 0,
		.extension    = msg ? msg->extension : 0,
	};

	sof_cdev_broadcast_event(priv, &evt);
}

/* -------------------------------------------------------------------------
 * Auxiliary Driver Probe / Remove
 * -------------------------------------------------------------------------
 */

static int sof_cdev_probe(struct auxiliary_device *auxdev,
			  const struct auxiliary_device_id *id)
{
	struct sof_client_dev *cdev = auxiliary_dev_to_sof_client_dev(auxdev);
	struct sof_cdev_priv *priv;
	int ret;

	priv = devm_kzalloc(&auxdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->cdev = cdev;
	mutex_init(&priv->io_lock);
	mutex_init(&priv->fh_lock);
	INIT_LIST_HEAD(&priv->fh_list);
	cdev->data = priv;

	snprintf(priv->dev_name, sizeof(priv->dev_name), "sof-dsp%d", auxdev->id);
	priv->miscdev.minor = MISC_DYNAMIC_MINOR;
	priv->miscdev.name = priv->dev_name;
	priv->miscdev.fops = &sof_cdev_fops;
	priv->miscdev.parent = &auxdev->dev;
	priv->miscdev.mode = 0666;

	ret = misc_register(&priv->miscdev);
	if (ret < 0)
		return ret;

	sof_client_register_fw_state_handler(cdev, sof_cdev_fw_state_cb);

	if (sof_client_get_ipc_type(cdev) == SOF_IPC_TYPE_4) {
		sof_client_register_ipc_rx_handler(cdev,
			SOF_IPC4_NOTIFY_RESOURCE_EVENT, sof_cdev_ipc_notif_cb);
		sof_client_register_ipc_rx_handler(cdev,
			SOF_IPC4_NOTIFY_EXCEPTION_CAUGHT, sof_cdev_ipc_notif_cb);
		sof_client_register_ipc_rx_handler(cdev,
			SOF_IPC4_NOTIFY_PHRASE_DETECTED, sof_cdev_ipc_notif_cb);
	}

	pm_runtime_set_autosuspend_delay(&auxdev->dev, SOF_CDEV_SUSPEND_DELAY_MS);
	pm_runtime_use_autosuspend(&auxdev->dev);
	pm_runtime_enable(&auxdev->dev);
	pm_runtime_mark_last_busy(&auxdev->dev);
	pm_runtime_idle(&auxdev->dev);

	dev_info(&auxdev->dev, "Registered SOF character device /dev/%s\n",
		 priv->dev_name);
	return 0;
}

static void sof_cdev_remove(struct auxiliary_device *auxdev)
{
	struct sof_client_dev *cdev = auxiliary_dev_to_sof_client_dev(auxdev);
	struct sof_cdev_priv *priv = cdev->data;

	pm_runtime_disable(&auxdev->dev);

	if (sof_client_get_ipc_type(cdev) == SOF_IPC_TYPE_4) {
		sof_client_unregister_ipc_rx_handler(cdev,
			SOF_IPC4_NOTIFY_PHRASE_DETECTED);
		sof_client_unregister_ipc_rx_handler(cdev,
			SOF_IPC4_NOTIFY_EXCEPTION_CAUGHT);
		sof_client_unregister_ipc_rx_handler(cdev,
			SOF_IPC4_NOTIFY_RESOURCE_EVENT);
	}

	sof_client_unregister_fw_state_handler(cdev);
	misc_deregister(&priv->miscdev);
}

static const struct auxiliary_device_id sof_cdev_id_table[] = {
	{ .name = "snd_sof.cdev" },
	{ },
};
MODULE_DEVICE_TABLE(auxiliary, sof_cdev_id_table);

static struct auxiliary_driver sof_cdev_drv = {
	.name		= "snd_sof_client_cdev",
	.probe		= sof_cdev_probe,
	.remove		= sof_cdev_remove,
	.id_table	= sof_cdev_id_table,
};

module_auxiliary_driver(sof_cdev_drv);

MODULE_DESCRIPTION("SOF Client Character Device Driver (/dev/sof-dspN)");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("SND_SOC_SOF_CLIENT");
MODULE_IMPORT_NS("SND_SOC_SOF");
