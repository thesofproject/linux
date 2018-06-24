// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
/*
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * Copyright(c) 2017 Intel Corporation. All rights reserved.
 *
 * Author: Liam Girdwood <liam.r.girdwood@linux.intel.com>
 */

/* Mixer Controls */

#include <linux/slab.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <sound/soc-topology.h>
#include <sound/soc.h>
#include <sound/control.h>
#include <uapi/sound/sof-ipc.h>
#include "sof-priv.h"

/* return the widget type of the comp the kcontrol is attached to */
static int get_widget_type(struct snd_sof_dev *sdev,
			   struct snd_sof_control *scontrol)
{
	struct snd_sof_widget *swidget;

	list_for_each_entry(swidget, &sdev->widget_list, list) {
		if (swidget->comp_id == scontrol->comp_id)
			return swidget->id;
	}

	/* standalone kcontrol */
	return -EINVAL;
}

/* helper function to send pcm params ipc */
static int siggen_pcm_params(struct snd_sof_control *scontrol,
			     struct snd_sof_dev *sdev)
{
	struct sof_ipc_pcm_params_reply ipc_params_reply;
	struct sof_ipc_pcm_params pcm;
	int ret = 0;

	memset(&pcm, 0, sizeof(pcm));

	/* set IPC PCM parameters */
	pcm.hdr.size = sizeof(pcm);
	pcm.hdr.cmd = SOF_IPC_GLB_STREAM_MSG | SOF_IPC_STREAM_PCM_PARAMS;
	pcm.comp_id = scontrol->comp_id;
	pcm.params.channels = scontrol->num_channels;
	pcm.params.direction = SOF_IPC_STREAM_PLAYBACK;

	/* send IPC to the DSP */
	ret = sof_ipc_tx_message(sdev->ipc, pcm.hdr.cmd, &pcm, sizeof(pcm),
				 &ipc_params_reply, sizeof(ipc_params_reply));
	if (ret < 0)
		dev_err(sdev->dev, "error: setting pcm params for siggen\n");

	return ret;
}

/* helper function to send stream trigger ipc for siggen pipeline */
static int siggen_trigger(struct snd_sof_control *scontrol,
			  struct snd_sof_dev *sdev, int cmd)
{
	struct sof_ipc_stream stream;
	struct sof_ipc_reply reply;
	int ret = 0;

	/* set IPC stream params */
	stream.hdr.size = sizeof(stream);
	stream.hdr.cmd = SOF_IPC_GLB_STREAM_MSG | cmd;
	stream.comp_id = scontrol->comp_id;

	/* send IPC to the DSP */
	ret = sof_ipc_tx_message(sdev->ipc, stream.hdr.cmd, &stream,
				 sizeof(stream), &reply, sizeof(reply));
	if (ret < 0)
		dev_err(sdev->dev, "error: failed to trigger stream\n");

	return ret;
}

/* set the active status for playback/capture for the virtual FE */
static int set_vfe_active_status(struct snd_sof_control *scontrol,
				 struct snd_soc_card *card, int status)
{
	struct snd_soc_pcm_runtime *rtd;

	list_for_each_entry(rtd, &card->rtd_list, list) {
		if (!strcmp(rtd->dai_link->name, scontrol->vfe_link_name)) {

			/* set playback status */
			if (rtd->dai_link->dpcm_playback) {
				rtd->cpu_dai->playback_active = status;
				rtd->codec_dai->playback_active = status;
			}

			/* set capture status */
			if (rtd->dai_link->dpcm_capture) {
				rtd->cpu_dai->capture_active = status;
				rtd->codec_dai->capture_active = status;
			}

			if (status) {
				/* increment the active count for cpu dai */
				rtd->cpu_dai->active++;
			} else {
				/* decrement the active count for cpu dai */
				rtd->cpu_dai->active--;
			}
		}
	}
}

/*
 * Helper function to send ipc's to trigger siggen pipeline
 * The siggen pipeline is enabled/disabled only if the
 * control values change from the old state.
 */
