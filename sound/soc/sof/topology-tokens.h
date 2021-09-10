/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause) */
//
// This file is provided under a dual BSD/GPLv2 license.  When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2021 Intel Corporation. All rights reserved.
//
// Author: Rander Wang <rander.wang@linux.intel.com>
//

#ifndef SOF_TOPOLOGY_COMMON_H
#define SOF_TOPOLOGY_COMMON_H

#define COMP_ID_UNASSIGNED		0xffffffff

/*
 * Supported Frame format types and lookup, add new ones to end of list.
 */

struct sof_frame_types {
	const char *name;
	enum sof_ipc_frame frame;
};

struct sof_dai_types {
	const char *name;
	enum sof_ipc_dai_type type;
};

/*
 * Topology Token Parsing.
 * New tokens should be added to headers and parsing tables below.
 */

struct sof_topology_token {
	u32 token;
	u32 type;
	int (*get_token)(void *elem, void *object, u32 offset, u32 size);
	u32 offset;
	u32 size;
};

struct sof_topology_token_entry {
	char *name;
	int size;
	const struct sof_topology_token *token;
};

enum sof_topology_token_index {
	SOF_TOKEN_EXT,
	SOF_TOKEN_DAI,
	SOF_TOKEN_DAI_LINK,
	SOF_TOKEN_DMIC,
	SOF_TOKEN_DMIC_PDM,
	SOF_TOKEN_SCHED,
	SOF_TOKEN_SSP
};

enum sof_ipc_frame find_format(const char *name);
enum sof_ipc_dai_type find_dai(const char *name);

int sof_parse_topology_tokens(struct snd_soc_component *scomp, void *object, int index,
			      struct snd_soc_tplg_vendor_array *array, int priv_size);

static inline int get_token_u32(void *elem, void *object, u32 offset, u32 size)
{
	struct snd_soc_tplg_vendor_value_elem *velem = elem;
	u32 *val = (u32 *)((u8 *)object + offset);

	*val = le32_to_cpu(velem->value);
	return 0;
}

static inline int get_token_u16(void *elem, void *object, u32 offset, u32 size)
{
	struct snd_soc_tplg_vendor_value_elem *velem = elem;
	u16 *val = (u16 *)((u8 *)object + offset);

	*val = (u16)le32_to_cpu(velem->value);
	return 0;
}

static inline int get_token_uuid(void *elem, void *object, u32 offset, u32 size)
{
	struct snd_soc_tplg_vendor_uuid_elem *velem = elem;
	u8 *dst = (u8 *)object + offset;

	memcpy(dst, velem->uuid, UUID_SIZE);

	return 0;
}

static inline int get_token_comp_format(void *elem, void *object, u32 offset, u32 size)
{
	struct snd_soc_tplg_vendor_string_elem *velem = elem;
	u32 *val = (u32 *)((u8 *)object + offset);

	*val = find_format(velem->string);
	return 0;
}

static inline int get_token_dai_type(void *elem, void *object, u32 offset, u32 size)
{
	struct snd_soc_tplg_vendor_string_elem *velem = elem;
	u32 *val = (u32 *)((u8 *)object + offset);

	*val = find_dai(velem->string);
	return 0;
}

struct sof_process_types {
	const char *name;
	enum sof_ipc_process_type type;
	enum sof_comp_type comp_type;
};

static const struct sof_process_types sof_process[] = {
	{"EQFIR", SOF_PROCESS_EQFIR, SOF_COMP_EQ_FIR},
	{"EQIIR", SOF_PROCESS_EQIIR, SOF_COMP_EQ_IIR},
	{"KEYWORD_DETECT", SOF_PROCESS_KEYWORD_DETECT, SOF_COMP_KEYWORD_DETECT},
	{"KPB", SOF_PROCESS_KPB, SOF_COMP_KPB},
	{"CHAN_SELECTOR", SOF_PROCESS_CHAN_SELECTOR, SOF_COMP_SELECTOR},
	{"MUX", SOF_PROCESS_MUX, SOF_COMP_MUX},
	{"DEMUX", SOF_PROCESS_DEMUX, SOF_COMP_DEMUX},
	{"DCBLOCK", SOF_PROCESS_DCBLOCK, SOF_COMP_DCBLOCK},
	{"SMART_AMP", SOF_PROCESS_SMART_AMP, SOF_COMP_SMART_AMP},
};

