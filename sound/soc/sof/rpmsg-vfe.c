// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020 Intel, Inc.
 */

#include <linux/completion.h>
#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/rpmsg.h>
#include <linux/scatterlist.h>
#include <linux/virtio_rpmsg.h>
#include <sound/sof.h>
#include <sound/sof/rpmsg.h>

#include "ops.h"
#include "sof-audio.h"
#include "sof-priv.h"

/* 600ms for VirtQ IPC */
#define SOF_VFE_DATA_TIMEOUT_MS 600

/* endpoint indices */
#define SOF_VFE_POSN (SOF_RPMSG_ADDR_POSN - 1)
#define SOF_VFE_DATA (SOF_RPMSG_ADDR_DATA - 1)

struct sov_vfe_posn_stream {
	struct work_struct work;
	struct snd_sof_pcm_stream *stream;
};

struct sof_vfe {
	struct snd_sof_dev *sdev;

	/* current pending cmd message */
	struct snd_sof_ipc_msg *msg;

	struct rpmsg_device *rpdev;
	/* One endpoint is embedded in rpdev */
	struct rpmsg_endpoint *ept[SOF_RPMSG_ADDR_COUNT - 1];

	/*
	 * IPC messages are blocked. "true" if the DSP hasn't been reset and
	 * therefore we don't have to re-send our topology.
	 */
	bool block_ipc;
	struct sof_rpmsg_ipc_tplg_resp tplg;

	struct completion completion;

	/* Pointers for virtual queue data buffers */
	struct sof_rpmsg_data_req *playback_buf;
	struct sof_rpmsg_data_resp *capture_buf;

	/* Headers, used as a playback response or capture request */
	struct sof_rpmsg_data_req hdr_req;
	struct sof_rpmsg_data_resp hdr_resp;
	struct sof_rpmsg_ipc_req ipc_buf;

	void __user *capture;
	size_t capture_size;

	struct workqueue_struct *posn_wq;
	struct sov_vfe_posn_stream posn_stream[2];
};

/* Firmware ready IPC. */
static int sof_vfe_fw_ready(struct snd_sof_dev *sdev, u32 msg_id)
{
	return 0;
};

/* Send IPC to vBE */
static int sof_vfe_send_msg(struct snd_sof_dev *sdev,
			    struct snd_sof_ipc_msg *msg)
{
	struct sof_ipc_reply *reply = msg->reply_data;
	struct sof_vfe *vfe = sdev->pdata->vfe;
	size_t msg_size = msg->msg_size;
	void *msg_data = msg->msg_data;
	int ret;

	if (vfe->block_ipc) {
		reply->error = 0;
		msg->reply_error = reply->error;
		/*
		 * No need to take .ipc_lock: we return > 0, so
		 * sof_ipc_tx_message_unlocked() won't overwrite .ipc_complete
		 */
		msg->ipc_complete = true;
		wake_up(&msg->waitq);

		return 1;
	}

	vfe->ipc_buf.reply_size = msg->reply_size;
	memcpy(vfe->ipc_buf.ipc_msg, msg_data, msg_size);

	ret = rpmsg_sendto(vfe->rpdev->ept, &vfe->ipc_buf,
			   msg_size + offsetof(struct sof_rpmsg_ipc_req, ipc_msg),
			   SOF_RPMSG_ADDR_IPC);
	if (ret < 0) {
		dev_err(sdev->dev, "%s(): error: sending IPC: %d\n",
			__func__, ret);
		return ret;
	}

	vfe->msg = msg;

	return 0;
}

static int sof_vfe_register(struct snd_sof_dev *sdev)
{
	sdev->pdata->vfe->sdev = sdev;
	sdev->next_comp_id = SOF_RPMSG_MAX_UOS_COMPS;

	return 0;
}

/* Some struct snd_sof_dsp_ops operations are compulsory, but unused by vFE */
static int sof_vfe_deregister(struct snd_sof_dev *sdev)
{
	return 0;
}

static int sof_vfe_run(struct snd_sof_dev *sdev)
{
	return 0;
}

static void sof_vfe_block_read(struct snd_sof_dev *sdev, u32 bar,
			       u32 offset, void *dest,
			       size_t size)
{
}

static void sof_vfe_block_write(struct snd_sof_dev *sdev, u32 bar,
				u32 offset, void *src,
				size_t size)
{
}

