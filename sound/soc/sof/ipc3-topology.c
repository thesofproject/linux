// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
//
// This file is provided under a dual BSD/GPLv2 license.  When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2021 Intel Corporation. All rights reserved.
//
//

#include <uapi/sound/sof/tokens.h>
#include "sof-priv.h"
#include "sof-audio.h"
#include "ops.h"

/*
 * update the IPC struct based on the tuple array.
 */
static void sof_update_ipc_object(void *object, const struct sof_topology_token *tokens, int count,
				  int num_tuples, struct snd_sof_tuple *tuples,
				  size_t object_size, int num_sets)
{
	int i,j;
	int num_tokens_matched = 0;
	int offset = 0;

	for (i = 0; i < count; i++) {
		for (j = 0; j < num_tuples; j++) {
			if (tokens[i].token == tuples[j].token) {
				switch (tokens[i].type) {
				case SND_SOC_TPLG_TUPLE_TYPE_WORD:
				{
					u32 *val = (u32 *)((u8 *)object + tokens[i].offset +
							   offset);

					*val = tuples[j].value;
					break;
				}
				case SND_SOC_TPLG_TUPLE_TYPE_SHORT:
				case SND_SOC_TPLG_TUPLE_TYPE_BOOL:
				{
					u16 *val = (u16 *)((u8 *)object + tokens[i].offset +
							    offset);

					*val = (u16) tuples[j].value;
					break;
				}
				default:
					break;
				}

				num_tokens_matched++;
				if (!(num_tokens_matched % count)) {
					if (num_sets == 1)
						return;

					offset += object_size;
				}
			}
		}
	}
}

int sof_widget_update_ipc_comp_host(struct snd_soc_component *scomp,
				    struct snd_sof_widget *swidget)
{
	struct sof_ipc_comp_host *host;
	size_t ipc_size = sizeof(*host);

	host = (struct sof_ipc_comp_host *)
	       sof_comp_alloc(swidget, &ipc_size, swidget->pipeline_id);
	if (!host)
		return -ENOMEM;

	/* configure host comp IPC message */
	host->comp.type = SOF_COMP_HOST;
	if (swidget->id == snd_soc_dapm_aif_out)
		host->direction = SOF_IPC_STREAM_CAPTURE;
	else
		host->direction = SOF_IPC_STREAM_PLAYBACK;
	host->config.hdr.size = sizeof(host->config);

	/* update pcm_tokens */
	sof_update_ipc_object(host, pcm_tokens, pcm_token_size, swidget->num_tuples,
			      swidget->tuples, sizeof(*host), 1);

	/* update comp_tokens */
	sof_update_ipc_object(&host->config, comp_tokens_new, comp_token_size, swidget->num_tuples,
			      swidget->tuples, sizeof(host->config), 1);

	dev_dbg(scomp->dev, "loaded host %s\n", swidget->widget->name);
	sof_dbg_comp_config(scomp, &host->config);

	swidget->private = host;

	return 0;
}
