// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
/*
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * Copyright(c) 2017-2019 Intel Corporation. All rights reserved.
 *
 * Author:	Libin Yang <libin.yang@intel.com>
 *		Luo Xionghu <xionghu.luo@intel.com>
 *		Liam Girdwood <liam.r.girdwood@linux.intel.com>
 *		Guennadi Liakhovetski <guennadi.liakhovetski@linux.intel.com>
 */

#include <asm/unaligned.h>

#include <linux/device.h>
#include <linux/file.h>
#include <linux/firmware.h>
#include <linux/fs.h>
#include <linux/hw_random.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/uio.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>

#include <sound/pcm_params.h>
#include <sound/sof.h>

#include <sound/sof/virtio.h>

#include "dsp.h"

#include "../sound/soc/sof/sof-priv.h"
#include "../sound/soc/sof/ops.h"

struct dsp_pipeline_connect {
	struct list_head list;
	int vq_idx;	/* VirtQ index */
	int guest_id;	/* Guest component (virtual DAI) ID */
	int host_id;	/* Host component (mixer) ID */
	enum sof_ipc_stream_direction direction;
};

struct snd_sof_widget *dsp_sof_find_swidget_id(struct snd_sof_dev *sdev,
					       unsigned int comp_id)
{
	struct snd_sof_widget *swidget;

	list_for_each_entry(swidget, &sdev->widget_list, list)
		if (swidget->comp_id == comp_id)
			return swidget;

	return NULL;
}

static struct snd_sof_dai *dsp_sof_find_dai_pipe(struct snd_sof_dev *sdev,
						 unsigned int pipeline_id)
{
	struct snd_sof_dai *dai;

	list_for_each_entry(dai, &sdev->dai_list, list)
		if (dai->pipeline_id == pipeline_id)
			return dai;

	return NULL;
}

/*
 * This function is used to find a BE substream. It uses the dai_link stream
 * name for that. The current dai_link stream names are "vm_fe_playback" and
 * "vm_fe_capture," which means only one Virtual Machine is supported and the VM
 * only supports one playback pcm and one capture pcm. After we switch to the
 * new topology, we can support multiple VMs and multiple PCM streams for each
 * VM. This function may be abandoned after switching to the new topology.
 */
static struct snd_pcm_substream *dsp_sof_get_substream(struct snd_sof_dev *sdev,
				struct snd_soc_pcm_runtime **rtd, int direction)
{
	struct snd_soc_card *card = sdev->card;
	struct snd_soc_pcm_runtime *r;

	for_each_card_rtds(card, r) {
		struct snd_pcm_substream *substream;
		struct snd_pcm_str *stream;
		struct snd_pcm *pcm = r->pcm;
		if (!pcm || !pcm->internal)
			continue;

		/*
		 * We need to find a dedicated substream:
		 * pcm->streams[dir].substream which is dedicated
		 * used for vFE.
		 */
		stream = &pcm->streams[direction];
		substream = stream->substream;
		if (substream) {
			struct snd_soc_dai_link *dai_link = r->dai_link;

			/* FIXME: replace hard-coded stream name */
			if (dai_link->stream_name &&
			    (!strcmp(dai_link->stream_name, "vm_fe_playback") ||
			     !strcmp(dai_link->stream_name, "vm_fe_capture"))) {
				if (rtd)
					*rtd = r;
				return substream;
			}
		}
	}

	return NULL;
}

#define dsp_sof_find_spcm_comp sdev->core_ops.find_spcm_comp

