// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
//
// This file is provided under a dual BSD/GPLv2 license.  When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2021 Intel Corporation. All rights reserved.
//
//

#include <linux/bits.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/firmware.h>
#include <linux/workqueue.h>
#include <sound/pcm_params.h>
#include <sound/sof/topology.h>
#include <sound/tlv.h>
#include <uapi/sound/sof/tokens.h>
#include "ops.h"
#include "sof-audio.h"
#include "sof-priv.h"
#include "ipc4.h"
#include "ipc4-topology.h"
#include "topology-tokens.h"

/* 20th root of 10 in Q1.31 fixed-point notation*/
#define VOL_FORTIETH_ROOT_OF_TEN	0x8f9e4d00ULL

/* max value in Q1.31 fixed-point */
#define IPC4_VOL_MAX	0x7fffffff
/*
 * Volume fractional word length define to 31 sets
 * the volume linear gain value to use Q1.31 format
 */
#define VOLUME_FWL	31

/* TLV data items */
#define TLV_ITEMS	3
#define TLV_MIN	0
#define TLV_STEP	1
#define TLV_MUTE	2

static inline u32 vol_compute_64(u64 i, u32 x)
{
	return (u32)(((i >> (x - 1)) + 1) >> 1);
}

static inline int get_tlv_data(const int *p, int tlv[TLV_ITEMS])
{
	/* we only support dB scale TLV type at the moment */
	if ((int)p[SNDRV_CTL_TLVO_TYPE] != SNDRV_CTL_TLVT_DB_SCALE)
		return -EINVAL;

	tlv[TLV_MIN] = (int)p[SNDRV_CTL_TLVO_DB_SCALE_MIN];

	/* volume steps */
	tlv[TLV_STEP] = (int)(p[SNDRV_CTL_TLVO_DB_SCALE_MUTE_AND_STEP] &
				TLV_DB_SCALE_MASK);

	/* mute ON/OFF */
	if ((p[SNDRV_CTL_TLVO_DB_SCALE_MUTE_AND_STEP] &
		TLV_DB_SCALE_MUTE) == 0)
		tlv[TLV_MUTE] = 0;
	else
		tlv[TLV_MUTE] = 1;

	return 0;
}

static int ipc4_set_up_volume_table(struct snd_sof_control *scontrol,
				    int tlv[TLV_ITEMS], int size)
{
	u64 tmp;
	int j;

	/* init the volume table */
	scontrol->volume_table = kcalloc(size, sizeof(u32), GFP_KERNEL);
	if (!scontrol->volume_table)
		return -ENOMEM;

	scontrol->volume_table[0] = 0;
	scontrol->volume_table[1] = tlv[TLV_MIN];
	/* populate the volume table */
	for (j = 2; j < size - 1; j++) {
		tmp = (u64)scontrol->volume_table[j - 1] * VOL_FORTIETH_ROOT_OF_TEN;
		scontrol->volume_table[j] = vol_compute_64(tmp, VOLUME_FWL);
	}
	scontrol->volume_table[size - 1] = IPC4_VOL_MAX;

	return 0;
}

int sof_ipc4_control_load_volume(struct snd_soc_component *scomp,
				 struct snd_sof_control *scontrol,
				 struct snd_kcontrol_new *kc,
				 struct snd_soc_tplg_ctl_hdr *hdr)
{
	struct snd_sof_dev *sdev = snd_soc_component_get_drvdata(scomp);
	struct snd_soc_tplg_mixer_control *mc =
		container_of(hdr, struct snd_soc_tplg_mixer_control, hdr);
	struct sof_ipc_ctrl_data *cdata;
	int tlv[TLV_ITEMS];
	unsigned int i;
	int ret;

	/* validate topology data */
	if (le32_to_cpu(mc->num_channels) > SND_SOC_TPLG_MAX_CHAN) {
		ret = -EINVAL;
		goto out;
	}

