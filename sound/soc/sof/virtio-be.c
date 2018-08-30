// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
/*
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * Copyright(c) 2017 Intel Corporation. All rights reserved.
 *
 *  Author: Libin Yang <libin.yang@intel.com>
 *	  Luo Xionghu <xionghu.luo@intel.com>
 *	  Liam Girdwood <liam.r.girdwood@linux.intel.com>
 *
 */

#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/hw_random.h>
#include <linux/uio.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/device.h>
#include <sound/sof.h>
#include <sound/pcm_params.h>
#include <uapi/sound/sof-ipc.h>
#include <linux/vbs/vq.h>
#include <linux/vbs/vbs.h>
#include <linux/vhm/acrn_common.h>
#include <linux/vhm/acrn_vhm_ioreq.h>
#include <linux/vhm/acrn_vhm_mm.h>
#include <linux/vhm/vhm_vm_mngt.h>
#include "sof-priv.h"
#include "ops.h"
#include "virtio-miscdev.h"

/* TODO: move the below macros to the headers */
#define iGS(x) ((x >> SOF_GLB_TYPE_SHIFT) & 0xf)
#define iCS(x) ((x >> SOF_CMD_TYPE_SHIFT) & 0xfff)

/* find client from client ID */
static struct sof_vbe_client *vbe_client_find(struct snd_sof_dev *sdev,
					      int client_id)
{
	struct sof_vbe_client *client;
	struct sof_vbe *vbe;

	list_for_each_entry(vbe, &sdev->vbe_list, list) {
		list_for_each_entry(client, &vbe->client_list, list) {
			if (client_id == client->vhm_client_id)
				return client;
		}
	}

	return NULL;
}

static int sof_virtio_send_ipc(struct snd_sof_dev *sdev, void *ipc_data,
			       void *reply_data, size_t count,
			       size_t reply_size)
{
	struct snd_sof_ipc *ipc = sdev->ipc;
	struct sof_ipc_hdr *hdr = (struct sof_ipc_hdr *)ipc_data;

	return sof_ipc_tx_message(ipc, hdr->cmd, ipc_data, count,
				  reply_data, reply_size);
}

/* To be implemented later */
static void sbe_ipc_fe_not_reply_get(struct sof_vbe *vbe, int vq_idx)
{}

/* validate component IPC */
static int sbe_ipc_comp(struct snd_sof_dev *sdev, int vm_id,
			struct sof_ipc_hdr *hdr)
{
	/*TODO validate host comp id range based on vm_id */

	/* Nothing to be done */
	return 0;
}

/* TODO: to be implement later */
static int sbe_ipc_stream(struct snd_sof_dev *sdev, int vm_id,
			  struct sof_ipc_hdr *hdr)
{
	return 0;
}

#define COMP_ID_UNASSIGNED		0xffffffff /* TODO: move to header */
static int sbe_ipc_tplg_comp_new(struct snd_sof_dev *sdev, int vm_id,
				 struct sof_ipc_hdr *hdr)
{
	struct snd_sof_pcm *spcm;
	struct sof_ipc_comp *comp = (struct sof_ipc_comp *)hdr;
	struct sof_ipc_comp_host *host;

	switch (comp->type) {
	case SOF_COMP_HOST:
		/*
		 * TODO: below is a temporary solution. next step is
		 * to create a whole pcm staff incluing substream
		 * based on Liam's suggestion.
		 */
		/*
		 * let's create spcm in HOST ipc
		 * spcm should be created in pcm load, but there is no such ipc
		 * so let create it here
		 */
		host = (struct sof_ipc_comp_host *)hdr;
		spcm = kzalloc(sizeof(*spcm), GFP_KERNEL);
		if (!spcm)
			return -ENOMEM;
		spcm->sdev = sdev;
		spcm->stream[SNDRV_PCM_STREAM_PLAYBACK].comp_id =
			COMP_ID_UNASSIGNED;
		spcm->stream[SNDRV_PCM_STREAM_CAPTURE].comp_id =
			COMP_ID_UNASSIGNED;
		mutex_init(&spcm->mutex);
		spcm->stream[host->direction].comp_id = host->comp.id;
		list_add(&spcm->list, &sdev->pcm_list);
		break;
	default:
		break;
	}
	return 0;
}

