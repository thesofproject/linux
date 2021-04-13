// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
//
// This file is provided under a dual BSD/GPLv2 license.  When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2021 Intel Corporation. All rights reserved.
// Author: Peter Ujfalusi <peter.ujfalusi@linux.intel.com>
//

#include <linux/auxiliary_bus.h>
#include <linux/module.h>
#include "../sof-client-probes.h"
#include "../sof-priv.h"
#include "hda.h"

static const struct auxiliary_device_id intel_probes_client_id_table[] = {
#if IS_ENABLED(CONFIG_SND_SOC_SOF_HDA_PROBES)
	{ .name = "snd_sof.hda-probes", .driver_data = (kernel_ulong_t)&hda_probe_ops, },
#endif
	{},
};
MODULE_DEVICE_TABLE(auxiliary, intel_probes_client_id_table);

/* driver name will be set based on KBUILD_MODNAME */
static struct auxiliary_driver intel_probes_client_drv = {
	.probe = sof_probes_client_probe,
	.remove = sof_probes_client_remove,

	.id_table = intel_probes_client_id_table,
};

module_auxiliary_driver(intel_probes_client_drv);

MODULE_DESCRIPTION("SOF Client driver for Probes on Intel platform");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(SND_SOC_SOF_DEBUG_PROBES);
MODULE_IMPORT_NS(SND_SOC_SOF_INTEL_HDA_COMMON);
