// SPDX-License-Identifier: GPL-2.0-only
//
// aw88399_hda_i2c.c -- AW88399 HDA I2C side codec driver
//
// Based on cs35l41_hda_i2c.c
//

#include <linux/i2c.h>
#include <linux/acpi.h>
#include <linux/module.h>
#include <sound/hda_codec.h>
#include "aw88399_hda.h"

static int aw88399_hda_i2c_probe(struct i2c_client *clt)
{
	const char *device_name;

	/* Match ACPI, serial-multi-instantiate, and manual devices */
	if (strstr(dev_name(&clt->dev), "AWDZ8399"))
		device_name = "AWDZ8399";
	else if (!strcmp(clt->name, "aw88399-hda") || !strcmp(clt->name, "aw88399"))
		device_name = "aw88399-hda";  /* manual or SMI instantiation */
	else
		return -ENODEV;

	return aw88399_hda_probe(&clt->dev, device_name, clt->addr, clt->irq);
}

static void aw88399_hda_i2c_remove(struct i2c_client *clt)
{
	aw88399_hda_remove(&clt->dev);
}

static const struct i2c_device_id aw88399_hda_i2c_id[] = {
	{ "aw88399-hda", 0 },
	{ "aw88399", 0 },
	{}
};
MODULE_DEVICE_TABLE(i2c, aw88399_hda_i2c_id);

static const struct acpi_device_id aw88399_acpi_hda_match[] = {
	{ "AWDZ8399", 0 },
	{}
};
MODULE_DEVICE_TABLE(acpi, aw88399_acpi_hda_match);

static struct i2c_driver aw88399_hda_i2c_driver = {
	.driver = {
		.name		= "aw88399-hda",
		.acpi_match_table = aw88399_acpi_hda_match,
		.pm		= &aw88399_hda_pm_ops,
	},
	.probe		= aw88399_hda_i2c_probe,
	.remove		= aw88399_hda_i2c_remove,
	.id_table	= aw88399_hda_i2c_id,
};
module_i2c_driver(aw88399_hda_i2c_driver);

MODULE_DESCRIPTION("HDA AW88399 I2C driver");
MODULE_AUTHOR("Lyapsus");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("SND_HDA_SCODEC_AW88399");