static int siggen_pipeline_trigger(struct snd_sof_control *scontrol,
				   struct snd_sof_dev *sdev,
				   struct snd_soc_card *card,
				   int new_state)
{
	int ret = 0;

	if (!new_state) {

		/* set runtime status as inactive for virtual FE */
		set_vfe_active_status(scontrol, card, 0);

		/* free pcm and reset pipeline */
		ret = siggen_trigger(scontrol, sdev,
				     SOF_IPC_STREAM_PCM_FREE);
	} else {

		/* set runtime status as active for virtual FE */
		set_vfe_active_status(scontrol, card, 1);

		/* enable BE DAI */
		snd_soc_dpcm_runtime_update(card);

		/* set pcm params */
		ret = siggen_pcm_params(scontrol, sdev);
		if (ret < 0)
			return ret;

		/* enable siggen */
		ret = siggen_trigger(scontrol, sdev,
				     SOF_IPC_STREAM_TRIG_START);
	}

	return ret;
}

static inline u32 mixer_to_ipc(unsigned int value, u32 *volume_map, int size)
{
	if (value >= size)
		return volume_map[size - 1];
	else
		return volume_map[value];
}

static inline u32 ipc_to_mixer(u32 value, u32 *volume_map, int size)
{
	int i;

	for (i = 0; i < size; i++) {
		if (volume_map[i] >= value)
			return i;
	}

	return i - 1;
}

int snd_sof_volume_get(struct snd_kcontrol *kcontrol,
		       struct snd_ctl_elem_value *ucontrol)
{
	struct soc_mixer_control *sm =
		(struct soc_mixer_control *)kcontrol->private_value;
	struct snd_sof_control *scontrol = sm->dobj.private;
	struct snd_sof_dev *sdev = scontrol->sdev;
	struct sof_ipc_ctrl_data *cdata = scontrol->control_data;
	unsigned int i, channels = scontrol->num_channels;

	pm_runtime_get_sync(sdev->dev);

	/* get all the mixer data from DSP */
	snd_sof_ipc_get_comp_data(sdev->ipc, scontrol, SOF_IPC_COMP_GET_VALUE,
				  SOF_CTRL_TYPE_VALUE_CHAN_GET,
				  SOF_CTRL_CMD_VOLUME);

	/* read back each channel */
	for (i = 0; i < channels; i++)
		ucontrol->value.integer.value[i] =
			ipc_to_mixer(cdata->chanv[i].value,
				     scontrol->volume_table, sm->max + 1);

	pm_runtime_mark_last_busy(sdev->dev);
	pm_runtime_put_autosuspend(sdev->dev);
	return 0;
}

int snd_sof_volume_put(struct snd_kcontrol *kcontrol,
		       struct snd_ctl_elem_value *ucontrol)
{
	struct soc_mixer_control *sm =
		(struct soc_mixer_control *)kcontrol->private_value;
	struct snd_sof_control *scontrol = sm->dobj.private;
	struct snd_sof_dev *sdev = scontrol->sdev;
	struct sof_ipc_ctrl_data *cdata = scontrol->control_data;
	unsigned int i, channels = scontrol->num_channels;

	pm_runtime_get_sync(sdev->dev);

	/* update each channel */
	for (i = 0; i < channels; i++) {
		cdata->chanv[i].value =
			mixer_to_ipc(ucontrol->value.integer.value[i],
				     scontrol->volume_table, sm->max + 1);
		cdata->chanv[i].channel = i;
	}

	/* notify DSP of mixer updates */
	snd_sof_ipc_set_comp_data(sdev->ipc, scontrol, SOF_IPC_COMP_SET_VALUE,
				  SOF_CTRL_TYPE_VALUE_CHAN_GET,
				  SOF_CTRL_CMD_VOLUME);

	pm_runtime_mark_last_busy(sdev->dev);
	pm_runtime_put_autosuspend(sdev->dev);
	return 0;
}

int snd_sof_enum_get(struct snd_kcontrol *kcontrol,
		     struct snd_ctl_elem_value *ucontrol)
{
	struct soc_enum *se =
		(struct soc_enum *)kcontrol->private_value;
	struct snd_sof_control *scontrol = se->dobj.private;
	struct snd_sof_dev *sdev = scontrol->sdev;
	struct sof_ipc_ctrl_data *cdata = scontrol->control_data;
	unsigned int i, channels = scontrol->num_channels;

	pm_runtime_get_sync(sdev->dev);

