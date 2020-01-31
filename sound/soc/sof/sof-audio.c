// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
//
// This file is provided under a dual BSD/GPLv2 license.  When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2019 Intel Corporation. All rights reserved.
//
// Author: Ranjani Sridharan <ranjani.sridharan@linux.intel.com>
//

#include <linux/device.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include "sof-audio.h"
#include "ops.h"

/* IPC stream position */
void sof_audio_ipc_period_elapsed(struct device *dev, u32 msg_id)
{
	struct snd_sof_pcm_stream *stream;
	struct sof_ipc_stream_posn posn;
	struct snd_sof_pcm *spcm;
	int direction;

	/* if spcm is not found, this IPC is likely not for this client */
	spcm = snd_sof_find_spcm_comp(dev, msg_id, &direction);
	if (!spcm)
		return;

	stream = &spcm->stream[direction];
	sof_client_ipc_msg_data(dev, stream->substream, &posn, sizeof(posn));

	dev_dbg(dev, "posn : host 0x%llx dai 0x%llx wall 0x%llx\n",
		posn.host_posn, posn.dai_posn, posn.wallclock);

	memcpy(&stream->posn, &posn, sizeof(posn));

	/* only inform ALSA for period_wakeup mode */
	if (!stream->substream->runtime->no_period_wakeup)
		snd_sof_pcm_period_elapsed(stream->substream);
}
EXPORT_SYMBOL_NS(sof_audio_ipc_period_elapsed, SND_SOC_SOF_AUDIO);

/* DSP notifies host of an XRUN within FW */
void sof_audio_ipc_xrun(struct device *dev, u32 msg_id)
{
	struct snd_sof_pcm_stream *stream;
	struct sof_ipc_stream_posn posn;
	struct snd_sof_pcm *spcm;
	int direction;

	/* if spcm is not found, this IPC is likely not for this client */
	spcm = snd_sof_find_spcm_comp(dev, msg_id, &direction);
	if (!spcm)
		return;

	stream = &spcm->stream[direction];
	sof_client_ipc_msg_data(dev, stream->substream, &posn, sizeof(posn));

	dev_dbg(dev,  "posn XRUN: host %llx comp %d size %d\n",
		posn.host_posn, posn.xrun_comp_id, posn.xrun_size);

#if defined(CONFIG_SND_SOC_SOF_DEBUG_XRUN_STOP)
	/* stop PCM on XRUN - used for pipeline debug */
	memcpy(&stream->posn, &posn, sizeof(posn));
	snd_pcm_stop_xrun(stream->substream);
#endif
}
EXPORT_SYMBOL_NS(sof_audio_ipc_xrun, SND_SOC_SOF_AUDIO);

/* stream notifications from DSP FW */
static void sof_audio_ipc_rx(struct device *dev, u32 msg_cmd)
{
	/* get msg cmd type and msd id */
	u32 msg_type = msg_cmd & SOF_CMD_TYPE_MASK;
	u32 msg_id = SOF_IPC_MESSAGE_ID(msg_cmd);

	switch (msg_type) {
	case SOF_IPC_STREAM_POSITION:
		sof_audio_ipc_period_elapsed(dev, msg_id);
		break;
	case SOF_IPC_STREAM_TRIG_XRUN:
		sof_audio_ipc_xrun(dev, msg_id);
		break;
	default:
		/* ignore unsupported messages */
		break;
	}
}

/*
 * IPC get()/set() for kcontrols.
 */
