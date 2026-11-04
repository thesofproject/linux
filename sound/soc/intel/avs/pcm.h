/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright(c) 2024 Intel Corporation
 *
 * Authors: Cezary Rojewski <cezary.rojewski@intel.com>
 *          Amadeusz Slawinski <amadeuszx.slawinski@linux.intel.com>
 */

#ifndef __SOUND_SOC_INTEL_AVS_PCM_H
#define __SOUND_SOC_INTEL_AVS_PCM_H

#include <linux/device.h>
#include <sound/hdaudio_ext.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc-acpi.h>
#include <sound/soc-acpi-intel-match.h>
#include <sound/soc-component.h>
#include <sound/soc-dai.h>

struct avs_dma_data {
	struct avs_tplg_path_template *template;
	struct avs_path *path;
	struct avs_dev *adev;

	/* LINK-stream utilized in BE operations while HOST in FE ones. */
	union {
		struct hdac_ext_stream *link_stream;
		struct hdac_ext_stream *host_stream;
	};

	struct snd_pcm_hw_constraint_list rate_list;
	struct snd_pcm_hw_constraint_list channels_list;
	struct snd_pcm_hw_constraint_list sample_bits_list;

	struct work_struct period_elapsed_work;
	struct hdac_ext_link *link;
	struct sdw_stream_runtime *sdw_stream;
	struct sdw_cdns_pdi *pdi;
	bool sdw_suspended;
	struct snd_pcm_substream *substream;
};

void avs_period_elapsed(struct snd_pcm_substream *substream);
int avs_dai_startup(struct snd_pcm_substream *substream, struct snd_soc_dai *dai);
void avs_dai_shutdown(struct snd_pcm_substream *substream, struct snd_soc_dai *dai);
int avs_dai_hw_params(struct snd_pcm_substream *substream,
		      struct snd_pcm_hw_params *fe_params,
		      struct snd_pcm_hw_params *be_params,
		      struct snd_soc_dai *dai, int dma_id);
int avs_dai_be_hw_params(struct snd_pcm_substream *substream,
			 struct snd_pcm_hw_params *be_params,
			 struct snd_soc_dai *dai, int dma_id);
int avs_dai_prepare(struct snd_pcm_substream *substream, struct snd_soc_dai *dai);

extern struct snd_soc_component_driver avs_component_driver;
int avs_register_component(struct device *dev, const char *name,
			   struct snd_soc_component_driver *drv,
			   struct snd_soc_dai_driver *cpu_dais, int num_cpu_dais);

int avs_register_dmic_component(struct avs_dev *adev, const char *name);
int avs_register_i2s_component(struct avs_dev *adev, const char *name, unsigned long port_mask,
			       unsigned long *tdms);
int avs_register_hda_component(struct avs_dev *adev, const char *name);

#endif
