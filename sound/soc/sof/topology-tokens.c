// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
//
// This file is provided under a dual BSD/GPLv2 license.  When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2021 Intel Corporation. All rights reserved.
//
// Author: Rander Wang <rander.wang@linux.intel.com>
//

#include <linux/bits.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/firmware.h>
#include <linux/workqueue.h>
#include <sound/tlv.h>
#include <sound/pcm_params.h>
#include <uapi/sound/sof/tokens.h>
#include "sof-priv.h"
#include "sof-audio.h"
#include "ops.h"
#include "topology-tokens.h"

static const struct sof_frame_types sof_frames[] = {
	{"s16le", SOF_IPC_FRAME_S16_LE},
	{"s24le", SOF_IPC_FRAME_S24_4LE},
	{"s32le", SOF_IPC_FRAME_S32_LE},
	{"float", SOF_IPC_FRAME_FLOAT},
};

static const struct sof_dai_types sof_dais[] = {
	{"SSP", SOF_DAI_INTEL_SSP},
	{"HDA", SOF_DAI_INTEL_HDA},
	{"DMIC", SOF_DAI_INTEL_DMIC},
	{"ALH", SOF_DAI_INTEL_ALH},
	{"SAI", SOF_DAI_IMX_SAI},
	{"ESAI", SOF_DAI_IMX_ESAI},
	{"ACP", SOF_DAI_AMD_BT},
	{"ACPSP", SOF_DAI_AMD_SP},
	{"ACPDMIC", SOF_DAI_AMD_DMIC},
	{"AFE", SOF_DAI_MEDIATEK_AFE},
};

enum sof_ipc_frame find_format(const char *name)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(sof_frames); i++) {
		if (strcmp(name, sof_frames[i].name) == 0)
			return sof_frames[i].frame;
	}

	/* use s32le if nothing is specified */
	return SOF_IPC_FRAME_S32_LE;
}

enum sof_ipc_dai_type find_dai(const char *name)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(sof_dais); i++) {
		if (strcmp(name, sof_dais[i].name) == 0)
			return sof_dais[i].type;
	}

	return SOF_DAI_INTEL_NONE;
}

static int sof_parse_uuid_tokens(struct snd_soc_component *scomp,
				 void *object,
				 const struct sof_topology_token *tokens,
				 int count,
				 struct snd_soc_tplg_vendor_array *array,
				 size_t offset)
{
	struct snd_soc_tplg_vendor_uuid_elem *elem;
	int found = 0;
	int i, j;

	/* parse element by element */
	for (i = 0; i < le32_to_cpu(array->num_elems); i++) {
		elem = &array->uuid[i];

		/* search for token */
		for (j = 0; j < count; j++) {
			/* match token type */
			if (tokens[j].type != SND_SOC_TPLG_TUPLE_TYPE_UUID)
				continue;

			/* match token id */
			if (tokens[j].token != le32_to_cpu(elem->token))
				continue;

			/* matched - now load token */
			tokens[j].get_token(elem, object,
					    offset + tokens[j].offset,
					    tokens[j].size);

			found++;
		}
	}

	return found;
}

static int sof_parse_string_tokens(struct snd_soc_component *scomp,
				   void *object,
				   const struct sof_topology_token *tokens,
				   int count,
				   struct snd_soc_tplg_vendor_array *array,
				   size_t offset)
{
	struct snd_soc_tplg_vendor_string_elem *elem;
	int found = 0;
	int i, j;

	/* parse element by element */
	for (i = 0; i < le32_to_cpu(array->num_elems); i++) {
		elem = &array->string[i];

		/* search for token */
		for (j = 0; j < count; j++) {
			/* match token type */
			if (tokens[j].type != SND_SOC_TPLG_TUPLE_TYPE_STRING)
				continue;

			/* match token id */
			if (tokens[j].token != le32_to_cpu(elem->token))
				continue;

			/* matched - now load token */
			tokens[j].get_token(elem, object,
					    offset + tokens[j].offset,
					    tokens[j].size);

			found++;
		}
	}

	return found;
}