int sof_audio_ipc_set_get_comp_data(struct snd_sof_control *scontrol,
				    u32 ipc_cmd,
				    enum sof_ipc_ctrl_type ctrl_type,
				    enum sof_ipc_ctrl_cmd ctrl_cmd,
				    bool send)
{
	struct snd_soc_component *scomp = scontrol->scomp;
	struct sof_ipc_ctrl_data *cdata = scontrol->control_data;
	struct snd_sof_dev *sdev = snd_soc_component_get_drvdata(scomp);
	struct sof_ipc_ctrl_data_params sparams;
	size_t send_bytes;
	int err;

	/* read or write firmware volume */
	if (scontrol->readback_offset != 0) {
		/* write/read value header via mmaped region */
		send_bytes = sizeof(struct sof_ipc_ctrl_value_chan) *
		cdata->num_elems;
		if (send)
			snd_sof_dsp_block_write(sdev, sdev->mmio_bar,
						scontrol->readback_offset,
						cdata->chanv, send_bytes);

		else
			snd_sof_dsp_block_read(sdev, sdev->mmio_bar,
					       scontrol->readback_offset,
					       cdata->chanv, send_bytes);
		return 0;
	}

	cdata->rhdr.hdr.cmd = SOF_IPC_GLB_COMP_MSG | ipc_cmd;
	cdata->cmd = ctrl_cmd;
	cdata->type = ctrl_type;
	cdata->comp_id = scontrol->comp_id;
	cdata->msg_index = 0;

	/* calculate header and data size */
	switch (cdata->type) {
	case SOF_CTRL_TYPE_VALUE_CHAN_GET:
	case SOF_CTRL_TYPE_VALUE_CHAN_SET:
		sparams.msg_bytes = scontrol->num_channels *
			sizeof(struct sof_ipc_ctrl_value_chan);
		sparams.hdr_bytes = sizeof(struct sof_ipc_ctrl_data);
		sparams.elems = scontrol->num_channels;
		break;
	case SOF_CTRL_TYPE_VALUE_COMP_GET:
	case SOF_CTRL_TYPE_VALUE_COMP_SET:
		sparams.msg_bytes = scontrol->num_channels *
			sizeof(struct sof_ipc_ctrl_value_comp);
		sparams.hdr_bytes = sizeof(struct sof_ipc_ctrl_data);
		sparams.elems = scontrol->num_channels;
		break;
	case SOF_CTRL_TYPE_DATA_GET:
	case SOF_CTRL_TYPE_DATA_SET:
		sparams.msg_bytes = cdata->data->size;
		sparams.hdr_bytes = sizeof(struct sof_ipc_ctrl_data) +
			sizeof(struct sof_abi_hdr);
		sparams.elems = cdata->data->size;
		break;
	default:
		return -EINVAL;
	}

	cdata->rhdr.hdr.size = sparams.msg_bytes + sparams.hdr_bytes;
	cdata->num_elems = sparams.elems;
	cdata->elems_remaining = 0;

	/* send normal size ipc in one part */
	if (cdata->rhdr.hdr.size <= SOF_IPC_MSG_MAX_SIZE) {
		err = sof_ipc_tx_message(sdev->ipc, cdata->rhdr.hdr.cmd, cdata,
					 cdata->rhdr.hdr.size, cdata,
					 cdata->rhdr.hdr.size);

		if (err < 0)
			dev_err(sdev->dev, "error: set/get ctrl ipc comp %d\n",
				cdata->comp_id);

		return err;
	}

	/* data is bigger than max ipc size, chop into smaller pieces */
	dev_dbg(sdev->dev, "large ipc size %u, control size %u\n",
		cdata->rhdr.hdr.size, scontrol->size);

	err = sof_ipc_set_get_large_ctrl_data(sdev->dev, cdata, &sparams, send);
	if (err < 0)
		dev_err(sdev->dev, "error: set/get large ctrl ipc comp %d\n",
			cdata->comp_id);

	return err;
}
EXPORT_SYMBOL(sof_audio_ipc_set_get_comp_data);

/*
 * helper to determine if there are only D0i3 compatible
 * streams active
 */
bool snd_sof_dsp_only_d0i3_compatible_stream_active(struct snd_sof_dev *sdev)
{
	struct snd_sof_audio_data *audio_data = sdev->sof_audio_data;
	struct snd_pcm_substream *substream;
	struct snd_sof_pcm *spcm;
	bool d0i3_compatible_active = false;
	int dir;

	list_for_each_entry(spcm, &audio_data->pcm_list, list) {
		for (dir = 0; dir <= SNDRV_PCM_STREAM_CAPTURE; dir++) {
			substream = spcm->stream[dir].substream;
			if (!substream || !substream->runtime)
				continue;

			/*
			 * substream->runtime being not NULL indicates that
			 * that the stream is open. No need to check the
			 * stream state.
			 */
			if (!spcm->stream[dir].d0i3_compatible)
				return false;

			d0i3_compatible_active = true;
		}
	}

	return d0i3_compatible_active;
}
EXPORT_SYMBOL(snd_sof_dsp_only_d0i3_compatible_stream_active);

