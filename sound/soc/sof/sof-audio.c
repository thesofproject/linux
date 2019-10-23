// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
//
// Copyright(c) 2019 Intel Corporation. All rights reserved.
//
// Author: Ranjani Sridharan <ranjani.sridharan@linux.intel.com>
//

#include <linux/device.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <sound/sof.h>
#include "sof-priv.h"
#include "sof-client.h"
#include "sof-audio.h"
#include "audio-ops.h"
#include "ops.h"

int sof_register_audio_clients(struct snd_sof_dev *sdev)
{
	struct snd_sof_pdata *plat_data = sdev->pdata;
	const struct sof_dev_desc *desc = plat_data->desc;
	const struct snd_sof_audio_ops *audio_ops = desc->audio_ops;
	char *client_name;
	size_t size;
	int count;
	u32 type;
	int i;

	/* number of audio clients to register */
	sdev->sof_num_clients = audio_ops->num_dai_drv_info;

	size = sdev->sof_num_clients * sizeof(*sdev->sof_clients);

	sdev->sof_clients = devm_kzalloc(sdev->dev, size, GFP_KERNEL);
	if (!sdev->sof_clients)
		return -ENOMEM;

	/* register clients. Any of these can fail but keep going */
	for (i = 0; i < audio_ops->num_dai_drv_info; i++) {
		type = audio_ops->dai_drv_info[i].type;
		count = snd_sof_get_dai_drv_count(audio_ops, type);

		switch (type) {
		case SOF_DAI_INTEL_SSP:
			client_name = "sof-ssp-audio";
			break;
		case SOF_DAI_INTEL_DMIC:
			client_name = "sof-dmic-audio";
			break;
		case SOF_DAI_INTEL_HDA:
			client_name = "sof-hda-audio";
			break;
		}

		sdev->sof_clients[i] =
				sof_client_dev_register(sdev, client_name);
		if (!sdev->sof_clients[i])
			dev_warn(sdev->dev, "%s client failed to register\n",
				 client_name);
	}

	return 0;
}
EXPORT_SYMBOL(sof_register_audio_clients);

int sof_unregister_audio_clients(struct snd_sof_dev *sdev)
{
	int i;

	for (i = 0; i < sdev->sof_num_clients; i++)
		sof_client_dev_unregister(sdev->sof_clients[i]);

	return 0;
}
EXPORT_SYMBOL(sof_unregister_audio_clients);

static void ipc_period_elapsed(struct snd_sof_client *client, u32 msg_id)
{
	struct sof_audio_dev *sof_audio = client->client_data;
	struct snd_soc_component *scomp = sof_audio->component;
	struct snd_sof_dev *sdev = dev_get_drvdata(scomp->dev->parent);
	struct snd_sof_pcm_stream *stream;
	struct sof_ipc_stream_posn posn;
	struct snd_sof_pcm *spcm;
	int direction;

	spcm = snd_sof_find_spcm_comp(scomp, msg_id, &direction);
	if (!spcm) {
		dev_err(scomp->dev,
			"error: period elapsed for unknown stream, msg_id %d\n",
			msg_id);
		return;
	}

	stream = &spcm->stream[direction];
	snd_sof_ipc_msg_data(sdev, stream->substream, &posn, sizeof(posn));

	dev_dbg(scomp->dev, "posn : host 0x%llx dai 0x%llx wall 0x%llx\n",
		posn.host_posn, posn.dai_posn, posn.wallclock);

	memcpy(&stream->posn, &posn, sizeof(posn));

	/* only inform ALSA for period_wakeup mode */
	if (!stream->substream->runtime->no_period_wakeup)
		snd_sof_pcm_period_elapsed(stream->substream);
}