static enum sof_ipc_process_type find_process(const char *name)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(sof_process); i++) {
		if (strcmp(name, sof_process[i].name) == 0)
			return sof_process[i].type;
	}

	return SOF_PROCESS_NONE;
}

static inline int get_token_process_type(void *elem, void *object, u32 offset,
					 u32 size)
{
	struct snd_soc_tplg_vendor_string_elem *velem = elem;
	u32 *val = (u32 *)((u8 *)object + offset);

	*val = find_process(velem->string);
	return 0;
}

int sof_parse_token_sets(struct snd_soc_component *scomp, void *object,
			 const struct sof_topology_token *tokens, int count,
			 struct snd_soc_tplg_vendor_array *array,
			 int priv_size, int sets, size_t object_size);

int sof_parse_tokens(struct snd_soc_component *scomp, void *object,
		     const struct sof_topology_token *tokens, int count,
		     struct snd_soc_tplg_vendor_array *array, int priv_size);

struct sof_ipc_comp *sof_comp_alloc(struct snd_sof_widget *swidget,
				    size_t *ipc_size, int index);

/* Buffers */
static const __maybe_unused struct sof_topology_token buffer_tokens[] = {
	{SOF_TKN_BUF_SIZE, SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc_buffer, size), 0},
	{SOF_TKN_BUF_CAPS, SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc_buffer, caps), 0},
};

static const __maybe_unused struct sof_topology_token pipeline_tokens[] = {
	{SOF_TKN_SCHED_DYNAMIC_PIPELINE, SND_SOC_TPLG_TUPLE_TYPE_BOOL, get_token_u16,
		offsetof(struct snd_sof_widget, dynamic_pipeline_widget), 0},

};

/* volume */
static const __maybe_unused struct sof_topology_token volume_tokens[] = {
	{SOF_TKN_VOLUME_RAMP_STEP_TYPE, SND_SOC_TPLG_TUPLE_TYPE_WORD,
		get_token_u32, offsetof(struct sof_ipc_comp_volume, ramp), 0},
	{SOF_TKN_VOLUME_RAMP_STEP_MS,
		SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc_comp_volume, initial_ramp), 0},
};

/* SRC */
static const __maybe_unused struct sof_topology_token src_tokens[] = {
	{SOF_TKN_SRC_RATE_IN, SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc_comp_src, source_rate), 0},
	{SOF_TKN_SRC_RATE_OUT, SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc_comp_src, sink_rate), 0},
};

/* ASRC */
static const __maybe_unused struct sof_topology_token asrc_tokens[] = {
	{SOF_TKN_ASRC_RATE_IN, SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc_comp_asrc, source_rate), 0},
	{SOF_TKN_ASRC_RATE_OUT, SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc_comp_asrc, sink_rate), 0},
	{SOF_TKN_ASRC_ASYNCHRONOUS_MODE, SND_SOC_TPLG_TUPLE_TYPE_WORD,
		get_token_u32,
		offsetof(struct sof_ipc_comp_asrc, asynchronous_mode), 0},
	{SOF_TKN_ASRC_OPERATION_MODE, SND_SOC_TPLG_TUPLE_TYPE_WORD,
		get_token_u32,
		offsetof(struct sof_ipc_comp_asrc, operation_mode), 0},
};

/* Tone */
static const __maybe_unused struct sof_topology_token tone_tokens[] = {
};

/* EFFECT */
static const __maybe_unused struct sof_topology_token process_tokens[] = {
	{SOF_TKN_PROCESS_TYPE, SND_SOC_TPLG_TUPLE_TYPE_STRING,
		get_token_process_type,
		offsetof(struct sof_ipc_comp_process, type), 0},
};

