/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (c) 2021 Mediatek Corporation. All rights reserved.
 *
 * Header file for the DSP IPC implementation
 */

#ifndef ADSP_IPC_H
#define ADSP_IPC_H

#include <linux/device.h>
#include <linux/types.h>
#include <linux/mailbox_controller.h>
#include <linux/mailbox_client.h>
#include <sound/sof.h>
#include "../ops.h"

struct mtk_adsp_ipi;

struct mtk_adsp_ipi_ops {
	void (*handle_recv)(struct mtk_adsp_ipi *ipc);
};

struct mtk_adsp_ipi {
	struct mbox_client cl;
	struct mbox_chan *chan;
	struct mtk_adsp_ipi_ops *ops;
	void *private_data;
};

struct adsp_mbox_ch_info {
	u32 id;
	u32 ipi_op_val;
	struct adsp_priv *priv;
};

#define DSP_MBOX_NUM	  3

#define ADSP_IPI_MBOX_REQ 0
#define ADSP_IPI_MBOX_RSP 1
#define ADSP_IPI_OP_REQ 0x1
#define ADSP_IPI_OP_RSP 0x2

void adsp_ipi_request(struct snd_sof_dev *sdev, int idx, struct mtk_adsp_ipi_ops *ops);
void *adsp_ipi_get_data(struct mtk_adsp_ipi *ipi);
int adsp_ipi_send(struct snd_sof_dev *sdev, int idx, uint32_t op);
#endif /* ADSP_IPC_H */
