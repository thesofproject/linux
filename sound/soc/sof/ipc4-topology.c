// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
//
// This file is provided under a dual BSD/GPLv2 license.  When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2022 Intel Corporation. All rights reserved.
//
//
#include <uapi/sound/sof/tokens.h>
#include <sound/pcm_params.h>
#include "sof-priv.h"
#include "sof-audio.h"
#include "ipc4-ops.h"
#include "ipc4-topology.h"
#include "ops.h"

/* Full volume for default values */
#define VOL_ZERO_DB	0x7fffffff

struct sof_widget_data {
	int ctrl_type;
	int ipc_cmd;
	struct sof_abi_hdr *pdata;
	struct snd_sof_control *control;
};

static const struct sof_topology_token ipc4_sched_tokens[] = {
	{SOF_TKN_SCHED_LP_MODE, SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc4_pipeline, lp_mode)}
};

/* gain */
static const struct sof_topology_token gain_tokens[] = {
	{SOF_TKN_GAIN_RAMP_TYPE, SND_SOC_TPLG_TUPLE_TYPE_WORD,
		get_token_u32, offsetof(struct sof_ipc4_gain_data, curve_type)},
	{SOF_TKN_GAIN_RAMP_DURATION,
		SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc4_gain_data, curve_duration)},
	{SOF_TKN_GAIN_VAL, SND_SOC_TPLG_TUPLE_TYPE_WORD,
		get_token_u32, offsetof(struct sof_ipc4_gain_data, init_val)},
};

/* Generic components */
static const struct sof_topology_token ipc4_comp_tokens[] = {
	{SOF_TKN_COMP_CPC, SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc4_base_module_cfg, cpc)},
	{SOF_TKN_CAVS_AUDIO_FORMAT_IBS, SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc4_base_module_cfg, ibs)},
	{SOF_TKN_CAVS_AUDIO_FORMAT_OBS, SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc4_base_module_cfg, obs)},
	{SOF_TKN_COMP_IS_PAGES, SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc4_base_module_cfg, is_pages)},
};

static const struct sof_topology_token ipc4_mixer_tokens[] = {
	{SOF_TKN_MIXER_TYPE, SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc4_mixer, type)},
};

static const struct sof_topology_token ipc4_in_audio_format_tokens[] = {
	{SOF_TKN_CAVS_AUDIO_FORMAT_IN_RATE, SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc4_audio_format, sampling_frequency)},
	{SOF_TKN_CAVS_AUDIO_FORMAT_IN_BIT_DEPTH, SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc4_audio_format, bit_depth)},
	{SOF_TKN_CAVS_AUDIO_FORMAT_IN_CH_MAP, SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc4_audio_format, ch_map)},
	{SOF_TKN_CAVS_AUDIO_FORMAT_IN_CH_CFG, SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc4_audio_format, ch_cfg)},
	{SOF_TKN_CAVS_AUDIO_FORMAT_IN_INTERLEAVING_STYLE, SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc4_audio_format, interleaving_style)},
	{SOF_TKN_CAVS_AUDIO_FORMAT_IN_FMT_CFG, SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc4_audio_format, fmt_cfg)},
};

static const struct sof_topology_token ipc4_out_audio_format_tokens[] = {
	{SOF_TKN_CAVS_AUDIO_FORMAT_OUT_RATE, SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc4_audio_format, sampling_frequency)},
	{SOF_TKN_CAVS_AUDIO_FORMAT_OUT_BIT_DEPTH, SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc4_audio_format, bit_depth)},
	{SOF_TKN_CAVS_AUDIO_FORMAT_OUT_CH_MAP, SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc4_audio_format, ch_map)},
	{SOF_TKN_CAVS_AUDIO_FORMAT_OUT_CH_CFG, SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc4_audio_format, ch_cfg)},
	{SOF_TKN_CAVS_AUDIO_FORMAT_OUT_INTERLEAVING_STYLE, SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc4_audio_format, interleaving_style)},
	{SOF_TKN_CAVS_AUDIO_FORMAT_OUT_FMT_CFG, SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc4_audio_format, fmt_cfg)},
};

