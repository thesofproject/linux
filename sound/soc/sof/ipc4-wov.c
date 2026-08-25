// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
//
// Copyright(c) 2026 Intel Corporation

#include <linux/bits.h>
#include <sound/pcm.h>
#include "sof-audio.h"
#include "ipc4-wov.h"

/* IPC4 PHRASE_DETECTED primary/extension field layout */
#define SOF_IPC4_PHRASE_WORD_ID_MASK	GENMASK(15, 0)
#define SOF_IPC4_PHRASE_WORD_ID_SHIFT	0
#define SOF_IPC4_PHRASE_SV_SCORE_MASK	GENMASK(15, 0)

/* PCM ID for WoV keyword detection capture stream */
#define SOF_WOV_PCM_ID	11

void sof_ipc4_wov_phrase_detected(struct snd_sof_dev *sdev,
				  struct sof_ipc4_msg *ipc4_msg)
{
	struct snd_sof_pcm *spcm;
	struct snd_pcm_substream *substream;
	u32 word_id = (ipc4_msg->primary & SOF_IPC4_PHRASE_WORD_ID_MASK)
			>> SOF_IPC4_PHRASE_WORD_ID_SHIFT;
	u32 sv_score = ipc4_msg->extension & SOF_IPC4_PHRASE_SV_SCORE_MASK;

	dev_dbg(sdev->dev, "WoV: PHRASE_DETECTED word_id=%u sv_score=%u\n",
		word_id, sv_score);

	pm_wakeup_event(sdev->dev, 2000);

	list_for_each_entry(spcm, &sdev->pcm_list, list) {
		if (le32_to_cpu(spcm->pcm.pcm_id) != SOF_WOV_PCM_ID)
			continue;

		substream = spcm->stream[SNDRV_PCM_STREAM_CAPTURE].substream;
		if (!substream || !substream->runtime) {
			dev_warn(sdev->dev, "WoV: PCM %d not open\n",
				 SOF_WOV_PCM_ID);
			return;
		}

		snd_sof_pcm_period_elapsed(substream);
		return;
	}

	dev_warn(sdev->dev, "WoV: PHRASE_DETECTED but PCM %d not found\n",
		 SOF_WOV_PCM_ID);
}
