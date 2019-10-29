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

/*
 * virt IO FE driver
 *
 * The SOF driver thinks this driver is another audio DSP, however the calls
 * made by the SOF driver core do not directly go to HW, but over a virtIO
 * message Q to the virtIO BE driver.
 *
 * The virtIO message Q will use the *exact* same IPC structures as we currently
 * use in the mailbox.
 *
 * The mailbox IO and TX/RX msg functions below will do IO on the virt IO Q.
 */

#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/scatterlist.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/virtio_ids.h>
#include <linux/virtio_ring.h>
#include <sound/sof.h>
#include <sound/sof/virtio.h>

#include "ops.h"
#include "sof-priv.h"

#define SOF_COMPONENT_NAME	"sof-audio-component"
#define DATA_TIMEOUT_MS		600

static const char *const sof_vq_names[SOF_VIRTIO_NUM_OF_VQS] = {
	SOF_VIRTIO_IPC_CMD_VQ_NAME,
	SOF_VIRTIO_IPC_PSN_VQ_NAME,
	SOF_VIRTIO_DATA_VQ_NAME,
};

struct sof_vfe {
	struct snd_sof_dev *sdev;

	/* IPC cmd from frontend to backend */
	struct virtqueue *ipc_cmd_vq;

	/* IPC position update from backend to frontend */
	struct virtqueue *ipc_psn_vq;

	/* audio data in both directions */
	struct virtqueue *data_vq;

	/* position update work */
	struct work_struct posn_update_work;

	/* current pending cmd message */
	struct snd_sof_ipc_msg *msg;

	/* current and pending notification */
	struct snd_sof_ipc_msg *not;
	struct sof_ipc_stream_posn posn;

	struct sof_vfe_ipc_tplg_resp tplg;

	struct wait_queue_head data_wq;

	bool data_done;

	/* A shared capture / playback virtual queue data buffer */
	union {
		struct dsp_sof_data_req data_req;
		struct dsp_sof_data_resp data_resp;
	};

	/* Headers, used as a playback response or capture request */
	union {
		u8 hdr_req[HDR_SIZE_REQ];
		u8 hdr_resp[HDR_SIZE_RESP];
	};
};

/*
 * IPC Firmware ready.
 */
static int sof_vfe_fw_ready(struct snd_sof_dev *sdev, u32 msg_id)
{
	return 0;
};

/* used to send IPC to BE */
static int sof_vfe_send_msg(struct snd_sof_dev *sdev,
			    struct snd_sof_ipc_msg *msg)
{
	struct sof_vfe *vfe = sdev->pdata->vfe;
	int ret;
	struct scatterlist sg_out, sg_in, *sgs[] = {&sg_out, &sg_in};
	size_t msg_size = msg->msg_size;
	void *msg_data = msg->msg_data;

	switch (msg->header) {
	case SOF_IPC_GLB_STREAM_MSG | SOF_IPC_STREAM_TRIG_START:
		sg_init_one(&sg_in, &vfe->posn,
			    sizeof(struct sof_ipc_stream_posn));
		ret = virtqueue_add_inbuf(vfe->ipc_psn_vq,
					  &sg_in, 1, &vfe->posn, GFP_KERNEL);
		if (ret < 0) {
			dev_err(sdev->dev, "%s(): failed %d to add a buffer\n",
				__func__, ret);
			return ret;
		}

		virtqueue_kick(vfe->ipc_psn_vq);

		break;
	}

	sg_init_one(&sg_out, msg_data, msg_size);
	sg_init_one(&sg_in, msg->reply_data, msg->reply_size);

	ret = virtqueue_add_sgs(vfe->ipc_cmd_vq, sgs, 1, 1, msg_data,
				GFP_ATOMIC);
	if (ret < 0)
		dev_err(sdev->dev, "error: could not send IPC %d\n", ret);

	vfe->msg = msg;

	virtqueue_kick(vfe->ipc_cmd_vq);

	return ret;
}

