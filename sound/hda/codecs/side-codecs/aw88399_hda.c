// SPDX-License-Identifier: GPL-2.0-only
//
// aw88399_hda.c -- AW88399 HDA side codec driver
//
// Based on cs35l41_hda.c and aw88399.c
//

#include <linux/acpi.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <sound/hda_codec.h>
#include <sound/soc.h>
#include "hda_component.h"
#include "../generic.h"
#include "aw88399_hda.h"

/* Import register definitions from ASoC driver */
#include "../../soc/codecs/aw88399.h"
#include "../../soc/codecs/aw88395/aw88395_device.h"

static const struct regmap_config aw88399_hda_regmap_i2c = {
	.reg_bits = 8,
	.val_bits = 16,
	.max_register = AW88399_REG_MAX,
	.reg_format_endian = REGMAP_ENDIAN_LITTLE,
	.val_format_endian = REGMAP_ENDIAN_BIG,
};

static void aw88399_hda_playback_hook(struct device *dev, int action)
{
	struct aw88399_hda *aw88399 = dev_get_drvdata(dev);
	struct aw_device *aw_dev = aw88399->aw_dev;
	int ret;

	dev_dbg(dev, "Playback action: %d\n", action);

	switch (action) {
	case HDA_GEN_PCM_ACT_OPEN:
		pm_runtime_get_sync(dev);
		aw88399->playing = true;
		break;
	case HDA_GEN_PCM_ACT_PREPARE:
		/* Start amplifier */
		ret = aw88395_dev_start(aw_dev);
		if (ret)
			dev_err(dev, "Failed to start amplifier: %d\n", ret);
		break;
	case HDA_GEN_PCM_ACT_CLEANUP:
		/* Stop amplifier */
		ret = aw88395_dev_stop(aw_dev);
		if (ret)
			dev_err(dev, "Failed to stop amplifier: %d\n", ret);
		break;
	case HDA_GEN_PCM_ACT_CLOSE:
		aw88399->playing = false;
		pm_runtime_mark_last_busy(dev);
		pm_runtime_put_autosuspend(dev);
		break;
	default:
		dev_warn(dev, "Unsupported action: %d\n", action);
		break;
	}
}

static int aw88399_hda_bind(struct device *dev, struct device *master, void *master_data)
{
	struct aw88399_hda *aw88399 = dev_get_drvdata(dev);
	struct hda_component_parent *parent = master_data;
	struct hda_component *comp;

	comp = hda_component_from_index(parent, aw88399->index);
	if (!comp)
		return -EINVAL;

	comp->dev = dev;
	aw88399->codec = parent->codec;

	/* Set up playback hooks */
	comp->playback_hook = aw88399_hda_playback_hook;

	dev_info(dev, "Bound to HDA codec, channel %d\n", aw88399->channel);

	return 0;
}

static void aw88399_hda_unbind(struct device *dev, struct device *master, void *master_data)
{
	struct aw88399_hda *aw88399 = dev_get_drvdata(dev);

	aw88399->codec = NULL;
	dev_info(dev, "Unbound from HDA codec\n");
}

static const struct component_ops aw88399_hda_comp_ops = {
	.bind = aw88399_hda_bind,
	.unbind = aw88399_hda_unbind,
};

static void aw88399_hda_hw_reset(struct aw88399_hda *aw88399)
{
	if (!aw88399->reset_gpio)
		return;

	gpiod_set_value_cansleep(aw88399->reset_gpio, 0);
	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(aw88399->reset_gpio, 1);
	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(aw88399->reset_gpio, 0);
	usleep_range(3000, 4000);
}

static int aw88399_hda_init(struct aw88399_hda *aw88399)
{
	struct device *dev = aw88399->dev;
	struct i2c_client *i2c = to_i2c_client(dev);
	struct aw_device *aw_dev;
	unsigned int chip_id;
	int ret;

	/* Hardware reset */
	aw88399_hda_hw_reset(aw88399);

	/* Read and verify chip ID */
	ret = regmap_read(aw88399->regmap, AW88399_ID_REG, &chip_id);
	if (ret) {
		dev_err(dev, "Failed to read chip ID: %d\n", ret);
		return ret;
	}

	if (chip_id != AW88399_CHIP_ID) {
		dev_err(dev, "Unsupported chip ID: 0x%04x (expected 0x%04x)\n",
			chip_id, AW88399_CHIP_ID);
		return -ENODEV;
	}

	dev_info(dev, "AW88399 chip ID: 0x%04x\n", chip_id);

	/* Allocate aw_device */
	aw_dev = devm_kzalloc(dev, sizeof(*aw_dev), GFP_KERNEL);
	if (!aw_dev)
		return -ENOMEM;

	aw_dev->i2c = i2c;
	aw_dev->dev = dev;
	aw_dev->regmap = aw88399->regmap;
	aw_dev->chip_id = chip_id;

	/* Derive channel from I2C address: 0x34 = left (0), 0x35 = right (1) */
	aw_dev->channel = i2c->addr - 0x34;
	aw88399->channel = aw_dev->channel;

	dev_info(dev, "Using I2C address-based channel %d (addr 0x%02x)\n",
		 aw_dev->channel, i2c->addr);

	mutex_init(&aw_dev->dsp_lock);

	/* Initialize device using existing aw88395 device layer */
	ret = aw88395_init(&aw_dev, i2c, aw88399->regmap);
	if (ret) {
		dev_err(dev, "Failed to initialize device: %d\n", ret);
		return ret;
	}

	aw88399->aw_dev = aw_dev;

	return 0;
}