	/* get all the mixer data from DSP */
	snd_sof_ipc_get_comp_data(sdev->ipc, scontrol, SOF_IPC_COMP_GET_VALUE,
				  SOF_CTRL_TYPE_VALUE_CHAN_GET,
				  SOF_CTRL_CMD_ENUM);

	/* read back each channel */
	for (i = 0; i < channels; i++)
		ucontrol->value.integer.value[i] = cdata->chanv[i].value;

	pm_runtime_mark_last_busy(sdev->dev);
	pm_runtime_put_autosuspend(sdev->dev);
	return 0;
}

int snd_sof_enum_put(struct snd_kcontrol *kcontrol,
		     struct snd_ctl_elem_value *ucontrol)
{
	struct soc_enum *se =
		(struct soc_enum *)kcontrol->private_value;
	struct snd_sof_control *scontrol = se->dobj.private;
	struct snd_sof_dev *sdev = scontrol->sdev;
	struct sof_ipc_ctrl_data *cdata = scontrol->control_data;
	unsigned int i, channels = scontrol->num_channels;

	pm_runtime_get_sync(sdev->dev);

	/* update each channel */
	for (i = 0; i < channels; i++)
		cdata->chanv[i].value = ucontrol->value.integer.value[i];

	/* notify DSP of mixer updates */
	snd_sof_ipc_set_comp_data(sdev->ipc, scontrol, SOF_IPC_COMP_SET_VALUE,
				  SOF_CTRL_TYPE_VALUE_CHAN_SET,
				  SOF_CTRL_CMD_ENUM);

	pm_runtime_mark_last_busy(sdev->dev);
	pm_runtime_put_autosuspend(sdev->dev);
	return 0;
}

int snd_sof_bytes_get(struct snd_kcontrol *kcontrol,
		      struct snd_ctl_elem_value *ucontrol)
{
	struct soc_bytes_ext *be =
		(struct soc_bytes_ext *)kcontrol->private_value;
	struct snd_sof_control *scontrol = be->dobj.private;
	struct snd_sof_dev *sdev = scontrol->sdev;
	//struct sof_ipc_ctrl_data *cdata = scontrol->control_data;
	//unsigned int i, channels = scontrol->num_channels;

	pm_runtime_get_sync(sdev->dev);

	/* get all the mixer data from DSP */
	snd_sof_ipc_get_comp_data(sdev->ipc, scontrol, SOF_IPC_COMP_GET_DATA,
				  SOF_CTRL_TYPE_DATA_GET, scontrol->cmd);

	/* TODO: copy back to userspace */

	pm_runtime_mark_last_busy(sdev->dev);
	pm_runtime_put_autosuspend(sdev->dev);
	return 0;
}

int snd_sof_bytes_put(struct snd_kcontrol *kcontrol,
		      struct snd_ctl_elem_value *ucontrol)
{
	struct soc_bytes_ext *be =
		(struct soc_bytes_ext *)kcontrol->private_value;
	struct snd_sof_control *scontrol = be->dobj.private;
	struct snd_sof_dev *sdev = scontrol->sdev;
	//struct sof_ipc_ctrl_data *cdata = scontrol->control_data;
	//unsigned int i, channels = scontrol->num_channels;

	pm_runtime_get_sync(sdev->dev);

	/* TODO: copy from userspace */

	/* notify DSP of mixer updates */
	snd_sof_ipc_set_comp_data(sdev->ipc, scontrol, SOF_IPC_COMP_SET_DATA,
				  SOF_CTRL_TYPE_DATA_SET, scontrol->cmd);

	pm_runtime_mark_last_busy(sdev->dev);
	pm_runtime_put_autosuspend(sdev->dev);
	return 0;
}

/* IO handlers for switch kcontrol handlers */
int snd_sof_switch_get(struct snd_kcontrol *kcontrol,
		       struct snd_ctl_elem_value *ucontrol)
{
	struct soc_mixer_control *sm =
		(struct soc_mixer_control *)kcontrol->private_value;
	struct snd_sof_control *scontrol = sm->dobj.private;
	struct snd_sof_dev *sdev = scontrol->sdev;
	struct sof_ipc_ctrl_data *cdata = scontrol->control_data;
	unsigned int i, channels = scontrol->num_channels;

	pm_runtime_get_sync(sdev->dev);

