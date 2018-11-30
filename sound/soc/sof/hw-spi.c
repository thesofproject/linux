// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
//
// This file is provided under a dual BSD/GPLv2 license.  When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2018 Intel Corporation. All rights reserved.
//
// Author: Liam Girdwood <liam.r.girdwood@linux.intel.com>
//

/*
 * Hardware interface for audio DSPs via SPI
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <crypto/hash.h>
#include <sound/sof.h>
#include <uapi/sound/sof/fw.h>
#include "sof-priv.h"
#include "hw-spi.h"
#include "ops.h"

#define SPI_SIZE_ALIGN 16

/*
 * Memory copy.
 */

/*
 * At the moment it seems, that we don't need to support offset != 0. This might
 * change in the future.
 */
static void spi_block_read(struct snd_sof_dev *sdev, u32 offset, void *dest,
			   size_t size)
{
	struct snd_sof_spi *sof_spi = dev_get_drvdata(sdev->parent);
	size_t aligned_size = ALIGN(size + offset, SPI_SIZE_ALIGN);
	u8 *buf = sof_spi->ipc_buf;
	u32 *s = (u32 *)buf, *d = dest;
	int ret;

	if (size > PAGE_SIZE || offset) {
		dev_err(sdev->dev,
			"%s(): error: invalid size %u or offset %u\n",
			__func__, size, offset);
		return;
	}

	ret = spi_read(to_spi_device(sdev->parent), buf, aligned_size);
	if (ret < 0) {
		dev_err(sdev->dev, "%s(): error: SPI read failed: %d\n",
			__func__, ret);
		return;
	}

	be32_to_cpu_array(d, s, DIV_ROUND_UP(size, sizeof(*d)));
}

static void spi_write_work(struct work_struct *work)
{
	struct snd_sof_spi *sof_spi = container_of(work,
						struct snd_sof_spi, wr_work);
	struct snd_sof_pdata *sof_pdata = sof_spi->sof_plt->sof_pdata;
	int ret = spi_write(to_spi_device(sof_pdata->dev), sof_spi->ipc_buf,
			    sof_spi->wr_size);
	if (ret < 0)
		dev_err(sof_pdata->dev, "%s(): error: SPI write failed: %d\n",
			__func__, ret);

	sof_spi->wr_size = 0;
}

static int __spi_block_write(struct snd_sof_dev *sdev, u32 offset, void *src,
			     size_t size)
{
	struct snd_sof_spi *sof_spi = dev_get_drvdata(sdev->parent);
	size_t aligned_size = ALIGN(size + offset, SPI_SIZE_ALIGN);
	u32 *d = (u32 *)sof_spi->ipc_buf, *s = src;

	if (offset) {
		dev_err(sdev->dev, "%s(): error: only 0 offset supported %u\n",
			__func__, offset);
		return -EINVAL;
	}

	if (sof_spi->wr_size)
		return -EBUSY;

	cpu_to_be32_array(d, s, DIV_ROUND_UP(size, sizeof(*d)));

	memset(sof_spi->ipc_buf + size, 0, aligned_size - size);
	sof_spi->wr_size = aligned_size;

	queue_work(sof_spi->wr_wq, &sof_spi->wr_work);

	return 0;
}

static void spi_block_write(struct snd_sof_dev *sdev, u32 offset, void *src,
			    size_t size)
{
	__spi_block_write(sdev, offset, src, size);
}

/*
 * IPC Firmware ready.
 */
static int spi_fw_ready(struct snd_sof_dev *sdev, u32 msg_id)
{
	struct sof_ipc_fw_ready *fw_ready = &sdev->fw_ready;
	struct sof_ipc_fw_version *v = &fw_ready->version;

	dev_info(sdev->dev, "Firmware info: version %d:%d-%s build %d on %s:%s\n",
		 v->major, v->minor, v->tag, v->build, v->date, v->time);

	return 0;
}

/*
 * IPC Mailbox IO
 */

static void spi_mailbox_write(struct snd_sof_dev *sdev __maybe_unused,
			      u32 offset __maybe_unused,
			      void *message __maybe_unused,
			      size_t bytes __maybe_unused)
{
	/*
	 * this will copy to a local memory buffer that will be sent to DSP via
	 * SPI at next IPC
	 */
}