static int aw88399_hda_acpi_probe(struct aw88399_hda *aw88399)
{
	struct device *dev = aw88399->dev;
	struct acpi_device *adev;
	u64 uid;
	int ret = 0;

	adev = ACPI_COMPANION(dev);
	if (!adev) {
		dev_err(dev, "No ACPI companion\n");
		return -ENODEV;
	}

	/*
	 * Get component index from ACPI _UID if available.
	 * On Legion, we have only 1 ACPI device (at 0x35), so this will be 0.
	 * The 0x34 device is manually instantiated and won't have ACPI data.
	 */
	ret = acpi_dev_uid_to_integer(adev, &uid);
	if (ret) {
		/*
		 * If no _UID or error, derive index from I2C address.
		 * 0x34 = index 0 (left), 0x35 = index 1 (right)
		 */
		struct i2c_client *i2c = to_i2c_client(dev);
		aw88399->index = i2c->addr - 0x34;
		dev_info(dev, "No ACPI _UID, using address-based index %d\n", aw88399->index);
	} else {
		aw88399->index = (int)uid;
		dev_info(dev, "ACPI _UID: %d\n", aw88399->index);
	}

	return 0;
}

int aw88399_hda_probe(struct device *dev, const char *device_name, int id, int irq)
{
	struct aw88399_hda *aw88399;
	struct i2c_client *i2c;
	int ret;

	aw88399 = devm_kzalloc(dev, sizeof(*aw88399), GFP_KERNEL);
	if (!aw88399)
		return -ENOMEM;

	aw88399->dev = dev;
	dev_set_drvdata(dev, aw88399);

	i2c = to_i2c_client(dev);

	/* Get optional reset GPIO */
	aw88399->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(aw88399->reset_gpio)) {
		ret = PTR_ERR(aw88399->reset_gpio);
		dev_err(dev, "Failed to get reset GPIO: %d\n", ret);
		return ret;
	}

	/* Initialize regmap for I2C */
	aw88399->regmap = devm_regmap_init_i2c(i2c, &aw88399_hda_regmap_i2c);
	if (IS_ERR(aw88399->regmap)) {
		ret = PTR_ERR(aw88399->regmap);
		dev_err(dev, "Failed to init regmap: %d\n", ret);
		return ret;
	}

	/* Parse ACPI data */
	ret = aw88399_hda_acpi_probe(aw88399);
	if (ret < 0) {
		dev_err(dev, "ACPI probe failed: %d\n", ret);
		return ret;
	}

	/* Initialize chip */
	ret = aw88399_hda_init(aw88399);
	if (ret) {
		dev_err(dev, "Chip initialization failed: %d\n", ret);
		return ret;
	}

	/* Enable runtime PM */
	pm_runtime_set_autosuspend_delay(dev, 3000);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_mark_last_busy(dev);
	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);

	/* Register component */
	ret = component_add(dev, &aw88399_hda_comp_ops);
	if (ret) {
		dev_err(dev, "Failed to register component: %d\n", ret);
		pm_runtime_disable(dev);
		return ret;
	}

	dev_info(dev, "AW88399 HDA side codec registered successfully\n");

	return 0;
}
EXPORT_SYMBOL_NS_GPL(aw88399_hda_probe, "SND_HDA_SCODEC_AW88399");

void aw88399_hda_remove(struct device *dev)
{
	struct aw88399_hda *aw88399 = dev_get_drvdata(dev);

	pm_runtime_disable(dev);

	if (aw88399->aw_dev) {
		aw88395_dev_stop(aw88399->aw_dev);
	}

	component_del(dev, &aw88399_hda_comp_ops);

	dev_info(dev, "AW88399 HDA side codec removed\n");
}
EXPORT_SYMBOL_NS_GPL(aw88399_hda_remove, "SND_HDA_SCODEC_AW88399");

static int aw88399_hda_runtime_suspend(struct device *dev)
{
	struct aw88399_hda *aw88399 = dev_get_drvdata(dev);

	dev_dbg(dev, "Runtime suspend\n");

	if (aw88399->aw_dev && aw88399->playing) {
		aw88395_dev_stop(aw88399->aw_dev);
	}

	aw88399->suspended = true;

	return 0;
}

static int aw88399_hda_runtime_resume(struct device *dev)
{
	struct aw88399_hda *aw88399 = dev_get_drvdata(dev);

	dev_dbg(dev, "Runtime resume\n");

	aw88399->suspended = false;

	if (aw88399->aw_dev && aw88399->playing) {
		aw88395_dev_start(aw88399->aw_dev);
	}

	return 0;
}

const struct dev_pm_ops aw88399_hda_pm_ops = {
	RUNTIME_PM_OPS(aw88399_hda_runtime_suspend, aw88399_hda_runtime_resume, NULL)
};
EXPORT_SYMBOL_NS_GPL(aw88399_hda_pm_ops, "SND_HDA_SCODEC_AW88399");

MODULE_DESCRIPTION("HDA AW88399 driver");
MODULE_AUTHOR("Lyapsus");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("SND_SOC_AW88395_LIB");