static int dsp_sof_assemble_params(struct sof_ipc_pcm_params *pcm,
				   struct snd_pcm_hw_params *params)
{
	struct snd_mask *fmt = hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT);

	hw_param_interval(params, SNDRV_PCM_HW_PARAM_CHANNELS)->min =
		pcm->params.channels;

	hw_param_interval(params, SNDRV_PCM_HW_PARAM_RATE)->min =
		pcm->params.rate;

	hw_param_interval(params, SNDRV_PCM_HW_PARAM_PERIOD_BYTES)->min =
		pcm->params.host_period_bytes;

	hw_param_interval(params, SNDRV_PCM_HW_PARAM_BUFFER_BYTES)->min =
		pcm->params.buffer.size;

	snd_mask_none(fmt);
	switch (pcm->params.frame_fmt) {
	case SOF_IPC_FRAME_S16_LE:
		snd_mask_set(fmt, SNDRV_PCM_FORMAT_S16);
		break;
	case SOF_IPC_FRAME_S24_4LE:
		snd_mask_set(fmt, SNDRV_PCM_FORMAT_S24);
		break;
	case SOF_IPC_FRAME_S32_LE:
		snd_mask_set(fmt, SNDRV_PCM_FORMAT_S32);
		break;
	case SOF_IPC_FRAME_FLOAT:
		snd_mask_set(fmt, SNDRV_PCM_FORMAT_FLOAT);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int dsp_sof_stream_hw_params(struct snd_sof_dev *sdev,
				    struct sof_ipc_pcm_params *pcm)
{
	struct snd_pcm_substream *substream;
	struct snd_pcm_runtime *runtime;
	struct snd_pcm_hw_params params;
	int direction = pcm->params.direction;
	int ret;

	/* find the proper substream */
	substream = dsp_sof_get_substream(sdev, NULL, direction);
	if (!substream)
		return -ENODEV;

	runtime = substream->runtime;
	if (!runtime) {
		dev_err(sdev->dev, "no runtime is available for hw_params\n");
		return -ENODEV;
	}

	/* TODO: codec hw_params */

	/* Use different stream_tag from FE. This is the real tag */
	dsp_sof_assemble_params(pcm, &params);

	/* Allocate a duplicate of the guest buffer */
	ret = snd_pcm_lib_malloc_pages(substream, params_buffer_bytes(&params));
	if (ret < 0) {
		dev_err(sdev->dev,
			"error %d: could not allocate %d bytes for PCM \"%s\"\n",
			ret, params_buffer_bytes(&params), substream->pcm->name);
		return ret;
	}

	/* This function actually accesses dmab / sgbuf */
	return snd_sof_pcm_platform_hw_params(sdev, substream, &params,
					     &pcm->params);
}

/* Allocate a runtime object and buffer pages */
static int dsp_sof_pcm_open(struct snd_sof_dev *sdev, void *ipc_data)
{
	struct snd_pcm_substream *substream;
	struct snd_soc_pcm_runtime *rtd;
	struct sof_ipc_pcm_params *pcm = ipc_data;
	struct snd_pcm_runtime *runtime;
	struct snd_sof_pcm *spcm;
	u32 comp_id = pcm->comp_id;
	size_t size;
	int direction, ret;

	spcm = dsp_sof_find_spcm_comp(sdev, comp_id, &direction);
	if (!spcm)
		return -ENODEV;

	substream = dsp_sof_get_substream(sdev, &rtd, direction);
	if (!substream || !rtd)
		return -ENODEV;
	if (substream->ref_count > 0)
		return -EBUSY;
	substream->ref_count++;	/* set it used */

	runtime = kzalloc(sizeof(*runtime), GFP_KERNEL);
	if (!runtime)
		return -ENOMEM;

	size = PAGE_ALIGN(sizeof(struct snd_pcm_mmap_status));
	runtime->status = alloc_pages_exact(size, GFP_KERNEL);
	if (!runtime->status) {
		ret = -ENOMEM;
		goto eruntime;
	}
	memset((void *)runtime->status, 0, size);

	size = PAGE_ALIGN(sizeof(struct snd_pcm_mmap_control));
	runtime->control = alloc_pages_exact(size, GFP_KERNEL);
	if (!runtime->control) {
		dev_err(sdev->dev, "fail to alloc pages for runtime->control");
		ret = -ENOMEM;
		goto estatus;
	}
	memset((void *)runtime->control, 0, size);

	init_waitqueue_head(&runtime->sleep);
	init_waitqueue_head(&runtime->tsleep);
	runtime->status->state = SNDRV_PCM_STATE_OPEN;

	substream->runtime = runtime;
	substream->private_data = rtd;
	rtd->dpcm[direction].runtime = runtime;
	substream->stream = direction;

	substream->dma_buffer.dev.type = SNDRV_DMA_TYPE_DEV_SG;
	substream->dma_buffer.dev.dev = sdev->dev;

	/* check with spcm exists or not */
	spcm->stream[direction].posn.host_posn = 0;
	spcm->stream[direction].posn.dai_posn = 0;
	spcm->stream[direction].substream = substream;
	spcm->stream[direction].guest_offset = 0;

	/* TODO: codec open */

	snd_sof_pcm_platform_open(sdev, substream);

	return 0;

estatus:
	free_pages_exact(runtime->status,
			 PAGE_ALIGN(sizeof(struct snd_pcm_mmap_status)));
eruntime:
	kfree(runtime);
	return ret;
}

static int dsp_sof_pcm_close(struct snd_sof_dev *sdev, void *ipc_data)
{
	struct snd_pcm_substream *substream;
	struct snd_sof_pcm *spcm;
	struct snd_soc_pcm_runtime *rtd = NULL;
	struct sof_ipc_stream *stream;
	u32 comp_id;
	int direction;

	stream = (struct sof_ipc_stream *)ipc_data;
	comp_id = stream->comp_id;

	spcm = dsp_sof_find_spcm_comp(sdev, comp_id, &direction);
	if (!spcm)
		return 0;

	substream = dsp_sof_get_substream(sdev, &rtd, direction);
	if (substream) {
		snd_sof_pcm_platform_close(sdev, substream);

		/* TODO: codec close */

		substream->ref_count = 0;
		if (substream->runtime) {
			free_pages_exact(substream->runtime->status,
					 PAGE_ALIGN(sizeof(struct snd_pcm_mmap_status)));
			free_pages_exact(substream->runtime->control,
					 PAGE_ALIGN(sizeof(struct snd_pcm_mmap_control)));
			kfree(substream->runtime);
			substream->runtime = NULL;
		}
	}

	list_del(&spcm->list);
	kfree(spcm);

	return 0;
}

static int dsp_sof_ipc_stream_capture(struct snd_sof_pcm_stream *stream,
				      struct snd_pcm_runtime *runtime,
				      struct dsp_sof_data_req *req,
				      struct dsp_sof_data_resp *reply)
{
	size_t data_size = req->size;
	int ret;

	stream->guest_offset = req->offset;

	if (req->offset + data_size > runtime->dma_bytes) {
		reply->size = 0;
		ret = -ENOBUFS;
	} else {
		stream->guest_offset += data_size;

		memcpy(reply->data, runtime->dma_area + req->offset, data_size);
		reply->size = data_size;
		ret = 0;
	}

	reply->error = ret;

	return ret;
}

static int dsp_sof_ipc_stream_playback(struct snd_sof_pcm_stream *stream,
				       struct snd_pcm_runtime *runtime,
				       struct dsp_sof_data_req *req,
				       struct dsp_sof_data_resp *reply)
{
	size_t data_size = req->size;
	int ret;

	stream->guest_offset = req->offset;

	if (req->offset + data_size > runtime->dma_bytes) {
		ret = -ENOBUFS;
	} else {
		stream->guest_offset += data_size;

		memcpy(runtime->dma_area + req->offset, req->data, data_size);
		ret = 0;
	}

	reply->error = ret;
	reply->size = 0;

	return ret;
}

int dsp_sof_ipc_stream_data(struct snd_sof_dev *sdev,
			    struct dsp_sof_data_req *req,
			    struct dsp_sof_data_resp *reply)
{
	struct snd_soc_pcm_runtime *rtd;
	int direction;
	struct snd_sof_pcm *spcm = dsp_sof_find_spcm_comp(sdev,
						req->comp_id, &direction);
	struct snd_pcm_substream *substream = dsp_sof_get_substream(sdev, &rtd,
								    direction);

	if (!spcm || !substream) {
		reply->error = -ENODEV;
		reply->size = 0;
		return reply->error;
	}

	if (direction == SNDRV_PCM_STREAM_PLAYBACK)
		return dsp_sof_ipc_stream_playback(spcm->stream + direction,
						substream->runtime, req, reply);

	return dsp_sof_ipc_stream_capture(spcm->stream + direction,
					  substream->runtime, req, reply);
}

/* handle the stream ipc */
static int dsp_sof_ipc_stream(struct snd_sof_dev *sdev,
			      struct sof_ipc_cmd_hdr *hdr, void *reply_buf)
{
	struct sof_ipc_pcm_params *pcm;
	struct sof_ipc_stream *stream;
	struct snd_soc_pcm_runtime *rtd;
	struct snd_pcm_substream *substream;
	struct snd_soc_dai *codec_dai;
	const struct snd_soc_dai_ops *ops;
	int ret = 0, direction, comp_id, i;
	u32 cmd = hdr->cmd & SOF_CMD_TYPE_MASK;
	struct snd_soc_dpcm *dpcm;

	/* TODO validate host comp id range based on vm_id */

	switch (cmd) {
	case SOF_IPC_STREAM_PCM_PARAMS:
		ret = dsp_sof_pcm_open(sdev, hdr);
		if (ret < 0)
			break;
		pcm = (struct sof_ipc_pcm_params *)hdr;
		ret = dsp_sof_stream_hw_params(sdev, pcm);
		break;
	case SOF_IPC_STREAM_TRIG_START:
		stream = (struct sof_ipc_stream *)hdr;
		comp_id = stream->comp_id;
		if (!dsp_sof_find_spcm_comp(sdev, comp_id, &direction)) {
			ret = -ENODEV;
			break;
		}
		substream = dsp_sof_get_substream(sdev, &rtd, direction);
		if (!rtd) {
			ret = -ENODEV;
			break;
		}

		/* Create an RTD, a CPU DAI when parsing aif_in */
		snd_soc_runtime_activate(rtd, direction);
		soc_dpcm_runtime_update(sdev->card, SND_SOC_UPDATE_STARTUP);

		dpcm = list_first_entry(&rtd->dpcm[direction].be_clients,
					struct snd_soc_dpcm, list_be);

		if (list_empty(&rtd->dpcm[direction].be_clients))
			dev_warn(rtd->dev, "BE client list empty\n");
		else if (!dpcm->be)
			dev_warn(rtd->dev, "No BE\n");
		else
			dpcm->be->dpcm[direction].state = SND_SOC_DPCM_STATE_HW_PARAMS;

		ret = rtd->ops.prepare(substream);
		if (ret < 0)
			break;
		snd_sof_pcm_platform_trigger(sdev, substream,
					     SNDRV_PCM_TRIGGER_START);
		pm_runtime_get_noresume(sdev->dev);
		break;
	case SOF_IPC_STREAM_TRIG_STOP:
		stream = (struct sof_ipc_stream *)hdr;
		comp_id = stream->comp_id;
		if (!dsp_sof_find_spcm_comp(sdev, comp_id, &direction)) {
			ret = -ENODEV;
			break;
		}
		substream = dsp_sof_get_substream(sdev, &rtd, direction);
		if (!rtd) {
			ret = -ENODEV;
			break;
		}
		pm_runtime_put_noidle(sdev->dev);
		for (i = 0; i < rtd->num_codecs; i++) {
			codec_dai = rtd->codec_dais[i];
			ops = codec_dai->driver->ops;
			if (ops->trigger) {
				ret = ops->trigger(substream,
						   SNDRV_PCM_TRIGGER_STOP,
						   codec_dai);
				if (ret < 0) {
					dev_err(sdev->dev,
						"trigger stop fails\n");
					return ret;
				}
			}
		}
		snd_sof_pcm_platform_trigger(sdev, substream,
					     SNDRV_PCM_TRIGGER_STOP);
		soc_dpcm_runtime_update(sdev->card, SND_SOC_UPDATE_SHUTDOWN);
		snd_soc_runtime_deactivate(rtd, direction);
		break;
	case SOF_IPC_STREAM_PCM_FREE:
		dsp_sof_pcm_close(sdev, hdr);
		break;
	case SOF_IPC_STREAM_POSITION:
		/*
		 * TODO: this is special case, we do not send this IPC to DSP
		 * but read back position directly from memory (like SOS) and
		 * then reply to FE.
		 * Use stream ID to get correct stream data
		 */
		break;
	}

	return ret;
}

/* validate component IPC */
static int dsp_sof_ipc_comp(struct snd_sof_dev *sdev,
			    struct sof_ipc_cmd_hdr *hdr)
{
	/* TODO validate host comp id range based on vm_id */

	/* Nothing to be done */
	return 0;
}

static int dsp_sof_ipc_tplg_comp_new(struct vhost_dsp *dsp, int vq_idx,
				     struct sof_ipc_cmd_hdr *hdr)
{
	struct sof_ipc_comp *comp = (struct sof_ipc_comp *)hdr;
	struct snd_sof_dev *sdev = dsp->sdev;
	struct snd_sof_pcm *spcm, *last;
	struct sof_ipc_comp_host *host;
	struct sof_ipc_comp_dai *dai;
	struct dsp_pipeline_connect *conn;

	switch (comp->type) {
	case SOF_COMP_VIRT_CON:
		dai = (struct sof_ipc_comp_dai *)hdr;

		/* Add a new ID mapping to the list */
		conn = devm_kmalloc(sdev->dev, sizeof(*conn), GFP_KERNEL);
		if (!conn)
			return -ENOMEM;
		conn->vq_idx = vq_idx;
		conn->guest_id = dai->comp.id;
		conn->host_id = dai->config.ref_comp_id;
		conn->direction = dai->direction;
		list_add_tail(&conn->list, &dsp->pipe_conn);

		/* The firmware doesn't need this component */
		return 1;
	case SOF_COMP_HOST:
		/*
		 * TODO: below is a temporary solution. next step is
		 * to create a whole pcm stuff incluing substream
		 * based on Liam's suggestion.
		 */

		/*
		 * let's create spcm in HOST ipc
		 * spcm should be created in pcm load, but there is no such ipc
		 * so we create it here. It is needed for the "period elapsed"
		 * IPC from the firmware, which will use the host ID to route
		 * the IPC back to the PCM.
		 */
		host = (struct sof_ipc_comp_host *)hdr;
		spcm = kzalloc(sizeof(*spcm), GFP_KERNEL);
		if (!spcm)
			return -ENOMEM;

		spcm->sdev = sdev;
		spcm->stream[SNDRV_PCM_STREAM_PLAYBACK].comp_id =
			SOF_VIRTIO_COMP_ID_UNASSIGNED;
		spcm->stream[SNDRV_PCM_STREAM_CAPTURE].comp_id =
			SOF_VIRTIO_COMP_ID_UNASSIGNED;
		spcm->stream[host->direction].comp_id = host->comp.id;
		spcm->stream[SNDRV_PCM_STREAM_PLAYBACK].posn.comp_id =
			spcm->stream[SNDRV_PCM_STREAM_PLAYBACK].comp_id;
		spcm->stream[SNDRV_PCM_STREAM_CAPTURE].posn.comp_id =
			spcm->stream[SNDRV_PCM_STREAM_CAPTURE].comp_id;
		INIT_WORK(&spcm->stream[host->direction].period_elapsed_work,
			  sdev->core_ops.pcm_period_elapsed_work);
		dev_dbg(sdev->dev, "%s(): init %p\n", __func__,
			&spcm->stream[host->direction].period_elapsed_work);
		last = list_last_entry(&sdev->pcm_list, struct snd_sof_pcm, list);
		spcm->pcm.dai_id = last->pcm.dai_id + 1;
		list_add(&spcm->list, &sdev->pcm_list);
		break;
	default:
		break;
	}

	return 0;
}


static int dsp_sof_ipc_tplg_pipe_new(struct vhost_dsp *dsp, int vq_idx,
				     struct sof_ipc_cmd_hdr *hdr)
{
	struct sof_ipc_pipe_new *pipeline = (struct sof_ipc_pipe_new *)hdr;
	struct snd_sof_dev *sdev = dsp->sdev;
	struct dsp_pipeline_connect *conn;

	list_for_each_entry (conn, &dsp->pipe_conn, list)
		if (conn->vq_idx == vq_idx &&
		    pipeline->sched_id == conn->guest_id) {
			struct snd_sof_widget *mix_w =
				dsp_sof_find_swidget_id(sdev, conn->host_id);
			struct snd_sof_dai *dai;

			if (!mix_w) {
				dev_warn(sdev->dev,
					 "no mixer with ID %u found\n",
					 conn->host_id);
				continue;
			}

			dai = dsp_sof_find_dai_pipe(sdev, mix_w->pipeline_id);
			if (!dai) {
				dev_warn(sdev->dev,
					 "no DAI with pipe %u found\n",
					 mix_w->pipeline_id);
				continue;
			}

			/* Overwrite the scheduling sink ID with the DAI ID */
			pipeline->sched_id = dai->comp_dai.comp.id;
			break;
		}

	return 0;
}

static int dsp_sof_ipc_tplg_comp_connect(struct vhost_dsp *dsp, int vq_idx,
					 struct sof_ipc_cmd_hdr *hdr)
{
	struct sof_ipc_pipe_comp_connect *connect =
		(struct sof_ipc_pipe_comp_connect *)hdr;
	struct dsp_pipeline_connect *conn;

	list_for_each_entry (conn, &dsp->pipe_conn, list) {
		if (conn->vq_idx != vq_idx)
			continue;

		if (conn->direction == SOF_IPC_STREAM_PLAYBACK &&
		    connect->sink_id == conn->guest_id) {
			/* Overwrite the sink ID with the actual mixer component ID */
			connect->sink_id = conn->host_id;
			break;
		}

		if (conn->direction == SOF_IPC_STREAM_CAPTURE &&
		    connect->source_id == conn->guest_id) {
			/* Overwrite the source ID with the actual demux component ID */
			connect->source_id = conn->host_id;
			break;
		}
	}

	return 0;
}

static int dsp_sof_ipc_tplg_read(struct vhost_dsp *dsp,
				 struct sof_ipc_cmd_hdr *hdr,
				 void *reply_buf, size_t reply_sz)
{
	struct snd_sof_dev *sdev = dsp->sdev;
	struct sof_vfe_ipc_tplg_req *tplg = (struct sof_vfe_ipc_tplg_req *)hdr;
	struct sof_vfe_ipc_tplg_resp *partdata = reply_buf;
	const struct firmware *fw;
	size_t to_copy, remainder;
	int ret;

	if (reply_sz <= sizeof(partdata->reply)) {
		/* FIXME: send an error response */
		return -ENOBUFS;
	}

	if (!tplg->offset) {
		ret = pm_runtime_get_sync(sdev->dev);
		if (ret < 0) {
			dev_err_ratelimited(sdev->dev,
					    "error: failed to resume: %d\n",
					    ret);
			pm_runtime_put_noidle(sdev->dev);
			return ret;
		}

		ret = request_firmware(&fw, tplg->file_name, sdev->dev);
		if (ret < 0) {
			dev_err(sdev->dev,
				"error: request VFE topology %s failed: %d\n",
				tplg->file_name, ret);
			pm_runtime_put_noidle(sdev->dev);
			return ret;
		}

		dsp->fw = fw;
	} else if (dsp->fw) {
		fw = dsp->fw;
		ret = 0;
	} else {
		/* FIXME: send an error response */
		return -EINVAL;
	}

	remainder = fw->size - tplg->offset;

	partdata->reply.hdr.cmd = hdr->cmd;
	/*
	 * Non-standard size use: it's the remaining firmware bytes, plus
	 * the header, that way the last part will contain a correct size
	 */
	partdata->reply.hdr.size = remainder + sizeof(partdata->reply);

	to_copy = min_t(size_t, reply_sz - sizeof(partdata->reply),
			remainder);

	memcpy(partdata->data, fw->data + tplg->offset, to_copy);

	if (remainder == to_copy) {
		release_firmware(fw);
		dsp->fw = NULL;
		pm_runtime_mark_last_busy(sdev->dev);
		pm_runtime_put_autosuspend(sdev->dev);
	}

	return ret;
}

static int dsp_sof_ipc_tplg_comp_id(struct vhost_dsp *dsp,
				    struct sof_ipc_cmd_hdr *hdr,
				    void *reply_buf, size_t reply_sz)
{
	struct sof_vfe_ipc_tplg_resp *partdata = reply_buf;

	partdata->reply.hdr.cmd = hdr->cmd;
	partdata->reply.hdr.size = sizeof(partdata->reply) + sizeof(u32);
	*(u32 *)partdata->data = dsp->sdev->next_comp_id;

	dsp->comp_id_begin = dsp->sdev->next_comp_id;
	dsp->comp_id_end = dsp->comp_id_begin + SOF_VIRTIO_MAX_UOS_COMPS;

	return 0;
}

/* validate topology IPC */
static int dsp_sof_ipc_tplg(struct vhost_dsp *dsp, int vq_idx,
			    struct sof_ipc_cmd_hdr *hdr,
			    void *reply_buf, size_t reply_sz)
{
	/* TODO validate host comp id range based on vm_id */
	u32 cmd = hdr->cmd & SOF_CMD_TYPE_MASK;
	int ret;

	switch (cmd) {
	case SOF_IPC_TPLG_COMP_NEW:
		return dsp_sof_ipc_tplg_comp_new(dsp, vq_idx, hdr);
	case SOF_IPC_TPLG_PIPE_NEW:
		return dsp_sof_ipc_tplg_pipe_new(dsp, vq_idx, hdr);
	case SOF_IPC_TPLG_COMP_CONNECT:
		return dsp_sof_ipc_tplg_comp_connect(dsp, vq_idx, hdr);
	case SOF_IPC_TPLG_VFE_GET:
		ret = dsp_sof_ipc_tplg_read(dsp, hdr, reply_buf, reply_sz);
		if (ret < 0)
			return ret;
		return 1;
	case SOF_IPC_TPLG_VFE_COMP_ID:
		ret = dsp_sof_ipc_tplg_comp_id(dsp, hdr, reply_buf, reply_sz);
		if (ret < 0)
			return ret;
		return 1;
	}

	return 0;
}

static int sof_virtio_send_ipc(struct snd_sof_dev *sdev, void *ipc_data,
			       void *reply_data, size_t count,
			       size_t reply_size)
{
	struct snd_sof_ipc *ipc = sdev->ipc;
	struct sof_ipc_cmd_hdr *hdr = ipc_data;

	return sdev->core_ops.ipc_tx_message(ipc, hdr->cmd, ipc_data, count,
					     reply_data, reply_size);
}

static int dsp_sof_ipc_stream_param_post(struct snd_sof_dev *sdev,
					 void *reply_buf)
{
	struct sof_ipc_pcm_params_reply *reply = reply_buf;
	u32 comp_id = reply->comp_id;
	int direction, ret;
	struct snd_sof_pcm *spcm = dsp_sof_find_spcm_comp(sdev, comp_id,
							  &direction);
	if (!spcm)
		return -ENODEV;

	ret = snd_sof_ipc_pcm_params(sdev, spcm->stream[direction].substream,
				     reply);
	if (ret < 0)
		dev_err(sdev->dev, "error: got wrong reply for PCM %d\n",
			spcm->pcm.pcm_id);

	return ret;
}

/* handle the stream ipc */
static int dsp_sof_ipc_stream_codec(struct snd_sof_dev *sdev,
				    struct sof_ipc_cmd_hdr *hdr)
{
	struct sof_ipc_stream *stream = (struct sof_ipc_stream *)hdr;
	struct snd_pcm_substream *substream;
	struct snd_soc_pcm_runtime *rtd;
	unsigned int i;
	int direction;

	/* TODO validate host comp id range based on vm_id */

	dsp_sof_find_spcm_comp(sdev, stream->comp_id, &direction);
	substream = dsp_sof_get_substream(sdev, &rtd, direction);

	for (i = 0; i < rtd->num_codecs; i++) {
		struct snd_soc_dai *codec_dai = rtd->codec_dais[i];
		const struct snd_soc_dai_ops *ops = codec_dai->driver->ops;

		/*
		 * Now we are ready to trigger start.
		 * Let's unmute the codec firstly
		 */
		snd_soc_dai_digital_mute(codec_dai, 0, direction);
		if (ops->trigger) {
			int ret = ops->trigger(substream,
					       SNDRV_PCM_TRIGGER_START,
					       codec_dai);
			if (ret < 0)
				return ret;
		}
	}

	return 0;
}

/* Handle an IPC reply */
static int dsp_sof_ipc_post(struct snd_sof_dev *sdev,
			    struct sof_ipc_cmd_hdr *hdr, void *reply_buf)
{
	u32 type = hdr->cmd & SOF_GLB_TYPE_MASK;
	u32 cmd = hdr->cmd & SOF_CMD_TYPE_MASK;

	switch (type) {
	case SOF_IPC_GLB_STREAM_MSG:
		switch (cmd) {
		case SOF_IPC_STREAM_PCM_PARAMS:
			return dsp_sof_ipc_stream_param_post(sdev, reply_buf);
		case SOF_IPC_STREAM_TRIG_START:
			/* setup the codec */
			return dsp_sof_ipc_stream_codec(sdev, hdr);
		}
	}

	return 0;
}

int dsp_sof_ipc_fwd(struct vhost_dsp *dsp, int vq_idx,
		    void *ipc_buf, void *reply_buf,
		    size_t count, size_t reply_sz)
{
	struct snd_sof_dev *sdev = dsp->sdev;
	struct sof_ipc_cmd_hdr *hdr = ipc_buf;
	struct sof_ipc_reply *rhdr = reply_buf;
	u32 type;
	int ret = 0;

	/* validate IPC */
	if (!count) {
		dev_err(sdev->dev, "error: guest IPC size is 0\n");
		return -EINVAL;
	}

	ret = pm_runtime_get_sync(sdev->dev);
	if (ret < 0) {
		dev_err_ratelimited(sdev->dev,
				    "error: failed to resume: %d\n", ret);
		pm_runtime_put_noidle(sdev->dev);
		return ret;
	}

	type = hdr->cmd & SOF_GLB_TYPE_MASK;
	if (rhdr)
		rhdr->error = 0;

	/* validate the ipc */
	switch (type) {
	case SOF_IPC_GLB_COMP_MSG:
		ret = dsp_sof_ipc_comp(sdev, hdr);
		if (ret < 0)
			goto pm_put;
		break;
	case SOF_IPC_GLB_STREAM_MSG:
		ret = dsp_sof_ipc_stream(sdev, hdr, reply_buf);
		if (ret < 0) {
			dev_err(sdev->dev, "STREAM IPC 0x%x failed %d!\n",
				hdr->cmd, ret);
			if (rhdr)
				rhdr->error = ret;
			goto pm_put;
		}
		break;
	case SOF_IPC_GLB_DAI_MSG:
		/*
		 * After we use the new topology solution for FE,
		 * we will not touch DAI anymore.
		 */
		break;
	case SOF_IPC_GLB_TPLG_MSG:
		ret = dsp_sof_ipc_tplg(dsp, vq_idx, hdr, reply_buf, reply_sz);
		if (!ret)
			break;
		if (ret > 0)
			ret = 0;
		goto pm_put;
	case SOF_IPC_GLB_TRACE_MSG:
		/* Trace should be initialized in SOS, skip FE requirement */
		goto pm_put;
	default:
		dev_warn(sdev->dev, "unhandled IPC 0x%x!\n", hdr->cmd);
		break;
	}

	/* now send the IPC */
	ret = sof_virtio_send_ipc(sdev, ipc_buf, reply_buf, count, reply_sz);

	/* For some IPCs, the reply needs to be handled */
	if (!ret)
		ret = dsp_sof_ipc_post(sdev, hdr, reply_buf);

	if (ret < 0)
		dev_err(sdev->dev, "err: failed to send %u bytes virtio IPC 0x%x: %d\n",
			hdr->size, hdr->cmd, ret);

pm_put:
	pm_runtime_mark_last_busy(sdev->dev);
	pm_runtime_put_autosuspend(sdev->dev);

	return ret;
}

static struct vhost_dsp *dsp_sof_comp_id_to_snd(struct snd_sof_dev *sdev,
						int comp_id)
{
	struct vhost_dsp *dsp;

	list_for_each_entry(dsp, &sdev->vbe_list, list)
		if (comp_id < dsp->comp_id_end && comp_id >= dsp->comp_id_begin)
			return dsp;

	return NULL;
}

/* Always called from an interrupt thread context */
int dsp_sof_update_guest_posn(struct snd_sof_dev *sdev,
			      struct sof_ipc_stream_posn *posn)
{
	struct vhost_dsp *dsp = dsp_sof_comp_id_to_snd(sdev, posn->comp_id);
	struct vhost_dsp_posn *entry;

	/* posn update for SOS */
	if (!dsp)
		return 0;

	entry = kmalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	memcpy(&entry->posn, posn, sizeof(struct vhost_dsp_posn));

	/*
	 * Notification RX vq buffer is not available. Let's save the posn
	 * update msg. And send the msg when vq buffer is available.
	 */
	spin_lock_irq(&dsp->posn_lock);
	list_add_tail(&entry->list, &dsp->posn_list);
	spin_unlock_irq(&dsp->posn_lock);

	vhost_work_queue(&dsp->dev, &dsp->work);

	return 0;
}
EXPORT_SYMBOL_GPL(dsp_sof_update_guest_posn);
