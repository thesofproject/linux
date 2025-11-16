// SPDX-License-Identifier: GPL-2.0-only
//
// aw88399_hda.c -- AW88399 HDA side codec driver
//
// Based on cs35l41_hda.c and aw88399.c
//

#include <linux/acpi.h>
#include <linux/dmi.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <sound/hda_codec.h>
#include <sound/soc.h>
#include "hda_component.h"
#include "../generic.h"
#include "aw88399_hda.h"

/* Import register definitions and init function from ASoC driver */
#include "../../soc/codecs/aw88399.h"
#include "../../soc/codecs/aw88395/aw88395_device.h"

#define AW88399_HDA_I2C_BASE_ADDR	0x34
#define AW88399_HDA_MAX_AMPS		2

#define AW88399_ACPI_PROP_DEV_INDEX	"awinic,dev-index"
#define AW88399_ACPI_PROP_SPK_POS	"awinic,speaker-position"
#define AW88399_ACPI_PROP_SPK_ID	"awinic,speaker-id"

static const struct regmap_config aw88399_hda_regmap_i2c = {
	.reg_bits = 8,
	.val_bits = 16,
	.max_register = AW88399_REG_MAX,
	.reg_format_endian = REGMAP_ENDIAN_LITTLE,
	.val_format_endian = REGMAP_ENDIAN_BIG,
};

static void aw88399_hda_acpi_notify(acpi_handle handle, u32 event, struct device *dev);

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
	comp->acpi_notify = aw88399->adev ? aw88399_hda_acpi_notify : NULL;
	comp->adev = aw88399->adev;
	comp->acpi_notifications_supported = aw88399->acpi_notify_supported && aw88399->adev;

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

static void aw88399_hda_acpi_notify(acpi_handle handle, u32 event, struct device *dev)
{
	struct aw88399_hda *aw88399 = dev_get_drvdata(dev);

	dev_dbg(dev, "ACPI notify event 0x%x for channel %d\n", event, aw88399->channel);
}

static int aw88399_hda_index_from_i2c(struct aw88399_hda *aw88399)
{
	struct device *dev = aw88399->dev;
	struct i2c_client *i2c = to_i2c_client(dev);
	int index = i2c->addr - AW88399_HDA_I2C_BASE_ADDR;

	if (index < 0 || index >= AW88399_HDA_MAX_AMPS) {
		dev_warn(dev, "Unexpected I2C address 0x%02x, clamping index\n",
			i2c->addr);
		index = clamp(index, 0, AW88399_HDA_MAX_AMPS - 1);
	}

	return index;
}

static bool aw88399_hda_try_dsd_index(struct aw88399_hda *aw88399, int addr_index)
{
	struct device *dev = aw88399->dev;
	u32 value;

	if (device_property_read_u32(dev, AW88399_ACPI_PROP_DEV_INDEX, &value))
		return false;

	if (value >= AW88399_HDA_MAX_AMPS) {
		dev_warn(dev, "_DSD dev-index %u out of range, ignoring\n", value);
		return false;
	}

	aw88399->index = value;
	dev_info(dev, "Using _DSD dev-index %u (I2C suggested %d)\n", value, addr_index);

	return true;
}

static void aw88399_hda_parse_speaker_props(struct aw88399_hda *aw88399)
{
	struct device *dev = aw88399->dev;
	u32 value;

	aw88399->speaker_pos_valid = false;
	aw88399->speaker_id_valid = false;

	if (!device_property_read_u32(dev, AW88399_ACPI_PROP_SPK_POS, &value)) {
		if (value < AW88399_HDA_MAX_AMPS) {
			aw88399->speaker_pos = value;
			aw88399->speaker_pos_valid = true;
			dev_info(dev, "Speaker position from _DSD: %u\n", value);
		} else {
			dev_warn(dev, "_DSD speaker-position %u out of range\n", value);
		}
	}

	if (!device_property_read_u32(dev, AW88399_ACPI_PROP_SPK_ID, &value)) {
		aw88399->speaker_id = value;
		aw88399->speaker_id_valid = true;
		dev_info(dev, "Speaker ID from _DSD: %u\n", value);
	}
}

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
	if (aw88399->aw_dev) {
		if (aw88399->speaker_pos_valid)
			aw88399->aw_dev->channel = aw88399->speaker_pos;
		else
			aw88399->aw_dev->channel = aw88399->index;
	}

	return 0;
}