/* validate topology IPC */
static int sbe_ipc_tplg(struct snd_sof_dev *sdev, int vm_id,
			struct sof_ipc_hdr *hdr)
{
	/* TODO validate host comp id range based on vm_id */

	/* TODO adding new PCM ? then alloc snd_sg_page table for it */
	u32 cmd;
	int ret = 0;

	cmd = (hdr->cmd & SOF_CMD_TYPE_MASK) >> SOF_CMD_TYPE_SHIFT;

	switch (cmd) {
	case iCS(SOF_IPC_TPLG_COMP_NEW):
		ret = sbe_ipc_tplg_comp_new(sdev, vm_id, hdr);
		break;
	default:
		break;
	}

	return ret;
}

/* TODO: to be implement later */
static int sbe_ipc_post(struct snd_sof_dev *sdev,
			void *ipc_buf, void *reply_buf)
{
	return 0;
}

/*
 * TODO: The guest base ID is passed to guest at boot.
 * TODO rename function name, not submit but consume
 * TODO add topology ipc support and manage the multiple pcm and vms
 */
static int sbe_ipc_fwd(struct snd_sof_dev *sdev, int vm_id,
		       void *ipc_buf, void *reply_buf,
		       size_t count, size_t reply_sz)
{
	struct sof_ipc_hdr *hdr;
	u32 type;
	int ret = 0;

	/* validate IPC */
	if (!count) {
		dev_err(sdev->dev, "error: guest IPC size is 0\n");
		return -EINVAL;
	}

	hdr = (struct sof_ipc_hdr *)ipc_buf;
	type = (hdr->cmd & SOF_GLB_TYPE_MASK) >> SOF_GLB_TYPE_SHIFT;

	/* validate the ipc */
	switch (type) {
	case iGS(SOF_IPC_GLB_COMP_MSG):
		ret = sbe_ipc_comp(sdev, vm_id, hdr);
		if (ret < 0)
			return ret;
		break;
	case iGS(SOF_IPC_GLB_STREAM_MSG):
		ret = sbe_ipc_stream(sdev, vm_id, hdr);
		if (ret < 0)
			return ret;
		break;
	case iGS(SOF_IPC_GLB_DAI_MSG):
		/* DAI should be priviledged for SOS only */
		/*
		 * After we use the new topology solution for FE,
		 * we will not touch DAI anymore.
		 */
		/* return 0; */
		break;
	case iGS(SOF_IPC_GLB_TPLG_MSG):
		ret = sbe_ipc_tplg(sdev, vm_id, hdr);
		if (ret < 0)
			return ret;
		break;
	case iGS(SOF_IPC_GLB_TRACE_MSG):
		/* Trace should be initialized in SOS, skip FE requirement */
		/* setup the error reply */
		return 0;
	default:
		dev_info(sdev->dev, "unhandled IPC 0x%x!\n", type);
		break;
	}

	/* now send the IPC */
	ret = sof_virtio_send_ipc(sdev, ipc_buf, reply_buf, count, reply_sz);
	if (ret < 0) {
		dev_err(sdev->dev, "err: failed to send virtio IPC %d\n", ret);
		return ret;
	}

	/* For some IPCs, the reply needs to be handled */
	ret = sbe_ipc_post(sdev, ipc_buf, reply_buf);

	return ret;
}