/* Handle playback or capture data */
static void sof_vfe_handle_data(struct virtqueue *vq)
{
	struct sof_vfe *vfe = vq->vdev->priv;

	vfe->data_done = true;
	wake_up(&vfe->data_wq);
}

/* Send the IPC message completed. This means vBE has received the cmd */
static void sof_vfe_cmd_tx_done(struct virtqueue *vq)
{
	struct sof_vfe *vfe = vq->vdev->priv;
	DEFINE_SPINLOCK(lock);

	do {
		struct snd_sof_ipc_msg *msg = vfe->msg;
		struct sof_ipc_reply *reply = msg->reply_data;
		unsigned int len;

		virtqueue_disable_cb(vq);

		spin_lock(&lock);
		/*
		 * virtqueue_get_buf() returns the "token" that was provided to
		 * virtqueue_add_*() functions
		 */
		while (virtqueue_get_buf(vq, &len)) {
			msg->reply_error = reply->error;

			/* Firmware panic? */
			if (msg->reply_error == -ENODEV)
				vfe->sdev->ipc->disable_ipc_tx = true;

			msg->ipc_complete = true;
			wake_up(&msg->waitq);
		}
		spin_unlock(&lock);
	} while (!virtqueue_enable_cb(vq));
}

static void sof_vfe_posn_update(struct work_struct *work)
{
	struct sof_vfe *vfe = container_of(work, struct sof_vfe,
					   posn_update_work);
	struct sof_ipc_stream_posn *posn = &vfe->posn;
	struct virtqueue *vq = vfe->ipc_psn_vq;
	struct snd_sof_dev *sdev = vfe->sdev;
	struct snd_sof_pcm *spcm;
	struct scatterlist sg;
	unsigned int buflen;
	int direction;

	/* virtio protects and make sure no re-entry */
	while (virtqueue_get_buf(vq, &buflen)) {
		spcm = snd_sof_find_spcm_comp(sdev, posn->comp_id, &direction);
		if (!spcm) {
			dev_err(sdev->dev,
				"err: period elapsed for unused component %d\n",
					posn->comp_id);
		} else {
			/*
			 * The position update requirement is valid.
			 * Let's update the position now.
			 */
			memcpy(&spcm->stream[direction].posn, posn, sizeof(*posn));
			snd_pcm_period_elapsed(spcm->stream[direction].substream);
		}

		/* kick back the empty posn buffer immediately */
		sg_init_one(&sg, posn, sizeof(*posn));
		virtqueue_add_inbuf(vq, &sg, 1, posn, GFP_KERNEL);
		virtqueue_kick(vq);
	}
}

/*
 * handle the pos_update, receive the posn and send to up layer, then
 * resend the buffer to BE
 */
static void sof_vfe_psn_handle_rx(struct virtqueue *vq)
{
	struct sof_vfe *vfe = vq->vdev->priv;

	schedule_work(&vfe->posn_update_work);
}

static int sof_vfe_register(struct snd_sof_dev *sdev)
{
	return 0;
}

static int sof_vfe_unregister(struct snd_sof_dev *sdev)
{
	return 0;
}

#define SOF_VFE_FORMATS (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE | \
			 SNDRV_PCM_FMTBIT_S32_LE)

struct snd_soc_dai_driver virtio_dai[] = {
	{
		.name = "SSP4 Pin",
		.playback = SOF_DAI_STREAM("ssp4 Tx", 1, 8,
				SNDRV_PCM_RATE_8000_192000, SOF_VFE_FORMATS),
		.capture = SOF_DAI_STREAM("ssp4 Rx", 1, 8,
				SNDRV_PCM_RATE_8000_192000, SOF_VFE_FORMATS),
	},
};