static int sof_vfe_load_firmware(struct snd_sof_dev *sdev)
{
	return 0;
}

static void sof_vfe_ipc_msg_data(struct snd_sof_dev *sdev,
				 struct snd_pcm_substream *substream,
				 void *p, size_t sz)
{
}

static int sof_vfe_ipc_pcm_params(struct snd_sof_dev *sdev,
				  struct snd_pcm_substream *substream,
				  const struct sof_ipc_pcm_params_reply *reply)
{
	return 0;
}

static int sof_vfe_sof_runtime_dummy(struct snd_sof_dev *sdev)
{
	return 0;
}

/* Send the position queue address. */
static int sof_vfe_position_addr(struct snd_sof_dev *sdev)
{
	struct sof_vfe *vfe = sdev->pdata->vfe;
	int ret = rpmsg_sendto(vfe->ept[SOF_VFE_POSN],
			       &vfe->ept[SOF_VFE_POSN]->addr,
			       sizeof(vfe->ept[SOF_VFE_POSN]->addr),
			       SOF_RPMSG_ADDR_POSN);
	if (ret < 0)
		dev_err(sdev->dev, "%s(): failed %d to send address\n",
			__func__, ret);

	return ret;
}

static int sof_vfe_request_topology(struct snd_sof_dev *sdev,
				    struct firmware *fw)
{
	struct sof_rpmsg_ipc_tplg_req rq = {
		.hdr = {
			.size = sizeof(rq),
			.cmd = SOF_IPC_GLB_TPLG_MSG | SOF_IPC_TPLG_VFE_GET,
		},
	};
	struct sof_vfe *vfe = sdev->pdata->vfe;
	struct sof_rpmsg_ipc_tplg_resp *partdata =
		(struct sof_rpmsg_ipc_tplg_resp *)vfe->ipc_buf.ipc_msg;
	size_t data_size;
	struct device *dev = sdev->dev;
	int ret;

	if (!partdata)
		return -ENOMEM;

	ret = pm_runtime_get_sync(dev);
	if (ret < 0) {
		dev_err(dev,
			"Cannot resume VFE sof-audio device. Error %d\n", ret);
		return ret;
	}

	do {
		ret = sof_ipc_tx_message_no_pm(sdev->ipc, rq.hdr.cmd,
					       &rq, sizeof(rq), partdata,
					       SOF_IPC_MSG_MAX_SIZE);
		if (ret < 0)
			goto free;

		if (partdata->reply.hdr.size <= sizeof(partdata->reply)) {
			ret = -EINVAL;
			goto free;
		}

		/*
		 * Size is consistent and decreasing, we're guaranteed to exit
		 * this loop eventually
		 */
		data_size = partdata->reply.hdr.size - sizeof(partdata->reply);
		if (rq.offset + data_size > sizeof(vfe->tplg.data)) {
			ret = -ENOBUFS;
			goto free;
		}

		memcpy(vfe->tplg.data + rq.offset, partdata->data, data_size/*to_copy*/);
		rq.offset += data_size;
	} while (partdata->reply.hdr.size == SOF_IPC_MSG_MAX_SIZE);

	fw->size = rq.offset;
	fw->data = vfe->tplg.data;

	/* Get our first component ID */
	rq.hdr.cmd = SOF_IPC_GLB_TPLG_MSG | SOF_IPC_TPLG_VFE_COMP_ID;
	rq.hdr.size = sizeof(rq.hdr);
	ret = sof_ipc_tx_message_no_pm(sdev->ipc, rq.hdr.cmd,
				       &rq, rq.hdr.size, partdata,
				       sizeof(partdata->reply) + sizeof(u32));
	if (ret < 0)
		goto free;

	sdev->next_comp_id = *(u32 *)partdata->data;

	ret = sof_vfe_position_addr(sdev);

free:
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);

	return ret;
}

static unsigned long get_dma_offset(struct snd_pcm_runtime *runtime,
				    int channel, unsigned long hwoff)
{
	return hwoff + channel * (runtime->dma_bytes / runtime->channels);
}