bool snd_sof_stream_suspend_ignored(struct snd_sof_dev *sdev)
{
	struct snd_sof_audio_data *audio_data = sdev->sof_audio_data;
	struct snd_sof_pcm *spcm;

	list_for_each_entry(spcm, &audio_data->pcm_list, list) {
		if (spcm->stream[SNDRV_PCM_STREAM_PLAYBACK].suspend_ignored ||
		    spcm->stream[SNDRV_PCM_STREAM_CAPTURE].suspend_ignored)
			return true;
	}

	return false;
}

int sof_set_hw_params_upon_resume(struct device *dev)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(dev);
	struct snd_sof_audio_data *audio_data = sdev->sof_audio_data;
	struct snd_pcm_substream *substream;
	struct snd_sof_pcm *spcm;
	snd_pcm_state_t state;
	int dir;

	/*
	 * SOF requires hw_params to be set-up internally upon resume.
	 * So, set the flag to indicate this for those streams that
	 * have been suspended.
	 */
	list_for_each_entry(spcm, &audio_data->pcm_list, list) {
		for (dir = 0; dir <= SNDRV_PCM_STREAM_CAPTURE; dir++) {
			/*
			 * do not reset hw_params upon resume for streams that
			 * were kept running during suspend
			 */
			if (spcm->stream[dir].suspend_ignored)
				continue;

			substream = spcm->stream[dir].substream;
			if (!substream || !substream->runtime)
				continue;

			state = substream->runtime->status->state;
			if (state == SNDRV_PCM_STATE_SUSPENDED)
				spcm->prepared[dir] = false;
		}
	}

	/* set internal flag for BE */
	return snd_sof_dsp_hw_params_upon_resume(sdev);
}

static int sof_restore_kcontrols(struct device *dev)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(dev);
	struct snd_sof_audio_data *audio_data = sdev->sof_audio_data;
	struct snd_sof_control *scontrol;
	int ipc_cmd, ctrl_type;
	int ret = 0;

	/* restore kcontrol values */
	list_for_each_entry(scontrol, &audio_data->kcontrol_list, list) {
		/* reset readback offset for scontrol after resuming */
		scontrol->readback_offset = 0;

		/* notify DSP of kcontrol values */
		switch (scontrol->cmd) {
		case SOF_CTRL_CMD_VOLUME:
		case SOF_CTRL_CMD_ENUM:
		case SOF_CTRL_CMD_SWITCH:
			ipc_cmd = SOF_IPC_COMP_SET_VALUE;
			ctrl_type = SOF_CTRL_TYPE_VALUE_CHAN_SET;
			ret = sof_audio_ipc_set_get_comp_data(scontrol,
							      ipc_cmd,
							      ctrl_type,
							      scontrol->cmd,
							      true);
			break;
		case SOF_CTRL_CMD_BINARY:
			ipc_cmd = SOF_IPC_COMP_SET_DATA;
			ctrl_type = SOF_CTRL_TYPE_DATA_SET;
			ret = sof_audio_ipc_set_get_comp_data(scontrol,
							      ipc_cmd,
							      ctrl_type,
							      scontrol->cmd,
							      true);
			break;

		default:
			break;
		}

		if (ret < 0) {
			dev_err(dev,
				"error: failed kcontrol value set for widget: %d\n",
				scontrol->comp_id);

			return ret;
		}
	}

	return 0;
}