static const struct sof_topology_token ipc4_copier_gateway_cfg_tokens[] = {
	{SOF_TKN_CAVS_AUDIO_FORMAT_DMA_BUFFER_SIZE, SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_copier_gateway_cfg, dma_buffer_size)},
};

static const struct sof_topology_token ipc4_copier_tokens[] = {
	{SOF_TKN_INTEL_COPIER_NODE_TYPE, SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		0},
};

static const struct sof_topology_token ipc4_audio_fmt_num_tokens[] = {
	{SOF_TKN_COMP_NUM_AUDIO_FORMATS, SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		0},
};

/* DAI */
static const struct sof_topology_token dai_tokens[] = {
	{SOF_TKN_DAI_TYPE, SND_SOC_TPLG_TUPLE_TYPE_STRING, get_token_dai_type,
		offsetof(struct sof_ipc_comp_dai, type)},
	{SOF_TKN_DAI_INDEX, SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc_comp_dai, dai_index)},
	{SOF_TKN_DAI_DIRECTION, SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc_comp_dai, direction)},
};

/* BE DAI link */
static const struct sof_topology_token dai_link_tokens[] = {
	{SOF_TKN_DAI_TYPE, SND_SOC_TPLG_TUPLE_TYPE_STRING, get_token_dai_type,
		offsetof(struct sof_ipc_dai_config, type)},
	{SOF_TKN_DAI_INDEX, SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc_dai_config, dai_index)},
};

static const struct sof_token_info ipc4_token_list[SOF_TOKEN_COUNT] = {
	[SOF_DAI_TOKENS] = {"DAI tokens", dai_tokens, ARRAY_SIZE(dai_tokens)},
	[SOF_DAI_LINK_TOKENS] = {"DAI link tokens", dai_link_tokens, ARRAY_SIZE(dai_link_tokens)},
	[SOF_SCHED_TOKENS] = {"Scheduler tokens", ipc4_sched_tokens, ARRAY_SIZE(ipc4_sched_tokens)},
	[SOF_GAIN_TOKENS] = {"Gain tokens", gain_tokens, ARRAY_SIZE(gain_tokens)},
	[SOF_IPC4_COMP_TOKENS] = {"IPC4 Component tokens", ipc4_comp_tokens, ARRAY_SIZE(ipc4_comp_tokens)},
	[SOF_IPC4_MIXER_TOKENS] = {"IPC4 Mixer tokens", ipc4_mixer_tokens, ARRAY_SIZE(ipc4_mixer_tokens)},
	[SOF_IPC4_IN_AUDIO_FORMAT_TOKENS] = {"IPC4 Input Audio format tokens", ipc4_in_audio_format_tokens, ARRAY_SIZE(ipc4_in_audio_format_tokens)},
	[SOF_IPC4_OUT_AUDIO_FORMAT_TOKENS] = {"IPC4 Output Audio format tokens", ipc4_out_audio_format_tokens, ARRAY_SIZE(ipc4_out_audio_format_tokens)},
	[SOF_IPC4_COPIER_GATEWAY_CFG_TOKENS] = {"IPC4 Cpoier gateway config tokens", ipc4_copier_gateway_cfg_tokens, ARRAY_SIZE(ipc4_copier_gateway_cfg_tokens)},
	[SOF_IPC4_COPIER_TOKENS] = {"IPC4 Cpoier tokens", ipc4_copier_tokens, ARRAY_SIZE(ipc4_copier_tokens)},
	[SOF_IPC4_AUDIO_FMT_NUM_TOKENS] = {"IPC4 Audio format number tokens", ipc4_audio_fmt_num_tokens, ARRAY_SIZE(ipc4_audio_fmt_num_tokens)},
};

void sof_ipc4_dbg_audio_format(struct device *dev, struct sof_ipc4_audio_format *format,
			       size_t object_size, int num_format, const char *widget_name,
			       const char *function_name)
{
	struct sof_ipc4_audio_format *fmt;
	void *ptr = format;
	int i;

	for (i = 0; i < num_format; i++, ptr = (u8 *)ptr + object_size) {
		fmt = ptr;
		dev_dbg(dev, "%s i %d %s rate %u, bit_depth %u ch_map %u ch_cfg %u interleaving_style %u fmt_cfg %u\n",
			 function_name, i, widget_name,
			 fmt->sampling_frequency,
			 fmt->bit_depth,
			 fmt->ch_map,
			 fmt->ch_cfg,
			 fmt->interleaving_style,
			 fmt->fmt_cfg);
	}
}