static int aw88399_hda_acpi_probe(struct aw88399_hda *aw88399)
{
	struct device *dev = aw88399->dev;
	struct i2c_client *i2c = to_i2c_client(dev);
	struct acpi_device *adev;
	int addr_index;
	u64 uid;
	int ret;
	bool index_from_dsd = false;

	addr_index = aw88399_hda_index_from_i2c(aw88399);
	aw88399->index = addr_index;
	aw88399->adev = NULL;
	aw88399->acpi_notify_supported = false;

	adev = ACPI_COMPANION(dev);
	if (!adev) {
		dev_info(dev, "No ACPI companion, using I2C addr 0x%02x for index %d\n",
			 i2c->addr, aw88399->index);
		goto metadata;
	}

	aw88399->adev = adev;
	aw88399->acpi_notify_supported = true;
	index_from_dsd = aw88399_hda_try_dsd_index(aw88399, addr_index);

	if (!index_from_dsd) {
		ret = acpi_dev_uid_to_integer(adev, &uid);
		if (ret) {
			aw88399->index = addr_index;
			dev_info(dev, "No ACPI _UID, using I2C addr 0x%02x for index %d\n",
				 i2c->addr, aw88399->index);
		} else if (uid >= AW88399_HDA_MAX_AMPS) {
			dev_warn(dev, "ACPI _UID %llu out of range, falling back to I2C index %d\n",
				 uid, addr_index);
			aw88399->index = addr_index;
		} else {
			aw88399->index = (int)uid;
			if (aw88399->index != addr_index)
				dev_info(dev,
					 "ACPI _UID %d overrides I2C addr 0x%02x suggestion %d\n",
					 aw88399->index, i2c->addr, addr_index);
			else
				dev_info(dev, "ACPI _UID: %d (addr 0x%02x)\n",
					 aw88399->index, i2c->addr);
		}
	}

metadata:
	aw88399_hda_parse_speaker_props(aw88399);
	if (aw88399->speaker_pos_valid)
		aw88399->channel = aw88399->speaker_pos;
	else
		aw88399->channel = aw88399->index;

	/*
	 * Lenovo Legion Pro 7 16IAX10H (product 83F5, SSIDs 17aa:3906/3907/3d6c)
	 * has I2C devices wired backwards: 0x34 is physically right, 0x35 is left.
	 * Swap channels to correct L/R assignment. This is a hardware wiring issue
	 * specific to this model, not a driver bug.
	 */
	if (dmi_match(DMI_PRODUCT_NAME, "83F5")) {
		aw88399->channel = 1 - aw88399->channel;
		dev_info(dev, "Legion quirk: swapped to channel %d (index %d, addr 0x%02x)\n",
			 aw88399->channel, aw88399->index, i2c->addr);
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
	int ret;

	dev_dbg(dev, "System suspend\n");

	if (aw88399->aw_dev && aw88399->playing)
		aw88399_stop(aw88399->aw_dev);

	ret = pm_runtime_force_suspend(dev);
	if (ret)
		dev_err(dev, "Runtime force suspend failed: %d\n", ret);

	return ret;
}

static int aw88399_hda_system_resume(struct device *dev)
{
	struct aw88399_hda *aw88399 = dev_get_drvdata(dev);
	int ret;

	dev_dbg(dev, "System resume\n");

	if (aw88399->aw_dev) {
		aw88399_hda_hw_reset(aw88399);
		/* Chip will be fully reinitialized on next playback */
	}

	ret = pm_runtime_force_resume(dev);
	if (ret)
		dev_err(dev, "Runtime force resume failed: %d\n", ret);

	return ret;
}

const struct dev_pm_ops aw88399_hda_pm_ops = {
	RUNTIME_PM_OPS(aw88399_hda_runtime_suspend, aw88399_hda_runtime_resume, NULL)
	SYSTEM_SLEEP_PM_OPS(aw88399_hda_system_suspend, aw88399_hda_system_resume)
};
EXPORT_SYMBOL_NS_GPL(aw88399_hda_pm_ops, "SND_HDA_SCODEC_AW88399");

MODULE_DESCRIPTION("HDA AW88399 driver");
MODULE_AUTHOR("Lyapsus");
MODULE_LICENSE("GPL");