/* playback and capture are serialised by ipc->tx_mutex */
static int sof_vfe_pcm_read_part(struct snd_sof_dev *sdev,
				 struct snd_sof_pcm *spcm,
				 struct snd_pcm_substream *substream,
				 int channel, unsigned long posn,
				 void __user *buf, unsigned long chunk_size)
{
	struct sof_vfe *vfe = sdev->pdata->vfe;
	struct sof_rpmsg_data_resp *data = vfe->capture_buf;
	long remain;
	int ret;

	/* put response size in request */
	vfe->hdr_req.size = chunk_size;
	vfe->hdr_req.comp_id = spcm->stream[substream->stream].comp_id;
	vfe->hdr_req.offset = get_dma_offset(substream->runtime, channel, posn);

	vfe->capture = buf;
	vfe->capture_size = chunk_size;

	ret = rpmsg_sendto(vfe->ept[SOF_VFE_DATA], &vfe->hdr_req,
			   sizeof(vfe->hdr_req), SOF_RPMSG_ADDR_DATA);

	if (ret < 0) {
		dev_err(sdev->dev, "%s(): error: sending capture command %d\n",
			__func__, ret);
		return ret;
	}

	ret = wait_for_completion_timeout(&vfe->completion,
					  msecs_to_jiffies(SOF_VFE_DATA_TIMEOUT_MS));
	if (!ret) {
		dev_err(sdev->dev, "%s(): error: data read timeout\n", __func__);
		return -ETIMEDOUT;
	}
	if (ret < 0)
		return ret;

	remain = copy_to_user(vfe->capture, data->data, data->size);
	if (remain) {
		dev_err(sdev->dev, "%s(): copy_to_user() failed %ld\n",
			__func__, remain);
		return -EFAULT;
	}

	if (data->error < 0)
		return data->error;

	return 0;
}

/* playback and capture are serialised by ipc->tx_mutex */
static int sof_vfe_pcm_write_part(struct snd_sof_dev *sdev,
				  struct snd_sof_pcm *spcm,
				  struct snd_pcm_substream *substream,
				  int channel, unsigned long posn,
				  void __user *buf, unsigned long chunk_size)
{
	struct sof_vfe *vfe = sdev->pdata->vfe;
	struct sof_rpmsg_data_req *data = vfe->playback_buf;
	int ret;

	data->size = chunk_size;
	data->comp_id = spcm->stream[substream->stream].comp_id;
	data->offset = get_dma_offset(substream->runtime, channel, posn);

	if (copy_from_user(data->data, buf, chunk_size))
		return -EFAULT;

	vfe->capture = NULL;

	ret = rpmsg_sendto(vfe->ept[SOF_VFE_DATA], data,
			   chunk_size + sizeof(*data), SOF_RPMSG_ADDR_DATA);
	if (ret < 0) {
		dev_err(sdev->dev, "%s(): error: sending playback data: %d\n",
			__func__, ret);
		return ret;
	}

	ret = wait_for_completion_timeout(&vfe->completion,
				msecs_to_jiffies(SOF_VFE_DATA_TIMEOUT_MS));
	if (!ret)
		return -ETIMEDOUT;

	return ret < 0 ? ret : vfe->hdr_resp.error;
}

/* The slow version, using VirtQueues for playback and capture data */
int sof_vfe_pcm_copy_user(struct snd_soc_component *component,
			  struct snd_pcm_substream *substream, int channel,
			  unsigned long posn, void __user *buf,
			  unsigned long bytes)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_sof_dev *sdev = snd_soc_component_get_drvdata(component);
	struct snd_sof_pcm *spcm = snd_sof_find_spcm_dai(sdev->component, rtd);
	unsigned int i, n = (bytes + SOF_RPMSG_MAX_DATA_SIZE - 1) /
		SOF_RPMSG_MAX_DATA_SIZE;
	int ret = 0;

	if (!spcm || spcm->scomp != sdev->component) {
		dev_err(sdev->dev, "%s(): invalid SPCM 0x%p!\n", __func__,
			spcm);
		return -ENODEV;
	}

	/* TODO: is locking really needed here? */
	mutex_lock(&sdev->ipc->tx_mutex);

	for (i = 0; i < n; i++) {
		size_t n_bytes = i == n - 1 ? bytes % SOF_RPMSG_MAX_DATA_SIZE :
			SOF_RPMSG_MAX_DATA_SIZE;

		reinit_completion(&sdev->pdata->vfe->completion);

		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
			ret = sof_vfe_pcm_write_part(sdev, spcm, substream,
						channel, posn, buf, n_bytes);
		else
			ret = sof_vfe_pcm_read_part(sdev, spcm, substream,
						channel, posn, buf, n_bytes);

		if (ret < 0)
			break;

		buf += n_bytes;
		posn += n_bytes;
	}

	mutex_unlock(&sdev->ipc->tx_mutex);

	return ret;
}

