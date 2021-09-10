/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause) */
/*
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * Copyright(c) 2020 Intel Corporation. All rights reserved.
 */

#ifndef __INCLUDE_SOUND_SOF_TOPOLOGY2_H__
#define __INCLUDE_SOUND_SOF_TOPOLOGY2_H__

#include <sound/sof/ipc4/header.h>

struct sof_copier_gateway_cfg {
	uint32_t node_id; /**< ID of Gateway Node */

	uint32_t dma_buffer_size; /**< Preferred Gateway DMA buffer size (in bytes) */

	/* Length of gateway node configuration blob specified in #config_data */
	uint32_t config_length;

	uint32_t config_data[0]; /**< Gateway node configuration blob */
};

struct sof_ipc4_module_copier {
	struct sof_ipc4_basic_module_cfg base_config; /**< common config for all comps */
	struct sof_ipc4_audio_format out_format;
	uint32_t copier_feature_mask;
	struct sof_copier_gateway_cfg gtw_cfg;
};

struct sof_ipc4_pipeline {
	struct sof_ipc_pipe_new pipe_new;
	uint32_t lp_mode;	/**< low power mode */
	uint32_t mem_usage;
	int state;
};

struct sof_ipc4_host {
	struct snd_soc_component *scomp;
	struct sof_ipc4_module_copier copier;
	u32 *copier_config;
	uint32_t ipc_config_size;
	void *ipc_config_data;
};

struct sof_ipc4_dai {
	struct sof_ipc4_module_copier copier;
	uint32_t *copier_config;
	uint32_t ipc_config_size;
	void *ipc_config_data;
};

struct sof_ipc4_gain_data {
	uint32_t channels;
	uint32_t init_val;
	uint32_t curve_type;
	uint32_t reserved;
	uint32_t curve_duration;
} __aligned(8);

struct sof_ipc4_gain {
	struct sof_ipc4_basic_module_cfg base_config;
	struct sof_ipc4_gain_data data;
};

enum sof_ipc4_mixer_type {
	sof_ipc4_mix_in,
	sof_ipc4_mix_out
};

struct sof_ipc4_mixer {
	struct sof_ipc4_basic_module_cfg base_config;
	enum sof_ipc4_mixer_type type;
};

int sof_ipc4_widget_load_dai(struct snd_soc_component *scomp, int index,
			     struct snd_sof_widget *swidget,
			     struct snd_soc_tplg_dapm_widget *tw,
			     struct snd_sof_dai *dai);
int sof_ipc4_widget_load_pcm(struct snd_soc_component *scomp, int index,
			     struct snd_sof_widget *swidget,
			     enum sof_ipc_stream_direction dir,
			     struct snd_soc_tplg_dapm_widget *tw);
int sof_ipc4_widget_load_pipeline(struct snd_soc_component *scomp, int index,
				  struct snd_sof_widget *swidget,
				  struct snd_soc_tplg_dapm_widget *tw);
int sof_ipc4_widget_load_pga(struct snd_soc_component *scomp, int index,
			     struct snd_sof_widget *swidget,
			     struct snd_soc_tplg_dapm_widget *tw);
int sof_ipc4_widget_load_mixer(struct snd_soc_component *scomp, int index,
			       struct snd_sof_widget *swidget,
			       struct snd_soc_tplg_dapm_widget *tw);
int sof_ipc4_control_load_volume(struct snd_soc_component *scomp,
				 struct snd_sof_control *scontrol,
				 struct snd_kcontrol_new *kc,
				 struct snd_soc_tplg_ctl_hdr *hdr);
#endif
