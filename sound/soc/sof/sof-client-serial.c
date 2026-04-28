/* SPDX-License-Identifier: GPL-2.0-only */

/*
 * SOF serial port client driver for Zephyr ADSP shell backend.
 *
 * The Zephyr backend (subsys/shell/backends/shell_adsp_memory_window.c)
 * uses ADSP debug window slot type ADSP_DW_SLOT_SHELL and places two
 * sys_winstream instances in that slot:
 *   - rx_window[256]  : host writes shell input to DSP
 *   - tx_window[3840] : DSP writes shell output for host
 *
 * This driver maps that protocol to a Linux tty (ttysof0).
 */

#include <linux/auxiliary_bus.h>
#include <linux/debugfs.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#include "sof-client.h"

#define SOF_SERIAL_DRV_NAME			"sof_serial"
#define SOF_SERIAL_TTY_MINORS			1

#define SOF_SERIAL_ADSP_DW_SLOT_SIZE		0x1000
#define SOF_SERIAL_ADSP_DW_SLOT_SHELL		0x73686c6c

#define SOF_SERIAL_RX_WINDOW_SIZE		256
#define SOF_SERIAL_TX_WINDOW_SIZE		(SOF_SERIAL_ADSP_DW_SLOT_SIZE - \
						 SOF_SERIAL_RX_WINDOW_SIZE)

#define SOF_SERIAL_WINSTREAM_HDR_SIZE		16
#define SOF_SERIAL_WINSTREAM_DATA_MAX(win_size)	((win_size) - SOF_SERIAL_WINSTREAM_HDR_SIZE)

#define SOF_SERIAL_POLL_INTERVAL_MS		20
#define SOF_SERIAL_POLL_CHUNK			256

struct sof_serial_winstream {
	__le32 len;
	__le32 start;
	__le32 end;
	__le32 seq;
	u8 data[];
} __packed;

struct sof_serial_priv {
	struct sof_client_dev	*cdev;
	struct tty_driver	*tty_drv;
	struct tty_port		 port;
	enum sof_ipc_type	 ipc_type;
	ssize_t			 win_offset;
	struct dentry		*dfs_root;
	struct dentry		*dfs_memwin;
	struct workqueue_struct	*rx_wq;
	struct delayed_work	 rx_work;
	struct mutex		 io_lock;
	u32			 tx_seqno;
	u8			*rx_window;
	u8			*tx_window;
	u8			*poll_buf;
};

static u32 sof_serial_idx_mod(u32 idx, u32 len)
{
	return idx >= len ? idx - len : idx;
}

/* Computes modular a - b, assuming a and b are in [0, len). */
static u32 sof_serial_idx_sub(u32 a, u32 b, u32 len)
{
	return sof_serial_idx_mod(a + (len - b), len);
}

static int sof_serial_winstream_validate(struct sof_serial_winstream *ws,
					 size_t win_size, bool allow_init)
{
	u32 len = le32_to_cpu(ws->len);
	u32 expected = SOF_SERIAL_WINSTREAM_DATA_MAX(win_size);

	if (len == expected)
		return 0;

	if (!allow_init)
		return -EINVAL;

	ws->len = cpu_to_le32(expected);
	ws->start = 0;
	ws->end = 0;
	ws->seq = 0;

	return 0;
}

static void sof_serial_winstream_write(struct sof_serial_winstream *ws,
				       const u8 *data, u32 len0)
{
	u32 ws_len = le32_to_cpu(ws->len);
	u32 start = le32_to_cpu(ws->start);
	u32 end = le32_to_cpu(ws->end);
	u32 seq = le32_to_cpu(ws->seq);
	u32 len = len0;
	u32 suffix;

	if (!ws_len)
		return;

	if (len > ws_len - 1) {
		start = end;
		len = ws_len - 1;
	}

	len = min(len, ws_len);
	if (seq != 0) {
		u32 avail = (ws_len - 1) - sof_serial_idx_sub(end, start, ws_len);

		if (len > avail)
			start = sof_serial_idx_mod(start + (len - avail), ws_len);
	}

	if (len < len0) {
		start = end;
		data += len0 - len;
	}

	suffix = min(len, ws_len - end);
	memcpy(&ws->data[end], data, suffix);
	if (len > suffix)
		memcpy(&ws->data[0], data + suffix, len - suffix);

	end = sof_serial_idx_mod(end + len, ws_len);

	ws->start = cpu_to_le32(start);
	ws->end = cpu_to_le32(end);
	ws->seq = cpu_to_le32(seq + len0);
}

