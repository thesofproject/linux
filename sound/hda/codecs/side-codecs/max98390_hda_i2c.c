// SPDX-License-Identifier: GPL-2.0
//
// MAX98390 HDA I2C driver
//

#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/i2c.h>

#include "max98390_hda.h"
#include "../../../soc/codecs/max98390.h"

static int max98390_hda_i2c_probe(struct i2c_client *clt)
{
	const char *device_name;
	const char *name_suffix;
	int index = 0;

	/* Match device name from serial-multi-instantiate */
	if (strstr(dev_name(&clt->dev), "MAX98390") ||
	    strstr(dev_name(&clt->dev), "max98390"))
		device_name = "MAX98390";
	else
		return -ENODEV;

	/* Parse index from device name (e.g., "i2c-MAX98390:00-max98390-hda.0" -> index 0) */
	name_suffix = strrchr(dev_name(&clt->dev), '.');
	if (name_suffix) {
		if (kstrtoint(name_suffix + 1, 10, &index) < 0) {
			dev_err(&clt->dev, "Failed to parse device index from name: %s\n",
				dev_name(&clt->dev));
			return -EINVAL;
		}
	}

	return max98390_hda_probe(&clt->dev, device_name, index, clt->irq,
				  devm_regmap_init_i2c(clt, &max98390_regmap),
				  MAX98390_HDA_I2C, clt->addr);
}

static void max98390_hda_i2c_remove(struct i2c_client *clt)
{
	max98390_hda_remove(&clt->dev);
}

static const struct i2c_device_id max98390_hda_i2c_id[] = {
	{ "max98390-hda", 0 },
	{}
};
MODULE_DEVICE_TABLE(i2c, max98390_hda_i2c_id);

static const struct acpi_device_id max98390_acpi_hda_match[] = {
	{ "MAX98390", 0 },
	{ "MX98390", 0 },
	{}
};
MODULE_DEVICE_TABLE(acpi, max98390_acpi_hda_match);

static struct i2c_driver max98390_hda_i2c_driver = {
	.driver = {
		.name		= "max98390-hda",
		.acpi_match_table = max98390_acpi_hda_match,
		.pm		= &max98390_hda_pm_ops,
	},
	.id_table	= max98390_hda_i2c_id,
	.probe		= max98390_hda_i2c_probe,
	.remove		= max98390_hda_i2c_remove,
};
module_i2c_driver(max98390_hda_i2c_driver);

MODULE_DESCRIPTION("HDA MAX98390 driver");
MODULE_IMPORT_NS("SND_HDA_SCODEC_MAX98390");
MODULE_AUTHOR("Kevin Cuperus <cuperus.kevin@hotmail.com>");
MODULE_LICENSE("GPL");