int sof_restore_pipelines(struct device *dev)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(dev);
	struct snd_sof_audio_data *audio_data = sdev->sof_audio_data;
	struct snd_sof_widget *swidget;
	struct snd_sof_route *sroute;
	struct sof_ipc_pipe_new *pipeline;
	struct snd_sof_dai *dai;
	struct sof_ipc_comp_dai *comp_dai;
	struct sof_ipc_cmd_hdr *hdr;
	int ret;

	/* restore pipeline components */
	list_for_each_entry_reverse(swidget, &audio_data->widget_list, list) {
		struct sof_ipc_comp_reply r;

		/* skip if there is no private data */
		if (!swidget->private)
			continue;

		switch (swidget->id) {
		case snd_soc_dapm_dai_in:
		case snd_soc_dapm_dai_out:
			dai = swidget->private;
			comp_dai = &dai->comp_dai;
			ret = sof_ipc_tx_message(sdev->ipc,
						 comp_dai->comp.hdr.cmd,
						 comp_dai, sizeof(*comp_dai),
						 &r, sizeof(r));
			break;
		case snd_soc_dapm_scheduler:

			/*
			 * During suspend, all DSP cores are powered off.
			 * Therefore upon resume, create the pipeline comp
			 * and power up the core that the pipeline is
			 * scheduled on.
			 */
			pipeline = swidget->private;
			ret = sof_load_pipeline_ipc(dev, pipeline, &r);
			break;
		default:
			hdr = swidget->private;
			ret = sof_ipc_tx_message(sdev->ipc, hdr->cmd,
						 swidget->private, hdr->size,
						 &r, sizeof(r));
			break;
		}
		if (ret < 0) {
			dev_err(dev,
				"error: failed to load widget type %d with ID: %d\n",
				swidget->widget->id, swidget->comp_id);

			return ret;
		}
	}

	/* restore pipeline connections */
	list_for_each_entry_reverse(sroute, &audio_data->route_list, list) {
		struct sof_ipc_pipe_comp_connect *connect;
		struct sof_ipc_reply reply;

		/* skip if there's no private data */
		if (!sroute->private)
			continue;

		connect = sroute->private;

		/* send ipc */
		ret = sof_ipc_tx_message(sdev->ipc,
					 connect->hdr.cmd,
					 connect, sizeof(*connect),
					 &reply, sizeof(reply));
		if (ret < 0) {
			dev_err(dev,
				"error: failed to load route sink %s control %s source %s\n",
				sroute->route->sink,
				sroute->route->control ? sroute->route->control
					: "none",
				sroute->route->source);

			return ret;
		}
	}

	/* restore dai links */
	list_for_each_entry_reverse(dai, &audio_data->dai_list, list) {
		struct sof_ipc_reply reply;
		struct sof_ipc_dai_config *config = dai->dai_config;

		if (!config) {
			dev_err(dev, "error: no config for DAI %s\n",
				dai->name);
			continue;
		}

		/*
		 * The link DMA channel would be invalidated for running
		 * streams but not for streams that were in the PAUSED
		 * state during suspend. So invalidate it here before setting
		 * the dai config in the DSP.
		 */
		if (config->type == SOF_DAI_INTEL_HDA)
			config->hda.link_dma_ch = DMA_CHAN_INVALID;

		ret = sof_ipc_tx_message(sdev->ipc,
					 config->hdr.cmd, config,
					 config->hdr.size,
					 &reply, sizeof(reply));

		if (ret < 0) {
			dev_err(dev,
				"error: failed to set dai config for %s\n",
				dai->name);

			return ret;
		}
	}

	/* complete pipeline */
	list_for_each_entry(swidget, &audio_data->widget_list, list) {
		switch (swidget->id) {
		case snd_soc_dapm_scheduler:
			swidget->complete =
				snd_sof_complete_pipeline(dev, swidget);
			break;
		default:
			break;
		}
	}

	/* restore pipeline kcontrols */
	ret = sof_restore_kcontrols(dev);
	if (ret < 0)
		dev_err(dev,
			"error: restoring kcontrols after resume\n");

	return ret;
}

/*
 * Generic object lookup APIs.
 */

struct snd_sof_pcm *snd_sof_find_spcm_name(struct device *dev,
					   const char *name)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(dev);
	struct snd_sof_audio_data *audio_data = sdev->sof_audio_data;
	struct snd_sof_pcm *spcm;

	list_for_each_entry(spcm, &audio_data->pcm_list, list) {
		/* match with PCM dai name */
		if (strcmp(spcm->pcm.dai_name, name) == 0)
			return spcm;

		/* match with playback caps name if set */
		if (*spcm->pcm.caps[0].name &&
		    !strcmp(spcm->pcm.caps[0].name, name))
			return spcm;

		/* match with capture caps name if set */
		if (*spcm->pcm.caps[1].name &&
		    !strcmp(spcm->pcm.caps[1].name, name))
			return spcm;
	}

	return NULL;
}