static u32 sof_serial_winstream_read(struct sof_serial_winstream *ws, u32 *seq,
				     u8 *buf, u32 buflen)
{
	u32 seq0 = *seq;
	u32 ws_start;
	u32 ws_end;
	u32 ws_seq;
	u32 ws_len;
	u32 len;
	u32 behind;
	u32 copy;
	u32 suffix;

	ws_start = le32_to_cpu(ws->start);
	ws_end = le32_to_cpu(ws->end);
	ws_seq = le32_to_cpu(ws->seq);
	ws_len = le32_to_cpu(ws->len);

	if (!ws_len || !buflen)
		return 0;

	if (*seq == ws_seq || ws_start == ws_end) {
		*seq = ws_seq;
		return 0;
	}

	behind = ws_seq - *seq;
	if (behind > sof_serial_idx_sub(ws_end, ws_start, ws_len)) {
		*seq = ws_seq;
		return 0;
	}

	copy = sof_serial_idx_sub(ws_end, behind, ws_len);
	len = min(buflen, behind);
	suffix = min(len, ws_len - copy);

	memcpy(buf, &ws->data[copy], suffix);
	if (len > suffix)
		memcpy(buf + suffix, &ws->data[0], len - suffix);

	*seq = seq0 + len;
	return len;
}

static int sof_serial_read_window(struct sof_serial_priv *priv, u32 rel_offset,
				  void *dst, size_t bytes)
{
	if (priv->win_offset < 0)
		return -ENODEV;

	sof_client_mailbox_read(priv->cdev, priv->win_offset + rel_offset, dst, bytes);
	return 0;
}

static int sof_serial_write_window(struct sof_serial_priv *priv, u32 rel_offset,
				   const void *src, size_t bytes)
{
	if (priv->win_offset < 0)
		return -ENODEV;

	sof_client_mailbox_write(priv->cdev, priv->win_offset + rel_offset,
				 (void *)src, bytes);
	return 0;
}

static void sof_serial_fw_state_cb(struct sof_client_dev *cdev,
				    enum sof_fw_state state)
{
	struct sof_serial_priv *priv = cdev->data;

	switch (state) {
	case SOF_FW_BOOT_COMPLETE:
		/*
		 * Firmware is up (first boot or after recovery). Kick the
		 * poller; it will reschedule itself until removed.
		 */
		queue_delayed_work(priv->rx_wq, &priv->rx_work,
				   msecs_to_jiffies(SOF_SERIAL_POLL_INTERVAL_MS));
		break;
	case SOF_FW_CRASHED:
		/* Stop polling; the next BOOT_COMPLETE will restart it. */
		cancel_delayed_work(&priv->rx_work);
		break;
	default:
		break;
	}
}

static void sof_serial_rx_workfn(struct work_struct *work)
{
	struct sof_serial_priv *priv =
		container_of(to_delayed_work(work), struct sof_serial_priv, rx_work);
	struct sof_serial_winstream *ws;
	u32 read_len = 0;

	/*
	 * Only poll when firmware has finished booting. If the DSP is still
	 * initialising, stop rescheduling — the fw_state notifier will
	 * restart the work once BOOT_COMPLETE is reached.
	 */
	if (sof_client_get_fw_state(priv->cdev) != SOF_FW_BOOT_COMPLETE)
		return;

	mutex_lock(&priv->io_lock);
	if (!sof_serial_read_window(priv, SOF_SERIAL_RX_WINDOW_SIZE,
				    priv->tx_window, SOF_SERIAL_TX_WINDOW_SIZE)) {
		ws = (struct sof_serial_winstream *)priv->tx_window;
		if (!sof_serial_winstream_validate(ws, SOF_SERIAL_TX_WINDOW_SIZE, false))
			read_len = sof_serial_winstream_read(ws, &priv->tx_seqno,
						      priv->poll_buf,
						      SOF_SERIAL_POLL_CHUNK);
	}
	mutex_unlock(&priv->io_lock);

	if (read_len) {
		tty_insert_flip_string(&priv->port, priv->poll_buf, read_len);
		tty_flip_buffer_push(&priv->port);
	}

	queue_delayed_work(priv->rx_wq, &priv->rx_work,
			   msecs_to_jiffies(SOF_SERIAL_POLL_INTERVAL_MS));
}