/* IPC commands coming from FE to BE */
static void sbe_ipc_fe_cmd_get(struct sof_vbe *vbe, int vq_idx)
{
	struct virtio_vq_info *vq = &vbe->vqs[vq_idx];
	struct device *dev = vbe->sdev->dev;
	struct iovec iov[2];
	u16 idx;
	void *ipc_buf;
	void *reply_buf;
	size_t len1, len2;
	int vm_id;
	int ret, i;

	vm_id = vbe->vmid;
	memset(iov, 0, sizeof(iov));

	/* while there are mesages in virtio queue */
	while (virtio_vq_has_descs(vq)) {
		/* FE uses items, first is command second is reply data */
		ret = virtio_vq_getchain(vq, &idx, iov, 2, NULL);
		if (ret < 2) {
			/* something wrong in vq, no item is fetched */
			if (ret < 0) {
				/*
				 * This should never happen.
				 * FE should be aware this situation already
				 */
				virtio_vq_endchains(vq, 1);
				return;
			}

			dev_err(dev, "ipc buf and reply buf not paired\n");
			/* no enough items, let drop this kick */
			for (i = 0; i <= ret; i++) {
				virtio_vq_relchain(vq, idx + i,
						   iov[i].iov_len);
			}
			virtio_vq_endchains(vq, 1);
			return;
		}

		len1 = iov[SOF_VIRTIO_IPC_MSG].iov_len;
		len2 = iov[SOF_VIRTIO_IPC_REPLY].iov_len;
		if (!len1 || !len2) {
			if (len1)
				virtio_vq_relchain(vq, idx, len1);
			if (len2)
				virtio_vq_relchain(vq, idx + 1, len2);
		} else {
			ipc_buf = iov[SOF_VIRTIO_IPC_MSG].iov_base;
			reply_buf = iov[SOF_VIRTIO_IPC_REPLY].iov_base;

			/* send IPC to HW */
			ret = sbe_ipc_fwd(vbe->sdev, vm_id, ipc_buf, reply_buf,
					  len1, len2);
			if (ret < 0)
				dev_err(dev, "submit guest ipc command fail\n");

			virtio_vq_relchain(vq, idx, len1);
			virtio_vq_relchain(vq, idx + 1, len2);

			/*
			 * TODO now send the IPC reply from DSP to FE on
			 * SOF_VIRTIO_IPC_CMD_RX_VQ
			 * Currently, we doesn't use RX as the reply can
			 * share the same memory of TX
			 */
		}
	}

	/* BE has finished the operations, now let's kick back */
	virtio_vq_endchains(vq, 1);
}

static void handle_vq_kick(struct sof_vbe *vbe, int vq_idx)
{
	dev_dbg(vbe->sdev->dev, "vq_idx %d\n", vq_idx);

	switch (vq_idx) {
	case SOF_VIRTIO_IPC_CMD_TX_VQ:
		/* IPC command from FE to DSP */
		return sbe_ipc_fe_cmd_get(vbe, vq_idx);
	case SOF_VIRTIO_IPC_CMD_RX_VQ:
		/* IPC command reply from DSP to FE - NOT kick */
		break;
	case SOF_VIRTIO_IPC_NOT_TX_VQ:
		/* IPC notification reply from FE to DSP */
		return sbe_ipc_fe_not_reply_get(vbe, vq_idx);
	case SOF_VIRTIO_IPC_NOT_RX_VQ:
		/* IPC notification from DSP to FE - NOT kick */
		break;
	default:
		dev_err(vbe->sdev->dev, "idx %d is invalid\n", vq_idx);
		break;
	}
}

static int handle_kick(int client_id, unsigned long *ioreqs_map)
{
	struct vhm_request *req;
	struct sof_vbe_client *client;
	struct sof_vbe *vbe;
	struct snd_sof_dev *sdev = get_sof_dev();
	int i, handle;

	if (!sdev) {
		dev_err(sdev->dev, "error: no BE registered for SOF!\n");
		return -EINVAL;
	}

	dev_dbg(sdev->dev, "virtio audio kick handling!\n");

	/* get the client this notification is for/from? */
	client = vbe_client_find(sdev, client_id);
	if (!client) {
		dev_err(sdev->dev, "Ooops! client %d not found!\n", client_id);
		return -EINVAL;
	}
	vbe = client->vbe;

	/* go through all vcpu for the valid request buffer */
	for (i = 0; i < client->max_vcpu; i++) {
		req = &client->req_buf[i];
		handle = 0;

		/* is request valid and for this client */
		if (!req->valid)
			continue;
		if (req->client != client->vhm_client_id)
			continue;

		/* ignore if not processing state */
		if (req->processed != REQ_STATE_PROCESSING)
			continue;

		dev_dbg(sdev->dev,
			"ioreq type %d, direction %d, addr 0x%llx, size 0x%llx, value 0x%x\n",
			 req->type,
			 req->reqs.pio_request.direction,
			 req->reqs.pio_request.address,
			 req->reqs.pio_request.size,
			 req->reqs.pio_request.value);

		if (req->reqs.pio_request.direction == REQUEST_READ) {
			/*
			 * currently we handle kick only,
			 * so read will return 0
			 */
			req->reqs.pio_request.value = 0;
		} else {
			req->reqs.pio_request.value >= 0 ?
				(handle = 1) : (handle = 0);
		}

		req->processed = REQ_STATE_SUCCESS;

		// TODO comment what this does.
		acrn_ioreq_complete_request(client->vhm_client_id, i);

		/* handle VQ kick if needed */
		if (handle)
			handle_vq_kick(vbe, req->reqs.pio_request.value);
	}

	return 0;
}