	/*
	 * If control has more than 2 channels we need to override the info. This is because even if
	 * ASoC layer has defined topology's max channel count to SND_SOC_TPLG_MAX_CHAN = 8, the
	 * pre-defined dapm control types (and related functions) creating the actual control
	 * restrict the channels only to mono or stereo.
	 */
	if (le32_to_cpu(mc->num_channels) > 2)
		kc->info = snd_sof_volume_info;

	/* init the volume get/put data */
	scontrol->size = struct_size(scontrol->control_data, chanv,
				     le32_to_cpu(mc->num_channels));
	scontrol->control_data = kzalloc(scontrol->size, GFP_KERNEL);
	if (!scontrol->control_data) {
		ret = -ENOMEM;
		goto out;
	}

	scontrol->comp_id = sdev->next_comp_id;
	scontrol->min_volume_step = le32_to_cpu(mc->min);
	scontrol->max_volume_step = le32_to_cpu(mc->max);
	scontrol->num_channels = le32_to_cpu(mc->num_channels);
	scontrol->control_data->index = kc->index;

	/* set cmd for mixer control */
	if (le32_to_cpu(mc->max) == 1) {
		scontrol->cmd = SOF_CTRL_CMD_SWITCH;
		return 0;
	}

	scontrol->cmd = SOF_CTRL_CMD_VOLUME;

	if (!kc->tlv.p || get_tlv_data(kc->tlv.p, tlv) < 0) {
		dev_err(scomp->dev, "error: invalid TLV data\n");
		ret = -EINVAL;
		goto out_free;
	}

	/* set up volume table */
	ret = ipc4_set_up_volume_table(scontrol, tlv, le32_to_cpu(mc->max) + 1);
	if (ret < 0) {
		dev_err(scomp->dev, "error: setting up volume table\n");
		goto out_free;
	}

	/* set default volume values to 0dB in control */
	cdata = scontrol->control_data;
	for (i = 0; i < scontrol->num_channels; i++) {
		cdata->chanv[i].channel = i;
		cdata->chanv[i].value = IPC4_VOL_MAX;
	}

	return 0;

out_free:
	kfree(scontrol->control_data);
out:
	return ret;
}

int sof_ipc4_widget_load_dai(struct snd_soc_component *scomp, int index,
			     struct snd_sof_widget *swidget,
			     struct snd_soc_tplg_dapm_widget *tw,
			     struct snd_sof_dai *dai)
{
	struct snd_soc_tplg_private *private = &tw->priv;
	struct sof_ipc_comp_dai *comp_dai;
	size_t size = sizeof(*comp_dai);
	struct sof_ipc4_dai *ipc4_dai;
	int ret;

	comp_dai = (struct sof_ipc_comp_dai *)
		   sof_comp_alloc(swidget, &size, index);
	if (!comp_dai)
		return -ENOMEM;

	ret = sof_parse_tokens(scomp, comp_dai, dai_tokens,
			       ARRAY_SIZE(dai_tokens), private->array,
			       le32_to_cpu(private->size));
	if (ret != 0)
		goto finish;

	ipc4_dai = kzalloc(sizeof(*ipc4_dai), GFP_KERNEL);
	if (!ipc4_dai)
		return -ENOMEM;

	dev_dbg(scomp->dev, "dai %s, type %d\n", swidget->widget->name,
		comp_dai->type);

	if (dai) {
		dai->scomp = scomp;

		/*
		 * copy only the sof_ipc_comp_dai to avoid collapsing
		 * the snd_sof_dai, the extended data is kept in the
		 * snd_sof_widget.
		 */
		memcpy(&dai->comp_dai, comp_dai, sizeof(*comp_dai));
	}

finish:
	kfree(comp_dai);
	dai->private = ipc4_dai;
	return ret;
}

/*
 * PCM Topology
 */

int sof_ipc4_widget_load_pcm(struct snd_soc_component *scomp, int index,
			     struct snd_sof_widget *swidget,
			     enum sof_ipc_stream_direction dir,
			     struct snd_soc_tplg_dapm_widget *tw)
{
	struct sof_ipc4_host *host;
	size_t size = sizeof(*host);

	host = (struct sof_ipc4_host *)
	       sof_comp_alloc(swidget, &size, index);
	if (!host)
		return -ENOMEM;

