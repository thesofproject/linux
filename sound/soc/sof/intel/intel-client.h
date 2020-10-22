/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause) */
/*
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * Copyright(c) 2020 Intel Corporation. All rights reserved.
 *
 * Author: Ranjani Sridharan <ranjani.sridharan@linux.intel.com>
 */

#ifndef __INTEL_CLIENT_H
#define __INTEL_CLIENT_H

#if IS_ENABLED(CONFIG_SND_SOC_SOF_DEBUG_IPC_FLOOD_TEST_CLIENT)
int intel_register_ipc_test_clients(struct snd_sof_dev *sdev);
void intel_unregister_ipc_test_clients(struct snd_sof_dev *sdev);
#else
static inline int intel_register_ipc_test_clients(struct snd_sof_dev *sdev)
{
	return 0;
}

static void intel_unregister_ipc_test_clients(struct snd_sof_dev *sdev) {}
#endif

#endif