/* DSP notifies host of an XRUN within FW */
static void ipc_xrun(struct snd_sof_client *client, u32 msg_id)
{
	struct sof_audio_dev *sof_audio = client->client_data;
	struct snd_soc_component *scomp = sof_audio->component;
	struct snd_sof_dev *sdev = dev_get_drvdata(scomp->dev->parent);
	struct snd_sof_pcm_stream *stream;
	struct sof_ipc_stream_posn posn;
	struct snd_sof_pcm *spcm;
	int direction;

	spcm = snd_sof_find_spcm_comp(scomp, msg_id, &direction);
	if (!spcm) {
		dev_err(scomp->dev, "error: XRUN for unknown stream, msg_id %d\n",
			msg_id);
		return;
	}

	stream = &spcm->stream[direction];
	/* TODO: figure out how to do this from core */
	snd_sof_ipc_msg_data(sdev, stream->substream, &posn, sizeof(posn));

	dev_dbg(sdev->dev,  "posn XRUN: host %llx comp %d size %d\n",
		posn.host_posn, posn.xrun_comp_id, posn.xrun_size);

#if defined(CONFIG_SND_SOC_SOF_DEBUG_XRUN_STOP)
	/* stop PCM on XRUN - used for pipeline debug */
	memcpy(&stream->posn, &posn, sizeof(posn));
	snd_pcm_stop_xrun(stream->substream);
#endif
}

/* Audio client IPC RX callback */
void sof_audio_rx_message(struct snd_sof_client *client, u32 msg_cmd)
{
	struct platform_device *pdev = client->pdev;

	/* get msg cmd type and msd id */
	u32 msg_type = msg_cmd & SOF_CMD_TYPE_MASK;
	u32 msg_id = SOF_IPC_MESSAGE_ID(msg_cmd);

	switch (msg_type) {
	case SOF_IPC_STREAM_POSITION:
		ipc_period_elapsed(client, msg_id);
		break;
	case SOF_IPC_STREAM_TRIG_XRUN:
		ipc_xrun(client, msg_id);
		break;
	default:
		dev_err(&pdev->dev, "error: unhandled stream message %x\n",
			msg_id);
		break;
	}
}
EXPORT_SYMBOL(sof_audio_rx_message);

/*
 * Generic object lookup APIs.
 */

