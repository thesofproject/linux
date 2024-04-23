/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * This file incorporates work covered by the following copyright notice:
 * Copyright (c) 2020 Intel Corporation
 * Copyright(c) 2024 Advanced Micro Devices, Inc.
 *
 */

#ifndef SOC_SDW_UTILS_H
#define SOC_SDW_UTILS_H

#include <sound/soc.h>
#include <sound/soc-acpi.h>

#define SOC_SDW_MAX_DAI_NUM		8
#define MAX_NO_PROPS 2
#define SOC_JACK_JDSRC(quirk)		((quirk) & GENMASK(3, 0))
#define SDW_UNUSED_DAI_ID		-1
#define SDW_JACK_OUT_DAI_ID		0
#define SDW_JACK_IN_DAI_ID		1
#define SDW_AMP_OUT_DAI_ID		2
#define SDW_AMP_IN_DAI_ID		3
#define SDW_DMIC_DAI_ID			4

#define SOC_SDW_DAI_TYPE_JACK		0
#define SOC_SDW_DAI_TYPE_AMP		1
#define SOC_SDW_DAI_TYPE_MIC		2
#define SOC_SDW_CODEC_SPKR		BIT(15)

struct soc_sdw_codec_info;

struct soc_sdw_dai_info {
	const bool direction[2]; /* playback & capture support */
	const char *dai_name;
	const int dai_type;
	const int dailink[2]; /* dailink id for each direction */
	int  (*init)(struct snd_soc_card *card,
		     struct snd_soc_dai_link *dai_links,
		     struct soc_sdw_codec_info *info,
		     bool playback);
	int (*exit)(struct snd_soc_card *card, struct snd_soc_dai_link *dai_link);
	int (*rtd_init)(struct snd_soc_pcm_runtime *rtd);
	bool rtd_init_done; /* Indicate that the rtd_init callback is done */
	unsigned long quirk;
};

struct soc_sdw_codec_info {
	const int part_id;
	const int version_id;
	const char *codec_name;
	int amp_num;
	const u8 acpi_id[ACPI_ID_LEN];
	const bool ignore_internal_dmic;
	const struct snd_soc_ops *ops;
	struct soc_sdw_dai_info dais[SOC_SDW_MAX_DAI_NUM];
	const int dai_num;

	int (*codec_card_late_probe)(struct snd_soc_card *card);
};

struct mc_private {
	struct snd_soc_jack sdw_headset;
	struct device *headset_codec_dev; /* only one headset per card */
	struct device *amp_dev1, *amp_dev2;
	void *intel_mc_private;
	void *amd_mc_private;
	bool append_dai_type;
	bool ignore_internal_dmic;
	unsigned long sdw_quirk;
};

extern struct soc_sdw_codec_info codec_info_list[];
int get_sdw_codec_info_list_size(void);

int asoc_sdw_startup(struct snd_pcm_substream *substream);
int asoc_sdw_prepare(struct snd_pcm_substream *substream);
int asoc_sdw_prepare(struct snd_pcm_substream *substream);
int asoc_sdw_trigger(struct snd_pcm_substream *substream, int cmd);
int asoc_sdw_hw_params(struct snd_pcm_substream *substream,
		       struct snd_pcm_hw_params *params);
int asoc_sdw_hw_free(struct snd_pcm_substream *substream);
void asoc_sdw_shutdown(struct snd_pcm_substream *substream);

struct snd_soc_dai *get_sdw_codec_dai_by_name(struct snd_soc_pcm_runtime *rtd,
					      const char * const dai_name[],
					      int num_dais);

const char *get_sdw_codec_name(struct device *dev,
			       const struct soc_sdw_codec_info *codec_info,
			       const struct snd_soc_acpi_link_adr *adr_link,
			       int adr_index);

bool is_sdw_unique_device(const struct snd_soc_acpi_link_adr *adr_link,
			  unsigned int sdw_version,
			  unsigned int mfg_id,
			  unsigned int part_id,
			  unsigned int class_id,
			  int index_in_link);