#define SOF_VFE_FORMATS (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE | \
			 SNDRV_PCM_FMTBIT_S32_LE)

struct snd_soc_dai_driver virtio_dai[] = {
	{
		.name = "VirtIO DAI",
		/*
		 * non-NULL "stream" parameter interferes in
		 * snd_soc_dapm_new_dai_widgets()
		 */
		.playback = SOF_DAI_STREAM(NULL, 1, 8,
				SNDRV_PCM_RATE_8000_192000, SOF_VFE_FORMATS),
		.capture = SOF_DAI_STREAM(NULL, 1, 8,
				SNDRV_PCM_RATE_8000_192000, SOF_VFE_FORMATS),
	},
};

static int sof_vfe_pcm_open(struct snd_sof_dev *sdev,
			    struct snd_pcm_substream *substream)
{
	size_t overhead;
	int ret = pm_runtime_get_sync(sdev->dev);
	if (ret < 0)
		dev_err(sdev->dev,
			"Cannot resume VFE sof-audio device. Error %d\n", ret);

	overhead = substream->stream == SNDRV_PCM_STREAM_PLAYBACK ?
		ALIGN(sizeof(struct sof_rpmsg_data_req) +
		      sizeof(struct rpmsg_hdr), 16) :
		ALIGN(sizeof(struct sof_rpmsg_data_resp) +
		      sizeof(struct rpmsg_hdr), 16);

	snd_pcm_hw_constraint_minmax(substream->runtime,
				     SNDRV_PCM_HW_PARAM_BUFFER_BYTES, 0,
				     SOF_RPMSG_MAX_DATA_SIZE - overhead);

	return ret;
}

static int sof_vfe_pcm_close(struct snd_sof_dev *sdev,
			     struct snd_pcm_substream *substream)
{
	struct sof_vfe *vfe = sdev->pdata->vfe;

	pm_runtime_mark_last_busy(sdev->dev);
	pm_runtime_put_autosuspend(sdev->dev);

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		kfree(vfe->playback_buf);
		vfe->playback_buf = NULL;
	} else {
		kfree(vfe->capture_buf);
		vfe->capture_buf = NULL;
	}

	return 0;
}

static int sof_vfe_pcm_hw_params(struct snd_sof_dev *sdev,
				 struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *params,
				 struct sof_ipc_stream_params *ipc_params)
{
	struct sof_vfe *vfe = sdev->pdata->vfe;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK &&
	    !vfe->playback_buf) {
		vfe->playback_buf = kmalloc(sizeof(*vfe->playback_buf) +
					    SOF_RPMSG_MAX_DATA_SIZE, GFP_KERNEL);
		if (!vfe->playback_buf)
			return -ENOMEM;
	} else if (substream->stream == SNDRV_PCM_STREAM_CAPTURE &&
		   !vfe->capture_buf) {
		vfe->capture_buf = kmalloc(sizeof(*vfe->capture_buf) +
					   SOF_RPMSG_MAX_DATA_SIZE, GFP_KERNEL);
		if (!vfe->capture_buf)
			return -ENOMEM;
	}

	return 0;
}

/* IPC message sending completed. This means vBE has received the cmd */
static int sof_vfe_ept_ipc_cb(struct rpmsg_device *rpdev, void *buf, int len,
			      void *priv, u32 addr)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(&rpdev->dev);
	struct sof_vfe *vfe = sdev->pdata->vfe;
	struct snd_sof_ipc_msg *msg = vfe->msg;
	struct sof_ipc_reply *reply = msg->reply_data;
	unsigned long flags;

	if (len > msg->reply_size)
		return -ENOBUFS;

	memcpy(reply, buf, len);

	msg->reply_error = reply->error;

	dev_dbg(sdev->dev, "%s(): received %u bytes 0x%x error %d\n", __func__,
		reply->hdr.size, reply->hdr.cmd, reply->error);

	/* Firmware panic? */
	if (msg->reply_error == -ENODEV)
		sdev->ipc->disable_ipc_tx = true;

	spin_lock_irqsave(&sdev->ipc_lock, flags);
	msg->ipc_complete = true;
	wake_up(&msg->waitq);
	spin_unlock_irqrestore(&sdev->ipc_lock, flags);

	return 0;
}

