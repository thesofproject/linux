// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
//
// Copyright(c) 2026 Intel Corporation

#include <linux/bits.h>
#include <sound/pcm.h>
#include <sound/control.h>
#include <sound/soc.h>
#include "sof-audio.h"
#include "sof-priv.h"
#include "ipc4-wov.h"

/* IPC4 PHRASE_DETECTED primary/extension field layout */
#define SOF_IPC4_PHRASE_WORD_ID_MASK	GENMASK(15, 0)
#define SOF_IPC4_PHRASE_WORD_ID_SHIFT	0
#define SOF_IPC4_PHRASE_SV_SCORE_MASK	GENMASK(15, 0)

/* PCM ID for WoV keyword detection capture stream */
#define SOF_WOV_PCM_ID	11

/* kcontrol names as defined in the dmic-wov feature topology */
#define SOF_WOV_KEYWORD_ID_CTL	"wov_trigger_id"
#define SOF_WOV_EVENT_CTL	"wov_event"

/* sof_ipc4_wov_notify_kcontrol_by_name - Locate mixer control by name and notify */
static void sof_ipc4_wov_notify_kcontrol_by_name(struct snd_card *card,
						 const char *name)
{
	struct snd_ctl_elem_id id;
	struct snd_kcontrol *kctl;

	if (!card || !name)
		return;

	memset(&id, 0, sizeof(id));
	id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
	strscpy(id.name, name, sizeof(id.name));

	kctl = snd_ctl_find_id(card, &id);
	if (kctl)
		snd_ctl_notify_one(card, SNDRV_CTL_EVENT_MASK_VALUE, kctl, 0);
}

/* sof_ipc4_wov_notify_kcontrols - Iterate SOF controls and notify userspace */
static void sof_ipc4_wov_notify_kcontrols(struct snd_sof_dev *sdev)
{
	static const char * const wov_ctl_names[] = {
		SOF_WOV_KEYWORD_ID_CTL,
		SOF_WOV_EVENT_CTL,
	};
	struct snd_sof_control *scontrol;
	int i;

	list_for_each_entry(scontrol, &sdev->kcontrol_list, list) {
		for (i = 0; i < ARRAY_SIZE(wov_ctl_names); i++) {
			if (strcmp(scontrol->name, wov_ctl_names[i]))
				continue;

			/* Force firmware re-read on next .get */
			scontrol->comp_data_dirty = true;

			if (scontrol->scomp && scontrol->scomp->card) {
				sof_ipc4_wov_notify_kcontrol_by_name(
					scontrol->scomp->card->snd_card,
					scontrol->name);
			}
		}
	}
}

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

	/* 1. Notify WoV kcontrols */
	sof_ipc4_wov_notify_kcontrols(sdev);

	/* 2. Unblock capture PCM stream */
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
