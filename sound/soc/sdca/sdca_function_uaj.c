// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
// Copyright(c) 2024 Intel Corporation.

/*
 * Soundwire SDCA UAJ Function Driver
 */

#include <linux/acpi.h>
#include <linux/module.h>
#include <linux/auxiliary_bus.h>
#include <sound/sdca_function.h>
#include "sdca_function_device.h"

/*
 * probe and init (aux_dev_id argument is required by function prototype but not used)
 */
static int sdca_uaj_probe(struct auxiliary_device *auxdev,
			  const struct auxiliary_device_id *aux_dev_id)
{
	struct sdca_dev *sdev = auxiliary_dev_to_sdca_dev(auxdev);
	struct device *dev = &auxdev->dev;
	int ret;

	ret =  sdca_parse_function(dev,
				   auxdev->dev.fwnode,
				   &sdev->function);
	if (ret < 0)
		dev_err(dev, "%s: %pfwP: probe failed: %d\n",
			__func__, auxdev->dev.fwnode, ret);

	return 0;
}

static const struct auxiliary_device_id sdca_uaj_id_table[] = {
	{ .name = "snd_soc_sdca." SDCA_FUNCTION_TYPE_UAJ_NAME },
	{},
};
MODULE_DEVICE_TABLE(auxiliary, sdca_uaj_id_table);

static struct auxiliary_driver sdca_uaj_drv = {
	.probe = sdca_uaj_probe,
	.id_table = sdca_uaj_id_table
};
module_auxiliary_driver(sdca_uaj_drv);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("SDCA UAJ driver");
MODULE_IMPORT_NS(SND_SOC_SDCA);