static int sof_vfe_run(struct snd_sof_dev *sdev)
{
	sdev->boot_complete = true;
	wake_up(&sdev->boot_wait);
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

static int sof_vfe_request_topology(struct snd_sof_dev *sdev, const char *name,
				    struct firmware *fw)
{
	struct sof_vfe_ipc_tplg_req rq = {
		.hdr = {
			.size = sizeof(rq),
			.cmd = SOF_IPC_GLB_TPLG_MSG | SOF_IPC_TPLG_VFE_GET,
		},
	};
	struct sof_vfe *vfe = sdev->pdata->vfe;
	struct sof_vfe_ipc_tplg_resp *partdata = kmalloc(SOF_IPC_MSG_MAX_SIZE,
							 GFP_KERNEL);
	size_t part_size = SOF_IPC_MSG_MAX_SIZE - sizeof(partdata->reply),
		data_size;
	int ret;

	if (!partdata)
		return -ENOMEM;

	strncpy(rq.file_name, name, sizeof(rq.file_name));

	mutex_lock(&sdev->ipc->tx_mutex);

	do {
		size_t to_copy;

		ret = sof_ipc_tx_message_unlocked(sdev->ipc, rq.hdr.cmd,
						  &rq, sizeof(rq), partdata,
						  SOF_IPC_MSG_MAX_SIZE);
		if (ret < 0)
			goto free;

		data_size = partdata->reply.hdr.size - sizeof(partdata->reply);
		to_copy = min_t(size_t, data_size, part_size);
		memcpy(vfe->tplg.data + rq.offset, partdata->data, to_copy);
		if (!rq.offset)
			fw->size = data_size;
		rq.offset += part_size;
	} while (data_size > part_size);

	rq.hdr.cmd = SOF_IPC_GLB_TPLG_MSG | SOF_IPC_TPLG_VFE_COMP_ID;
	rq.hdr.size = sizeof(rq.hdr);
	ret = sof_ipc_tx_message_unlocked(sdev->ipc, rq.hdr.cmd,
					  &rq, rq.hdr.size, partdata,
					  sizeof(partdata->reply) + sizeof(u32));
	if (ret < 0)
		goto free;

	sdev->next_comp_id = *(u32 *)partdata->data;

	fw->data = vfe->tplg.data;
	fw->pages = NULL;

free:
	mutex_unlock(&sdev->ipc->tx_mutex);

	kfree(partdata);
	return 0;
}

static int sof_vfe_trace_init(struct snd_sof_dev *sdev, u32 *stream_tag)
{
	return -ENODEV;
}

static int sof_vfe_sof_runtime_suspend(struct snd_sof_dev *sof_dev,
				   int state)
{
	return 0;
}

static int sof_vfe_sof_runtime_resume(struct snd_sof_dev *sof_dev)
{
	return 0;
}

static unsigned long get_dma_offset(struct snd_pcm_runtime *runtime,
				    int channel, unsigned long hwoff)
{
	return hwoff + channel * (runtime->dma_bytes / runtime->channels);
}

static int sof_vfe_pcm_read_part(struct snd_sof_dev *sdev,
				 struct snd_sof_pcm *spcm,
				 struct snd_pcm_substream *substream,
				 int channel, unsigned long pos,
				 void __user *buf, unsigned long chunk_size)
{
	struct sof_vfe *vfe = sdev->pdata->vfe;
	struct dsp_sof_data_resp *data = &vfe->data_resp;
	struct scatterlist sg_out, sg_in, *sgs[] = {&sg_out, &sg_in};
	struct dsp_sof_data_req *req = (struct dsp_sof_data_req *)vfe->hdr_req;
	int ret;

	/* put response size in request */
	req->size = chunk_size;
	req->comp_id = spcm->stream[substream->stream].comp_id;
	req->offset = get_dma_offset(substream->runtime, channel, pos);

	sg_init_one(&sg_out, vfe->hdr_req, sizeof(vfe->hdr_req));
	sg_init_one(&sg_in, data, chunk_size + HDR_SIZE_RESP);

	ret = virtqueue_add_sgs(vfe->data_vq, sgs, 1, 1, vfe->hdr_req,
				GFP_ATOMIC);
	if (ret < 0)
		dev_err(sdev->dev, "error: could not send data %d\n", ret);

	virtqueue_kick(vfe->data_vq);

	ret = wait_event_timeout(vfe->data_wq, vfe->data_done,
				 msecs_to_jiffies(DATA_TIMEOUT_MS));
	if (!ret)
		return -ETIMEDOUT;
	if (ret < 0)
		return ret;
	if (data->error < 0)
		return data->error;

	if (copy_to_user((void __user *)buf, data->data, chunk_size))
		return -EFAULT;

	return 0;
}

static int sof_vfe_pcm_write_part(struct snd_sof_dev *sdev,
				  struct snd_sof_pcm *spcm,
				  struct snd_pcm_substream *substream,
				  int channel, unsigned long pos,
				  void __user *buf, unsigned long chunk_size)
{
	struct sof_vfe *vfe = sdev->pdata->vfe;
	struct dsp_sof_data_req *data = &vfe->data_req;
	struct scatterlist sg_out, sg_in, *sgs[] = {&sg_out, &sg_in};
	struct dsp_sof_data_resp *resp = (struct dsp_sof_data_resp *)vfe->hdr_resp;
	int ret;

	data->size = chunk_size;
	data->comp_id = spcm->stream[substream->stream].comp_id;
	data->offset = get_dma_offset(substream->runtime, channel, pos);

	if (copy_from_user(data->data, (void __user *)buf, chunk_size))
		return -EFAULT;

	sg_init_one(&sg_out, data, chunk_size + HDR_SIZE_REQ);
	sg_init_one(&sg_in, vfe->hdr_resp, sizeof(vfe->hdr_resp));

	ret = virtqueue_add_sgs(vfe->data_vq, sgs, 1, 1, vfe->hdr_resp,
				GFP_ATOMIC);
	if (ret < 0)
		dev_err(sdev->dev, "error: could not send data %d\n", ret);

	virtqueue_kick(vfe->data_vq);

	ret = wait_event_timeout(vfe->data_wq, vfe->data_done,
				 msecs_to_jiffies(DATA_TIMEOUT_MS));
	if (!ret)
		return -ETIMEDOUT;

	return ret < 0 ? ret : resp->error;
}

int sof_vfe_pcm_copy_user(struct snd_pcm_substream *substream, int channel,
			  unsigned long pos, void __user *buf,
			  unsigned long bytes)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_component *component =
		snd_soc_rtdcom_lookup(rtd, SOF_COMPONENT_NAME);
	struct snd_sof_dev *sdev = snd_soc_component_get_drvdata(component);
	struct snd_sof_pcm *spcm = snd_sof_find_spcm_dai(sdev, rtd);
	unsigned int i, n = (bytes + SOF_VFE_MAX_DATA_SIZE - 1) /
		SOF_VFE_MAX_DATA_SIZE;
	int ret = 0;

	if (!spcm || spcm->sdev != sdev) {
		dev_err(sdev->dev, "%s(): invalid SPCM 0x%p!\n", __func__,
			spcm);
		return -ENODEV;
	}

	mutex_lock(&sdev->ipc->tx_mutex);

	for (i = 0; i < n; i++) {
		size_t n_bytes = i == n - 1 ? bytes % SOF_VFE_MAX_DATA_SIZE :
			SOF_VFE_MAX_DATA_SIZE;

		sdev->pdata->vfe->data_done = false;

		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
			ret = sof_vfe_pcm_write_part(sdev, spcm, substream,
						channel, pos, buf, n_bytes);
		else
			ret = sof_vfe_pcm_read_part(sdev, spcm, substream,
						channel, pos, buf, n_bytes);

		if (ret < 0)
			break;

		buf += n_bytes;
		pos += n_bytes;
	}

	mutex_unlock(&sdev->ipc->tx_mutex);

	return ret;
}

