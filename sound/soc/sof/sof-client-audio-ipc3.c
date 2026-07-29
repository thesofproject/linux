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
#include <sound/sof/stream.h>
#include <trace/events/sof.h>

#include "sof-audio.h"
#include "sof-client.h"
#include "sof-client-audio.h"

/* IPC stream position. */
static void sof_audio_client_ipc3_period_elapsed(struct sof_client_dev *cdev,
						 struct snd_soc_component *component,
						 u32 msg_id)
{
	struct snd_sof_dev *sdev = sof_client_dev_to_sof_dev(cdev);
	struct snd_sof_pcm_stream *stream;
	struct sof_ipc_stream_posn posn;
	struct snd_sof_pcm *spcm;
	int direction, ret;

	spcm = snd_sof_find_spcm_comp(component, msg_id, &direction);
	if (!spcm) {
		/* The stream is not owned by this component */
		dev_dbg(&cdev->auxdev.dev,
			"period elapsed for unknown stream, msg_id %d\n", msg_id);
		return;
	}

	stream = &spcm->stream[direction];
	ret = sof_client_ipc_msg_data(cdev, stream, &posn, sizeof(posn));
	if (ret < 0) {
		dev_warn(&cdev->auxdev.dev,
			 "failed to read stream position: %d\n", ret);
		return;
	}

	trace_sof_ipc3_period_elapsed_position(sdev, &posn);

	memcpy(&stream->posn, &posn, sizeof(posn));

	if (spcm->pcm.compress)
		snd_sof_compr_fragment_elapsed(stream->cstream);
	else if (stream->substream->runtime &&
		 !stream->substream->runtime->no_period_wakeup)
		/* only inform ALSA for period_wakeup mode */
		snd_sof_pcm_period_elapsed(stream->substream);
}

/* DSP notifies host of an XRUN within FW */
static void sof_audio_client_ipc3_xrun(struct sof_client_dev *cdev,
				       struct snd_soc_component *component,
				       u32 msg_id)
{
	struct snd_sof_pcm_stream *stream;
	struct sof_ipc_stream_posn posn;
	struct snd_sof_pcm *spcm;
	int direction, ret;

	spcm = snd_sof_find_spcm_comp(component, msg_id, &direction);
	if (!spcm) {
		/* The stream is not owned by this component */
		dev_dbg(&cdev->auxdev.dev, "XRUN for unknown stream, msg_id %d\n",
			msg_id);
		return;
	}

	stream = &spcm->stream[direction];
	ret = sof_client_ipc_msg_data(cdev, stream, &posn, sizeof(posn));
	if (ret < 0) {
		dev_warn(&cdev->auxdev.dev,
			 "failed to read overrun position: %d\n", ret);
		return;
	}

	dev_dbg(&cdev->auxdev.dev, "posn XRUN: host %llx comp %d size %d\n",
		posn.host_posn, posn.xrun_comp_id, posn.xrun_size);

#if defined(CONFIG_SND_SOC_SOF_DEBUG_XRUN_STOP)
	/* stop PCM on XRUN - used for pipeline debug */
	memcpy(&stream->posn, &posn, sizeof(posn));
	snd_pcm_stop_xrun(stream->substream);
#endif
}

/* stream notifications from firmware */
static void sof_audio_client_ipc3_stream_message(struct sof_client_dev *cdev,
						 struct sof_ipc_cmd_hdr *hdr)
{
	struct sof_audio_client_pdata *pdata = dev_get_platdata(&cdev->auxdev.dev);
	u32 msg_type = hdr->cmd & SOF_CMD_TYPE_MASK;
	u32 msg_id = SOF_IPC_MESSAGE_ID(hdr->cmd);

	/* The component is only available while the sound card is bound */
	if (!pdata->component)
		return;

	switch (msg_type) {
	case SOF_IPC_STREAM_POSITION:
		sof_audio_client_ipc3_period_elapsed(cdev, pdata->component, msg_id);
		break;
	case SOF_IPC_STREAM_TRIG_XRUN:
		sof_audio_client_ipc3_xrun(cdev, pdata->component, msg_id);
		break;
	default:
		dev_err(&cdev->auxdev.dev, "unhandled stream message %#x\n",
			msg_id);
		break;
	}
}

/* component notifications from firmware */
static void sof_audio_client_ipc3_comp_notification(struct sof_client_dev *cdev,
						    struct sof_ipc_cmd_hdr *hdr)
{
	struct sof_audio_client_pdata *pdata = dev_get_platdata(&cdev->auxdev.dev);
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
		tplg_ops->control->update(pdata->component, hdr);
}

static void sof_audio_client_ipc3_rx_notification(struct sof_client_dev *cdev,
						  void *msg_buf)
{
	struct sof_ipc_cmd_hdr *hdr = msg_buf;

	switch (hdr->cmd & SOF_GLB_TYPE_MASK) {
	case SOF_IPC_GLB_COMP_MSG:
		sof_audio_client_ipc3_comp_notification(cdev, hdr);
		break;
	case SOF_IPC_GLB_STREAM_MSG:
		sof_audio_client_ipc3_stream_message(cdev, hdr);
		break;
	default:
		break;
	}
}

const struct sof_audio_client_ipc_ops sof_audio_client_ipc3_ops = {
	.rx_notification = sof_audio_client_ipc3_rx_notification,
};
