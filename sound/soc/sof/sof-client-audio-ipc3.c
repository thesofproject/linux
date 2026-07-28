// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
//
// Copyright(c) 2025 Intel Corporation
//
// Author: Peter Ujfalusi <peter.ujfalusi@linux.intel.com>
//
// IPC3 specific glue of the SOF audio client
//

#include <sound/soc.h>
#include <sound/sof/header.h>

#include "sof-audio.h"
#include "sof-client.h"
#include "sof-client-audio.h"

/* component notifications from firmware */
static void sof_audio_client_ipc3_comp_notification(struct sof_client_dev *cdev,
						    struct sof_ipc_cmd_hdr *hdr)
{
	struct sof_audio_client_pdata *pdata = dev_get_platdata(&cdev->auxdev.dev);
	struct snd_sof_dev *sdev = sof_client_dev_to_sof_dev(cdev);
	const struct sof_ipc_tplg_ops *tplg_ops;
	u32 msg_type = hdr->cmd & SOF_CMD_TYPE_MASK;

	/* The component is only available while the sound card is bound */
	if (!pdata->component)
		return;

	switch (msg_type) {
	case SOF_IPC_COMP_GET_VALUE:
	case SOF_IPC_COMP_GET_DATA:
		break;
	default:
		dev_err(&cdev->auxdev.dev, "unhandled component message %#x\n",
			msg_type);
		return;
	}

	tplg_ops = snd_sof_component_get_tplg_ops(pdata->component);
	if (tplg_ops && tplg_ops->control->update)
		tplg_ops->control->update(sdev, hdr);
}

static void sof_audio_client_ipc3_rx_notification(struct sof_client_dev *cdev,
						  void *msg_buf)
{
	struct sof_ipc_cmd_hdr *hdr = msg_buf;

	switch (hdr->cmd & SOF_GLB_TYPE_MASK) {
	case SOF_IPC_GLB_COMP_MSG:
		sof_audio_client_ipc3_comp_notification(cdev, hdr);
		break;
	default:
		break;
	}
}

const struct sof_audio_client_ipc_ops sof_audio_client_ipc3_ops = {
	.rx_notification = sof_audio_client_ipc3_rx_notification,
};
