// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
//
// Copyright(c) 2025 Intel Corporation
//
// Author: Peter Ujfalusi <peter.ujfalusi@linux.intel.com>
//
// IPC4 specific glue of the SOF audio client
//

#include <sound/soc.h>
#include <sound/sof/ipc4/header.h>

#include "sof-audio.h"
#include "ipc4-priv.h"
#include "sof-client.h"
#include "sof-client-audio.h"

static void sof_audio_client_ipc4_module_notification(struct sof_client_dev *cdev,
						      struct sof_ipc4_msg *ipc4_msg)
{
	struct sof_audio_client_pdata *pdata = dev_get_platdata(&cdev->auxdev.dev);
	struct sof_ipc4_notify_module_data *data = ipc4_msg->data_ptr;

	/* The component is only available while the sound card is bound */
	if (!pdata->component)
		return;

	switch (data->event_id & SOF_IPC4_NOTIFY_MODULE_EVENTID_SOF_MAGIC_MASK) {
	case SOF_IPC4_NOTIFY_MODULE_EVENTID_ALSA_MAGIC_VAL:
	{
		const struct sof_ipc_tplg_ops *tplg_ops;

		/* ALSA kcontrol notification */
		tplg_ops = snd_sof_component_get_tplg_ops(pdata->component);
		if (tplg_ops && tplg_ops->control->update)
			tplg_ops->control->update(pdata->component, ipc4_msg);

		break;
	}
	case SOF_IPC4_NOTIFY_MODULE_EVENTID_COMPR_MAGIC_VAL:
		sof_ipc4_compr_drain_done(pdata->component, ipc4_msg);
		break;
	default:
		break;
	}
}

static void sof_audio_client_ipc4_rx_notification(struct sof_client_dev *cdev,
						  void *msg_buf)
{
	struct sof_ipc4_msg *ipc4_msg = msg_buf;

	if (!ipc4_msg || !ipc4_msg->data_ptr)
		return;

	switch (SOF_IPC4_NOTIFICATION_TYPE_GET(ipc4_msg->primary)) {
	case SOF_IPC4_NOTIFY_MODULE_NOTIFICATION:
		sof_audio_client_ipc4_module_notification(cdev, ipc4_msg);
		break;
	default:
		break;
	}
}

const struct sof_audio_client_ipc_ops sof_audio_client_ipc4_ops = {
	.rx_notification = sof_audio_client_ipc4_rx_notification,
};