static int sof_serial_tty_install(struct tty_driver *driver,
				  struct tty_struct *tty)
{
	struct sof_serial_priv *priv = driver->driver_state;

	tty->driver_data = priv;
	return tty_port_install(&priv->port, driver, tty);
}

static int sof_serial_tty_open(struct tty_struct *tty, struct file *filp)
{
	struct sof_serial_priv *priv = tty->driver_data;
	struct sof_client_dev *cdev = priv->cdev;
	int ret;

	ret = sof_client_core_module_get(cdev);
	if (ret)
		return ret;

	ret = pm_runtime_get_sync(&cdev->auxdev.dev);
	if (ret < 0) {
		pm_runtime_put_noidle(&cdev->auxdev.dev);
		sof_client_core_module_put(cdev);
		return ret;
	}

	return tty_port_open(&priv->port, tty, filp);
}

static void sof_serial_tty_close(struct tty_struct *tty, struct file *filp)
{
	struct sof_serial_priv *priv = tty->driver_data;
	struct sof_client_dev *cdev = priv->cdev;

	tty_port_close(&priv->port, tty, filp);

	pm_runtime_put_autosuspend(&cdev->auxdev.dev);
	sof_client_core_module_put(cdev);
}

static ssize_t sof_serial_tty_write(struct tty_struct *tty, const u8 *buf,
				    size_t count)
{
	struct sof_serial_priv *priv = tty->driver_data;
	struct sof_serial_winstream *ws;

	if (sof_client_get_fw_state(priv->cdev) == SOF_FW_CRASHED)
		return -ENODEV;

	if (priv->ipc_type != SOF_IPC_TYPE_4)
		return -EOPNOTSUPP;

	mutex_lock(&priv->io_lock);

	if (sof_serial_read_window(priv, 0, priv->rx_window,
				   SOF_SERIAL_RX_WINDOW_SIZE)) {
		mutex_unlock(&priv->io_lock);
		return -EIO;
	}

	ws = (struct sof_serial_winstream *)priv->rx_window;
	if (sof_serial_winstream_validate(ws, SOF_SERIAL_RX_WINDOW_SIZE, true)) {
		mutex_unlock(&priv->io_lock);
		return -EIO;
	}

	sof_serial_winstream_write(ws, buf, count);

	if (sof_serial_write_window(priv, 0, priv->rx_window,
				    SOF_SERIAL_RX_WINDOW_SIZE)) {
		mutex_unlock(&priv->io_lock);
		return -EIO;
	}

	mutex_unlock(&priv->io_lock);
	return count;
}

static unsigned int sof_serial_tty_write_room(struct tty_struct *tty)
{
	return SOF_SERIAL_WINSTREAM_DATA_MAX(SOF_SERIAL_RX_WINDOW_SIZE);
}

static const struct tty_operations sof_serial_tty_ops = {
	.install	= sof_serial_tty_install,
	.open		= sof_serial_tty_open,
	.close		= sof_serial_tty_close,
	.write		= sof_serial_tty_write,
	.write_room	= sof_serial_tty_write_room,
};

static int sof_serial_port_activate(struct tty_port *port,
				    struct tty_struct *tty)
{
	return 0;
}

static void sof_serial_port_shutdown(struct tty_port *port)
{
}

static const struct tty_port_operations sof_serial_port_ops = {
	.activate	= sof_serial_port_activate,
	.shutdown	= sof_serial_port_shutdown,
};