struct snd_sof_pcm *snd_sof_find_spcm_comp(struct device *dev,
					   unsigned int comp_id,
					   int *direction)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(dev);
	struct snd_sof_audio_data *audio_data = sdev->sof_audio_data;
	struct snd_sof_pcm *spcm;
	int dir;

	list_for_each_entry(spcm, &audio_data->pcm_list, list) {
		dir = SNDRV_PCM_STREAM_PLAYBACK;
		if (spcm->stream[dir].comp_id == comp_id) {
			*direction = dir;
			return spcm;
		}

		dir = SNDRV_PCM_STREAM_CAPTURE;
		if (spcm->stream[dir].comp_id == comp_id) {
			*direction = dir;
			return spcm;
		}
	}

	return NULL;
}

struct snd_sof_pcm *snd_sof_find_spcm_pcm_id(struct device *dev,
					     unsigned int pcm_id)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(dev);
	struct snd_sof_audio_data *audio_data = sdev->sof_audio_data;
	struct snd_sof_pcm *spcm;

	list_for_each_entry(spcm, &audio_data->pcm_list, list) {
		if (le32_to_cpu(spcm->pcm.pcm_id) == pcm_id)
			return spcm;
	}

	return NULL;
}

struct snd_sof_widget *snd_sof_find_swidget(struct device *dev,
					    const char *name)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(dev);
	struct snd_sof_audio_data *audio_data = sdev->sof_audio_data;
	struct snd_sof_widget *swidget;

	list_for_each_entry(swidget, &audio_data->widget_list, list) {
		if (strcmp(name, swidget->widget->name) == 0)
			return swidget;
	}

	return NULL;
}

/* find widget by stream name and direction */
struct snd_sof_widget *
snd_sof_find_swidget_sname(struct device *dev,
			   const char *pcm_name, int dir)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(dev);
	struct snd_sof_audio_data *audio_data = sdev->sof_audio_data;
	struct snd_sof_widget *swidget;
	enum snd_soc_dapm_type type;

	if (dir == SNDRV_PCM_STREAM_PLAYBACK)
		type = snd_soc_dapm_aif_in;
	else
		type = snd_soc_dapm_aif_out;

	list_for_each_entry(swidget, &audio_data->widget_list, list) {
		if (!strcmp(pcm_name, swidget->widget->sname) &&
		    swidget->id == type)
			return swidget;
	}

	return NULL;
}

struct snd_sof_dai *snd_sof_find_dai(struct device *dev,
				     const char *name)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(dev);
	struct snd_sof_audio_data *audio_data = sdev->sof_audio_data;
	struct snd_sof_dai *dai;

	list_for_each_entry(dai, &audio_data->dai_list, list) {
		if (dai->name && (strcmp(name, dai->name) == 0))
			return dai;
	}

	return NULL;
}

/*
 * SOF Driver enumeration.
 */
int sof_machine_check(struct snd_sof_dev *sdev)
{
	struct snd_sof_pdata *sof_pdata = sdev->pdata;
	const struct sof_dev_desc *desc = sof_pdata->desc;
	struct snd_sof_audio_data *audio_data;
	struct snd_soc_acpi_mach *mach;
	int ret;

	/* create audio data */
	audio_data = devm_kzalloc(sdev->dev, sizeof(*audio_data), GFP_KERNEL);
	if (!audio_data)
		return -ENOMEM;

	audio_data->dev = sdev->dev;

	INIT_LIST_HEAD(&audio_data->pcm_list);
	INIT_LIST_HEAD(&audio_data->kcontrol_list);
	INIT_LIST_HEAD(&audio_data->widget_list);
	INIT_LIST_HEAD(&audio_data->dai_list);
	INIT_LIST_HEAD(&audio_data->route_list);

	sdev->sof_audio_data = audio_data;

	/*
	 * set default tplg path.
	 * TODO: set alternate path from kernel param
	 */
	audio_data->tplg_filename_prefix = sof_pdata->desc->default_tplg_path;

	/* force nocodec mode */
#if IS_ENABLED(CONFIG_SND_SOC_SOF_FORCE_NOCODEC_MODE)
		dev_warn(sdev->dev, "Force to use nocodec mode\n");
		goto nocodec;
#endif

	/* find machine */
	snd_sof_machine_select(sdev->dev);
	if (audio_data->machine) {
		snd_sof_set_mach_params(audio_data->machine, sdev->dev);
		return 0;
	}

#if !IS_ENABLED(CONFIG_SND_SOC_SOF_NOCODEC)
	dev_err(sdev->dev, "error: no matching ASoC machine driver found - aborting probe\n");
	return -ENODEV;
#endif
#if IS_ENABLED(CONFIG_SND_SOC_SOF_FORCE_NOCODEC_MODE)
nocodec:
#endif
	/* select nocodec mode */
	dev_warn(sdev->dev, "Using nocodec machine driver\n");
	mach = devm_kzalloc(sdev->dev, sizeof(*mach), GFP_KERNEL);
	if (!mach)
		return -ENOMEM;

	mach->drv_name = "sof-nocodec";
	audio_data->tplg_filename = desc->nocodec_tplg_filename;

	ret = sof_nocodec_setup(sdev->dev, desc->ops);
	if (ret < 0)
		return ret;

	audio_data->machine = mach;
	snd_sof_set_mach_params(audio_data->machine, sdev->dev);

	return 0;
}
EXPORT_SYMBOL(sof_machine_check);

