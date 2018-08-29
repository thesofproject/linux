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

/* To be implemented later */
static void sbe_ipc_fe_not_reply_get(struct sof_vbe *vbe, int vq_idx)
{}

/* To be implemented later */
static void sbe_ipc_fe_cmd_get(struct sof_vbe *vbe, int vq_idx)
{}

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