struct snd_sof_pcm *snd_sof_find_spcm_name(struct snd_soc_component *scomp,
					   const char *name)
{
	struct sof_audio_dev *sof_audio = sof_get_client_data(scomp->dev);
	struct snd_sof_pcm *spcm;

	list_for_each_entry(spcm, &sof_audio->pcm_list, list) {
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
EXPORT_SYMBOL(snd_sof_find_spcm_name);

struct snd_sof_pcm *snd_sof_find_spcm_comp(struct snd_soc_component *scomp,
					   unsigned int comp_id,
					   int *direction)
{
	struct sof_audio_dev *sof_audio = sof_get_client_data(scomp->dev);
	struct snd_sof_pcm *spcm;
	int i = 0;

	list_for_each_entry(spcm, &sof_audio->pcm_list, list) {
		for (i = 0; i <= SNDRV_PCM_STREAM_CAPTURE; i++) {
			if (spcm->stream[i].comp_id == comp_id) {
				*direction = i;
				return spcm;
			}
		}
	}

	return NULL;
}
EXPORT_SYMBOL(snd_sof_find_spcm_comp);

struct snd_sof_pcm *snd_sof_find_spcm_pcm_id(struct snd_soc_component *scomp,
					     unsigned int pcm_id)
{
	struct sof_audio_dev *sof_audio = sof_get_client_data(scomp->dev);
	struct snd_sof_pcm *spcm;

	list_for_each_entry(spcm, &sof_audio->pcm_list, list) {
		if (le32_to_cpu(spcm->pcm.pcm_id) == pcm_id)
			return spcm;
	}

	return NULL;
}
EXPORT_SYMBOL(snd_sof_find_spcm_pcm_id);

struct snd_sof_widget *snd_sof_find_swidget(struct snd_soc_component *scomp,
					    const char *name)
{
	struct sof_audio_dev *sof_audio = sof_get_client_data(scomp->dev);
	struct snd_sof_widget *swidget;

	list_for_each_entry(swidget, &sof_audio->widget_list, list) {
		if (strcmp(name, swidget->widget->name) == 0)
			return swidget;
	}

	return NULL;
}
EXPORT_SYMBOL(snd_sof_find_swidget);

/* find widget by stream name and direction */
struct snd_sof_widget *
snd_sof_find_swidget_sname(struct snd_soc_component *scomp,
			   const char *pcm_name, int dir)
{
	struct sof_audio_dev *sof_audio = sof_get_client_data(scomp->dev);
	struct snd_sof_widget *swidget;
	enum snd_soc_dapm_type type;

	if (dir == SNDRV_PCM_STREAM_PLAYBACK)
		type = snd_soc_dapm_aif_in;
	else
		type = snd_soc_dapm_aif_out;

	list_for_each_entry(swidget, &sof_audio->widget_list, list) {
		if (!strcmp(pcm_name, swidget->widget->sname) &&
		    swidget->id == type)
			return swidget;
	}

	return NULL;
}
EXPORT_SYMBOL(snd_sof_find_swidget_sname);

struct snd_sof_dai *snd_sof_find_dai(struct snd_soc_component *scomp,
				     const char *name)
{
	struct sof_audio_dev *sof_audio = sof_get_client_data(scomp->dev);
	struct snd_sof_dai *dai;

	list_for_each_entry(dai, &sof_audio->dai_list, list) {
		if (dai->name && (strcmp(name, dai->name) == 0))
			return dai;
	}

	return NULL;
}

/* get dai drv count based on type */
int snd_sof_get_dai_drv_count(const struct snd_sof_audio_ops *audio_ops,
			      u32 type)
{
	struct sof_dai_drv_info *dai_drv_info = audio_ops->dai_drv_info;
	int i;

	for (i = 0; i < audio_ops->num_dai_drv_info; i++)
		if (dai_drv_info[i].type == type)
			return dai_drv_info[i].count;

	return -EINVAL;
}
EXPORT_SYMBOL(snd_sof_get_dai_drv_count);

/* get dai drv offset based on type */
int snd_sof_get_dai_drv_offset(const struct snd_sof_audio_ops *audio_ops,
			       u32 type)
{
	struct sof_dai_drv_info *dai_drv_info = audio_ops->dai_drv_info;
	int i;

	for (i = 0; i < audio_ops->num_dai_drv_info; i++)
		if (dai_drv_info[i].type == type)
			return dai_drv_info[i].offset;

	return -EINVAL;
}
EXPORT_SYMBOL(snd_sof_get_dai_drv_offset);
/*
 * SOF Driver enumeration.
 */
static int sof_machine_check(struct platform_device *pdev,
			     const struct sof_dev_desc *desc)
{
	struct sof_audio_dev *sof_audio = sof_get_client_data(&pdev->dev);
#if IS_ENABLED(CONFIG_SND_SOC_SOF_NOCODEC)
	struct snd_soc_acpi_mach *machine;
	int ret;
#endif

	if (sof_audio->machine)
		return 0;

#if !IS_ENABLED(CONFIG_SND_SOC_SOF_NOCODEC)
	dev_warn(&pdev->dev, "error: no matching ASoC machine driver found - aborting probe\n");
	return -ENODEV;
#else
	/* fallback to nocodec mode */
	dev_warn(&pdev->dev, "No ASoC machine driver found - using nocodec\n");
	machine = devm_kzalloc(&pdev->dev, sizeof(*machine), GFP_KERNEL);
	if (!machine)
		return -ENOMEM;

	sof_audio->machine = machine;

	ret = sof_nocodec_setup(&pdev->dev, sof_audio, desc);
	if (ret < 0)
		return ret;

	machine->mach_params.platform = dev_name(&pdev->dev);

	return 0;
#endif
}

static int sof_set_hw_params_upon_resume(struct device *dev)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(dev->parent);
	struct snd_pcm_substream *substream;
	struct sof_audio_dev *sof_audio = sof_get_client_data(dev);
	struct snd_sof_pcm *spcm;
	snd_pcm_state_t state;
	int dir;

	/*
	 * SOF requires hw_params to be set-up internally upon resume.
	 * So, set the flag to indicate this for those streams that
	 * have been suspended.
	 */
	list_for_each_entry(spcm, &sof_audio->pcm_list, list) {
		for (dir = 0; dir <= SNDRV_PCM_STREAM_CAPTURE; dir++) {
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
	struct sof_audio_dev *sof_audio = sof_get_client_data(dev);
	struct snd_soc_component *scomp;
	struct snd_sof_control *scontrol;
	int ipc_cmd, ctrl_type;
	int ret = 0;

	scomp = sof_audio->component;

	/* restore kcontrol values */
	list_for_each_entry(scontrol, &sof_audio->kcontrol_list, list) {
		/* reset readback offset for scontrol after resuming */
		scontrol->readback_offset = 0;

		/* notify DSP of kcontrol values */
		switch (scontrol->cmd) {
		case SOF_CTRL_CMD_VOLUME:
		case SOF_CTRL_CMD_ENUM:
		case SOF_CTRL_CMD_SWITCH:
			ipc_cmd = SOF_IPC_COMP_SET_VALUE;
			ctrl_type = SOF_CTRL_TYPE_VALUE_CHAN_SET;
			ret = snd_sof_ipc_set_get_comp_data(scomp, scontrol,
							    ipc_cmd, ctrl_type,
							    scontrol->cmd,
							    true);
			break;
		case SOF_CTRL_CMD_BINARY:
			ipc_cmd = SOF_IPC_COMP_SET_DATA;
			ctrl_type = SOF_CTRL_TYPE_DATA_SET;
			ret = snd_sof_ipc_set_get_comp_data(scomp, scontrol,
							    ipc_cmd, ctrl_type,
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

static int sof_restore_pipelines(struct device *dev)
{
	struct sof_audio_dev *sof_audio = sof_get_client_data(dev);
	struct snd_soc_component *scomp;
	struct snd_sof_widget *swidget;
	struct snd_sof_route *sroute;
	struct sof_ipc_pipe_new *pipeline;
	struct snd_sof_dai *dai;
	struct sof_ipc_comp_dai *comp_dai;
	struct sof_ipc_cmd_hdr *hdr;
	int ret;

	scomp = sof_audio->component;

	/* restore pipeline components */
	list_for_each_entry_reverse(swidget, &sof_audio->widget_list, list) {
		struct sof_ipc_comp_reply r;

		/* skip if there is no private data */
		if (!swidget->private)
			continue;

		switch (swidget->id) {
		case snd_soc_dapm_dai_in:
		case snd_soc_dapm_dai_out:
			dai = swidget->private;
			comp_dai = &dai->comp_dai;
			ret = sof_client_tx_message(scomp->dev,
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
			ret = sof_load_pipeline_ipc(scomp, pipeline, &r);
			break;
		default:
			hdr = swidget->private;
			ret = sof_client_tx_message(scomp->dev, hdr->cmd,
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
	list_for_each_entry_reverse(sroute, &sof_audio->route_list, list) {
		struct sof_ipc_pipe_comp_connect *connect;
		struct sof_ipc_reply reply;

		/* skip if there's no private data */
		if (!sroute->private)
			continue;

		connect = sroute->private;

		/* send ipc */
		ret = sof_client_tx_message(scomp->dev,
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
	list_for_each_entry_reverse(dai, &sof_audio->dai_list, list) {
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

		ret = sof_client_tx_message(scomp->dev,
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
	list_for_each_entry(swidget, &sof_audio->widget_list, list) {
		switch (swidget->id) {
		case snd_soc_dapm_scheduler:
			swidget->complete =
				snd_sof_complete_pipeline(scomp, swidget);
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

static int sof_audio_select_machine(struct platform_device *pdev,
				    const struct sof_dev_desc *desc)
{
	struct sof_audio_dev *sof_audio = sof_get_client_data(&pdev->dev);
	struct snd_sof_dev *sdev = dev_get_drvdata(pdev->dev.parent);
	struct snd_soc_acpi_mach *mach;
	int ret;

#if IS_ENABLED(CONFIG_SND_SOC_SOF_FORCE_NOCODEC_MODE)
	/* force nocodec mode */
	dev_warn(&pdev->dev, "Force to use nocodec mode\n");
	mach = devm_kzalloc(&pdev->dev, sizeof(*mach), GFP_KERNEL);
	if (!mach)
		return -ENOMEM;

	sof_audio->machine = mach;

	ret = sof_nocodec_setup(&pdev->dev, sof_audio, desc);
	if (ret < 0)
		return ret;
	mach->mach_params.platform = dev_name(&pdev->dev);
#else
	/* find machine */
	mach = snd_soc_acpi_find_machine(desc->machines);
	if (!mach) {
		dev_warn(&pdev->dev, "warning: No matching ASoC machine driver found\n");
	} else {
		mach->mach_params.platform = dev_name(&pdev->dev);
		sof_audio->tplg_filename = mach->sof_tplg_filename;
		sof_audio->machine = mach;
	}
#endif /* CONFIG_SND_SOC_SOF_FORCE_NOCODEC_MODE */

	sof_audio->tplg_filename_prefix = desc->default_tplg_path;

	/* use platform-specific machine driver if mach is null */
	ret = snd_sof_machine_driver_select(sdev, sof_audio);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"error: invalid tplg filename for platform-specific machine\n");
		return ret;
	}

	/* check machine info */
	ret = sof_machine_check(pdev, desc);
	if (ret < 0)
		dev_err(&pdev->dev, "error: failed to get machine info %d\n",
			ret);

	return ret;
}

static int sof_destroy_pipelines(struct device *dev)
{
	struct sof_audio_dev *sof_audio = sof_get_client_data(dev);
	struct snd_sof_widget *swidget;
	struct sof_ipc_free ipc_free;
	struct sof_ipc_reply reply;
	int ret = 0;

	list_for_each_entry_reverse(swidget, &sof_audio->widget_list, list) {
		memset(&ipc_free, 0, sizeof(ipc_free));

		/* skip if there is no private data */
		if (!swidget->private)
			continue;

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
		ret = sof_client_tx_message(dev, ipc_free.hdr.cmd,
					    &ipc_free, sizeof(ipc_free), &reply,
					    sizeof(reply));
		if (ret < 0) {
			dev_err(dev,
				"error: failed to free widget type %d with ID: %d\n",
				swidget->widget->id, swidget->comp_id);

			return ret;
		}
	}

	return ret;
}

int sof_audio_resume(struct device *dev)
{
	return sof_restore_pipelines(dev);
}
EXPORT_SYMBOL(sof_audio_resume);

int sof_audio_suspend(struct device *dev)
{
	return sof_set_hw_params_upon_resume(dev);
}
EXPORT_SYMBOL(sof_audio_suspend);

int sof_audio_runtime_suspend(struct device *dev)
{
	return sof_destroy_pipelines(dev);
}
EXPORT_SYMBOL(sof_audio_runtime_suspend);

static const struct dev_pm_ops sof_audio_pm = {
	SET_SYSTEM_SLEEP_PM_OPS(sof_audio_suspend, sof_audio_resume)
	SET_RUNTIME_PM_OPS(sof_audio_runtime_suspend, sof_audio_resume, NULL)
};

static int sof_audio_probe(struct platform_device *pdev)
{
	struct snd_sof_client *audio_client = dev_get_platdata(&pdev->dev);
	struct snd_sof_dev *sdev = dev_get_drvdata(pdev->dev.parent);
	struct snd_sof_pdata *plat_data = sdev->pdata;
	struct snd_sof_client *audio_client = dev_get_platdata(&pdev->dev);
	const struct sof_dev_desc *desc = plat_data->desc;
	struct sof_audio_dev *sof_audio;
	struct ipc_rx_client *audio_rx;
	const char *drv_name;
	const void *machine;
	int size;
	int ret;

	/* set IPC RX and TX callback */
	audio_client->sof_client_rx_cb = sof_audio_rx_message;
	audio_client->sof_ipc_reply_cb = NULL;

	/* create SOF audio device */
	sof_audio = devm_kzalloc(&pdev->dev, sizeof(*sof_audio), GFP_KERNEL);
	if (!sof_audio)
		return -ENOMEM;

	INIT_LIST_HEAD(&sof_audio->pcm_list);
	INIT_LIST_HEAD(&sof_audio->kcontrol_list);
	INIT_LIST_HEAD(&sof_audio->widget_list);
	INIT_LIST_HEAD(&sof_audio->dai_list);
	INIT_LIST_HEAD(&sof_audio->route_list);

	sof_audio->audio_ops = desc->audio_ops;
	sof_audio->platform = dev_name(&pdev->dev);

	/* check for mandatory audio ops */
	if (!sof_audio || !sof_audio->audio_ops->ipc_pcm_params)
		return -EINVAL;

	audio_client->client_data = sof_audio;

	/* set IPC RX callback */
	audio_client->sof_client_rx_message = sof_audio_rx_message;

	/* register for stream message rx */
	audio_rx = devm_kzalloc(&pdev->dev, sizeof(*audio_rx), GFP_KERNEL);
	if (!audio_rx)
		return -ENOMEM;
	audio_rx->ipc_cmd = SOF_IPC_GLB_STREAM_MSG;
	audio_rx->dev = &pdev->dev;
	snd_sof_ipc_rx_register(sdev, audio_rx);

	/* select machine driver */
	ret = sof_audio_select_machine(pdev, desc);
	if (ret < 0)
		return ret;

	/* set up platform component driver */
	snd_sof_new_platform_drv(sof_audio);

	/* now register audio DSP platform driver and dai */
	ret = devm_snd_soc_register_component(&pdev->dev, &sof_audio->plat_drv,
					      sof_audio->audio_ops->drv,
					      sof_audio->audio_ops->num_drv);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"error: failed to register DSP DAI driver %d\n", ret);
		return ret;
	}

	machine = (const void *)sof_audio->machine;
	drv_name = sof_audio->machine->drv_name;
	size = sizeof(*sof_audio->machine);

	/* register machine driver, pass machine info as pdata */
	sof_audio->pdev_mach =
		platform_device_register_data(&pdev->dev, drv_name,
					      PLATFORM_DEVID_NONE,
					      machine, size);

	if (IS_ERR(sof_audio->pdev_mach)) {
		ret = PTR_ERR(sof_audio->pdev_mach);
		return ret;
	}

	dev_dbg(&pdev->dev, "created machine %s\n",
		dev_name(&sof_audio->pdev_mach->dev));

	/* enable runtime PM */
	pm_runtime_set_autosuspend_delay(&pdev->dev, SND_SOF_SUSPEND_DELAY_MS);
	pm_runtime_use_autosuspend(&pdev->dev);
	pm_runtime_enable(&pdev->dev);
	pm_runtime_mark_last_busy(&pdev->dev);
	pm_runtime_put_noidle(&pdev->dev);

	return 0;
}

static int sof_audio_remove(struct platform_device *pdev)
{
	struct sof_audio_dev *sof_audio = sof_get_client_data(&pdev->dev);

	pm_runtime_disable(&pdev->dev);

	if (!IS_ERR_OR_NULL(sof_audio->pdev_mach))
		platform_device_unregister(sof_audio->pdev_mach);

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
MODULE_ALIAS("platform:sof-audio");