/* virtio fe ops */
struct snd_sof_dsp_ops snd_sof_vfe_ops = {
	/* device init */
	.probe		= sof_vfe_register,
	.remove		= sof_vfe_unregister,

	/* PM */
	.runtime_suspend = sof_vfe_sof_runtime_suspend,
	.runtime_resume = sof_vfe_sof_runtime_resume,

	/* IPC */
	.send_msg	= sof_vfe_send_msg,
	.fw_ready	= sof_vfe_fw_ready,

	/* DAI drivers */
	.drv		= virtio_dai,
	.num_drv	= 1,

	.run		= sof_vfe_run,
	.block_read	= sof_vfe_block_read,
	.block_write	= sof_vfe_block_write,
	.load_firmware	= sof_vfe_load_firmware,
	.ipc_msg_data	= sof_vfe_ipc_msg_data,
	.ipc_pcm_params	= sof_vfe_ipc_pcm_params,

	.trace_init	= sof_vfe_trace_init,

	.request_topology = sof_vfe_request_topology,
};

static const struct sof_dev_desc virt_desc = {
	.nocodec_fw_filename	= NULL,
	.nocodec_tplg_filename	= "sof-apl-uos0.tplg",
	.default_tplg_path	= "intel/sof-tplg",
	.resindex_lpe_base	= -1,
	.resindex_pcicfg_base	= -1,
	.resindex_imr_base	= -1,
	.irqindex_host_ipc	= -1,
	.resindex_dma_base	= -1,
	.ops			= &snd_sof_vfe_ops,
};

