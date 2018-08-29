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

/* To be implemented later */
static int handle_kick(int client_id, unsigned long *ioreqs_map)
{
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