static ssize_t sof_serial_dfs_memwin_read(struct file *file, char __user *ubuf,
					  size_t len, loff_t *ppos)
{
	struct sof_client_dev *cdev = file->private_data;
	struct sof_serial_priv *priv = cdev->data;
	u8 *tmp;

	if (priv->win_offset < 0)
		return -ENODEV;

	if (*ppos >= SOF_SERIAL_ADSP_DW_SLOT_SIZE)
		return 0;

	len = min_t(size_t, len, SOF_SERIAL_ADSP_DW_SLOT_SIZE - *ppos);
	tmp = kmalloc(len, GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;

	mutex_lock(&priv->io_lock);
	sof_client_mailbox_read(cdev, priv->win_offset + *ppos, tmp, len);
	mutex_unlock(&priv->io_lock);

	if (copy_to_user(ubuf, tmp, len)) {
		kfree(tmp);
		return -EFAULT;
	}

	kfree(tmp);
	*ppos += len;
	return len;
}

static ssize_t sof_serial_dfs_memwin_write(struct file *file,
					   const char __user *ubuf,
					   size_t len, loff_t *ppos)
{
	struct sof_client_dev *cdev = file->private_data;
	struct sof_serial_priv *priv = cdev->data;
	u8 *tmp;

	if (priv->win_offset < 0)
		return -ENODEV;

	if (*ppos >= SOF_SERIAL_ADSP_DW_SLOT_SIZE)
		return -ENOSPC;

	len = min_t(size_t, len, SOF_SERIAL_ADSP_DW_SLOT_SIZE - *ppos);
	tmp = memdup_user(ubuf, len);
	if (IS_ERR(tmp))
		return PTR_ERR(tmp);

	mutex_lock(&priv->io_lock);
	sof_client_mailbox_write(cdev, priv->win_offset + *ppos, tmp, len);
	mutex_unlock(&priv->io_lock);

	kfree(tmp);
	*ppos += len;
	return len;
}

static const struct file_operations sof_serial_dfs_memwin_fops = {
	.open	 = simple_open,
	.read	 = sof_serial_dfs_memwin_read,
	.write	 = sof_serial_dfs_memwin_write,
	.llseek	 = default_llseek,
};

static int sof_serial_probe(struct auxiliary_device *auxdev,
			    const struct auxiliary_device_id *id)
{
	struct sof_client_dev *cdev = auxiliary_dev_to_sof_client_dev(auxdev);
	struct device *dev = &auxdev->dev;
	struct sof_serial_priv *priv;
	struct tty_driver *tty_drv;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->rx_window = devm_kmalloc(dev, SOF_SERIAL_RX_WINDOW_SIZE, GFP_KERNEL);
	priv->tx_window = devm_kmalloc(dev, SOF_SERIAL_TX_WINDOW_SIZE, GFP_KERNEL);
	priv->poll_buf = devm_kmalloc(dev, SOF_SERIAL_POLL_CHUNK, GFP_KERNEL);
	if (!priv->rx_window || !priv->tx_window || !priv->poll_buf)
		return -ENOMEM;

	priv->cdev = cdev;
	priv->ipc_type = sof_client_get_ipc_type(cdev);
	mutex_init(&priv->io_lock);
	cdev->data = priv;

	if (priv->ipc_type != SOF_IPC_TYPE_4) {
		dev_err(dev, "Only IPC4 is supported\n");
		return -EOPNOTSUPP;
	}

	priv->win_offset = sof_client_ipc4_find_debug_slot_offset_by_type(cdev,
							   SOF_SERIAL_ADSP_DW_SLOT_SHELL);
	if (priv->win_offset < 0) {
		dev_err(dev, "No ADSP shell debug slot found\n");
		return -ENODEV;
	}

	tty_drv = tty_alloc_driver(SOF_SERIAL_TTY_MINORS,
				   TTY_DRIVER_REAL_RAW |
				   TTY_DRIVER_DYNAMIC_DEV);
	if (IS_ERR(tty_drv))
		return PTR_ERR(tty_drv);

	tty_drv->driver_name	= SOF_SERIAL_DRV_NAME;
	tty_drv->name		= "ttysof";
	tty_drv->major		= 0;
	tty_drv->minor_start	= 0;
	tty_drv->type		= TTY_DRIVER_TYPE_SERIAL;
	tty_drv->subtype	= SERIAL_TYPE_NORMAL;
	tty_drv->init_termios	= tty_std_termios;
	tty_drv->driver_state	= priv;
	tty_set_operations(tty_drv, &sof_serial_tty_ops);

	tty_port_init(&priv->port);
	priv->port.ops = &sof_serial_port_ops;

	ret = tty_register_driver(tty_drv);
	if (ret) {
		dev_err(dev, "Failed to register tty driver: %d\n", ret);
		goto err_tty_port;
	}

	tty_register_device(tty_drv, 0, dev);
	priv->tty_drv = tty_drv;

	priv->dfs_root = debugfs_create_dir(SOF_SERIAL_DRV_NAME,
					    sof_client_get_debugfs_root(cdev));
	if (!IS_ERR_OR_NULL(priv->dfs_root))
		priv->dfs_memwin =
			debugfs_create_file("memwin", 0600, priv->dfs_root,
					    cdev, &sof_serial_dfs_memwin_fops);

	pm_runtime_set_autosuspend_delay(dev, 3000);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_enable(dev);
	pm_runtime_mark_last_busy(dev);
	pm_runtime_idle(dev);

	priv->rx_wq = alloc_ordered_workqueue("sof_serial_rx", WQ_UNBOUND);
	if (!priv->rx_wq) {
		ret = -ENOMEM;
		goto err_tty_dev;
	}

	INIT_DELAYED_WORK(&priv->rx_work, sof_serial_rx_workfn);

	/*
	 * Defer first poll until firmware is up. The workfn will abort and
	 * not reschedule itself if BOOT_COMPLETE is not yet reached; when the
	 * SOF core delivers a state-change notification we restart it.
	 * Queue once here so that if the firmware was already booted (e.g.
	 * module loaded late) we do not miss the window.
	 */
	ret = sof_client_register_fw_state_handler(cdev, sof_serial_fw_state_cb);
	if (ret) {
		dev_err(dev, "Failed to register fw state handler: %d\n", ret);
		goto err_wq;
	}

	/*
	 * If firmware is already booted (late module load), start polling now.
	 * Otherwise the fw-state callback will start it on BOOT_COMPLETE.
	 */
	if (sof_client_get_fw_state(cdev) == SOF_FW_BOOT_COMPLETE)
		queue_delayed_work(priv->rx_wq, &priv->rx_work,
				   msecs_to_jiffies(SOF_SERIAL_POLL_INTERVAL_MS));

	dev_dbg(dev, "SOF serial client probed for Zephyr shell (tty: ttysof0)\n");
	return 0;

err_wq:
	destroy_workqueue(priv->rx_wq);

err_tty_dev:
	tty_unregister_device(tty_drv, 0);
	tty_unregister_driver(tty_drv);

err_tty_port:
	tty_port_destroy(&priv->port);
	tty_driver_kref_put(tty_drv);
	return ret;
}

static void sof_serial_remove(struct auxiliary_device *auxdev)
{
	struct sof_client_dev *cdev = auxiliary_dev_to_sof_client_dev(auxdev);
	struct sof_serial_priv *priv = cdev->data;
	struct device *dev = &auxdev->dev;

	sof_client_unregister_fw_state_handler(cdev);
	cancel_delayed_work_sync(&priv->rx_work);
	destroy_workqueue(priv->rx_wq);
	pm_runtime_disable(dev);

	debugfs_remove_recursive(priv->dfs_root);

	tty_unregister_device(priv->tty_drv, 0);
	tty_unregister_driver(priv->tty_drv);
	tty_port_destroy(&priv->port);
	tty_driver_kref_put(priv->tty_drv);
}

static const struct auxiliary_device_id sof_serial_id_table[] = {
	{ .name = "snd_sof.serial" },
	{}
};
MODULE_DEVICE_TABLE(auxiliary, sof_serial_id_table);

static struct auxiliary_driver sof_serial_driver = {
	.name	= SOF_SERIAL_DRV_NAME,
	.probe	= sof_serial_probe,
	.remove	= sof_serial_remove,
	.id_table = sof_serial_id_table,
};
module_auxiliary_driver(sof_serial_driver);

MODULE_DESCRIPTION("SOF serial port client driver for Zephyr ADSP shell backend");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("SND_SOC_SOF_CLIENT");
