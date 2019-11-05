// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
//
// This file is provided under a dual BSD/GPLv2 license.  When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2019 Intel Corporation. All rights reserved.
//
// Author: Ranjani Sridharan <ranjani.sridharan@linux.intel.com>
//

#include "sof-client.h"
#include "sof-priv.h"

void *sof_get_client_data(struct device *dev)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(dev);

	return sdev->sof_audio->client_data;
}
EXPORT_SYMBOL(sof_get_client_data);