/**
 * sof_ipc4_get_audio_fmt - get available audo formats from swidget->tuples
 * @scomp: pointer to pointer to SOC component
 * @swidget: pointer to struct snd_sof_widget containing tuples
 * @available_fmt: pointer to struct sof_ipc4_available_audio_format being filling in
 * @has_out_format: true if available_fmt contains output format
 *
 * Return: 0 if successful
 */
static int sof_ipc4_get_audio_fmt(struct snd_soc_component *scomp,
				  struct snd_sof_widget *swidget,
				  struct sof_ipc4_available_audio_format *available_fmt,
				  bool has_out_format)
{
	struct sof_ipc4_base_module_cfg *base_config;
	struct sof_ipc4_audio_format *out_format;
	int audio_fmt_num = 0;
	int ret;

	ret = sof_update_ipc_object(scomp, &audio_fmt_num,
				    SOF_IPC4_AUDIO_FMT_NUM_TOKENS, swidget->tuples,
				    swidget->num_tuples, sizeof(audio_fmt_num), 1);
	if (ret != 0 || audio_fmt_num <= 0) {
		dev_err(scomp->dev, "Invalid number of audio formats: %d\n", audio_fmt_num);
		return -EINVAL;
	}
	available_fmt->audio_fmt_num = audio_fmt_num;

	base_config = kcalloc(available_fmt->audio_fmt_num, sizeof(*base_config), GFP_KERNEL);
	if (!base_config)
		return -ENOMEM;

	ret = sof_update_ipc_object(scomp, base_config,
				    SOF_IPC4_COMP_TOKENS, swidget->tuples,
				    swidget->num_tuples, sizeof(*base_config),
				    available_fmt->audio_fmt_num);
	if (ret != 0) {
		dev_err(scomp->dev, "parse base_config tokens failed %d\n", ret);
		goto err_in;
	}

	ret = sof_update_ipc_object(scomp, &base_config->audio_fmt,
				    SOF_IPC4_IN_AUDIO_FORMAT_TOKENS, swidget->tuples,
				    swidget->num_tuples, sizeof(*base_config),
				    available_fmt->audio_fmt_num);
	if (ret != 0) {
		dev_err(scomp->dev, "parse base_config audio_fmt tokens failed %d\n", ret);
		goto err_in;
	}

	sof_ipc4_dbg_audio_format(scomp->dev, &base_config->audio_fmt,
				  sizeof(*base_config),
				  available_fmt->audio_fmt_num,
				  swidget->widget->name, __func__);

	available_fmt->base_config = base_config;

	if (!has_out_format)
		return 0;

	out_format = kcalloc(available_fmt->audio_fmt_num, sizeof(*out_format), GFP_KERNEL);
	if (!out_format) {
		ret = -ENOMEM;
		goto err_in;
	}

	ret = sof_update_ipc_object(scomp, out_format,
				    SOF_IPC4_OUT_AUDIO_FORMAT_TOKENS, swidget->tuples,
				    swidget->num_tuples, sizeof(*out_format),
				    available_fmt->audio_fmt_num);

	if (ret != 0) {
		dev_err(scomp->dev, "parse output audio_fmt tokens failed\n");
		goto err_out;
	}

	available_fmt->out_audio_fmt = out_format;
	sof_ipc4_dbg_audio_format(scomp->dev, out_format, sizeof(*out_format),
				  available_fmt->audio_fmt_num,
				  swidget->widget->name, __func__);

	return 0;

err_out:
	kfree(out_format);
err_in:
	kfree(base_config);

	return ret;
}

static void sof_ipc4_widget_free_comp(struct snd_sof_widget *swidget)
{
	kfree(swidget->private);
}