static void spi_mailbox_read(struct snd_sof_dev *sdev,
			     u32 offset, void *message, size_t bytes)
{
	struct snd_sof_spi *sof_spi = dev_get_drvdata(sdev->parent);

	if (offset + bytes <= PAGE_SIZE)
		/*
		 * copy from a local memory buffer that was received
		 * from DSP via SPI at last IPC
		 */
		memcpy(message, sof_spi->ipc_buf + offset, bytes);
}

/*
 * IPC Doorbell IRQ handler thread.
 */

static irqreturn_t spi_irq_thread(int irq __maybe_unused, void *context)
{
	struct snd_sof_dev *sdev = context;
	struct snd_sof_spi *sof_spi = dev_get_drvdata(sdev->parent);

	if (sof_spi->fw_loading) {
		/* We're still talking to the stock ROM */
		disable_irq_nosync(irq);
		sof_spi->wake = true;
		wake_up_interruptible(&sof_spi->wq);
	} else {
		/* Boot completed, handle an SOF GPIO IRQ */
		struct sof_ipc_cmd_hdr *hdr =
			(struct sof_ipc_cmd_hdr *)sof_spi->ipc_buf;
		int ret;

		/*
		 * the IRQ line is triggered on rising edge, then holding for
		 * 1ms
		 */
		ret = spi_read(to_spi_device(sdev->parent), hdr, sizeof(*hdr));
		if (ret < 0) {
			dev_err(sdev->dev,
				"%s(): error: SPI read header failed: %d\n",
				__func__, ret);
			return IRQ_HANDLED;
		}

		hdr->size = be32_to_cpu(hdr->size);
		hdr->cmd = be32_to_cpu(hdr->cmd);

		if (hdr->size < sizeof(*hdr)) {
			dev_err(sdev->dev,
				"%s(): error: invalid IPC header: size = %u\n",
				__func__, hdr->size);
			return IRQ_HANDLED;
		}

		if (hdr->size > sizeof(*hdr) && hdr->size <= PAGE_SIZE) {
			u32 *p = (u32 *)(hdr + 1);
			size_t size = ALIGN(hdr->size, SPI_SIZE_ALIGN) -
				sizeof(*hdr);

			ret = spi_read(to_spi_device(sdev->parent), p, size);
			if (ret < 0) {
				dev_err(sdev->dev,
					"%s(): error: SPI read message failed: %d\n",
					__func__, ret);
				return IRQ_HANDLED;
			}

			/* Size should be 4-byte aligned */
			be32_to_cpu_array(p, p, DIV_ROUND_UP(size, sizeof(*p)));
		}

		switch (hdr->cmd) {
		case SOF_IPC_FW_READY:
			memcpy(&sdev->fw_ready, hdr, sizeof(sdev->fw_ready));
		}

		if (sof_spi->msg_hdr) {
			snd_sof_ipc_reply(sdev, sof_spi->msg_hdr);
			sof_spi->msg_hdr = 0;
		}

		/* Handle messages from DSP Core */
		snd_sof_ipc_msgs_rx(sdev);
	}

	return IRQ_HANDLED;
}

static int spi_is_ready(struct snd_sof_dev *sdev __maybe_unused)
{
	// use local variable to store DSP command state. either DSP is ready
	// for new cmd or still processing current cmd.

	return 1;
}

static int spi_send_msg(struct snd_sof_dev *sdev, struct snd_sof_ipc_msg *msg)
{
	struct snd_sof_spi *sof_spi = dev_get_drvdata(sdev->parent);
	int ret;

	sof_spi->msg_hdr = msg->header;

	/* send the message */
	ret = __spi_block_write(sdev, 0, msg->msg_data, msg->msg_size);
	if (ret < 0)
		sof_spi->msg_hdr = 0;

	return ret;
}

