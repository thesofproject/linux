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

#include <crypto/hash.h>
#include <sound/sof.h>
#include <uapi/sound/sof/fw.h>
#include "sof-priv.h"
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
	struct snd_sof_pdata *sof_pdata = dev_get_platdata(sdev->dev);
	size_t aligned_size = ALIGN(size + offset, SPI_SIZE_ALIGN);
	u8 *buf = sof_pdata->ipc_buf;
	u32 *s = (u32 *)buf, *d = dest;
	int ret;

	if (size > PAGE_SIZE || offset) {
		dev_err(sdev->dev, "%s(): Invalid size %u or offset %u\n",
			__func__, size, offset);
		return;
	}

	ret = spi_read(to_spi_device(sdev->parent), buf, aligned_size);
	if (ret < 0) {
		dev_err(sdev->dev, "%s(): SPI read failed: %d\n",
			__func__, ret);
		return;
	}

	be32_to_cpu_array(d, s, DIV_ROUND_UP(size, sizeof(*d)));
}

static int __spi_block_write(struct snd_sof_dev *sdev, u32 offset, void *src,
			     size_t size)
{
	struct snd_sof_pdata *sof_pdata = dev_get_platdata(sdev->dev);
	size_t aligned_size = ALIGN(size + offset, SPI_SIZE_ALIGN);
	u32 *d = (u32 *)sof_pdata->ipc_buf, *s = src;
	int ret;

	if (offset) {
		dev_err(sdev->dev, "%s(): Only 0 offset supported %u\n",
			__func__, offset);
		return -EINVAL;
	}

	cpu_to_be32_array(d, s, DIV_ROUND_UP(size, sizeof(*d)));

	memset(sof_pdata->ipc_buf + size, 0, aligned_size - size);

	ret = spi_write(to_spi_device(sdev->parent), sof_pdata->ipc_buf,
			aligned_size);
	if (ret < 0)
		dev_err(sdev->dev, "%s(): SPI write failed: %d\n",
			__func__, ret);

	return ret;
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

	dev_dbg(sdev->dev, "ipc: DSP is ready 0x%8.8x\n", msg_id);

	// read local buffer with SPI data

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
	struct snd_sof_pdata *sof_pdata = dev_get_platdata(sdev->dev);

	if (offset + bytes <= PAGE_SIZE)
		/*
		 * copy from a local memory buffer that was received
		 * from DSP via SPI at last IPC
		 */
		memcpy(message, sof_pdata->ipc_buf + offset, bytes);
}

/*
 * IPC Doorbell IRQ handler thread.
 */