static int sof_ipc4_widget_setup_pcm(struct snd_sof_widget *swidget)
{
	struct snd_soc_component *scomp = swidget->scomp;
	struct sof_ipc4_copier *ipc4_copier;
	int node_type = 0;
	int ret;

	ipc4_copier = kzalloc(sizeof(*ipc4_copier), GFP_KERNEL);
	if (!ipc4_copier)
		return -ENOMEM;

	swidget->private = ipc4_copier;

	ret = sof_ipc4_get_audio_fmt(scomp, swidget, &ipc4_copier->available_fmt, true);
	if (ret != 0)
		goto err;

	ret = sof_update_ipc_object(scomp, &ipc4_copier->copier.gtw_cfg,
				    SOF_IPC4_COPIER_GATEWAY_CFG_TOKENS, swidget->tuples,
				    swidget->num_tuples, sizeof(ipc4_copier->copier.gtw_cfg), 1);

	if (ret != 0 || !ipc4_copier->copier.gtw_cfg.dma_buffer_size) {
		dev_err(scomp->dev, "Invalid host dma_buffer_size %d\n",
			ipc4_copier->copier.gtw_cfg.dma_buffer_size);
		goto err;
	}
	dev_dbg(scomp->dev, "host %s gtw_cfg dma_buffer_size %u\n",
		swidget->widget->name, ipc4_copier->copier.gtw_cfg.dma_buffer_size);

	ret = sof_update_ipc_object(scomp, &node_type,
				    SOF_IPC4_COPIER_TOKENS, swidget->tuples,
				    swidget->num_tuples, sizeof(node_type), 1);

	if (ret != 0) {
		dev_err(scomp->dev, "parse host copier_node tokens failed %d\n",
			ret);
		goto err;
	}
	dev_dbg(scomp->dev, "host %s node_type %u\n",
		swidget->widget->name, node_type);

	ipc4_copier->copier.gtw_cfg.node_id = SOF_IPC4_NODE_TYPE(node_type);

	return 0;
err:
	kfree(ipc4_copier);
	return ret;
}

static int sof_ipc4_widget_setup_comp_dai(struct snd_sof_widget *swidget)
{
	struct snd_soc_component *scomp = swidget->scomp;
	struct snd_sof_dai *dai = swidget->private;
	struct sof_ipc4_copier *ipc4_copier;
	int node_type = 0;
	int ret;

	ipc4_copier = kzalloc(sizeof(*ipc4_copier), GFP_KERNEL);
	if (!ipc4_copier)
		return -ENOMEM;

	ret = sof_ipc4_get_audio_fmt(scomp, swidget, &ipc4_copier->available_fmt, true);
	if (ret != 0)
		return ret;

	ret = sof_update_ipc_object(scomp, &ipc4_copier->copier.gtw_cfg,
				    SOF_IPC4_COPIER_GATEWAY_CFG_TOKENS, swidget->tuples,
				    swidget->num_tuples, sizeof(ipc4_copier->copier.gtw_cfg), 1);

	if (ret != 0 || !ipc4_copier->copier.gtw_cfg.dma_buffer_size) {
		dev_err(scomp->dev, "Invalid dai dma_buffer_size %d\n",
			ipc4_copier->copier.gtw_cfg.dma_buffer_size);
		return -EINVAL;
	}
	dev_dbg(scomp->dev, "dai %s gtw_cfg dma_buffer_size %u\n",
		swidget->widget->name, ipc4_copier->copier.gtw_cfg.dma_buffer_size);

	ret = sof_update_ipc_object(scomp, &node_type,
				    SOF_IPC4_COPIER_TOKENS, swidget->tuples,
				    swidget->num_tuples, sizeof(node_type), 1);

	if (ret != 0) {
		dev_err(scomp->dev, "parse dai copier_node tokens failed %d\n",
			ret);
		return ret;
	}
	dev_dbg(scomp->dev, "dai %s node_type %u\n",
		swidget->widget->name, node_type);

	ipc4_copier->copier.gtw_cfg.node_id = SOF_IPC4_NODE_TYPE(node_type);

	dai->scomp = scomp;

	dev_dbg(scomp->dev, "dai %s cpc %d\n", swidget->widget->name,
		ipc4_copier->copier.base_config.cpc);

	dai->private = ipc4_copier;
	return ret;
}

