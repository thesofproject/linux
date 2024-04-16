// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
// Copyright(c) 2024 Intel Corporation

/*
 * The MIPI SDCA specification is available for public downloads at
 * https://www.mipi.org/mipi-sdca-v1-0-download
 */

#include <linux/acpi.h>
#include <linux/soundwire/sdw.h>
#include <sound/sdca_function.h>
#include <sound/sdca.h>

int sdca_alloc(void **context,
	       struct device *parent,
	       struct sdw_slave *slave)
{
	struct sdca_data *sdca;

	sdca = devm_kzalloc(parent, sizeof(*sdca), GFP_KERNEL);
	if (!sdca)
		return -ENOMEM;

	sdca->parent = parent;
	sdca->slave = slave;

	*context = sdca;

	return 0;
}
EXPORT_SYMBOL_NS(sdca_alloc, SND_SOC_SDCA);

MODULE_LICENSE("Dual BSD/GPL");

