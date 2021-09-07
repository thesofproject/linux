// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
//
// Copyright(c) 2021 Mediatek Corporation. All rights reserved.
//
//Author: Allen-KH Cheng <allen-kh.cheng@mediatek.com>
//
// The Mediatek ADSP IPC implementation
//

#include <linux/firmware.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/mailbox_controller.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <sound/sof.h>
#include <sound/sof/xtensa.h>
#include "../adsp_ipc.h"
#include "../adsp_helper.h"
#include "mt8195-ipc.h"
#include "mt8195.h"

int trace_boot_event;

static void mt8195_get_reply(struct snd_sof_dev *sdev)
{
	struct snd_sof_ipc_msg *msg = sdev->msg;
	struct sof_ipc_reply reply;
	int ret = 0;

	if (!msg) {
		dev_warn(sdev->dev, "unexpected ipc interrupt\n");
		return;
	}

	/* get reply */
	sof_mailbox_read(sdev, sdev->host_box.offset, &reply, sizeof(reply));

	if (reply.error < 0) {
		memcpy(msg->reply_data, &reply, sizeof(reply));
		ret = reply.error;
	} else {
		/* reply has correct size? */
		if (reply.hdr.size != msg->reply_size) {
			dev_err(sdev->dev, "error: reply expected %zu got %u bytes\n",
				msg->reply_size, reply.hdr.size);
			ret = -EINVAL;
		}

		/* read the message */
		if (msg->reply_size > 0)
			sof_mailbox_read(sdev, sdev->host_box.offset,
					 msg->reply_data, msg->reply_size);
	}

	msg->reply_error = ret;
}

static void mt8195_dsp_handle_reply(struct mtk_adsp_ipi *ipi)
{
	struct adsp_priv *priv = adsp_ipi_get_data(ipi);
	unsigned long flags;

	spin_lock_irqsave(&priv->sdev->ipc_lock, flags);
	mt8195_get_reply(priv->sdev);
	snd_sof_ipc_reply(priv->sdev, 0);
	spin_unlock_irqrestore(&priv->sdev->ipc_lock, flags);
}

static void mt8195_dsp_handle_request(struct mtk_adsp_ipi *ipi)
{
	struct adsp_priv *priv = adsp_ipi_get_data(ipi);
	u32 p; /* panic code */
	int ret;

	/* Read the message from the debug box. */
	sof_mailbox_read(priv->sdev, priv->sdev->debug_box.offset + 4,
			 &p, sizeof(p));

	/* Check to see if the message is a panic code 0x0dead*** */
	if ((p & SOF_IPC_PANIC_MAGIC_MASK) == SOF_IPC_PANIC_MAGIC) {
		snd_sof_dsp_panic(priv->sdev, p);
	} else {
		snd_sof_ipc_msgs_rx(priv->sdev);

		/* tell DSP cmd is done */
		ret = adsp_ipi_send(priv->sdev, ADSP_IPI_MBOX_RSP, ADSP_IPI_OP_RSP);
		if (ret)
			dev_err(priv->dev, "request send ipi failed");
	}
}

static void mt8195_dsp_handle_debug_message(struct mtk_adsp_ipi *ipi)
{
	trace_boot_event = *(u32 *)DSP_MBOX_OUT_MSG0(2);
}

static struct mtk_adsp_ipi_ops mt8195_ipi_dsp_reply = {
	.handle_recv		= mt8195_dsp_handle_reply,
};

static struct mtk_adsp_ipi_ops mt8195_ipi_dsp_request = {
	.handle_recv		= mt8195_dsp_handle_request,
};

static struct mtk_adsp_ipi_ops mt8195_debug_dsp_message = {
	.handle_recv		= mt8195_dsp_handle_debug_message,
};

static irqreturn_t mt8195_ipi_irq_handler(int irq, void *data)
{
	struct mbox_chan *ch = (struct mbox_chan *)data;
	struct adsp_mbox_ch_info *ch_info = ch->con_priv;
	u32 id = ch_info->id;
	u32 op = *(u32 *)DSP_MBOX_OUT_CMD(id);

	*(u32 *)DSP_MBOX_OUT_CMD_CLR(id) = op; /* clear DSP->CPU int */
	return IRQ_WAKE_THREAD;
}

