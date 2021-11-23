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


int sof_widget_update_ipc_comp_dai(struct snd_soc_component *scomp,
				    struct snd_sof_widget *swidget)
{
	struct sof_ipc_comp_dai *comp_dai;
	size_t ipc_size = sizeof(*comp_dai);
	struct snd_sof_dai *dai = swidget->private;

	comp_dai = (struct sof_ipc_comp_dai *)
	       sof_comp_alloc(swidget, &ipc_size, swidget->pipeline_id);
	if (!comp_dai)
		return -ENOMEM;

	/* configure dai IPC message */
	comp_dai->comp.type = SOF_COMP_DAI;
	comp_dai->config.hdr.size = sizeof(comp_dai->config);

	/* update dai_tokens */
	sof_update_ipc_object(comp_dai, dai_tokens_new, dai_token_size, dai->num_tuples,
			      dai->tuples, sizeof(*comp_dai), 1);

	/* update comp_tokens */
	sof_update_ipc_object(&comp_dai->config, comp_tokens_new, comp_token_size, dai->num_tuples,
			      dai->tuples, sizeof(comp_dai->config), 1);

	/*
	 * copy only the sof_ipc_comp_dai to avoid collapsing
	 * the snd_sof_dai, the extended data is kept in the
	 * snd_sof_widget.
	 */
	memcpy(&dai->comp_dai, comp_dai, sizeof(*comp_dai));

	dev_dbg(scomp->dev, "%s dai %s: type %d index %d\n",
		__func__, swidget->widget->name, comp_dai->type, comp_dai->dai_index);
	sof_dbg_comp_config(scomp, &comp_dai->config);
	return 0;
}

int sof_widget_update_ipc_comp_pipeline(struct snd_soc_component *scomp,
					struct snd_sof_widget *swidget)
{
	struct sof_ipc_pipe_new *pipeline;
	struct snd_sof_widget *comp_swidget;
	size_t ipc_size = sizeof(*pipeline);

	pipeline = (struct sof_ipc_pipe_new *)
	       sof_comp_alloc(swidget, &ipc_size, swidget->pipeline_id);
	if (!pipeline)
		return -ENOMEM;

	/* configure dai IPC message */
	pipeline->hdr.size = sizeof(*pipeline);
	pipeline->hdr.cmd = SOF_IPC_GLB_TPLG_MSG | SOF_IPC_TPLG_PIPE_NEW;
	pipeline->pipeline_id = swidget->pipeline_id;
	pipeline->comp_id = swidget->comp_id;

	/* component at start of pipeline is our stream id */
	comp_swidget = snd_sof_find_swidget(scomp, swidget->sname);;

	if (!comp_swidget) {
		dev_err(scomp->dev, "error: widget %s refers to non existent widget %s\n",
			swidget->widget->name, swidget->sname);
		return -EINVAL;
	}

	pipeline->sched_id = comp_swidget->comp_id;

	sof_update_ipc_object(pipeline, sched_tokens, sched_token_size, swidget->num_tuples,
			      swidget->tuples, sizeof(*pipeline), 1);

	sof_update_ipc_object(swidget, pipeline_tokens, pipeline_token_size, swidget->num_tuples,
			      swidget->tuples, sizeof(*swidget), 1);

	if (sof_debug_check_flag(SOF_DBG_DISABLE_MULTICORE))
		pipeline->core = SOF_DSP_PRIMARY_CORE;

	if (sof_debug_check_flag(SOF_DBG_DYNAMIC_PIPELINES_OVERRIDE))
		swidget->dynamic_pipeline_widget =
			sof_debug_check_flag(SOF_DBG_DYNAMIC_PIPELINES_ENABLE);

	dev_dbg(scomp->dev, "pipeline %s: period %d pri %d mips %d core %d frames %d dynamic %d\n",
		swidget->widget->name, pipeline->period, pipeline->priority,
		pipeline->period_mips, pipeline->core, pipeline->frames_per_sched,
		swidget->dynamic_pipeline_widget);

	swidget->private = pipeline;

	return 0;
}
