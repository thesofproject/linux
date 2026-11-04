// SPDX-License-Identifier: GPL-2.0-only
//
// Copyright(c) 2021-2026 Intel Corporation
//
// Authors: Cezary Rojewski <cezary.rojewski@intel.com>
//          Amadeusz Slawinski <amade@asmblr.net>
//

#include <linux/acpi.h>
#include <linux/soundwire/sdw.h>
#include <linux/soundwire/sdw_intel.h>
#include <sound/hdaudio_ext.h>
#include "avs.h"
#include "registers.h"
#include "soundwire.h"

static struct sdw_intel_hw_ops avs_sdw_cnl_ops;

int avs_sdw_probe(struct avs_dev *adev)
{
	struct hdac_bus *hbus = &adev->base.core;
	struct sdw_intel_acpi_info info;
	struct sdw_intel_ctx *ctx;
	struct sdw_intel_res res;
	int ret;

	if (avs_platattr_test(adev, ALTHDA))
		return -ENODEV;

	ret = sdw_intel_acpi_scan(ACPI_HANDLE(adev->dev), &info);
	if (ret || !info.link_mask)
		return ret;

	memset(&res, 0, sizeof(res));
	res.mmio_base = adev->dsp_ba;
	res.shim_base = SDW_SHIM_BASE;
	res.alh_base = SDW_ALH_BASE;
	res.ext = false;
	res.irq = adev->base.core.irq;
	res.handle = info.handle;
	res.parent = adev->dev;
	res.dev = adev->dev;
	res.clock_stop_quirks = SDW_INTEL_CLK_STOP_BUS_RESET;
	res.hbus = hbus;

	memcpy(&avs_sdw_cnl_ops, &sdw_intel_cnl_hw_ops, sizeof(avs_sdw_cnl_ops));
	avs_sdw_cnl_ops.register_dai = NULL;
	res.hw_ops = &avs_sdw_cnl_ops;

	/* we could filter links here if needed, e.g.: for quirks */
	res.count = info.count;
	res.link_mask = info.link_mask;

	ctx = sdw_intel_probe(&res);
	if (!ctx) {
		dev_err(adev->dev, "SoundWire probe failed\n");
		return -EINVAL;
	}

	adev->sdw = ctx;
	return 0;
}

int avs_sdw_startup(struct avs_dev *adev)
{
	if (adev->sdw)
		return sdw_intel_startup(adev->sdw);
	return 0;
}

void avs_sdw_exit(struct avs_dev *adev)
{
	struct sdw_intel_ctx *ctx = adev->sdw;

	if (ctx) {
		adev->sdw = NULL;
		sdw_intel_exit(ctx);
	}
}

// TODO split tgl vs mtl
static void sdw_intel_enable_irq(void __iomem *mmio_base, bool enable)
{
	u32 val;

	val = readl(mmio_base + AVS_ADSP_REG_ADSPIC2);

	if (enable)
		val |= AVS_ADSP_ADSPIC2_SNDW;
	else
		val &= ~AVS_ADSP_ADSPIC2_SNDW;

	writel(val, mmio_base + AVS_ADSP_REG_ADSPIC2);
}

void avs_sdw_enable_irq(struct avs_dev *adev, bool enable)
{
	if (adev->sdw)
		sdw_intel_enable_irq(adev->dsp_ba, enable);
}

void avs_sdw_process_wakeen_event(struct avs_dev *adev)
{
	if (adev->sdw)
		sdw_intel_process_wakeen_event(adev->sdw);
}

irqreturn_t avs_sdw_thread(int irq, struct avs_dev *adev)
{
	if (adev->sdw)
		return sdw_intel_thread(irq, adev->sdw);
	return IRQ_HANDLED;
}

MODULE_IMPORT_NS("SOUNDWIRE_INTEL");
MODULE_IMPORT_NS("SOUNDWIRE_INTEL_INIT");
MODULE_IMPORT_NS("SND_INTEL_SOUNDWIRE_ACPI");