static int sof_ipc4_widget_setup_comp_pipeline(struct snd_sof_widget *swidget)
{
	struct snd_soc_component *scomp = swidget->scomp;
	struct sof_ipc4_pipeline *pipeline;
	int ret;

	pipeline = kzalloc(sizeof(*pipeline), GFP_KERNEL);
	if (!pipeline)
		return -ENOMEM;

	ret = sof_update_ipc_object(scomp, pipeline, SOF_SCHED_TOKENS, swidget->tuples,
				    swidget->num_tuples, sizeof(*pipeline), 1);
	if (ret != 0) {
		dev_err(scomp->dev, "parse pipeline tokens failed\n");
		kfree(pipeline);
		return ret;
	}

	/* TODO: Get priority from topology */
	pipeline->priority = 0;

	dev_dbg(scomp->dev, "pipeline %s: id %d pri %d lp mode %d\n",
		swidget->widget->name, swidget->pipeline_id,
		pipeline->priority, pipeline->lp_mode);

	swidget->private = pipeline;

	return 0;
}

/*
 * PGA Topology
 */

static int sof_ipc4_widget_setup_comp_pga(struct snd_sof_widget *swidget)
{
	struct snd_soc_component *scomp = swidget->scomp;
	struct sof_ipc4_gain *gain;
	int ret;

	gain = kzalloc(sizeof(*gain), GFP_KERNEL);
	if (!gain)
		return -ENOMEM;

	swidget->private = gain;

	/* The out_audio_fmt in topology is ignored as it is not required to be sent to the FW */
	ret = sof_ipc4_get_audio_fmt(scomp, swidget, &gain->available_fmt, false);
	if (ret != 0)
		goto err;

	ret = sof_update_ipc_object(scomp, &gain->data, SOF_GAIN_TOKENS, swidget->tuples,
				    swidget->num_tuples, sizeof(gain->data), 1);
	if (ret != 0) {
		dev_err(scomp->dev, "parse gain tokens failed\n");
		goto err;
	}

	dev_dbg(scomp->dev, "tplg2: ready widget %s, ramp_type %d, duration %d, val %d  cpc %d",
		swidget->widget->name, gain->data.curve_type, gain->data.curve_duration,
		gain->data.init_val, gain->base_config.cpc);

	return 0;
err:
	kfree(gain);
	return ret;
}

static int sof_ipc4_widget_setup_comp_mixer(struct snd_sof_widget *swidget)
{
	struct snd_soc_component *scomp = swidget->scomp;
	struct sof_ipc4_mixer *mixer;
	int ret;

	mixer = kzalloc(sizeof(*mixer), GFP_KERNEL);
	if (!mixer)
		return -ENOMEM;

	swidget->private = mixer;

	/* The out_audio_fmt in topology is ignored as it is not required to be sent to the FW */
	ret = sof_ipc4_get_audio_fmt(scomp, swidget, &mixer->available_fmt, false);
	if (ret != 0)
		goto err;

	ret = sof_update_ipc_object(scomp, &mixer, SOF_IPC4_MIXER_TOKENS, swidget->tuples,
				    swidget->num_tuples, sizeof(*mixer), 1);
	if (ret != 0) {
		dev_err(scomp->dev, "parse mixer tokens failed\n");
		goto err;
	}

	dev_dbg(scomp->dev, "mixer type %d", mixer->type);

	return 0;
err:
	kfree(mixer);
	return ret;
}

static void sof_ipc4_widget_free_comp_dai(struct snd_sof_widget *swidget)
{
	switch (swidget->id) {
	case snd_soc_dapm_dai_in:
	case snd_soc_dapm_dai_out:
	{
		struct snd_sof_dai *dai = swidget->private;
		struct sof_ipc4_copier *ipc4_copier;

		if (!dai)
			return;

		ipc4_copier = dai->private;
		if (ipc4_copier)
			kfree(ipc4_copier);

		kfree(dai);
		break;
	}
	default:
		break;
	}
}

static int sof_ipc4_widget_free(struct snd_sof_dev *sdev, struct snd_sof_widget *swidget)
{
	/* TODO: sof_ipc4_delete_pipeline? */
	swidget->complete = 0;
	swidget->use_count--;
	return 0;
}

