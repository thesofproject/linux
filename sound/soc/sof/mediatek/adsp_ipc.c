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
#include "adsp_helper.h"

/* DSP SOF IPC API */

static void adsp_ipi_recv(struct mbox_client *c, void *mssg)
{
	struct mtk_adsp_ipi *ipi = container_of(c, struct mtk_adsp_ipi, cl);

	if (ipi->ops && ipi->ops->handle_recv)
		ipi->ops->handle_recv(ipi);
}

int adsp_ipi_send(struct snd_sof_dev *sdev, int idx, uint32_t op)
{
	struct adsp_priv *priv = sdev->pdata->hw_pdata;
	struct adsp_mbox_ch_info *ch_info = priv->ipi[idx].chan->con_priv;
	int ret;

	ch_info->ipi_op_val = op;
	ret = mbox_send_message(priv->ipi[idx].chan, NULL);
	if (ret < 0)
		dev_err(sdev->dev, "failed to send message via mbox: %d\n", ret);

	return ret;
}

void adsp_ipi_request(struct snd_sof_dev *sdev, int idx, struct mtk_adsp_ipi_ops *ops)
{
	struct adsp_priv *priv = sdev->pdata->hw_pdata;
	struct mbox_client *cl;

	cl = &priv->ipi[idx].cl;
	cl->dev = sdev->dev;
	cl->tx_block = false;
	cl->knows_txdone = false;
	cl->tx_prepare = NULL;
	cl->rx_callback = adsp_ipi_recv;

	priv->ipi[idx].chan = mbox_request_channel(cl, idx);
	priv->ipi[idx].ops = ops;
	priv->ipi[idx].private_data = priv;
}

void *adsp_ipi_get_data(struct mtk_adsp_ipi *ipi)
{
	if (!ipi)
		return NULL;

	return ipi->private_data;
}