int sof_machine_register(void *data)
{
	struct snd_sof_audio_data *audio_data =
		(struct snd_sof_audio_data *)data;
	const char *drv_name;
	const void *mach;
	int size;

	drv_name = audio_data->machine->drv_name;
	mach = (const void *)audio_data->machine;
	size = sizeof(*audio_data->machine);

	/* register machine driver, pass machine info as pdata */
	audio_data->pdev_mach =
		platform_device_register_data(audio_data->dev, drv_name,
					      PLATFORM_DEVID_NONE, mach, size);
	if (IS_ERR(audio_data->pdev_mach))
		return PTR_ERR(audio_data->pdev_mach);

	dev_dbg(audio_data->dev, "created machine %s\n",
		dev_name(&audio_data->pdev_mach->dev));

	return 0;
}
EXPORT_SYMBOL(sof_machine_register);

void sof_machine_unregister(void *data)
{
	struct snd_sof_audio_data *audio_data =
		(struct snd_sof_audio_data *)data;

	if (!IS_ERR_OR_NULL(audio_data->pdev_mach))
		platform_device_unregister(audio_data->pdev_mach);
}
EXPORT_SYMBOL(sof_machine_unregister);

static int sof_destroy_pipelines(struct device *dev)
{
	struct snd_sof_audio_data *audio_data =	sof_get_client_data(dev);
	struct snd_sof_widget *swidget;
	struct sof_ipc_reply reply;
	int ret = 0;
	int ret1 = 0;

	list_for_each_entry_reverse(swidget, &audio_data->widget_list, list) {
		struct sof_ipc_free ipc_free;

		/* skip if there is no private data */
		if (!swidget->private)
			continue;

		memset(&ipc_free, 0, sizeof(ipc_free));

		/* configure ipc free message */
		ipc_free.hdr.size = sizeof(ipc_free);
		ipc_free.hdr.cmd = SOF_IPC_GLB_TPLG_MSG;
		ipc_free.id = swidget->comp_id;

		switch (swidget->id) {
		case snd_soc_dapm_scheduler:
			ipc_free.hdr.cmd |= SOF_IPC_TPLG_PIPE_FREE;
			break;
		case snd_soc_dapm_buffer:
			ipc_free.hdr.cmd |= SOF_IPC_TPLG_BUFFER_FREE;
			break;
		default:
			ipc_free.hdr.cmd |= SOF_IPC_TPLG_COMP_FREE;
			break;
		}

		/*
		 * This can fail but continue to free as many as possible
		 * and return the error at the end.
		 */
		ret = sof_client_ipc_tx_message(dev, ipc_free.hdr.cmd,
						&ipc_free, sizeof(ipc_free),
						&reply, sizeof(reply));
		if (ret < 0) {
			ret1 = ret;
			dev_err(dev,
				"error: failed to free widget type %d with ID: %d\n",
				swidget->widget->id, swidget->comp_id);
		}
	}

	return ret1;
}

static int sof_audio_resume(struct device *dev)
{
	return sof_restore_pipelines(dev);
}

static int sof_audio_suspend(struct device *dev)
{
	return sof_set_hw_params_upon_resume(dev);
}

static int sof_audio_runtime_suspend(struct device *dev)
{
	return sof_destroy_pipelines(dev);
}

static const struct dev_pm_ops sof_audio_pm = {
	SET_SYSTEM_SLEEP_PM_OPS(sof_audio_suspend, sof_audio_resume)
	SET_RUNTIME_PM_OPS(sof_audio_runtime_suspend, sof_audio_resume, NULL)
};