	/* get all the mixer data from DSP */
	snd_sof_ipc_get_comp_data(sdev->ipc, scontrol, SOF_IPC_COMP_GET_VALUE,
				  SOF_CTRL_TYPE_VALUE_CHAN_GET,
				  SOF_CTRL_CMD_SWITCH);

	/* read back each channel */
	for (i = 0; i < channels; i++)
		ucontrol->value.integer.value[i] = cdata->chanv[i].value;

	pm_runtime_mark_last_busy(sdev->dev);
	pm_runtime_put_autosuspend(sdev->dev);
	return 0;
}

int snd_sof_switch_put(struct snd_kcontrol *kcontrol,
		       struct snd_ctl_elem_value *ucontrol)
{
	struct soc_mixer_control *sm =
		(struct soc_mixer_control *)kcontrol->private_value;
	struct snd_sof_control *scontrol = sm->dobj.private;
	struct snd_sof_dev *sdev = scontrol->sdev;
	struct sof_ipc_ctrl_data *cdata = scontrol->control_data;
	struct snd_soc_dapm_context *dapm =
		snd_soc_dapm_kcontrol_dapm(kcontrol);
	struct snd_soc_card *card = dapm->card;
	unsigned int i, channels = scontrol->num_channels;
	int ret = 0, new_state, old_state;
	int changed = 0;

	pm_runtime_get_sync(sdev->dev);

	switch (get_widget_type(sdev, scontrol)) {
	case snd_soc_dapm_pga:

		/*
		 * if the kcontrol is used for processing as in the case of pga,
		 * values are channel-specific
		 */
		for (i = 0; i < channels; i++) {
			new_state = ucontrol->value.integer.value[i];
			old_state = cdata->chanv[i].value;
			if (new_state != old_state)
				changed = 1;
			cdata->chanv[i].value = new_state;
			cdata->chanv[i].channel = i;
		}

		/*
		 * notify DSP of switch state update
		 * if the control values are different
		 */
		if (changed)
			snd_sof_ipc_set_comp_data(sdev->ipc, scontrol,
						  SOF_IPC_COMP_SET_VALUE,
						  SOF_CTRL_TYPE_VALUE_CHAN_GET,
						  SOF_CTRL_CMD_SWITCH);
		break;
	case snd_soc_dapm_siggen:

		/*
		 * A siggen kcontrol is used as an ON/OFF switch,
		 * so all channel values are assumed to be identical
		 */
		for (i = 0; i < channels; i++) {
			new_state = ucontrol->value.integer.value[0];
			old_state = cdata->chanv[0].value;
			if (new_state != old_state)
				changed = 1;
			cdata->chanv[i].value = new_state;
			cdata->chanv[i].channel = i;
		}

		if (changed) {

			/*
			 * notify DSP of switch state update
			 * if the control values are different
			 */
			snd_sof_ipc_set_comp_data(sdev->ipc, scontrol,
						  SOF_IPC_COMP_SET_VALUE,
						  SOF_CTRL_TYPE_VALUE_CHAN_GET,
						  SOF_CTRL_CMD_SWITCH);

			/* trigger siggen pipeline */
			ret = siggen_pipeline_trigger(scontrol, sdev,
						      card, new_state);
			if (ret < 0) {
				dev_err(sdev->dev,
					"error: triggering siggen pipeline\n");
				changed = ret;
			}
		}
		break;
	default:

		/*
		 * if the kcontrol is for routing or a standalone control,
		 * all channel values are assumed to be identical
		 */
		for (i = 0; i < channels; i++) {
			new_state = ucontrol->value.integer.value[0];
			old_state = cdata->chanv[0].value;
			if (new_state != old_state)
				changed = 1;
			cdata->chanv[i].value = new_state;
			cdata->chanv[i].channel = i;
		}

		/*
		 * notify DSP of switch state update
		 * if the control values are different
		 */
		if (changed)
			snd_sof_ipc_set_comp_data(sdev->ipc, scontrol,
						  SOF_IPC_COMP_SET_VALUE,
						  SOF_CTRL_TYPE_VALUE_CHAN_GET,
						  SOF_CTRL_CMD_SWITCH);

		break;
	}

	pm_runtime_mark_last_busy(sdev->dev);
	pm_runtime_put_autosuspend(sdev->dev);
	return changed;
}