static void sof_virtio_vfe_init(struct snd_sof_dev *sdev,
				struct sof_vfe *vfe)
{
	sdev->is_vfe = IS_ENABLED(CONFIG_SND_SOC_SOF_VIRTIO_FE);

	/*
	 * Currently we only support one VM. comp_id from 0 to
	 * SOF_VIRTIO_MAX_UOS_COMPS - 1 is for SOS. Other comp_id numbers
	 * are for VM1.
	 * TBD: comp_id number range should be dynamically assigned when
	 * multiple VMs are supported.
	 */
	sdev->next_comp_id = SOF_VIRTIO_MAX_UOS_COMPS;
	vfe->sdev = sdev;
}

static int sof_vfe_init(struct virtio_device *vdev)
{
	struct device *dev;
	struct snd_soc_acpi_mach *mach;
	struct snd_sof_pdata *sof_pdata;
	int ret;

	dev = &vdev->dev;

	sof_pdata = devm_kzalloc(dev, sizeof(*sof_pdata), GFP_KERNEL);
	if (!sof_pdata)
		return -ENOMEM;

	mach = devm_kzalloc(dev, sizeof(*mach), GFP_KERNEL);
	if (!mach)
		return -ENOMEM;

	ret = sof_nocodec_setup(dev, sof_pdata, mach, &virt_desc,
				&snd_sof_vfe_ops);
	if (ret < 0)
		return ret;

	mach->pdata = &snd_sof_vfe_ops;

	sof_pdata->name = dev_name(&vdev->dev);
	sof_pdata->machine = mach;
	sof_pdata->desc = &virt_desc;
	sof_pdata->dev = dev;
	sof_pdata->vfe = vdev->priv;
	sof_pdata->tplg_filename_prefix = virt_desc.default_tplg_path;

	ret = snd_sof_device_probe(dev, sof_pdata);
	if (ret < 0)
		dev_err(dev, "Cannot register device sof-audio. Error %d\n",
			ret);
	else {
		sof_virtio_vfe_init(dev_get_drvdata(dev), vdev->priv);

		dev_dbg(dev, "created machine %s\n",
			dev_name(&sof_pdata->pdev_mach->dev));
	}

	/* allow runtime_pm */
	pm_runtime_set_autosuspend_delay(dev, SND_SOF_SUSPEND_DELAY_MS);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_enable(dev);

	return ret;
}

