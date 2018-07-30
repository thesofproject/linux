// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
/*
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * Copyright(c) 2018 Intel Corporation. All rights reserved.
 *
 * Authors: Jeeja KP <jeeja.kp@intel.com>
 *          Keyon Jie <yang.jie@linux.intel.com>
 */

#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <sound/hdaudio_ext.h>
#include <sound/sof.h>
#include <sound/hdaudio.h>
#include <sound/hda_i915.h>
#include <sound/hda_register.h>
#include <sound/hdaudio.h>

#include "../../../pci/hda/hda_codec.h"
#include "../../codecs/hdac_hda.h"

#include "../sof-priv.h"
#include "../ops.h"
#include "hda.h"

#define IDISP_INTEL_VENDOR_ID	0x80860000

/* load the legacy codec driver */
#ifdef MODULE
static void hda_codec_load_module(struct hda_codec *codec)
{
	char modalias[MODULE_NAME_LEN];
	const char *mod = modalias;

	snd_hdac_codec_modalias(&codec->core, modalias, sizeof(modalias));
	dev_dbg(&codec->core.dev, "loading %s codec module\n", mod);
	request_module(mod);
}
#else
static void hda_codec_load_module(struct hda_codec *codec) {}
#endif

/* probe individual codec */
static int hda_codec_probe(struct snd_sof_dev *sdev, int addr)
{
	struct hda_bus *hbus = sof_to_hbus(sdev);
	unsigned int cmd = (addr << 28) | (AC_NODE_ROOT << 20) |
		(AC_VERB_PARAMETERS << 8) | AC_PAR_VENDOR_ID;
	unsigned int res = -1;
	struct hdac_hda_priv *hda_codec;
	struct hdac_device *hdev;
	int err;

	/* make sure codec responds at this address */
	mutex_lock(&hbus->core.cmd_mutex);
	snd_hdac_bus_send_cmd(&hbus->core, cmd);
	snd_hdac_bus_get_response(&hbus->core, addr, &res);
	mutex_unlock(&hbus->core.cmd_mutex);
	if (res == -1)
		return -EIO;
	dev_dbg(sdev->dev, "codec #%d probed OK: %x\n", addr, res);

	hda_codec = devm_kzalloc(&hbus->pci->dev, sizeof(*hda_codec),
				 GFP_KERNEL);
	if (!hda_codec)
		return -ENOMEM;

	hda_codec->codec.bus = hbus;
	hdev = &hda_codec->codec.core;

	err = snd_hdac_ext_bus_device_init(&hbus->core, addr, hdev);
	if (err < 0)
		return err;

	/* use legacy bus only for HDA codecs, idisp uses ext bus */
	if ((res & 0xFFFF0000) != IDISP_INTEL_VENDOR_ID) {
		hdev->type = HDA_DEV_LEGACY;
		hda_codec_load_module(&hda_codec->codec);
	}

	return 0;
}

/* Codec initialization */
int hda_codec_probe_bus(struct snd_sof_dev *sdev)
{
	struct hdac_bus *bus = sof_to_bus(sdev);
	int c, max_slots, ret = 0;

	max_slots = HDA_MAX_CODECS;

	/* probe codecs in avail slots */
	for (c = 0; c < max_slots; c++) {

		if (!(bus->codec_mask & (1 << c)))
			continue;

		ret = hda_codec_probe(sdev, c);
		if (ret < 0) {
			dev_err(bus->dev,"codec #%d probe error %d\n", c, ret);
			return ret;
		}
	}

	return 0;
}
EXPORT_SYMBOL(hda_codec_probe_bus);

int hda_codec_i915_init(struct snd_sof_dev *sdev)
{
	struct hdac_bus *bus = sof_to_bus(sdev);
	int ret;

	/* i915 exposes a HDA codec for HDMI audio */
	ret = snd_hdac_i915_init(bus);
	if (ret < 0)
		return ret;

	ret = snd_hdac_display_power(bus, true);
	if (ret < 0)
		dev_err(bus->dev, "i915 HDAC power on failed %d\n", ret);

	return ret;
}
EXPORT_SYMBOL(hda_codec_i915_init);

MODULE_LICENSE("Dual BSD/GPL");