/* PCM */
static const __maybe_unused struct sof_topology_token pcm_tokens[] = {
	{SOF_TKN_PCM_DMAC_CONFIG, SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc_comp_host, dmac_config), 0},
};

/* PCM */
static const __maybe_unused struct sof_topology_token stream_tokens[] = {
	{SOF_TKN_STREAM_PLAYBACK_COMPATIBLE_D0I3,
		SND_SOC_TPLG_TUPLE_TYPE_BOOL, get_token_u16,
		offsetof(struct snd_sof_pcm, stream[0].d0i3_compatible), 0},
	{SOF_TKN_STREAM_CAPTURE_COMPATIBLE_D0I3,
		SND_SOC_TPLG_TUPLE_TYPE_BOOL, get_token_u16,
		offsetof(struct snd_sof_pcm, stream[1].d0i3_compatible), 0},
};

/* Component extended tokens */
static const __maybe_unused struct sof_topology_token comp_ext_tokens[] = {
	{SOF_TKN_COMP_UUID,
		SND_SOC_TPLG_TUPLE_TYPE_UUID, get_token_uuid,
		offsetof(struct sof_ipc_comp_ext, uuid), 0},
};

/* Generic components */
static const __maybe_unused struct sof_topology_token comp_tokens[] = {
	{SOF_TKN_COMP_PERIOD_SINK_COUNT,
		SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc_comp_config, periods_sink), 0},
	{SOF_TKN_COMP_PERIOD_SOURCE_COUNT,
		SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc_comp_config, periods_source), 0},
	{SOF_TKN_COMP_FORMAT,
		SND_SOC_TPLG_TUPLE_TYPE_STRING, get_token_comp_format,
		offsetof(struct sof_ipc_comp_config, frame_fmt), 0},
};

/* SSP */
static const __maybe_unused struct sof_topology_token ssp_tokens[] = {
	{SOF_TKN_INTEL_SSP_CLKS_CONTROL,
		SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc_dai_ssp_params, clks_control), 0},
	{SOF_TKN_INTEL_SSP_MCLK_ID,
		SND_SOC_TPLG_TUPLE_TYPE_SHORT, get_token_u16,
		offsetof(struct sof_ipc_dai_ssp_params, mclk_id), 0},
	{SOF_TKN_INTEL_SSP_SAMPLE_BITS, SND_SOC_TPLG_TUPLE_TYPE_WORD,
		get_token_u32,
		offsetof(struct sof_ipc_dai_ssp_params, sample_valid_bits), 0},
	{SOF_TKN_INTEL_SSP_FRAME_PULSE_WIDTH, SND_SOC_TPLG_TUPLE_TYPE_SHORT,
		get_token_u16,
		offsetof(struct sof_ipc_dai_ssp_params, frame_pulse_width), 0},
	{SOF_TKN_INTEL_SSP_QUIRKS, SND_SOC_TPLG_TUPLE_TYPE_WORD,
		get_token_u32,
		offsetof(struct sof_ipc_dai_ssp_params, quirks), 0},
	{SOF_TKN_INTEL_SSP_TDM_PADDING_PER_SLOT, SND_SOC_TPLG_TUPLE_TYPE_BOOL,
		get_token_u16,
		offsetof(struct sof_ipc_dai_ssp_params,
			 tdm_per_slot_padding_flag), 0},
	{SOF_TKN_INTEL_SSP_BCLK_DELAY, SND_SOC_TPLG_TUPLE_TYPE_WORD,
		get_token_u32,
		offsetof(struct sof_ipc_dai_ssp_params, bclk_delay), 0},

};

/* ALH */
static const __maybe_unused struct sof_topology_token alh_tokens[] = {
	{SOF_TKN_INTEL_ALH_RATE,
		SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc_dai_alh_params, rate), 0},
	{SOF_TKN_INTEL_ALH_CH,
		SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc_dai_alh_params, channels), 0},
};

