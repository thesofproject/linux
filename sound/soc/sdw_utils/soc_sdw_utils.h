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

#define SOF_SDW_MAX_DAI_NUM             8

struct sof_sdw_codec_info;

struct sof_sdw_dai_info {
	const bool direction[2]; /* playback & capture support */
	const char *dai_name;
	const int dai_type;
	const int dailink[2]; /* dailink id for each direction */
	const struct snd_kcontrol_new *controls;
	const int num_controls;
	const struct snd_soc_dapm_widget *widgets;
	const int num_widgets;
	int  (*init)(struct snd_soc_card *card,
		     struct snd_soc_dai_link *dai_links,
		     struct sof_sdw_codec_info *info,
		     bool playback);
	int (*exit)(struct snd_soc_card *card, struct snd_soc_dai_link *dai_link);
	int (*rtd_init)(struct snd_soc_pcm_runtime *rtd, struct snd_soc_dai *dai);
	bool rtd_init_done; /* Indicate that the rtd_init callback is done */
	unsigned long quirk;
};

struct sof_sdw_codec_info {
	const int part_id;
	const int version_id;
	const char *codec_name;
	int amp_num;
	const u8 acpi_id[ACPI_ID_LEN];
	const bool ignore_internal_dmic;
	const struct snd_soc_ops *ops;
	struct sof_sdw_dai_info dais[SOF_SDW_MAX_DAI_NUM];
	const int dai_num;

	int (*codec_card_late_probe)(struct snd_soc_card *card);

	int  (*count_sidecar)(struct snd_soc_card *card,
			      int *num_dais, int *num_devs);
	int  (*add_sidecar)(struct snd_soc_card *card,
			    struct snd_soc_dai_link **dai_links,
			    struct snd_soc_codec_conf **codec_conf);
};

int asoc_sdw_startup(struct snd_pcm_substream *substream);
int asoc_sdw_prepare(struct snd_pcm_substream *substream);
int asoc_sdw_prepare(struct snd_pcm_substream *substream);
int asoc_sdw_trigger(struct snd_pcm_substream *substream, int cmd);
int asoc_sdw_hw_params(struct snd_pcm_substream *substream,
		       struct snd_pcm_hw_params *params);
int asoc_sdw_hw_free(struct snd_pcm_substream *substream);
void asoc_sdw_shutdown(struct snd_pcm_substream *substream);

bool asoc_sdw_is_unique_device(const struct snd_soc_acpi_link_adr *adr_link,
			       unsigned int sdw_version,
			       unsigned int mfg_id,
			       unsigned int part_id,
			       unsigned int class_id,
			       int index_in_link);

const char *asoc_sdw_get_codec_name(struct device *dev,
				    const struct sof_sdw_codec_info *codec_info,
				    const struct snd_soc_acpi_link_adr *adr_link,
				    int adr_index);

#endif