static int spi_get_reply(struct snd_sof_dev *sdev, struct snd_sof_ipc_msg *msg)
{
	struct sof_ipc_reply *reply = msg->reply_data;
	u32 size;
	int ret;

	/* get reply */
	if (msg->reply_size < sizeof(*reply) || msg->reply_size > PAGE_SIZE ||
	    !reply)
		return -EINVAL;

	/* read the message header */
	spi_mailbox_read(sdev, 0, reply, sizeof(*reply));
	if (reply->error < 0) {
		size = sizeof(*reply);
		ret = reply->error;
	} else {
		/* reply correct size ? */
		if (reply->hdr.size != msg->reply_size) {
			dev_err(sdev->dev,
				"error: reply to 0x%x expected 0x%zx got 0x%x bytes @ %u\n",
				reply->hdr.cmd, msg->reply_size,
				reply->hdr.size, sdev->host_box.offset);
			ret = -EINVAL;
		} else {
			ret = 0;
		}

		size = msg->reply_size;
	}

	/* read the message body */
	if (size > sizeof(*reply))
		spi_mailbox_read(sdev, sizeof(*reply), reply + 1,
				 size - sizeof(*reply));

	return ret;
}

/*
 * Probe and remove.
 */

static int spi_sof_probe(struct snd_sof_dev *sdev)
{
	struct snd_sof_spi *sof_spi = dev_get_drvdata(sdev->parent);
	/* get IRQ from Device tree or ACPI - register our IRQ */
	struct irq_data *irqd;
	struct spi_device *spi = to_spi_device(sdev->parent);
	unsigned int irq_trigger, irq_sense;
	int ret;

	init_waitqueue_head(&sof_spi->wq);

	sof_spi->fw_loading = true;

	sdev->ipc_irq = spi->irq;
	spi->mode = SPI_MODE_3;
	spi->max_speed_hz = 12500000;
	irqd = irq_get_irq_data(sdev->ipc_irq);
	if (!irqd)
		return -EINVAL;

	irq_trigger = irqd_get_trigger_type(irqd);
	irq_sense = irq_trigger & IRQ_TYPE_SENSE_MASK;

	dev_dbg(sdev->dev, "%s(): Using IRQ %d trigger 0x%x\n",
		__func__, sdev->ipc_irq, irq_trigger);

	ret = devm_request_threaded_irq(sdev->dev, sdev->ipc_irq,
					NULL, spi_irq_thread,
					irq_sense | IRQF_ONESHOT,
					"AudioDSP", sdev);
	if (ret < 0) {
		dev_err(sdev->dev, "%s(): error: failed to register IRQ %d\n",
			__func__, sdev->ipc_irq);
		return ret;
	}

	INIT_WORK(&sof_spi->wr_work, spi_write_work);
	sof_spi->wr_wq = create_singlethread_workqueue("sof-spi");
	if (!sof_spi->wr_wq)
		ret = -ENOMEM;

	return ret;
}

static int spi_sof_remove(struct snd_sof_dev *sdev)
{
	struct snd_sof_spi *sof_spi = dev_get_drvdata(sdev->parent);

	cancel_work_sync(&sof_spi->wr_work);
	destroy_workqueue(sof_spi->wr_wq);

	return 0;
}

#define SUE_CREEK_LOAD_ADDR 0xbe066000

struct spi_fw_header {
	u32 command;
	u32 flags;
	u32 payload[3];
	u8 sha256[32];
	u8 padding[12];	/* Pad to 64 bytes */
} __attribute__((packed));

#define REQUEST_MASK			0x81000000
#define RESPONSE_MASK			0xA1000000

#define ROM_CONTROL_LOAD		0x02
#define ROM_CONTROL_MEM_READ		0x10
#define ROM_CONTROL_MEM_WRITE		0x11
#define ROM_CONTROL_MEM_WRITE_BLOCK	0x12
#define ROM_CONTROL_EXEC		0x13
#define ROM_CONTROL_WAIT		0x14
#define ROM_CONTROL_ROM_READY		0x20

#define MAX_SPI_XFER_SIZE (4 * 1024U)

/*
 * Eventually we will remove this flag, once SOF can boot on Sue Creek without
 * JTAG
 */
#define FW_LOAD_NO_EXEC_FLAG	(1 << 26)
#define CLOCK_SELECT_SPI_SLAVE	(1 << 21)

