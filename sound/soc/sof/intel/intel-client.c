// SPDX-License-Identifier: GPL-2.0-only
//
// Copyright(c) 2020 Intel Corporation. All rights reserved.
//
// Author: Ranjani Sridharan <ranjani.sridharan@linux.intel.com>
//

#include <linux/module.h>
#include "../sof-priv.h"
#include "../sof-client.h"
#include "intel-client.h"

#if IS_ENABLED(CONFIG_SND_SOC_SOF_DEBUG_IPC_FLOOD_TEST_CLIENT)
int intel_register_ipc_test_clients(struct snd_sof_dev *sdev)
{
	int ret;

	/*
	 * Register 2 IPC clients to facilitate tandem flood test. The device name below is
	 * appended with the device ID assigned automatically when the auxiliary device is
	 * registered making them unique.
	 */
	ret = sof_client_dev_register(sdev, "ipc_test", 0);
	if (ret < 0)
		return ret;

	return sof_client_dev_register(sdev, "ipc_test", 1);
}
EXPORT_SYMBOL_NS_GPL(intel_register_ipc_test_clients, SND_SOC_SOF_INTEL_CLIENT);

void intel_unregister_ipc_test_clients(struct snd_sof_dev *sdev)
{
	sof_client_dev_unregister(sdev, "ipc_test", 0);
	sof_client_dev_unregister(sdev, "ipc_test", 1);
}
EXPORT_SYMBOL_NS_GPL(intel_unregister_ipc_test_clients, SND_SOC_SOF_INTEL_CLIENT);
#endif

MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(SND_SOC_SOF_CLIENT);
