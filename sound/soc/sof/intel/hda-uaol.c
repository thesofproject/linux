// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
//
// This file is provided under a dual BSD/GPLv2 license.  When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2026 Intel Corporation
//
// USB Audio Offload Link (UAOL) DAIs: the DSP drives a USB audio endpoint
// over the UAOL instead of the host.
//

#include <sound/pcm_params.h>
#include <sound/hdaudio_ext.h>
#include <sound/hda-mlink.h>
#include <sound/sof/ipc4/header.h>
#include <uapi/sound/sof/header.h>
#include "../ipc4-priv.h"
#include "../ipc4-topology.h"
#include "../sof-priv.h"
#include "../sof-audio.h"
#include "hda.h"

/* The link DMA is run like the one of the SSP and DMIC DAIs */
static const struct snd_soc_dai_ops uaol_dai_ops = {
	.hw_params = non_hda_dai_hw_params,
	.hw_free = hda_dai_hw_free,
	.trigger = hda_dai_trigger,
	.prepare = non_hda_dai_prepare,
};

void hda_uaol_set_dai_drv_ops(struct snd_sof_dev *sdev, struct snd_sof_dsp_ops *ops)
{
	const struct sof_intel_dsp_desc *chip;
	int i;

	chip = get_chip_info(sdev->pdata);

	if (chip->hw_ip_version >= SOF_INTEL_ACE_2_0) {
		for (i = 0; i < ops->num_drv; i++) {
			if (strstr(ops->drv[i].name, "UAOL"))
				ops->drv[i].ops = &uaol_dai_ops;
		}
	}
}