struct soc_sdw_codec_info *find_sdw_codec_info_part(const u64 adr);

struct soc_sdw_codec_info *find_sdw_codec_info_acpi(const u8 *acpi_id);

struct soc_sdw_codec_info *find_sdw_codec_info_dai(const char *dai_name,
						   int *dai_index);

int soc_sdw_rtd_init(struct snd_soc_pcm_runtime *rtd);

struct snd_soc_dai_link *mc_find_codec_dai_used(struct snd_soc_card *card,
						const char *dai_name);

void mc_dailink_exit_loop(struct snd_soc_card *card);

int soc_sdw_card_late_probe(struct snd_soc_card *card);

/* DMIC support */
int asoc_sdw_dmic_init(struct snd_soc_pcm_runtime *rtd);
int rt_sdw_dmic_rtd_init(struct snd_soc_pcm_runtime *rtd);
int rt700_sdw_rtd_init(struct snd_soc_pcm_runtime *rtd);
int rt711_sdw_rtd_init(struct snd_soc_pcm_runtime *rtd);
int rt712_sdw_spk_rtd_init(struct snd_soc_pcm_runtime *rtd);
int rt722_sdw_spk_rtd_init(struct snd_soc_pcm_runtime *rtd);
int rt5682_sdw_rtd_init(struct snd_soc_pcm_runtime *rtd);
int rt_amp_spk_rtd_init(struct snd_soc_pcm_runtime *rtd);
int rt_sdca_jack_rtd_init(struct snd_soc_pcm_runtime *rtd);
int cs42l42_sdw_rtd_init(struct snd_soc_pcm_runtime *rtd);
int cs42l43_sdw_hs_rtd_init(struct snd_soc_pcm_runtime *rtd);
int cs42l43_sdw_spk_rtd_init(struct snd_soc_pcm_runtime *rtd);
int cs42l43_sdw_dmic_rtd_init(struct snd_soc_pcm_runtime *rtd);
int cs_sdw_spk_rtd_init(struct snd_soc_pcm_runtime *rtd);
int maxim_sdw_spk_rtd_init(struct snd_soc_pcm_runtime *rtd);

int soc_sdw_rt_sdca_jack_exit(struct snd_soc_card *card,
			      struct snd_soc_dai_link *dai_link);

int soc_sdw_rt_sdca_jack_init(struct snd_soc_card *card,
			      struct snd_soc_dai_link *dai_links,
			      struct soc_sdw_codec_info *info,
			      bool playback);

int soc_sdw_rt711_exit(struct snd_soc_card *card, struct snd_soc_dai_link *dai_link);
int soc_sdw_rt711_init(struct snd_soc_card *card,
		       struct snd_soc_dai_link *dai_links,
		       struct soc_sdw_codec_info *info,
		       bool playback);

/* RT1308 I2S support */
extern struct snd_soc_ops soc_sdw_rt1308_i2s_ops;

/* generic amp support */
int soc_sdw_rt_amp_init(struct snd_soc_card *card,
			struct snd_soc_dai_link *dai_links,
			struct soc_sdw_codec_info *info,
			bool playback);
int soc_sdw_rt_amp_exit(struct snd_soc_card *card, struct snd_soc_dai_link *dai_link);

/* CS42L43 support */
int soc_sdw_cs42l43_spk_init(struct snd_soc_card *card,
			     struct snd_soc_dai_link *dai_links,
			     struct soc_sdw_codec_info *info,
			     bool playback);

/* CS AMP support */
int soc_sdw_cs_amp_init(struct snd_soc_card *card,
			struct snd_soc_dai_link *dai_links,
			struct soc_sdw_codec_info *info,
			bool playback);

/* MAXIM codec support */
int soc_sdw_maxim_init(struct snd_soc_card *card,
		       struct snd_soc_dai_link *dai_links,
		       struct soc_sdw_codec_info *info,
		       bool playback);

#endif
