/* SPDX-License-Identifier: GPL-2.0-only
 *
 * aw88399_hda.h -- AW88399 HDA side codec driver
 *
 * Based on cs35l41_hda.h
 */

#ifndef __AW88399_HDA_H__
#define __AW88399_HDA_H__

#include <linux/regulator/consumer.h>
#include <linux/gpio/consumer.h>
#include <linux/device.h>
#include <sound/hda_codec.h>

struct acpi_device;

struct aw88399;
struct aw_device;

struct aw88399_hda {
	struct device *dev;
	struct regmap *regmap;
	struct gpio_desc *reset_gpio;
	struct aw_device *aw_dev;
	struct aw88399 *core;

	struct hda_codec *codec;
	struct acpi_device *adev;
	int index;
	int channel;
	int speaker_pos;
	int speaker_id;
	bool speaker_pos_valid;
	bool speaker_id_valid;
	bool acpi_notify_supported;

	bool playing;
	bool suspended;
};

int aw88399_hda_probe(struct device *dev, const char *device_name,
		      int id, int irq);
void aw88399_hda_remove(struct device *dev);

extern const struct dev_pm_ops aw88399_hda_pm_ops;

#endif /* __AW88399_HDA_H__ */