static irqreturn_t spi_irq_thread(int irq __maybe_unused, void *context)
{
	struct snd_sof_dev *sdev = context;
	struct snd_sof_pdata *sof_pdata = dev_get_platdata(sdev->dev);
	// read SPI data into local buffer and determine IPC cmd or reply

	/*
	 * if reply. Handle Immediate reply from DSP Core and set DSP
	 * state to ready
	 */
	if (sof_pdata->fw_loading) {
		/* We're still talking to the stock ROM */
		disable_irq_nosync(irq);
		sof_pdata->wake = true;
		wake_up_interruptible(&sof_pdata->wq);
	} else {
		struct sof_ipc_hdr *hdr = (struct sof_ipc_hdr *)sof_pdata->ipc_buf;
		int ret;

		/*
		 * the IRQ line is triggered on rising edge, then holding for
		 * 1ms
		 */
		ret = spi_read(to_spi_device(sdev->parent), hdr, sizeof(*hdr));
		if (ret < 0) {
			dev_err(sdev->dev, "%s(): SPI read header failed: %d\n",
				__func__, ret);
			return IRQ_HANDLED;
		}

		hdr->size = be32_to_cpu(hdr->size);
		hdr->cmd = be32_to_cpu(hdr->cmd);

		if (hdr->size <= PAGE_SIZE) {
			u32 *p = (u32 *)(sof_pdata->ipc_buf + sizeof(*hdr));
			size_t size = ALIGN(hdr->size, SPI_SIZE_ALIGN) -
				sizeof(*hdr);

			ret = spi_read(to_spi_device(sdev->parent), p, size);
			if (ret < 0) {
				dev_err(sdev->dev,
					"%s(): SPI read message failed: %d\n",
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

		snd_sof_ipc_msgs_rx(sdev);
	}

	/* if cmd, Handle messages from DSP Core */

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
	struct snd_sof_pdata *sof_pdata = dev_get_platdata(sdev->dev);
	int ret;

	sof_pdata->wake = false;

	/* send the message */
	ret = __spi_block_write(sdev, 0, msg->msg_data, msg->msg_size);
	if (!ret)
		snd_sof_ipc_reply(sdev, msg->header);

	return ret;
}

static int spi_get_reply(struct snd_sof_dev *sdev, struct snd_sof_ipc_msg *msg)
{
	struct snd_sof_pdata *sof_pdata = dev_get_platdata(sdev->dev);
	struct sof_ipc_reply *reply = msg->reply_data;
	u32 size;
	int ret;

	/* get reply */
	ret = wait_event_interruptible_timeout(sof_pdata->wq, sof_pdata->wake,
					       msecs_to_jiffies(50));
	sof_pdata->wake = false;
	if (ret <= 0)
		return ret < 0 ? ret : -ETIMEDOUT;

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
				reply->hdr.cmd, msg->reply_size, reply->hdr.size,
				sdev->host_box.offset);
			ret = -EINVAL;
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
	struct platform_device *pdev =
		container_of(sdev->dev, struct platform_device, dev);
	struct snd_sof_pdata *sof_pdata = dev_get_platdata(&pdev->dev);
	/* get IRQ from Device tree or ACPI - register our IRQ */
	struct irq_data *irqd;
	struct spi_device *spi = to_spi_device(pdev->dev.parent);
	unsigned int irq_trigger, irq_sense;
	int ret;

	init_waitqueue_head(&sof_pdata->wq);

	sof_pdata->fw_loading = true;

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
	if (ret < 0)
		dev_err(sdev->dev, "%s(): error: failed to register IRQ %d\n",
			__func__, sdev->ipc_irq);

	return ret;
}

static int spi_sof_remove(struct snd_sof_dev *sdev)
{
	return 0;
}

static int spi_cmd_done(struct snd_sof_dev *sdev, int dir __maybe_unused)
{
	struct snd_sof_pdata *sof_pdata = dev_get_platdata(sdev->dev);

	sof_pdata->wake = true;
	wake_up_interruptible(&sof_pdata->wq);

	return 0;
}

#define SUE_CREEK_LOAD_ADDR 0xbe100000

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

#define MAX_SPI_XFER_SIZE (4 * 1024)

/*
 * Eventually we will remove this flag, once SOF can boot on Sue Creek without
 * JTAG
 */
#define FW_LOAD_NO_EXEC_FLAG	(1 << 26)
#define CLOCK_SELECT_SPI_SLAVE	(1 << 21)

/* FIXME: This shall be reworked to reuse code, group calls etc. */
static int spi_fw_run(struct snd_sof_dev *sdev)
{
	struct snd_sof_pdata *sof_pdata = dev_get_platdata(sdev->dev);
	struct spi_device *spi = to_spi_device(sdev->parent);
	struct crypto_shash *tfm;
	ssize_t size;
	const u8 *data;
	__be32 *buf;
	int ret;
	struct spi_fw_header *hdr = kzalloc(sizeof(*hdr), GFP_KERNEL);
	if (!hdr)
		return -ENOMEM;

	buf = kmalloc(MAX_SPI_XFER_SIZE, GFP_KERNEL);
	if (!buf) {
		ret = -ENOMEM;
		goto free;
	}

	/* Reset the board, using the reset GPIO */
	gpio_set_value(sof_pdata->reset, 0);
	usleep_range(100000, 200000);
	gpio_set_value(sof_pdata->reset, 1);

	/* Wait for the "ROM Ready" IRQ */
	ret = wait_event_interruptible_timeout(sof_pdata->wq, sof_pdata->wake,
					       msecs_to_jiffies(2000));
	/* The stock firmware doesn't handle the IRQ GPIO well */
	if (ret <= 0) {
		if (!ret)
			ret = -ETIMEDOUT;
		goto free;
	} else {
		dev_dbg(sdev->dev, "Reset complete\n");
	}

	sof_pdata->wake = false;

	/* Probably superfluous before an spi_read() */
	memset(hdr, 0, sizeof(*hdr));

	/* Read the "ROM Ready" message */
	spi_read(spi, hdr, sizeof(*hdr));

	/* Write to memory: "Setup retention delay" copied from python scripts */
	memset(hdr, 0, sizeof(*hdr));
	hdr->command = cpu_to_be32(REQUEST_MASK | ROM_CONTROL_MEM_WRITE);
	hdr->flags = cpu_to_be32(2);
	hdr->payload[0] = cpu_to_be32(0x304628);
	hdr->payload[1] = cpu_to_be32(0xd);
	ret = spi_write(spi, hdr, sizeof(*hdr));
	if (ret < 0) {
		dev_err(sdev->dev, "Failed sending MEM_WRITE IPC: %d\n", ret);
		goto free;
	}

	enable_irq(spi->irq);
	ret = wait_event_interruptible_timeout(sof_pdata->wq, sof_pdata->wake,
					       msecs_to_jiffies(10));
	sof_pdata->wake = false;
	if (ret <= 0) {
		dev_warn(sdev->dev, "%s().%d: no IRQ: %d\n", __func__, __LINE__, ret);
		if (ret < 0) {
			usleep_range(10000, 12000);
			ret = 0;
		}
	} else {
		dev_dbg(sdev->dev, "Setup complete\n");
	}

	memset(hdr, 0, sizeof(*hdr));
	spi_read(spi, hdr, sizeof(*hdr));

	memset(hdr, 0, sizeof(*hdr));
	spi_read(spi, hdr, sizeof(*hdr));

	/* Send the "LOAD" message */
	hdr->command = cpu_to_be32(REQUEST_MASK | ROM_CONTROL_LOAD);
	hdr->flags = cpu_to_be32(CLOCK_SELECT_SPI_SLAVE | FW_LOAD_NO_EXEC_FLAG |
				 ((sizeof(hdr->payload) + sizeof(hdr->sha256)) / sizeof(u32)));
	hdr->payload[0] = cpu_to_be32(SUE_CREEK_LOAD_ADDR);
	hdr->payload[2] = cpu_to_be32(sof_pdata->fw->size);

	tfm = crypto_alloc_shash("sha256", 0, 0);
	if (IS_ERR(tfm)) {
		dev_err(sdev->dev, "Unable to create SHA256 crypto context\n");
		ret = PTR_ERR(tfm);
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

	ret = spi_write(spi, hdr, sizeof(*hdr));
	if (ret < 0) {
		dev_err(sdev->dev, "Failed sending LOAD IPC: %d\n", ret);
		goto free;
	}

	enable_irq(spi->irq);
	ret = wait_event_interruptible_timeout(sof_pdata->wq, sof_pdata->wake,
					       msecs_to_jiffies(350));
	sof_pdata->wake = false;
	if (ret <= 0) {
		dev_warn(sdev->dev, "%s().%d: no IRQ: %d\n", __func__, __LINE__, ret);
		if (ret < 0) {
			usleep_range(350000, 400000);
			ret = 0;
		}
	} else {
		dev_dbg(sdev->dev, "Load prepare complete\n");
	}

	for (data = sof_pdata->fw->data, size = sof_pdata->fw->size;
	     size > 0;
	     data += MAX_SPI_XFER_SIZE, size -= MAX_SPI_XFER_SIZE) {
		size_t left = min(size, MAX_SPI_XFER_SIZE);

		cpu_to_be32_array(buf, (const u32 *)data, left / 4);
		ret = spi_write(spi, buf, left);
		if (ret < 0) {
			dev_err(sdev->dev, "Failed sending FW image: %d\n", ret);
			goto free;
		}
	}

	enable_irq(spi->irq);
	ret = wait_event_interruptible_timeout(sof_pdata->wq, sof_pdata->wake,
					       msecs_to_jiffies(10));
	sof_pdata->wake = false;
	if (ret <= 0) {
		dev_warn(sdev->dev, "%s().%d: no IRQ: %d\n", __func__, __LINE__, ret);
		if (ret < 0) {
			usleep_range(10000, 12000);
			ret = 0;
		}
	} else {
		dev_dbg(sdev->dev, "FW load complete\n");
	}

	memset(hdr, 0, sizeof(*hdr));
	spi_read(spi, hdr, sizeof(*hdr));

	memset(hdr, 0, sizeof(*hdr));
	spi_read(spi, hdr, sizeof(*hdr));

	memset(hdr, 0, sizeof(*hdr));
	hdr->command = cpu_to_be32(REQUEST_MASK | ROM_CONTROL_MEM_READ);
	hdr->flags = cpu_to_be32(1);
	hdr->payload[0] = cpu_to_be32(0x71f7c);
	ret = spi_write(spi, hdr, sizeof(*hdr));

	enable_irq(spi->irq);
	ret = wait_event_interruptible_timeout(sof_pdata->wq, sof_pdata->wake,
					       msecs_to_jiffies(20));
	sof_pdata->wake = false;
	if (ret <= 0) {
		dev_warn(sdev->dev, "%s().%d: no IRQ: %d\n", __func__, __LINE__, ret);
		if (ret < 0) {
			usleep_range(20000, 22000);
			ret = 0;
		}
	} else {
		dev_dbg(sdev->dev, "Mem read complete\n");
	}

	memset(hdr, 0, sizeof(*hdr));
	spi_read(spi, hdr, sizeof(*hdr));

	memset(hdr, 0, sizeof(*hdr));
	spi_read(spi, hdr, sizeof(*hdr));

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
		dev_err(sdev->dev, "Failed sending EXEC IPC: %d\n", ret);

	enable_irq(spi->irq);

	sof_pdata->fw_loading = false;

free:
	kfree(buf);
	kfree(hdr);

	return ret;
}

/* SPI SOF ops */
static struct snd_sof_dsp_ops snd_sof_spi_ops = {
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
	.cmd_done	= spi_cmd_done,

	/* Firmware loading */
	.load_firmware	= snd_sof_load_firmware_raw,
	.run		= spi_fw_run,
};
EXPORT_SYMBOL(snd_sof_spi_ops);

MODULE_LICENSE("Dual BSD/GPL");
