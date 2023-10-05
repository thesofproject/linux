/* SPDX-License-Identifier: GPL-2.0-only
 *  Copyright (c) 2024 Advanced Micro Devices, Inc. All rights reserved
 */

/*
 *  sof_amd_sdw_common.h - prototypes for common helpers
 */

#ifndef SND_SOC_SOF_AMD_SDW_COMMON_H
#define SND_SOC_SOF_AMD_SDW_COMMON_H

#include <linux/bits.h>
#include <linux/types.h>
#include <sound/soc.h>

#define MAX_NO_PROPS 2
#define SDW_UNUSED_DAI_ID -1
#define SDW_JACK_OUT_DAI_ID 0
#define SDW_JACK_IN_DAI_ID 1
#define SDW_AMP_OUT_DAI_ID 2
#define SDW_AMP_IN_DAI_ID 3
#define SDW_DMIC_DAI_ID 4
#define SDW_MAX_CPU_DAIS 8
#define SDW_MAX_LINKS 2

#define SDW_MAX_GROUPS 9

#define SOF_JACK_JDSRC(quirk)		((quirk) & GENMASK(3, 0))
#define SOF_SDW_FOUR_SPK		BIT(4)
#define SOF_SDW_ACP_DMIC		BIT(5)
#define SOF_SDW_NO_AGGREGATION		BIT(6)

#define SOF_SDW_DAI_TYPE_JACK		0
#define SOF_SDW_DAI_TYPE_AMP		1
#define SOF_SDW_DAI_TYPE_MIC		2

#define SOF_SDW_MAX_DAI_NUM		3

#define AMD_SDW0	0
#define AMD_SDW1	1
#define SW0_AUDIO0_TX	0
#define SW0_AUDIO1_TX	1
#define SW0_AUDIO2_TX	2

#define SW0_AUDIO0_RX	3
#define SW0_AUDIO1_RX	4
#define SW0_AUDIO2_RX	5

#define SW1_AUDIO0_TX	0
#define SW1_AUDIO0_RX	1

struct sof_sdw_codec_info;

struct sof_sdw_dai_info {
	const bool direction[2]; /* playback & capture support */
	const char *dai_name;
	const int dai_type;
	const int dailink[2]; /* dailink id for each direction */
	int  (*init)(struct snd_soc_card *card,
		     const struct snd_soc_acpi_link_adr *link,
		     struct snd_soc_dai_link *dai_links,
		     struct sof_sdw_codec_info *info,
		     bool playback);
	int (*exit)(struct snd_soc_card *card, struct snd_soc_dai_link *dai_link);
	int (*rtd_init)(struct snd_soc_pcm_runtime *rtd);
	bool rtd_init_done; /* Indicate that the rtd_init callback is done */
};

struct sof_sdw_codec_info {
	const int part_id;
	const int version_id;
	const char *codec_name;
	int amp_num;
	const u8 acpi_id[ACPI_ID_LEN];
	const bool ignore_acp_dmic;
	const struct snd_soc_ops *ops;
	struct sof_sdw_dai_info dais[SOF_SDW_MAX_DAI_NUM];
	const int dai_num;

	int (*codec_card_late_probe)(struct snd_soc_card *card);
};

struct mc_private {
	struct snd_soc_jack sdw_headset;
	struct device *headset_codec_dev; /* only one headset per card */
	struct device *amp_dev1, *amp_dev2;
	bool append_dai_type;
	bool ignore_acp_dmic;
};

extern unsigned long sof_sdw_quirk;

struct snd_soc_dai *amd_get_codec_dai_by_name(struct snd_soc_pcm_runtime *rtd,
					      const char * const dai_name[],
					      int num_dais);

int sdw_startup(struct snd_pcm_substream *substream);
int sdw_prepare(struct snd_pcm_substream *substream);
int sdw_trigger(struct snd_pcm_substream *substream, int cmd);
int sdw_hw_params(struct snd_pcm_substream *substream,
		  struct snd_pcm_hw_params *params);
int sdw_hw_free(struct snd_pcm_substream *substream);
void sdw_shutdown(struct snd_pcm_substream *substream);

/* DMIC support */
int sof_sdw_dmic_init(struct snd_soc_pcm_runtime *rtd);

/* RT711 support */
int sof_sdw_rt711_init(struct snd_soc_card *card,
		       const struct snd_soc_acpi_link_adr *link,
		       struct snd_soc_dai_link *dai_links,
		       struct sof_sdw_codec_info *info,
		       bool playback);
int sof_sdw_rt711_exit(struct snd_soc_card *card, struct snd_soc_dai_link *dai_link);

/* RT711-SDCA support */
int sof_sdw_rt_sdca_jack_init(struct snd_soc_card *card,
			      const struct snd_soc_acpi_link_adr *link,
			      struct snd_soc_dai_link *dai_links,
			      struct sof_sdw_codec_info *info,
			      bool playback);
int sof_sdw_rt_sdca_jack_exit(struct snd_soc_card *card, struct snd_soc_dai_link *dai_link);

/* generic amp support */
int sof_sdw_rt_amp_init(struct snd_soc_card *card,
			const struct snd_soc_acpi_link_adr *link,
			struct snd_soc_dai_link *dai_links,
			struct sof_sdw_codec_info *info,
			bool playback);
int sof_sdw_rt_amp_exit(struct snd_soc_card *card, struct snd_soc_dai_link *dai_link);

/* dai_link init callbacks */
int rt711_rtd_init(struct snd_soc_pcm_runtime *rtd);
int rt712_sdca_dmic_rtd_init(struct snd_soc_pcm_runtime *rtd);
int rt712_spk_rtd_init(struct snd_soc_pcm_runtime *rtd);
int rt715_rtd_init(struct snd_soc_pcm_runtime *rtd);
int rt715_sdca_rtd_init(struct snd_soc_pcm_runtime *rtd);
int rt_amp_spk_rtd_init(struct snd_soc_pcm_runtime *rtd);
int rt_sdca_jack_rtd_init(struct snd_soc_pcm_runtime *rtd);

#endif