static int spi_fw_write_single(struct snd_sof_dev *sdev,
			       const struct spi_fw_header *hdr, const u8 *data,
			       size_t len, unsigned int timeout_ms)
{
	struct snd_sof_spi *sof_spi = dev_get_drvdata(sdev->parent);
	struct spi_device *spi = to_spi_device(sdev->parent);
	struct spi_fw_header h;
	int ret = spi_write(spi, hdr, sizeof(*hdr));
	if (ret < 0) {
		dev_err(sdev->dev, "error: failed sending 0x%08x IPC: %d\n",
			hdr->command, ret);
		return ret;
	}

	enable_irq(spi->irq);
	ret = wait_event_interruptible_timeout(sof_spi->wq, sof_spi->wake,
					       msecs_to_jiffies(timeout_ms));
	sof_spi->wake = false;
	if (ret <= 0) {
		dev_warn(sdev->dev,
			 "%s(): no IRQ cmd 0x%08x with %ums timeout: %d\n",
			 __func__, hdr->command, timeout_ms, ret);
		if (ret < 0) {
			unsigned long us = 1000 * timeout_ms;
			usleep_range(us, us + 1000);
		}
	} else {
		dev_dbg(sdev->dev, "%s(): command 0x%08x complete\n",
			__func__, hdr->command);
	}

	if (len) {
		__be32 *buf;
		size_t left;

		buf = kmalloc(MAX_SPI_XFER_SIZE, GFP_KERNEL);
		if (!buf)
			return -ENOMEM;

		for (left = len; left;
		     data += MAX_SPI_XFER_SIZE, left -= MAX_SPI_XFER_SIZE) {
			size_t block = min(len, MAX_SPI_XFER_SIZE);

			cpu_to_be32_array(buf, (const u32 *)data, block / 4);
			ret = spi_write(spi, buf, block);
			if (ret < 0) {
				dev_err(sdev->dev,
					"%s(): error: failed %zu data for 0x%08x: %d\n",
					__func__, len, hdr->command, ret);
				kfree(buf);
				return ret;
			}
		}

		enable_irq(spi->irq);
		ret = wait_event_interruptible_timeout(sof_spi->wq,
						sof_spi->wake,
						msecs_to_jiffies(timeout_ms));
		sof_spi->wake = false;
		if (ret <= 0) {
			dev_warn(sdev->dev,
				 "%s(): no IRQ %zu bytes cmd 0x%08x with %ums timeout: %d\n",
				 __func__, len, hdr->command, timeout_ms, ret);
			if (ret < 0) {
				unsigned long us = 1000 * timeout_ms;
				usleep_range(us, us + 1000);
			}
		} else {
			dev_dbg(sdev->dev,
				"%s(): %zu bytes data for cmd 0x%08x sent\n",
				__func__, len, hdr->command);
		}

		kfree(buf);
	}

	/* Emulating the python script behaviour with a double read */
	spi_read(spi, &h, sizeof(h));

	spi_read(spi, &h, sizeof(h));

	return 0;
}