static int sof_vfe_ept_data_cb(struct rpmsg_device *rpdev, void *buf, int len,
			       void *priv, u32 addr)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(&rpdev->dev);
	struct sof_vfe *vfe = sdev->pdata->vfe;
	struct sof_rpmsg_data_resp *data = buf;

	/* playback and capture are serialised by the IPC mutex */
	if (vfe->capture) {
		if (addr != SOF_RPMSG_ADDR_DATA || len < sizeof(*data) ||
		    data->size != vfe->capture_size) {
			dev_err(sdev->dev, "%s(): got %u instead of %zu bytes\n",
				__func__, data->size, vfe->capture_size);
			return -EINVAL;
		}

		memcpy(vfe->capture_buf, data,
		       data->size + sizeof(struct sof_rpmsg_data_resp));
	} else {
		if (addr != SOF_RPMSG_ADDR_DATA || len < sizeof(*data) ||
		    data->size)
			return -EINVAL;

		memcpy(&vfe->hdr_resp, buf, sizeof(*data));
	}

	complete(&vfe->completion);

	return 0;
}

/* The high latency version, using VirtQueues */
static int sof_vfe_ept_posn_cb(struct rpmsg_device *rpdev, void *buf, int len,
			       void *priv, u32 addr)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(&rpdev->dev);
	struct sof_ipc_stream_posn *posn = buf;
	struct snd_sof_pcm *spcm;
	int direction;

	if (addr != SOF_RPMSG_ADDR_POSN)
		return -EINVAL;

	if (!len)
		return 0;

	spcm = snd_sof_find_spcm_comp(sdev->component, posn->comp_id,
				      &direction);
	if (!spcm) {
		dev_err(sdev->dev,
			"err: period elapsed for unused component %d\n",
			posn->comp_id);
	} else {
		/*
		 * The position update requirement is valid.
		 * Let's update the position now.
		 */
		struct sof_vfe *vfe = sdev->pdata->vfe;
		struct sov_vfe_posn_stream *pstream = &vfe->posn_stream[direction];

		pstream->stream = &spcm->stream[direction];
		memcpy(&pstream->stream->posn, posn, sizeof(*posn));
		queue_work(vfe->posn_wq, &pstream->work);
	}

	return 0;
}

static int sof_vfe_runtime_suspend(struct device *dev)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(dev);
	struct sof_rpmsg_ipc_power_req rq = {
		.hdr = {
			.size = sizeof(rq),
			.cmd = SOF_IPC_GLB_PM_MSG | SOF_IPC_PM_VFE_POWER_STATUS,
		},
		.power = 0,
	};
	struct sof_rpmsg_ipc_power_resp resp = {.reply.error = 0};

	return sof_ipc_tx_message(sdev->ipc, rq.hdr.cmd, &rq, sizeof(rq),
				  &resp, sizeof(resp));
}

static int sof_vfe_runtime_resume(struct device *dev)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(dev);
	struct sof_vfe *vfe = sdev->pdata->vfe;
	struct sof_rpmsg_ipc_power_req rq = {
		.hdr = {
			.size = sizeof(rq),
			.cmd = SOF_IPC_GLB_PM_MSG | SOF_IPC_PM_VFE_POWER_STATUS,
		},
		.power = 1,
	};
	struct sof_rpmsg_ipc_power_resp resp = {.reply.error = 0};
	int ret = sof_ipc_tx_message(sdev->ipc, rq.hdr.cmd, &rq, sizeof(rq),
				     &resp, sizeof(resp));
	if (ret < 0)
		return ret;

	if (resp.reply.error < 0)
		return resp.reply.error;

	/*
	 * We are resuming. Check if the host needs the topology. We could in
	 * principle skip restoring pipelines completely, but it also does
	 * certain additional things, e.g. setting an enabled core mask
	 */
	vfe->block_ipc = resp.reset_status == SOF_RPMSG_IPC_RESET_NONE;

	/* restore pipelines */
	ret = sof_restore_pipelines(sdev->dev);
	if (ret < 0)
		dev_err(dev,
			"error: failed to restore pipeline after resume %d\n",
			ret);

	/* We're done resuming, from now all IPC have to be sent */
	vfe->block_ipc = false;

	return ret;
}

/* virtio fe ops */
static struct snd_sof_dsp_ops snd_sof_vfe_ops = {
	/* device init */
	.probe		= sof_vfe_register,
	.remove		= sof_vfe_deregister,