static int sof_parse_word_tokens(struct snd_soc_component *scomp,
				 void *object,
				 const struct sof_topology_token *tokens,
				 int count,
				 struct snd_soc_tplg_vendor_array *array,
				 size_t offset)
{
	struct snd_soc_tplg_vendor_value_elem *elem;
	int found = 0;
	int i, j;

	/* parse element by element */
	for (i = 0; i < le32_to_cpu(array->num_elems); i++) {
		elem = &array->value[i];

		/* search for token */
		for (j = 0; j < count; j++) {
			/* match token type */
			if (!(tokens[j].type == SND_SOC_TPLG_TUPLE_TYPE_WORD ||
			      tokens[j].type == SND_SOC_TPLG_TUPLE_TYPE_SHORT ||
			      tokens[j].type == SND_SOC_TPLG_TUPLE_TYPE_BYTE ||
			      tokens[j].type == SND_SOC_TPLG_TUPLE_TYPE_BOOL))
				continue;

			/* match token id */
			if (tokens[j].token != le32_to_cpu(elem->token))
				continue;

			/* load token */
			tokens[j].get_token(elem, object,
					    offset + tokens[j].offset,
					    tokens[j].size);

			found++;
		}
	}

	return found;
}

/**
 * sof_parse_token_sets - Parse multiple sets of tokens
 * @scomp: pointer to soc component
 * @object: target ipc struct for parsed values
 * @tokens: token definition array describing what tokens to parse
 * @count: number of tokens in definition array
 * @array: source pointer to consecutive vendor arrays to be parsed
 * @priv_size: total size of the consecutive source arrays
 * @sets: number of similar token sets to be parsed, 1 set has count elements
 * @object_size: offset to next target ipc struct with multiple sets
 *
 * This function parses multiple sets of tokens in vendor arrays into
 * consecutive ipc structs.
 */
int sof_parse_token_sets(struct snd_soc_component *scomp, void *object,
			 const struct sof_topology_token *tokens, int count,
			 struct snd_soc_tplg_vendor_array *array,
			 int priv_size, int sets, size_t object_size)
{
	size_t offset = 0;
	int found = 0;
	int total = 0;
	int asize;

	while (priv_size > 0 && total < count * sets) {
		asize = le32_to_cpu(array->size);

		/* validate asize */
		if (asize < 0) { /* FIXME: A zero-size array makes no sense */
			dev_err(scomp->dev, "error: invalid array size 0x%x\n",
				asize);
			return -EINVAL;
		}

		/* make sure there is enough data before parsing */
		priv_size -= asize;
		if (priv_size < 0) {
			dev_err(scomp->dev, "error: invalid array size 0x%x\n",
				asize);
			return -EINVAL;
		}

		/* call correct parser depending on type */
		switch (le32_to_cpu(array->type)) {
		case SND_SOC_TPLG_TUPLE_TYPE_UUID:
			found += sof_parse_uuid_tokens(scomp, object, tokens,
						       count, array, offset);
			break;
		case SND_SOC_TPLG_TUPLE_TYPE_STRING:
			found += sof_parse_string_tokens(scomp, object, tokens,
							 count, array, offset);
			break;
		case SND_SOC_TPLG_TUPLE_TYPE_BOOL:
		case SND_SOC_TPLG_TUPLE_TYPE_BYTE:
		case SND_SOC_TPLG_TUPLE_TYPE_WORD:
		case SND_SOC_TPLG_TUPLE_TYPE_SHORT:
			found += sof_parse_word_tokens(scomp, object, tokens,
						       count, array, offset);
			break;
		default:
			dev_err(scomp->dev, "error: unknown token type %d\n",
				array->type);
			return -EINVAL;
		}

		/* next array */
		array = (struct snd_soc_tplg_vendor_array *)((u8 *)array
			+ asize);

		/* move to next target struct */
		if (found >= count) {
			offset += object_size;
			total += found;
			found = 0;
		}
	}

	return 0;
}

int sof_parse_tokens(struct snd_soc_component *scomp, void *object,
		     const struct sof_topology_token *tokens, int count,
		     struct snd_soc_tplg_vendor_array *array, int priv_size)
{
	/*
	 * sof_parse_tokens is used when topology contains only a single set of
	 * identical tuples arrays. So additional parameters to
	 * sof_parse_token_sets are sets = 1 (only 1 set) and
	 * object_size = 0 (irrelevant).
	 */
	return sof_parse_token_sets(scomp, object, tokens, count, array,
				    priv_size, 1, 0);
}