/* Probe and remove. */
static int sof_vfe_probe(struct virtio_device *vdev)
{
	struct virtqueue *vqs[SOF_VIRTIO_NUM_OF_VQS];
	/* the processing callback number must be the same as the vqueues.*/
	vq_callback_t *cbs[SOF_VIRTIO_NUM_OF_VQS] = {
		sof_vfe_cmd_tx_done,
		sof_vfe_psn_handle_rx,
		sof_vfe_handle_data,
	};
	struct device *dev = &vdev->dev;
	struct sof_vfe *vfe;
	int ret;

	/*
	 * The below two shouldn't be necessary, it's done in
	 * virtio_pci_modern_probe() by calling dma_set_mask_and_coherent()
	 */

	ret = dma_set_coherent_mask(dev, DMA_BIT_MASK(64));
	if (ret < 0)
		ret = dma_set_coherent_mask(dev, DMA_BIT_MASK(32));
	if (ret < 0)
		dev_warn(dev, "failed to set DMA mask: %d\n", ret);

	vfe = devm_kzalloc(dev, sizeof(*vfe), GFP_KERNEL);
	if (!vfe)
		return -ENOMEM;

	vdev->priv = vfe;

	INIT_WORK(&vfe->posn_update_work, sof_vfe_posn_update);
	init_waitqueue_head(&vfe->data_wq);

	/* create virt queue for vfe to send/receive IPC message. */
	ret = virtio_find_vqs(vdev, SOF_VIRTIO_NUM_OF_VQS,
			      vqs, cbs, sof_vq_names, NULL);
	if (ret) {
		dev_err(dev, "error: find vqs fail with %d\n", ret);
		return ret;
	}

	/* virtques */
	vfe->ipc_cmd_vq = vqs[SOF_VIRTIO_IPC_CMD_VQ];
	vfe->ipc_psn_vq = vqs[SOF_VIRTIO_IPC_PSN_VQ];
	vfe->data_vq = vqs[SOF_VIRTIO_DATA_VQ];

	virtio_device_ready(vdev);

	return sof_vfe_init(vdev);
}

static void sof_vfe_remove(struct virtio_device *vdev)
{
	/* free virtio resurces and unregister device */
	struct sof_vfe *vfe = vdev->priv;

	vdev->config->reset(vdev);
	vdev->config->del_vqs(vdev);
	cancel_work_sync(&vfe->posn_update_work);

	/* unregister the SOF device */
	snd_sof_device_remove(&vdev->dev);

	return;
}

static void virtaudio_config_changed(struct virtio_device *vdev)
{
}

/*
 * Need to patch QEMU to create a virtio audio device, e.g. per
 * -device virtio-snd-pci,snd=snd0 where Device ID must be
 * 0x1040 + VIRTIO_ID_DSP and Vendor ID = PCI_VENDOR_ID_REDHAT_QUMRANET
 */
static const struct virtio_device_id id_table[] = {
	{VIRTIO_ID_DSP, VIRTIO_DEV_ANY_ID},
	{0},
};

/*
 * TODO: There still need a shutdown to handle the case the UOS
 * is poweroff, restart.
 */

static int sof_vfe_runtime_suspend(struct device *dev)
{
	dev_dbg(dev, "%s()\n", __func__);
	return 0;
}

static int sof_vfe_runtime_resume(struct device *dev)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(dev);
	int ret;

	dev_dbg(dev, "restore pipelines for resume\n");

	/* restore pipelines */
	ret = sof_restore_pipelines(sdev);
	if (ret < 0)
		dev_err(dev,
			"error: failed to restore pipeline after resume %d\n",
			ret);

	return ret;
}

static const struct dev_pm_ops sof_vfe_pm = {
	SET_RUNTIME_PM_OPS(sof_vfe_runtime_suspend, sof_vfe_runtime_resume,
			   NULL)
};

static struct virtio_driver sof_vfe_audio_driver = {
	.driver = {
		.name	= KBUILD_MODNAME,
		.owner	= THIS_MODULE,
		.pm	= &sof_vfe_pm,
	},
	.id_table	= id_table,
	.probe		= sof_vfe_probe,
	.remove		= sof_vfe_remove,
	.config_changed	= virtaudio_config_changed,
};

module_virtio_driver(sof_vfe_audio_driver);

MODULE_DEVICE_TABLE(virtio, id_table);