/* token list for each topology object */
static enum sof_tokens host_token_list[] = {
	SOF_IPC4_COMP_TOKENS,
	SOF_IPC4_COPIER_GATEWAY_CFG_TOKENS,
	SOF_IPC4_IN_AUDIO_FORMAT_TOKENS,
	SOF_IPC4_OUT_AUDIO_FORMAT_TOKENS,
	SOF_IPC4_COPIER_GATEWAY_CFG_TOKENS,
	SOF_IPC4_COPIER_TOKENS,
	SOF_IPC4_AUDIO_FMT_NUM_TOKENS,
};

static enum sof_tokens mixer_token_list[] = {
	SOF_IPC4_COMP_TOKENS,
	SOF_IPC4_MIXER_TOKENS,
	SOF_IPC4_IN_AUDIO_FORMAT_TOKENS,
	SOF_IPC4_COPIER_GATEWAY_CFG_TOKENS,
	SOF_IPC4_COPIER_TOKENS,
	SOF_IPC4_AUDIO_FMT_NUM_TOKENS,
};

static enum sof_tokens pipeline_token_list[] = {
	SOF_IPC4_COMP_TOKENS,
	SOF_IPC4_IN_AUDIO_FORMAT_TOKENS,
	SOF_IPC4_OUT_AUDIO_FORMAT_TOKENS,
	SOF_IPC4_COPIER_GATEWAY_CFG_TOKENS,
	SOF_IPC4_COPIER_TOKENS,
	SOF_IPC4_AUDIO_FMT_NUM_TOKENS,
};

static enum sof_tokens pga_token_list[] = {
	SOF_IPC4_COMP_TOKENS,
	SOF_GAIN_TOKENS,
	SOF_IPC4_IN_AUDIO_FORMAT_TOKENS,
	SOF_IPC4_COPIER_GATEWAY_CFG_TOKENS,
	SOF_IPC4_COPIER_TOKENS,
	SOF_IPC4_AUDIO_FMT_NUM_TOKENS,
};

static enum sof_tokens dai_token_list[] = {
	SOF_IPC4_COMP_TOKENS,
	SOF_IPC4_IN_AUDIO_FORMAT_TOKENS,
	SOF_IPC4_OUT_AUDIO_FORMAT_TOKENS,
	SOF_IPC4_COPIER_GATEWAY_CFG_TOKENS,
	SOF_IPC4_COPIER_TOKENS,
	SOF_IPC4_AUDIO_FMT_NUM_TOKENS,
};

static const struct ipc_tplg_widget_ops tplg_ipc4_widget_ops[SND_SOC_DAPM_TYPE_COUNT] = {
	[snd_soc_dapm_aif_in] =  {sof_ipc4_widget_setup_pcm, sof_ipc4_widget_free_comp,
				  host_token_list, ARRAY_SIZE(host_token_list), NULL},
	[snd_soc_dapm_aif_out] = {sof_ipc4_widget_setup_pcm, sof_ipc4_widget_free_comp,
				  host_token_list, ARRAY_SIZE(host_token_list), NULL},

	[snd_soc_dapm_dai_in] = {sof_ipc4_widget_setup_comp_dai, sof_ipc4_widget_free_comp_dai,
				 dai_token_list, ARRAY_SIZE(dai_token_list), NULL},
	[snd_soc_dapm_dai_out] = {sof_ipc4_widget_setup_comp_dai, sof_ipc4_widget_free_comp_dai,
				  dai_token_list, ARRAY_SIZE(dai_token_list), NULL},
	[snd_soc_dapm_mixer] = {sof_ipc4_widget_setup_comp_mixer, sof_ipc4_widget_free_comp,
				mixer_token_list, ARRAY_SIZE(mixer_token_list),
				NULL},
	[snd_soc_dapm_scheduler] = {sof_ipc4_widget_setup_comp_pipeline, sof_ipc4_widget_free_comp,
				    pipeline_token_list, ARRAY_SIZE(pipeline_token_list), NULL},
	[snd_soc_dapm_pga] = {sof_ipc4_widget_setup_comp_pga, sof_ipc4_widget_free_comp,
			      pga_token_list, ARRAY_SIZE(pga_token_list), NULL},
};

const struct ipc_tplg_ops ipc4_tplg_ops = {
	.widget = tplg_ipc4_widget_ops,
	.token_list = ipc4_token_list,
	.widget_free = sof_ipc4_widget_free,
};