/*
 * DSP can enter a low-power D0 substate iff only D0I3-compatible
 * streams are active
 */
static bool sof_audio_allow_LP_D0_substate(struct device *dev)
{
	return snd_sof_dsp_only_d0i3_compatible_stream_active(dev);
}

/*
 * Currently, the only criterion for retaining the DSP in D0
 * is that there are streams that ignored the suspend trigger.
 * Additional criteria such Soundwire clock-stop mode and
 * device suspend latency considerations will be added later.
 */
static bool sof_audio_request_D0_during_suspend(struct device *dev)
{
	return snd_sof_stream_suspend_ignored(dev);
}

static int sof_audio_probe(struct platform_device *pdev)
{
	struct snd_sof_client *audio_client = dev_get_platdata(&pdev->dev);
	struct snd_sof_audio_data *audio_data;
	struct snd_soc_dai_driver *dai_drv;
	int num_dai_drv;
	int ret;

	audio_client->pdev = pdev;

	/* create audio data */
	audio_data = devm_kzalloc(&pdev->dev, sizeof(*audio_data), GFP_KERNEL);
	if (!audio_data)
		return -ENOMEM;

	INIT_LIST_HEAD(&audio_data->pcm_list);
	INIT_LIST_HEAD(&audio_data->kcontrol_list);
	INIT_LIST_HEAD(&audio_data->widget_list);
	INIT_LIST_HEAD(&audio_data->dai_list);
	INIT_LIST_HEAD(&audio_data->route_list);

	audio_data->dev = &pdev->dev;
	audio_data->dma_dev = pdev->dev.parent;
	audio_client->client_data = audio_data;

	/* set client callbacks */
	audio_client->allow_lp_d0_substate_in_s0 =
		sof_audio_allow_LP_D0_substate;
	audio_client->request_d0_during_suspend =
		sof_audio_request_D0_during_suspend;
	audio_client->sof_client_ipc_rx = sof_audio_ipc_rx;

	/* check machine info */
	ret = sof_machine_check(pdev);
	if (ret < 0) {
		dev_err(&pdev->dev, "error: failed to get machine info %d\n",
			ret);
		return ret;
	}

	/* set up platform component driver */
	snd_sof_new_platform_drv(&pdev->dev);

	/* now register audio DSP platform driver and dai */
	dai_drv = sof_client_get_dai_drv(&pdev->dev);
	num_dai_drv = sof_client_get_num_dai_drv(&pdev->dev);
	ret = devm_snd_soc_register_component(&pdev->dev,
					      &audio_data->plat_drv,
					      dai_drv, num_dai_drv);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"error: failed to register DSP DAI driver %d\n", ret);
		return ret;
	}

	ret = sof_client_machine_register(&pdev->dev, audio_data);
	if (ret < 0)
		return ret;

	/* probe complete, register with SOF core */
	sof_client_register(&pdev->dev);

	/* enable runtime PM */
	pm_runtime_set_autosuspend_delay(&pdev->dev,
					 SND_SOF_AUDIO_SUSPEND_DELAY_MS);
	pm_runtime_use_autosuspend(&pdev->dev);
	pm_runtime_set_active(&pdev->dev);
	pm_runtime_enable(&pdev->dev);
	pm_runtime_mark_last_busy(&pdev->dev);
	pm_runtime_put_autosuspend(&pdev->dev);

	return 0;
}

static int sof_audio_remove(struct platform_device *pdev)
{
	struct snd_sof_audio_data *audio_data =
		sof_get_client_data(&pdev->dev);

	pm_runtime_disable(&pdev->dev);

	sof_machine_unregister(audio_data);

	return 0;
}

static struct platform_driver sof_audio_driver = {
	.driver = {
		.name = "sof-audio",
		.pm = &sof_audio_pm,
	},

	.probe = sof_audio_probe,
	.remove = sof_audio_remove,
};

module_platform_driver(sof_audio_driver);

MODULE_DESCRIPTION("SOF Audio Client Platform Driver");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_IMPORT_NS(SND_SOC_SOF_CLIENT);
MODULE_IMPORT_NS(SND_SOC_SOF_AUDIO_NOCODEC);
MODULE_ALIAS("platform:sof-audio");
