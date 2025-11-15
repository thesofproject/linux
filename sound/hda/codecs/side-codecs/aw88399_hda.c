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

/* Import register definitions and init function from ASoC driver */
#include "../../soc/codecs/aw88399.h"

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
	struct aw88399 *core = aw88399->core;
	int ret = 0;

	dev_dbg(dev, "Playback action: %d\n", action);

	switch (action) {
	case HDA_GEN_PCM_ACT_OPEN:
		pm_runtime_get_sync(dev);
		aw88399->playing = true;
		break;
	case HDA_GEN_PCM_ACT_PREPARE:
		/* Start amplifier */
		if (core)
			aw88399_start(core, AW88399_SYNC_START);
		break;
	case HDA_GEN_PCM_ACT_CLEANUP:
		/* Stop amplifier */
		if (aw88399->aw_dev)
			ret = aw88399_stop(aw88399->aw_dev);
		if (ret)
			dev_err(dev, "Failed to stop amplifier: %d\n", ret);
		break;
	case HDA_GEN_PCM_ACT_CLOSE:
		if (aw88399->aw_dev)
			aw88399_stop(aw88399->aw_dev);
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

	if (comp->dev)
		return -EBUSY;

	comp->dev = dev;
	aw88399->codec = parent->codec;

	strscpy(comp->name, dev_name(dev), sizeof(comp->name));

	/* Set up playback hooks */
	comp->playback_hook = aw88399_hda_playback_hook;

	dev_info(dev, "Bound to HDA codec, channel %d\n", aw88399->channel);

	return 0;
}

static void aw88399_hda_unbind(struct device *dev, struct device *master, void *master_data)
{
	struct aw88399_hda *aw88399 = dev_get_drvdata(dev);
	struct hda_component_parent *parent = master_data;
	struct hda_component *comp;

	comp = hda_component_from_index(parent, aw88399->index);
	if (comp && (comp->dev == dev))
		memset(comp, 0, sizeof(*comp));

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
	struct aw88399 *core;
	int ret;

	/* Hardware reset */
	aw88399_hda_hw_reset(aw88399);

	core = devm_kzalloc(dev, sizeof(*core), GFP_KERNEL);
	if (!core)
		return -ENOMEM;

	mutex_init(&core->lock);
	core->reset_gpio = aw88399->reset_gpio;
	core->regmap = aw88399->regmap;

	ret = aw88399_init(core, i2c, aw88399->regmap);
	if (ret)
		return ret;

	ret = aw88399_request_firmware_file(core);
	if (ret)
		return ret;

	aw88399->core = core;
	aw88399->aw_dev = core->aw_pa;

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
		const char *name = dev_name(dev);
		char *suffix;

		/*
		 * For serial-multi-instantiate devices, derive index from device name suffix.
		 * Device names: "i2c-AWDZ8399:00-aw88399-hda.0", "i2c-AWDZ8399:00-aw88399-hda.1"
		 * Component match expects index to match suffix number.
		 */
		suffix = strrchr(name, '.');
		if (suffix && *(suffix + 1) >= '0' && *(suffix + 1) <= '9') {
			aw88399->index = *(suffix + 1) - '0';
			aw88399->channel = aw88399->index;
			dev_info(dev, "Derived index %d from device name\n", aw88399->index);
			return 0;
		}

		/* Fallback for manual sysfs devices: use I2C address */
		struct i2c_client *i2c = to_i2c_client(dev);

		aw88399->index = i2c->addr - 0x34;
		aw88399->channel = aw88399->index;
		dev_warn(dev, "No ACPI companion, using address-based index %d\n",
			 aw88399->index);
		return 0;
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

	aw88399->channel = aw88399->index;

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

	if (aw88399->aw_dev)
		aw88399_stop(aw88399->aw_dev);

	component_del(dev, &aw88399_hda_comp_ops);

	dev_info(dev, "AW88399 HDA side codec removed\n");
}
EXPORT_SYMBOL_NS_GPL(aw88399_hda_remove, "SND_HDA_SCODEC_AW88399");

static int aw88399_hda_runtime_suspend(struct device *dev)
{
	struct aw88399_hda *aw88399 = dev_get_drvdata(dev);

	dev_dbg(dev, "Runtime suspend\n");

	if (aw88399->aw_dev && aw88399->playing)
		aw88399_stop(aw88399->aw_dev);

	aw88399->suspended = true;

	return 0;
}

static int aw88399_hda_runtime_resume(struct device *dev)
{
	struct aw88399_hda *aw88399 = dev_get_drvdata(dev);

	dev_dbg(dev, "Runtime resume\n");

	aw88399->suspended = false;

	if (aw88399->core && aw88399->aw_dev && aw88399->playing)
		aw88399_start(aw88399->core, AW88399_SYNC_START);

	return 0;
}

static int aw88399_hda_system_suspend(struct device *dev)
{
	struct aw88399_hda *aw88399 = dev_get_drvdata(dev);

	dev_dbg(dev, "System suspend\n");

	/* Stop amplifier before system sleep */
	if (aw88399->aw_dev && aw88399->playing)
		aw88399_stop(aw88399->aw_dev);

	return 0;
}

static int aw88399_hda_system_resume(struct device *dev)
{
	struct aw88399_hda *aw88399 = dev_get_drvdata(dev);

	dev_dbg(dev, "System resume\n");

	/* Reset chip after system sleep */
	if (aw88399->aw_dev) {
		aw88399_hda_hw_reset(aw88399);
		/* Chip will be fully reinitialized on next playback */
	}

	return 0;
}

const struct dev_pm_ops aw88399_hda_pm_ops = {
	RUNTIME_PM_OPS(aw88399_hda_runtime_suspend, aw88399_hda_runtime_resume, NULL)
	SYSTEM_SLEEP_PM_OPS(aw88399_hda_system_suspend, aw88399_hda_system_resume)
};
EXPORT_SYMBOL_NS_GPL(aw88399_hda_pm_ops, "SND_HDA_SCODEC_AW88399");

MODULE_DESCRIPTION("HDA AW88399 driver");
MODULE_AUTHOR("Lyapsus");
MODULE_LICENSE("GPL");