static int spi_fw_run(struct snd_sof_dev *sdev)
{
	struct snd_sof_spi *sof_spi = dev_get_drvdata(sdev->parent);
	struct snd_sof_pdata *sof_pdata = dev_get_platdata(sdev->dev);
	struct spi_device *spi = to_spi_device(sdev->parent);
	struct crypto_shash *tfm;
	int ret;
	struct spi_fw_header *hdr = kzalloc(sizeof(*hdr), GFP_KERNEL);
	if (!hdr)
		return -ENOMEM;

	/* Reset the board, using the reset GPIO */
	gpio_set_value(sof_pdata->reset, 0);
	usleep_range(100000, 200000);
	gpio_set_value(sof_pdata->reset, 1);

	/* Wait for the "ROM Ready" IRQ */
	ret = wait_event_interruptible_timeout(sof_spi->wq, sof_spi->wake,
					       msecs_to_jiffies(2000));
	/* The stock firmware doesn't handle the IRQ GPIO well */
	if (ret <= 0) {
		if (!ret)
			ret = -ETIMEDOUT;
		goto free;
	} else {
		dev_dbg(sdev->dev, "%s(): reset complete\n", __func__);
	}

	sof_spi->wake = false;

	/* Read the "ROM Ready" message */
	spi_read(spi, hdr, sizeof(*hdr));

	/* Write to memory: "Setup retention delay" copied from python scripts */
	memset(hdr, 0, sizeof(*hdr));
	hdr->command = cpu_to_be32(REQUEST_MASK | ROM_CONTROL_MEM_WRITE);
	hdr->flags = cpu_to_be32(2);
	hdr->payload[0] = cpu_to_be32(0x304628);
	hdr->payload[1] = cpu_to_be32(0xd);

	ret = spi_fw_write_single(sdev, hdr, NULL, 0, 10);
	if (ret < 0)
		goto free;

	/* Send the "LOAD" message */
	hdr->command = cpu_to_be32(REQUEST_MASK | ROM_CONTROL_LOAD);
	hdr->flags = cpu_to_be32(CLOCK_SELECT_SPI_SLAVE | FW_LOAD_NO_EXEC_FLAG |
				 ((sizeof(hdr->payload) + sizeof(hdr->sha256)) / sizeof(u32)));
	hdr->payload[0] = cpu_to_be32(SUE_CREEK_LOAD_ADDR);
	hdr->payload[2] = cpu_to_be32(sof_pdata->fw->size);

	tfm = crypto_alloc_shash("sha256", 0, 0);
	if (IS_ERR(tfm)) {
		ret = PTR_ERR(tfm);
		dev_err(sdev->dev,
			"%s(): error: unable to create SHA256 crypto context: %d\n",
			__func__, ret);
	} else {
		SHASH_DESC_ON_STACK(sha, tfm);
		sha->tfm = tfm;
		sha->flags = 0;
		ret = crypto_shash_digest(sha, sof_pdata->fw->data,
					  sof_pdata->fw->size, hdr->sha256);
		crypto_free_shash(tfm);
		cpu_to_be32_array((__be32 *)hdr->sha256, (const u32 *)hdr->sha256,
				  sizeof(hdr->sha256) / 4);
	}

	if (ret < 0)
		goto free;

	ret = spi_fw_write_single(sdev, hdr, sof_pdata->fw->data,
				  sof_pdata->fw->size, 350);
	if (ret < 0)
		goto free;

	memset(hdr, 0, sizeof(*hdr));
	hdr->command = cpu_to_be32(REQUEST_MASK | ROM_CONTROL_MEM_READ);
	hdr->flags = cpu_to_be32(1);
	hdr->payload[0] = cpu_to_be32(0x71f7c);

	ret = spi_fw_write_single(sdev, hdr, NULL, 0, 20);
	if (ret < 0)
		goto free;

	/*
	 * Debugging: this is a 30 second sleep. This gives time to start xt-ocd
	 * and xt-gdb. This will be removed once we figure out how to boot
	 * without needing the two memory writes, performed by the gdb script.
	 */
	usleep_range(30000000, 31000000);

	memset(hdr, 0, sizeof(*hdr));
	hdr->command = cpu_to_be32(REQUEST_MASK | ROM_CONTROL_EXEC);
	hdr->flags = cpu_to_be32(1);
	hdr->payload[0] = cpu_to_be32(SUE_CREEK_LOAD_ADDR);

	ret = spi_write(spi, hdr, sizeof(*hdr));
	if (ret < 0)
		dev_err(sdev->dev, "%s(): error: failed sending EXEC IPC: %d\n",
			__func__,  ret);

	enable_irq(spi->irq);

	sof_spi->fw_loading = false;

free:
	kfree(hdr);

	return ret;
}

/* SPI SOF ops */
const struct snd_sof_dsp_ops snd_sof_spi_ops = {
	/* device init */
	.probe		= spi_sof_probe,
	.remove		= spi_sof_remove,

	/* Block IO */
	.block_read	= spi_block_read,
	.block_write	= spi_block_write,

	/* mailbox */
	.mailbox_read	= spi_mailbox_read,
	.mailbox_write	= spi_mailbox_write,

	/* ipc */
	.send_msg	= spi_send_msg,
	.get_reply	= spi_get_reply,
	.fw_ready	= spi_fw_ready,
	.is_ready	= spi_is_ready,

	/* Firmware loading */
	.load_firmware	= snd_sof_load_firmware_raw,
	.run		= spi_fw_run,
};
EXPORT_SYMBOL(snd_sof_spi_ops);

MODULE_LICENSE("Dual BSD/GPL");