/* DMIC */
static const __maybe_unused struct sof_topology_token dmic_tokens[] = {
	{SOF_TKN_INTEL_DMIC_DRIVER_VERSION,
		SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc_dai_dmic_params, driver_ipc_version),
		0},
	{SOF_TKN_INTEL_DMIC_CLK_MIN,
		SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc_dai_dmic_params, pdmclk_min), 0},
	{SOF_TKN_INTEL_DMIC_CLK_MAX,
		SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc_dai_dmic_params, pdmclk_max), 0},
	{SOF_TKN_INTEL_DMIC_SAMPLE_RATE,
		SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc_dai_dmic_params, fifo_fs), 0},
	{SOF_TKN_INTEL_DMIC_DUTY_MIN,
		SND_SOC_TPLG_TUPLE_TYPE_SHORT, get_token_u16,
		offsetof(struct sof_ipc_dai_dmic_params, duty_min), 0},
	{SOF_TKN_INTEL_DMIC_DUTY_MAX,
		SND_SOC_TPLG_TUPLE_TYPE_SHORT, get_token_u16,
		offsetof(struct sof_ipc_dai_dmic_params, duty_max), 0},
	{SOF_TKN_INTEL_DMIC_NUM_PDM_ACTIVE,
		SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc_dai_dmic_params,
			 num_pdm_active), 0},
	{SOF_TKN_INTEL_DMIC_FIFO_WORD_LENGTH,
		SND_SOC_TPLG_TUPLE_TYPE_SHORT, get_token_u16,
		offsetof(struct sof_ipc_dai_dmic_params, fifo_bits), 0},
	{SOF_TKN_INTEL_DMIC_UNMUTE_RAMP_TIME_MS,
		SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc_dai_dmic_params, unmute_ramp_time), 0},

};

/* DAI */
static const __maybe_unused struct sof_topology_token dai_tokens[] = {
	{SOF_TKN_DAI_TYPE, SND_SOC_TPLG_TUPLE_TYPE_STRING, get_token_dai_type,
		offsetof(struct sof_ipc_comp_dai, type), 0},
	{SOF_TKN_DAI_INDEX, SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc_comp_dai, dai_index), 0},
	{SOF_TKN_DAI_DIRECTION, SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc_comp_dai, direction), 0},
};

/* ESAI */
static const __maybe_unused struct sof_topology_token esai_tokens[] = {
	{SOF_TKN_IMX_ESAI_MCLK_ID,
		SND_SOC_TPLG_TUPLE_TYPE_SHORT, get_token_u16,
		offsetof(struct sof_ipc_dai_esai_params, mclk_id), 0},
};

/* SAI */
static const __maybe_unused struct sof_topology_token sai_tokens[] = {
	{SOF_TKN_IMX_SAI_MCLK_ID,
		SND_SOC_TPLG_TUPLE_TYPE_SHORT, get_token_u16,
		offsetof(struct sof_ipc_dai_sai_params, mclk_id), 0},
};

/* Core tokens */
static const __maybe_unused struct sof_topology_token core_tokens[] = {
	{SOF_TKN_COMP_CORE_ID,
		SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc_comp, core), 0},
};

/* BE DAI link */
static const __maybe_unused struct sof_topology_token dai_link_tokens[] = {
	{SOF_TKN_DAI_TYPE, SND_SOC_TPLG_TUPLE_TYPE_STRING, get_token_dai_type,
		offsetof(struct sof_ipc_dai_config, type), 0},
	{SOF_TKN_DAI_INDEX, SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc_dai_config, dai_index), 0},
};

/*
 * DMIC PDM Tokens
 * SOF_TKN_INTEL_DMIC_PDM_CTRL_ID should be the first token
 * as it increments the index while parsing the array of pdm tokens
 * and determines the correct offset
 */
