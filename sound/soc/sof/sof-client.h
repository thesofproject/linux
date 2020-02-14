/* SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause) */
/*
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * Copyright(c) 2020 Intel Corporation. All rights reserved.
 *
 * Author: Ranjani Sridharan <ranjani.sridharan@linux.intel.com>
 */
#ifndef __SOUND_SOC_SOF_CLIENT_H
#define __SOUND_SOC_SOF_CLIENT_H

#include <sound/sof/stream.h> /* needs to be included before control.h */
#include <sound/sof/control.h>
#include <sound/sof/dai.h>
#include <sound/sof/topology.h>
#include <sound/sof/header.h>
#include <sound/soc.h>
#include <sound/sof.h>

enum sof_client_type {
	SOF_CLIENT_AUDIO,
	SOF_CLIENT_IPC,
};

struct sof_client_ops {
	int (*probe)(struct device *dev); /* mandatory */
	int (*remove)(struct device *dev); /* mandatory */

	/* Optional IPC RX callback */
	void (*sof_client_ipc_rx)(struct device *dev, u32 msg_cmd);

	/*
	 * Optional callback to check if the client's current status allows the
	 * DSP to enter a low-power D0 substate when the system is in S0.
	 */
	bool (*allow_lp_d0_substate_in_s0)(struct device *dev);

	/*
	 * Optional callback to check if the client is requesting to remain in
	 * D0 when the system suspends to S0IX.
	 */
	bool (*request_d0_during_suspend)(struct device *dev);
};

struct sof_client_drv {
	char *name;
	enum sof_client_type type;
	struct sof_client_ops *ops;
	struct list_head list;	/* item in SOF client drv list */
};

struct sof_client_dev {
	struct device *dev;
	struct sof_client_driver *drv;
	int id; /* unique client ID */
	void *client_data;
};

struct sof_client_dev *
sof_client_drv_register(struct sof_client_drv *drv, struct device *dev);
void sof_client_drv_unregister(struct sof_client_drv *drv);

/* IPC TX */
int sof_client_ipc_tx_message(struct sof_client_dev *cdev, u32 header,
			      void *msg_data, size_t msg_bytes,
			      void *reply_data, size_t reply_bytes);

#endif
