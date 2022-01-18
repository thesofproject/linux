/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause) */
/*
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * Copyright(c) 2022 Intel Corporation. All rights reserved.
 */

#ifndef __INCLUDE_SOUND_SOF_IPC4_TOPOLOGY_H__
#define __INCLUDE_SOUND_SOF_IPC4_TOPOLOGY_H__

#include <sound/sof/ipc4/header.h>

#define SOF_IPC4_NODE_INDEX(x)	(x)
#define SOF_IPC4_NODE_TYPE(x)  ((x) << 8)

/**
 * struct sof_copier_gateway_cfg - IPC gateway configuration
 * @node_id: ID of Gateway Node
 * @dma_buffer_size: Preferred Gateway DMA buffer size (in bytes)
 * @config_length: Length of gateway node configuration blob specified in #config_data
 * config_data: Gateway node configuration blob
 */
struct sof_copier_gateway_cfg {
	uint32_t node_id;
	uint32_t dma_buffer_size;
	uint32_t config_length;
	uint32_t config_data[];
};

/**
 * struct sof_ipc4_module_copier - IPC data for copier
 * @base_config: Base configuration including input audio format
 * @out_format: Output audio format
 * @copier_feature_mask: Copier feature mask
 * @gtw_cfg: Gateway configuration
 */
struct sof_ipc4_module_copier {
	struct sof_ipc4_base_module_cfg base_config;
	struct sof_ipc4_audio_format out_format;
	uint32_t copier_feature_mask;
	struct sof_copier_gateway_cfg gtw_cfg;
};

/**
 * struct sof_ipc4_pipeline - pipeline config data
 * @priority: Priority of this pipeline
 * @lp_mode: Low power mode
 * @mem_usage: Memory usage
 * @state: Pipeline state
 */
struct sof_ipc4_pipeline {
	uint32_t priority;
	uint32_t lp_mode;
	uint32_t mem_usage;
	int state;
};

/**
 * struct sof_ipc4_available_audio_format - Available audio formats
 * @base_config: Available base config
 * @out_audio_fmt: Available output audio format
 * @ref_audio_fmt: Reference audio format to match runtime audio format
 */
struct sof_ipc4_available_audio_format {
	struct sof_ipc4_base_module_cfg *base_config;
	struct sof_ipc4_audio_format *out_audio_fmt;
	struct sof_ipc4_audio_format *ref_audio_fmt;
	int audio_fmt_num;
};

/**
 * struct sof_ipc4_copier - copier config data
 * @copier: IPC copier data
 * @copier_config: Copier + blob
 * @ipc_config_size: Size of copier_config
 * @available_fmt: Available audio format
 */
struct sof_ipc4_copier {
	struct sof_ipc4_module_copier copier;
	u32 *copier_config;
	uint32_t ipc_config_size;
	void *ipc_config_data;
	struct sof_ipc4_available_audio_format available_fmt;
};

/**
 * struct sof_ipc4_gain_data - IPC gain blob
 */
struct sof_ipc4_gain_data {
	uint32_t channels;
	uint32_t init_val;
	uint32_t curve_type;
	uint32_t reserved;
	uint32_t curve_duration;
} __aligned(8);

/**
 * struct sof_ipc4_gain - gain config data
 * @base_config: IPC base config data
 * @data: IPC gain blob
 * @available_fmt: Available audio format
 */
struct sof_ipc4_gain {
	struct sof_ipc4_base_module_cfg base_config;
	struct sof_ipc4_gain_data data;
	struct sof_ipc4_available_audio_format available_fmt;
};

enum sof_ipc4_mixer_type {
	sof_ipc4_mix_in,
	sof_ipc4_mix_out
};

/**
 * struct sof_ipc4_mixer - mixer config data
 * @base_config: IPC base config data
 * @type: mixer type (in or out)
 * @available_fmt: Available audio format
 */
struct sof_ipc4_mixer {
	struct sof_ipc4_base_module_cfg base_config;
	enum sof_ipc4_mixer_type type;
	struct sof_ipc4_available_audio_format available_fmt;
};

void sof_ipc4_dbg_audio_format(struct device *dev, struct sof_ipc4_audio_format *format,
			       size_t object_size, int num_format, const char *widget_name,
			       const char *function_name);

int snd_sof_load_topology2(struct snd_soc_component *scomp, const char *file);
#endif
