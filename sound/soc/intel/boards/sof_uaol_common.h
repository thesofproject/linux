/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright(c) 2026 Intel Corporation.
 */

#ifndef __SOF_UAOL_COMMON_H
#define __SOF_UAOL_COMMON_H

#include <linux/bitops.h>
#include <sound/pcm.h>
#include <sound/soc-acpi.h>

/*
 * Each UAOL link has a number of streams per direction, and every one of
 * them gets a backend DAI link of its own, unidirectional: the backend
 * hw_params fixup is called without the stream direction, so a duplex link
 * could not tell which of the two DAI copiers of the topology describes the
 * stream being configured. Both names take the UAOL link index and then the
 * ordinal of the stream among those of its direction. The ordinal only tells
 * the backends apart, it is not the UAOL stream the backend ends up on.
 *
 * The backend names are abbreviated: the topology's DAI copier widget is
 * named "dai-copier.UAOL.<backend>.<direction>", and alsa-lib silently drops
 * the widget's tuples once that exceeds 37 characters.
 */
#define SOF_UAOL_LINK_NAME(dir) \
	((dir) == SNDRV_PCM_STREAM_PLAYBACK ? "UAOL%d-Play%d" : "UAOL%d-Cap%d")
#define SOF_UAOL_CPU_DAI_NAME(dir) \
	((dir) == SNDRV_PCM_STREAM_PLAYBACK ? "UAOL%d Playback Pin%d" : "UAOL%d Capture Pin%d")

static inline unsigned int
sof_uaol_stream_num(const struct snd_soc_acpi_mach_params *mach_params, int dir)
{
	return dir == SNDRV_PCM_STREAM_PLAYBACK ? mach_params->uaol_playback_num :
						  mach_params->uaol_capture_num;
}

/* Backend DAI links of all the streams of all the UAOL links in the mask */
static inline unsigned int
sof_uaol_link_count(const struct snd_soc_acpi_mach_params *mach_params)
{
	return hweight32(mach_params->uaol_link_mask) *
	       (mach_params->uaol_playback_num + mach_params->uaol_capture_num);
}

#endif /* __SOF_UAOL_COMMON_H */