static irqreturn_t mt8195_ipi_handler(int irq, void *data)
{
	struct mbox_chan *ch = (struct mbox_chan *)data;
	struct adsp_mbox_ch_info *ch_info = ch->con_priv;

	mbox_chan_received_data(ch, ch_info);

	return IRQ_HANDLED;
}

static struct mbox_chan *mt8195_mbox_xlate(struct mbox_controller *mbox,
					   const struct of_phandle_args *sp)
{
	return &mbox->chans[sp->args[0]];
}

static int mt8195_mbox_startup(struct mbox_chan *chan)
{
	struct adsp_mbox_ch_info *ch_info = chan->con_priv;
	struct device *dev = chan->mbox->dev;
	struct snd_sof_dev *sdev;
	struct platform_device *pdev;
	int ret;
	int irq;
	char *name;

	sdev = ch_info->priv->sdev;
	pdev = container_of(sdev->dev, struct platform_device, dev);

	name = kasprintf(GFP_KERNEL, "mbox%d", ch_info->id);
	if (!name)
		return -ENOMEM;

	irq = platform_get_irq_byname(pdev, name);
	if (irq < 0) {
		dev_err(sdev->dev, "Failed to get ipc irq\n");
		ret = -ENODEV;
		goto err_name_free;
	}

	ret = devm_request_threaded_irq(dev, irq,
					mt8195_ipi_irq_handler, mt8195_ipi_handler,
					IRQF_TRIGGER_NONE, name,
					chan);
	if (ret < 0)
		dev_err(dev, "failed to request irq %d\n", irq);

err_name_free:
	kfree(name);
	return ret;
}

/* TODO:: implement later , cuurently no use*/
static void mt8195_mbox_shutdown(struct mbox_chan *chan)
{
}

static int mt8195_mbox_send_data(struct mbox_chan *chan, void *data)
{
	struct adsp_mbox_ch_info *ch_info = chan->con_priv;

	*(u32 *)DSP_MBOX_IN_CMD(ch_info->id) = ch_info->ipi_op_val;

	return 0;
}

static bool mt8195_mbox_last_tx_done(struct mbox_chan *chan)
{
	return true;
}

static const struct mbox_chan_ops adsp_mbox_chan_ops = {
	.send_data	= mt8195_mbox_send_data,
	.startup	= mt8195_mbox_startup,
	.shutdown	= mt8195_mbox_shutdown,
	.last_tx_done	= mt8195_mbox_last_tx_done,
};

int mt8195_mbox_init(struct snd_sof_dev *sdev)
{
	struct device *dev = sdev->dev;
	struct adsp_priv *priv = sdev->pdata->hw_pdata;
	struct mbox_controller *mbox;
	int ret;
	int i;

	mbox = devm_kzalloc(dev, sizeof(*mbox), GFP_KERNEL);
	if (!mbox)
		return -ENOMEM;

	priv->adsp_mbox = mbox;
	mbox->dev = dev;
	mbox->ops = &adsp_mbox_chan_ops;
	mbox->txdone_irq = false;
	mbox->txdone_poll = true;
	mbox->of_xlate = mt8195_mbox_xlate;
	mbox->num_chans = DSP_MBOX_NUM;
	mbox->chans = devm_kcalloc(mbox->dev, mbox->num_chans,
				   sizeof(*mbox->chans), GFP_KERNEL);
	if (!mbox->chans)
		return -ENOMEM;

	for (i = 0; i < mbox->num_chans; i++) {
		struct adsp_mbox_ch_info *ch_info;

		ch_info = devm_kzalloc(mbox->dev, sizeof(*ch_info), GFP_KERNEL);
		if (!ch_info)
			return -ENOMEM;

		ch_info->id = i;
		ch_info->priv = priv;
		mbox->chans[i].con_priv = ch_info;
	}

	ret = mbox_controller_register(mbox);
	if (ret < 0) {
		dev_err(dev, "error: failed to register mailbox:%d\n", ret);
		return ret;
	}

	/* init value */
	trace_boot_event = 0xffff;
	adsp_ipi_request(sdev, 0, &mt8195_ipi_dsp_reply);
	adsp_ipi_request(sdev, 1, &mt8195_ipi_dsp_request);
	adsp_ipi_request(sdev, 2, &mt8195_debug_dsp_message);

	return ret;
}