	dev_dbg(scomp->dev, "loaded host %s\n", swidget->widget->name);

	swidget->private = host;

	return 0;
}

int sof_ipc4_widget_load_pipeline(struct snd_soc_component *scomp, int index,
				  struct snd_sof_widget *swidget,
				  struct snd_soc_tplg_dapm_widget *tw)
{
	struct snd_soc_tplg_private *private = &tw->priv;
	struct sof_ipc4_pipeline *pipeline;
	int ret;

	pipeline = kzalloc(sizeof(*pipeline), GFP_KERNEL);
	if (!pipeline)
		return -ENOMEM;

	pipeline->pipe_new.pipeline_id = swidget->pipeline_id;

	ret = sof_parse_tokens(scomp, pipeline, sched_tokens,
			       ARRAY_SIZE(sched_tokens), private->array,
			       le32_to_cpu(private->size));
	if (ret != 0)
		goto err;

	ret = sof_parse_tokens(scomp, pipeline, ipc4_sched_tokens,
			       ARRAY_SIZE(ipc4_sched_tokens), private->array,
			       le32_to_cpu(private->size));
	if (ret != 0) {
		dev_err(scomp->dev, "error: parse pipeline tokens failed %d\n",
			private->size);
		goto err;
	}

	dev_dbg(scomp->dev, "pipeline %s: id %d pri %d core %d lp mode %d\n",
		swidget->widget->name, pipeline->pipe_new.pipeline_id,
		pipeline->pipe_new.priority, pipeline->pipe_new.core,
		pipeline->lp_mode);

	swidget->private = pipeline;

	return 0;
err:
	kfree(pipeline);
	return ret;
}

int sof_ipc4_widget_load_pga(struct snd_soc_component *scomp, int index,
			     struct snd_sof_widget *swidget,
			     struct snd_soc_tplg_dapm_widget *tw)
{
	struct snd_soc_tplg_private *private = &tw->priv;
	struct sof_ipc4_gain *gain;
	int ret;

	gain = kzalloc(sizeof(*gain), GFP_KERNEL);
	if (!gain)
		return -ENOMEM;

	if (!le32_to_cpu(tw->num_kcontrols)) {
		dev_err(scomp->dev, "error: invalid kcontrol count %d for volume\n",
			tw->num_kcontrols);
		ret = -EINVAL;
		goto err;
	}

	ret = sof_parse_tokens(scomp, &gain->data, gain_tokens,
			       ARRAY_SIZE(gain_tokens), private->array,
			       le32_to_cpu(private->size));
	if (ret != 0) {
		dev_err(scomp->dev, "error: parse gain tokens failed %d\n",
			private->size);
		goto err;
	}

	dev_dbg(scomp->dev, "tplg2: ready widget %s, ramp_type %d, duration %d, val %d",
		swidget->widget->name, gain->data.curve_type, gain->data.curve_duration,
		gain->data.init_val);

	swidget->private = gain;

	return 0;
err:
	kfree(gain);
	return ret;
}

int sof_ipc4_widget_load_mixer(struct snd_soc_component *scomp, int index,
			       struct snd_sof_widget *swidget,
			       struct snd_soc_tplg_dapm_widget *tw)
{
	struct snd_soc_tplg_private *private = &tw->priv;
	struct sof_ipc4_mixer *mixer;
	size_t ipc_size = sizeof(*mixer);
	int ret;

	mixer = (struct sof_ipc4_mixer *)sof_comp_alloc(swidget, &ipc_size, index);
	if (!mixer)
		return -ENOMEM;

	ret = sof_parse_tokens(scomp, &mixer, ipc4_mixer_tokens,
			       ARRAY_SIZE(ipc4_mixer_tokens), private->array,
			       le32_to_cpu(private->size));
	if (ret != 0) {
		dev_err(scomp->dev, "error: parse mixer tokens failed %d\n",
			private->size);
		kfree(mixer);
		return ret;
	}

	dev_dbg(scomp->dev, "mixer type %d", mixer->type);

	swidget->private = mixer;

	return 0;
}