	/*
	 * PM: these are never called, they are only needed to prevent core.c
	 * from disabling runtime PM
	 */
	.runtime_suspend = sof_vfe_sof_runtime_dummy,
	.runtime_resume = sof_vfe_sof_runtime_dummy,

	/* IPC */
	.send_msg	= sof_vfe_send_msg,
	.fw_ready	= sof_vfe_fw_ready,

	/* machine driver */
	.machine_register = sof_machine_register,
	.machine_unregister = sof_machine_unregister,

	/* DAI drivers */
	.drv		= virtio_dai,
	.num_drv	= 1,

	.pcm_open	= sof_vfe_pcm_open,
	.pcm_close	= sof_vfe_pcm_close,
	.pcm_hw_params	= sof_vfe_pcm_hw_params,

	.run		= sof_vfe_run,
	.block_read	= sof_vfe_block_read,
	.block_write	= sof_vfe_block_write,
	.load_firmware	= sof_vfe_load_firmware,
	.ipc_msg_data	= sof_vfe_ipc_msg_data,
	.ipc_pcm_params	= sof_vfe_ipc_pcm_params,

	.request_topology = sof_vfe_request_topology,

	.hw_info	= SNDRV_PCM_INFO_INTERLEAVED,
};

static const struct sof_dev_desc virt_desc = {
	.nocodec_tplg_filename	= "",
	.default_tplg_path	= "",
	.resindex_lpe_base	= -1,
	.resindex_pcicfg_base	= -1,
	.resindex_imr_base	= -1,
	.irqindex_host_ipc	= -1,
	.resindex_dma_base	= -1,
	.ipc_timeout		= SOF_VFE_DATA_TIMEOUT_MS,
	.ops			= &snd_sof_vfe_ops,
};

static void sof_vfe_posn_update(struct work_struct *work)
{
	struct sov_vfe_posn_stream *pstream = container_of(work,
					struct sov_vfe_posn_stream, work);

	snd_pcm_period_elapsed(pstream->stream->substream);
}

