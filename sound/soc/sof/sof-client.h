/* SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause) */
/*
 * Copyright(c) 2019 Intel Corporation. All rights reserved.
 *
 * Author: Ranjani Sridharan <ranjani.sridharan@linux.intel.com>
 */
#include <linux/platform_device.h>
#include "sof-priv.h"

#ifndef __SOUND_SOC_SOF_CLIENT_H
#define __SOUND_SOC_SOF_CLIENT_H

/* client register/unregister */
struct snd_sof_client *sof_client_dev_register(struct snd_sof_dev *sdev,
					       const char *name);
void sof_client_dev_unregister(struct snd_sof_client *client);

#endif