static const __maybe_unused struct sof_topology_token dmic_pdm_tokens[] = {
	{SOF_TKN_INTEL_DMIC_PDM_CTRL_ID,
		SND_SOC_TPLG_TUPLE_TYPE_SHORT, get_token_u16,
		offsetof(struct sof_ipc_dai_dmic_pdm_ctrl, id),
		0},
	{SOF_TKN_INTEL_DMIC_PDM_MIC_A_Enable,
		SND_SOC_TPLG_TUPLE_TYPE_SHORT, get_token_u16,
		offsetof(struct sof_ipc_dai_dmic_pdm_ctrl, enable_mic_a),
		0},
	{SOF_TKN_INTEL_DMIC_PDM_MIC_B_Enable,
		SND_SOC_TPLG_TUPLE_TYPE_SHORT, get_token_u16,
		offsetof(struct sof_ipc_dai_dmic_pdm_ctrl, enable_mic_b),
		0},
	{SOF_TKN_INTEL_DMIC_PDM_POLARITY_A,
		SND_SOC_TPLG_TUPLE_TYPE_SHORT, get_token_u16,
		offsetof(struct sof_ipc_dai_dmic_pdm_ctrl, polarity_mic_a),
		0},
	{SOF_TKN_INTEL_DMIC_PDM_POLARITY_B,
		SND_SOC_TPLG_TUPLE_TYPE_SHORT, get_token_u16,
		offsetof(struct sof_ipc_dai_dmic_pdm_ctrl, polarity_mic_b),
		0},
	{SOF_TKN_INTEL_DMIC_PDM_CLK_EDGE,
		SND_SOC_TPLG_TUPLE_TYPE_SHORT, get_token_u16,
		offsetof(struct sof_ipc_dai_dmic_pdm_ctrl, clk_edge),
		0},
	{SOF_TKN_INTEL_DMIC_PDM_SKEW,
		SND_SOC_TPLG_TUPLE_TYPE_SHORT, get_token_u16,
		offsetof(struct sof_ipc_dai_dmic_pdm_ctrl, skew),
		0},
};

/* HDA */
static const __maybe_unused struct sof_topology_token hda_tokens[] = {
	{SOF_TKN_INTEL_HDA_RATE,
		SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc_dai_hda_params, rate), 0},
	{SOF_TKN_INTEL_HDA_CH,
		SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc_dai_hda_params, channels), 0},
};

/* Leds */
static const __maybe_unused struct sof_topology_token led_tokens[] = {
	{SOF_TKN_MUTE_LED_USE, SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
	 offsetof(struct snd_sof_led_control, use_led), 0},
	{SOF_TKN_MUTE_LED_DIRECTION, SND_SOC_TPLG_TUPLE_TYPE_WORD,
	 get_token_u32, offsetof(struct snd_sof_led_control, direction), 0},
};

/* AFE */
static const __maybe_unused struct sof_topology_token afe_tokens[] = {
	{SOF_TKN_MEDIATEK_AFE_RATE,
		SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc_dai_mtk_afe_params, rate), 0},
	{SOF_TKN_MEDIATEK_AFE_CH,
		SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc_dai_mtk_afe_params, channels), 0},
	{SOF_TKN_MEDIATEK_AFE_FORMAT,
		SND_SOC_TPLG_TUPLE_TYPE_STRING, get_token_comp_format,
		offsetof(struct sof_ipc_dai_mtk_afe_params, format), 0},
};

/* scheduling */
static const __maybe_unused struct sof_topology_token sched_tokens[] = {
	{SOF_TKN_SCHED_PERIOD, SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc_pipe_new, period), 0},
	{SOF_TKN_SCHED_PRIORITY, SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc_pipe_new, priority), 0},
	{SOF_TKN_SCHED_MIPS, SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc_pipe_new, period_mips), 0},
	{SOF_TKN_SCHED_CORE, SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc_pipe_new, core), 0},
	{SOF_TKN_SCHED_FRAMES, SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc_pipe_new, frames_per_sched), 0},
	{SOF_TKN_SCHED_TIME_DOMAIN, SND_SOC_TPLG_TUPLE_TYPE_WORD, get_token_u32,
		offsetof(struct sof_ipc_pipe_new, time_domain), 0},
};

#endif //SOF_TOPOLOGY_COMMON_H