static int sof_vfe_probe(struct rpmsg_device *rpdev)
{
	struct device *dev = &rpdev->dev;
	struct rpmsg_channel_info chinfo;
	struct snd_soc_acpi_mach *mach;
	struct snd_sof_pdata *sof_pdata;
	struct snd_sof_dev *sdev;
	struct sof_vfe *vfe;
	struct {
		struct snd_soc_acpi_mach mach;
		struct snd_sof_pdata pdata;
		struct sof_vfe vfe;
	} *drvdata;
	int ret, dir;

	/*
	 * The below two shouldn't be necessary, it's done in
	 * virtio_pci_modern_probe() by calling dma_set_mask_and_coherent()
	 */

	ret = dma_set_coherent_mask(dev, DMA_BIT_MASK(64));
	if (ret < 0)
		ret = dma_set_coherent_mask(dev, DMA_BIT_MASK(32));
	if (ret < 0)
		dev_warn(dev, "failed to set DMA mask: %d\n", ret);

	drvdata = devm_kzalloc(dev, sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

	sof_pdata = &drvdata->pdata;
	mach = &drvdata->mach;
	vfe = &drvdata->vfe;
	vfe->rpdev = rpdev;
	init_completion(&vfe->completion);

	for_each_pcm_streams(dir)
		INIT_WORK(&vfe->posn_stream[dir].work, sof_vfe_posn_update);

	vfe->posn_wq = alloc_workqueue("dsp-vfe-%d", 0, current->pid);
	if (!vfe->posn_wq)
		return -ENOMEM;

	mach->drv_name = "sof-nocodec";
	mach->mach_params.platform = dev_name(dev);
	sof_pdata->tplg_filename = virt_desc.nocodec_tplg_filename;

	ret = sof_nocodec_setup(dev, &snd_sof_vfe_ops);
	if (ret < 0)
		return ret;

	mach->pdata = &snd_sof_vfe_ops;

	sof_pdata->name = dev_name(&rpdev->dev);
	sof_pdata->machine = mach;
	sof_pdata->desc = &virt_desc;
	sof_pdata->dev = dev;
	sof_pdata->vfe = vfe;
	sof_pdata->tplg_filename_prefix = virt_desc.default_tplg_path;

	/* allow runtime_pm */
	pm_runtime_set_autosuspend_delay(dev, SND_SOF_SUSPEND_DELAY_MS);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_enable(dev);

	/*
	 * The RPMSG device name comes with the namespace announcement from the
	 * remote. That will also be used as a name of the first automatically-
	 * created channel
	 */

	strlcpy(chinfo.name, "ADSP audio data", RPMSG_NAME_SIZE);
	chinfo.src = RPMSG_ADDR_ANY;
	chinfo.dst = SOF_RPMSG_ADDR_DATA;

	get_device(dev);

	vfe->ept[SOF_VFE_DATA] = rpmsg_create_ept(rpdev, sof_vfe_ept_data_cb,
						    vfe, chinfo);
	if (!vfe->ept[SOF_VFE_DATA]) {
		dev_err(dev, "failed to create %s\n", chinfo.name);
		put_device(dev);
		return -EINVAL;
	}

	strlcpy(chinfo.name, "ADSP position update", RPMSG_NAME_SIZE);
	/* RPMSG_ADDR_ANY: automatically allocated from RPMSG_RESERVED_ADDRESSES... */
	chinfo.src = RPMSG_ADDR_ANY;
	chinfo.dst = SOF_RPMSG_ADDR_POSN;

	vfe->ept[SOF_VFE_POSN] = rpmsg_create_ept(rpdev, sof_vfe_ept_posn_cb,
						    vfe, chinfo);
	if (!vfe->ept[SOF_VFE_POSN]) {
		dev_err(dev, "failed to create %s\n", chinfo.name);

		ret = -EINVAL;
		goto e_posn;
	}

	ret = snd_sof_device_probe(dev, sof_pdata);
	if (ret < 0) {
		dev_err(dev, "Cannot register VFE sof-audio device. Error %d\n",
			ret);

		goto e_sof;
	}

	sdev = dev_get_drvdata(dev);
	vfe->sdev = sdev;

	/*
	 * Currently we only support one VM. comp_id from 0 to
	 * SOF_RPMSG_MAX_UOS_COMPS - 1 is for the host. Other comp_id numbers
	 * are for VM1.
	 * This will be overwritten during topology setup.
	 */
	sdev->next_comp_id = SOF_RPMSG_MAX_UOS_COMPS;

	dev_dbg(dev, "created VFE machine %s\n",
		dev_name(&sof_pdata->pdev_mach->dev));

	return 0;

e_sof:
	rpmsg_destroy_ept(vfe->ept[SOF_VFE_POSN]);
e_posn:
	rpmsg_destroy_ept(vfe->ept[SOF_VFE_DATA]);
	put_device(dev);

	return ret;
}

static void sof_vfe_remove(struct rpmsg_device *rpdev)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(&rpdev->dev);
	struct sof_vfe *vfe = sdev->pdata->vfe;

	/* free rpmsg resurces and unregister device */
	sof_vfe_runtime_suspend(&rpdev->dev);

	pm_runtime_disable(&rpdev->dev);

	if (vfe->ept[SOF_VFE_DATA]) {
		rpmsg_destroy_ept(vfe->ept[SOF_VFE_DATA]);
		vfe->ept[SOF_VFE_DATA] = NULL;
	}

	if (vfe->ept[SOF_VFE_POSN]) {
		rpmsg_destroy_ept(vfe->ept[SOF_VFE_POSN]);
		vfe->ept[SOF_VFE_POSN] = NULL;
	}

	/* unregister the SOF device */
	snd_sof_device_remove(&rpdev->dev);

	put_device(&rpdev->dev);
}

static const struct rpmsg_device_id sof_vfe_match[] = {
	{ "sof_rpmsg" },
	{}
};

static const struct dev_pm_ops sof_vfe_pm = {
	SET_RUNTIME_PM_OPS(sof_vfe_runtime_suspend, sof_vfe_runtime_resume,
			   NULL)
};

static struct rpmsg_driver sof_vfe_driver = {
	.probe		= sof_vfe_probe,
	.remove		= sof_vfe_remove,
	.callback	= sof_vfe_ept_ipc_cb,
	.id_table	= sof_vfe_match,
	.drv		= {
		.name	= KBUILD_MODNAME,
		.owner	= THIS_MODULE,
		.pm	= &sof_vfe_pm,
	},
};

module_rpmsg_driver(sof_vfe_driver);

MODULE_AUTHOR("Intel, Inc.");
MODULE_DESCRIPTION("SOF RPMSG driver");
MODULE_LICENSE("GPL v2");
