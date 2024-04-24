/* SPDX-License-Identifier: GPL-2.0-only
 *  Copyright (c) 2020 Intel Corporation
 */

/*
 *  sof_sdw_common.h - prototypes for common helpers
 */

#ifndef SND_SOC_SOF_SDW_COMMON_H
#define SND_SOC_SOF_SDW_COMMON_H

#include <linux/bits.h>
#include <linux/types.h>
#include <sound/soc.h>
#include "sof_hdmi_common.h"
#include "../../sdw_utils/soc_sdw_utils.h"

#define MAX_HDMI_NUM 4
#define SDW_UNUSED_DAI_ID -1
#define SDW_JACK_OUT_DAI_ID 0
#define SDW_JACK_IN_DAI_ID 1
#define SDW_AMP_OUT_DAI_ID 2
#define SDW_AMP_IN_DAI_ID 3
#define SDW_DMIC_DAI_ID 4
#define SDW_MAX_CPU_DAIS 16
#define SDW_INTEL_BIDIR_PDI_BASE 2

#define SDW_MAX_LINKS		4

/* 8 combinations with 4 links + unused group 0 */
#define SDW_MAX_GROUPS 9

enum {
	SOF_PRE_TGL_HDMI_COUNT = 3,
	SOF_TGL_HDMI_COUNT = 4,
};

enum {
	SOF_I2S_SSP0 = BIT(0),
	SOF_I2S_SSP1 = BIT(1),
	SOF_I2S_SSP2 = BIT(2),
	SOF_I2S_SSP3 = BIT(3),
	SOF_I2S_SSP4 = BIT(4),
	SOF_I2S_SSP5 = BIT(5),
};

#define SOF_JACK_JDSRC(quirk)		((quirk) & GENMASK(3, 0))
#define SOF_SDW_FOUR_SPK		BIT(4)
#define SOF_SDW_TGL_HDMI		BIT(5)
#define SOF_SDW_PCH_DMIC		BIT(6)
#define SOF_SSP_PORT(x)		(((x) & GENMASK(5, 0)) << 7)
#define SOF_SSP_GET_PORT(quirk)	(((quirk) >> 7) & GENMASK(5, 0))
/* Deprecated and no longer supported by the code */
#define SOF_SDW_NO_AGGREGATION		BIT(14)
/* If a CODEC has an optional speaker output, this quirk will enable it */
#define SOF_CODEC_SPKR			BIT(15)

/* BT audio offload: reserve 3 bits for future */
#define SOF_BT_OFFLOAD_SSP_SHIFT	15
#define SOF_BT_OFFLOAD_SSP_MASK	(GENMASK(17, 15))
#define SOF_BT_OFFLOAD_SSP(quirk)	\
	(((quirk) << SOF_BT_OFFLOAD_SSP_SHIFT) & SOF_BT_OFFLOAD_SSP_MASK)
#define SOF_SSP_BT_OFFLOAD_PRESENT	BIT(18)

#define SOF_SDW_DAI_TYPE_JACK		0
#define SOF_SDW_DAI_TYPE_AMP		1
#define SOF_SDW_DAI_TYPE_MIC		2

struct intel_mc_ctx {
	struct sof_hdmi_private hdmi;
	/* To store SDW Pin index for each SoundWire link */
	unsigned int sdw_pin_index[SDW_MAX_LINKS];
};

extern unsigned long sof_sdw_quirk;

/* generic HDMI support */
int sof_sdw_hdmi_init(struct snd_soc_pcm_runtime *rtd);

int sof_sdw_hdmi_card_late_probe(struct snd_soc_card *card);

/* MAXIM codec support */
int sof_sdw_maxim_init(struct snd_soc_card *card,
		       struct snd_soc_dai_link *dai_links,
		       struct sof_sdw_codec_info *info,
		       bool playback);

/* CS42L43 support */
int sof_sdw_cs42l43_spk_init(struct snd_soc_card *card,
			     struct snd_soc_dai_link *dai_links,
			     struct sof_sdw_codec_info *info,
			     bool playback);

/* CS AMP support */
int sof_sdw_cs_amp_init(struct snd_soc_card *card,
			struct snd_soc_dai_link *dai_links,
			struct sof_sdw_codec_info *info,
			bool playback);

/* dai_link init callbacks */

int cs42l42_rtd_init(struct snd_soc_pcm_runtime *rtd);
int cs42l43_hs_rtd_init(struct snd_soc_pcm_runtime *rtd);
int cs42l43_spk_rtd_init(struct snd_soc_pcm_runtime *rtd);
int cs42l43_dmic_rtd_init(struct snd_soc_pcm_runtime *rtd);
int cs_spk_rtd_init(struct snd_soc_pcm_runtime *rtd);
int maxim_spk_rtd_init(struct snd_soc_pcm_runtime *rtd);

#endif