/*
 * register vhm client with virtio.
 * vhm use the client to handle the io access from FE
 */
int sof_vbe_register_client(struct sof_vbe *vbe)
{
	struct virtio_dev_info *dev_info = &vbe->dev_info;
	struct snd_sof_dev *sdev = vbe->sdev;
	struct vm_info info;
	struct sof_vbe_client *client;
	unsigned int vmid;
	int ret;

	/*
	 * vbs core has mechanism to manage the client
	 * there is no need to handle this in the special BE driver
	 * let's use the vbs core client management later
	 */
	client = devm_kzalloc(sdev->dev, sizeof(*client), GFP_KERNEL);
	if (!client)
		return -EINVAL;
	client->vbe = vbe;

	vmid = dev_info->_ctx.vmid;
	client->vhm_client_id = acrn_ioreq_create_client(vmid, handle_kick,
							 "sof_vbe kick init\n");
	if (client->vhm_client_id < 0) {
		dev_err(sdev->dev, "failed to create client of acrn ioreq!\n");
		return client->vhm_client_id;
	}

	ret = acrn_ioreq_add_iorange(client->vhm_client_id, REQ_PORTIO,
				     dev_info->io_range_start,
				     dev_info->io_range_start +
				     dev_info->io_range_len - 1);
	if (ret < 0) {
		dev_err(sdev->dev, "failed to add iorange to acrn ioreq!\n");
		goto err;
	}

	/*
	 * setup the vm information, such as max_vcpu and max_gfn
	 * BE need this information to handle the vqs
	 */
	ret = vhm_get_vm_info(vmid, &info);
	if (ret < 0) {
		dev_err(sdev->dev, "failed in vhm_get_vm_info!\n");
		goto err;
	}
	client->max_vcpu = info.max_vcpu;

	/* TODO: comment what this is doing */
	client->req_buf = acrn_ioreq_get_reqbuf(client->vhm_client_id);
	if (!client->req_buf) {
		dev_err(sdev->dev, "failed in acrn_ioreq_get_reqbuf!\n");
		goto err;
	}

	/* just attach once as vhm will kick kthread */
	acrn_ioreq_attach_client(client->vhm_client_id, 0);

	/* complete client init and add to list */
	list_add(&client->list, &vbe->client_list);

	return 0;
err:
	acrn_ioreq_destroy_client(client->vhm_client_id);
	return -EINVAL;
}

/* register SOF audio BE with virtio/acrn */
int sof_vbe_register(struct snd_sof_dev *sdev, struct sof_vbe **svbe)
{
	struct sof_vbe *vbe;
	struct virtio_vq_info *vqs;
	int i;

	vbe = devm_kzalloc(sdev->dev, sizeof(*vbe), GFP_KERNEL);
	if (!vbe)
		return -ENOMEM;

	INIT_LIST_HEAD(&vbe->client_list);
	INIT_LIST_HEAD(&vbe->posn_list);
	spin_lock_init(&vbe->posn_lock);
	vbe->sdev = sdev;

	/*
	 * We currently only support one VM. The comp_id range will be
	 * dynamically assigned when multiple VMs are supported.
	 */
	vbe->comp_id_begin = SOF_COMP_NUM_MAX;
	vbe->comp_id_end = vbe->comp_id_begin + SOF_COMP_NUM_MAX;

	vqs = vbe->vqs;
	for (i = 0; i < SOF_VIRTIO_NUM_OF_VQS; i++) {
		vqs[i].dev = &vbe->dev_info;
		/*
		 * currently relies on VHM to kick us,
		 * thus vq_notify not used
		 */
		vqs[i].vq_notify = NULL;
	}

	/* link dev and vqs */
	vbe->dev_info.vqs = vqs;

	virtio_dev_init(&vbe->dev_info, vqs, SOF_VIRTIO_NUM_OF_VQS);

	*svbe = vbe;

	return 0;
}
