// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
//
// Copyright(c) 2021 Mediatek Inc. All rights reserved.
//
// Author: YC Hung <yc.hung@mediatek.com>
//

/*
 * Hardware interface for audio DSP on mt8195
 */

#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/of_reserved_mem.h>
#include <linux/module.h>
#include <linux/pm_domain.h>

#include <sound/sof.h>
#include <sound/sof/xtensa.h>
#include "mt8195.h"
#include "mt8195-clk.h"
#include "../adsp_helper.h"
#include "../../ops.h"
#include "../../sof-audio.h"

static struct adsp_chip_info *adsp_info;

static struct snd_soc_acpi_mach sof_mt8195_mach = {
	.id = "819501",
	.drv_name = "mt8195_mt6359_rt1019_rt5682",
	.sof_tplg_filename = "sof-mt8195-mt6359-rt1019-rt5682.tplg",
};

static void *get_adsp_chip_data(void)
{
	return (void *)adsp_info;
}

void __iomem *get_mbox_reg_base(u32 id)
{
	struct adsp_chip_info *adsp;

	if (id >= DSP_MBOX_NUM)
		return NULL;

	adsp = get_adsp_chip_data();
	if (!adsp)
		return NULL;

	return adsp->va_mboxreg[id];
}
EXPORT_SYMBOL(get_mbox_reg_base);

static int platform_parse_resource(struct platform_device *pdev, void *data)
{
	struct resource *res;
	struct resource resource;
	struct device_node *mem_region;
	struct device *dev = &pdev->dev;
	struct adsp_chip_info *adsp = data;
	int i, ret;

	res = &resource;
	mem_region = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!mem_region) {
		dev_err(dev, "no 'memory-region' phandle\n");
		return -ENODEV;
	}

	ret = of_address_to_resource(mem_region, 0, res);
	if (ret) {
		dev_err(dev, "of_address_to_resource failed\n");
		return ret;
	}

	dev_info(dev, "[ADSP][DMA] pbase=0x%llx, size=0x%llx\n",
		 (phys_addr_t)res->start,
		 resource_size(res));

	ret = of_reserved_mem_device_init(dev);
	if (ret) {
		dev_err(dev, "of_reserved_mem_device_init failed\n");
		return ret;
	}

	mem_region = of_parse_phandle(dev->of_node, "memory-region", 1);
	if (!mem_region) {
		dev_err(dev, "no 'memory-region' phandle\n");
		return -ENODEV;
	}

	ret = of_address_to_resource(mem_region, 0, res);
	if (ret) {
		dev_err(dev, "of_address_to_resource failed\n");
		return ret;
	}

	adsp->pa_dram = (phys_addr_t)res->start;
	adsp->dramsize = (u32)resource_size(res);
	if (((u32)adsp->pa_dram) & 0x1000) {
		dev_err(dev, "adsp memory(0x%x) is not 4K-aligned\n",
			(u32)adsp->pa_dram);
		return -EINVAL;
	}

	if (adsp->dramsize < TOTAL_SIZE_SHARED_DRAM_FROM_TAIL) {
		dev_err(dev, "adsp memory(0x%x) is not enough for share\n",
			(u32)adsp->dramsize);
		return -EINVAL;
	}

	dev_dbg(dev, "[ADSP] dram pbase=%pa, dramsize=0x%x\n",
		&adsp->pa_dram, adsp->dramsize);

	/* Parse CFG base */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(dev, "no ADSP-CFG register resource\n");
		return -ENXIO;
	}
	/* remap for DSP register accessing */
	adsp->va_cfgreg = devm_ioremap_resource(dev, res);
	if (IS_ERR(adsp->va_cfgreg))
		return PTR_ERR(adsp->va_cfgreg);

	adsp->pa_cfgreg = (phys_addr_t)res->start;
	adsp->cfgregsize = (u32)resource_size(res);

	dev_dbg(dev, "[ADSP] cfgreg-vbase=%p, cfgregsize=0x%x\n",
		adsp->va_cfgreg, adsp->cfgregsize);

	/* Parse SRAM */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!res) {
		dev_err(dev, "no SRAM resource\n");
		return -ENXIO;
	}

	adsp->pa_sram = (phys_addr_t)res->start;
	adsp->srammsize = (u32)resource_size(res);
	if (adsp->srammsize < TOTAL_SIZE_SHARED_SRAM_FROM_TAIL) {
		dev_err(dev, "adsp SRAM(0x%x) is not enough for share\n",
			(u32)adsp->srammsize);
		return -EINVAL;
	}

	dev_dbg(dev, "[ADSP] sram pbase=%pa,0x%x\n", &adsp->pa_sram, adsp->srammsize);

	/* Parse MBOX base */
	for (i = 0; i < DSP_MBOX_NUM; i++) {
		res = platform_get_resource(pdev, IORESOURCE_MEM, 2 + i);
		if (!res) {
			dev_err(dev, "no MBOX resource %d\n", i);
			return -ENXIO;
		}

		adsp->pa_mboxreg[i] = (phys_addr_t)res->start;
		adsp->va_mboxreg[i] = devm_ioremap_resource(dev, res);
		if (IS_ERR(adsp->va_mboxreg[i]))
			return PTR_ERR(adsp->va_mboxreg[i]);

		dev_dbg(dev, "[ADSP]MBOX%d pa_mboxreg:%pa, va_mboxreg:%p\n",
			i, &adsp->pa_mboxreg[i], adsp->va_mboxreg[i]);
	}

	return ret;
}

/*  Init the basic DSP DRAM address */
static int adsp_memory_remap_init(struct device *dev)
{
	void __iomem *vaddr_emi_map;
	struct adsp_chip_info *adsp;
	int offset;

	adsp = get_adsp_chip_data();
	if (!adsp)
		return -ENXIO;

	vaddr_emi_map = devm_ioremap(dev, DSP_EMI_MAP_ADDR, 0x4);
	if (!vaddr_emi_map) {
		dev_err(dev, "error: failed to ioremap emi map base 0x%x\n",
			DSP_EMI_MAP_ADDR);
		return -ENOMEM;
	}

	offset = adsp->pa_dram - DRAM_PHYS_BASE_FROM_DSP_VIEW;
	adsp->dram_offset  =  offset;
	offset >>= 12;
	dev_info(dev, "adsp->pa_dram %llx, offset 0x%x\n", adsp->pa_dram,
		 offset);
	WARN_ON(offset < 0);
	writel(offset, vaddr_emi_map);
	dev_info(dev, "After vaddr_emi_map 0x%x\n", readl(vaddr_emi_map));

	return 0;
}

static int adsp_shared_base_ioremap(struct platform_device *pdev, void *data)
{
	struct device *dev = &pdev->dev;
	struct adsp_chip_info *adsp = data;
	u32 shared_size;
	int ret = 0;

	/* remap shared-dram base to be non-cachable */
	shared_size = TOTAL_SIZE_SHARED_DRAM_FROM_TAIL;
	adsp->pa_shared_dram = adsp->pa_dram + adsp->dramsize - shared_size;
	if (adsp->va_dram) {
		adsp->shared_dram = adsp->va_dram + DSP_DRAM_SIZE - shared_size;
	} else {
		adsp->shared_dram = devm_ioremap(dev, adsp->pa_shared_dram,
						 shared_size);
		if (!adsp->shared_dram) {
			dev_err(dev, "ioremap failed at line %d\n", __LINE__);
			ret = -ENOMEM;
			goto err;
		}
	}
	dev_info(dev, "[ADSP] shared-dram vbase=%p, phy addr :%llx,  size=0x%x\n",
		 adsp->shared_dram, adsp->pa_shared_dram, shared_size);
err:
	return ret;
}

static int mt8195_dsp_probe(struct snd_sof_dev *sdev)
{
	struct platform_device *pdev = container_of(sdev->dev, struct platform_device, dev);
	struct adsp_priv *priv;
	int ret, mailbox_type;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	sdev->pdata->hw_pdata = priv;
	priv->dev = sdev->dev;
	priv->sdev = sdev;

	priv->adsp = devm_kzalloc(&pdev->dev, sizeof(struct adsp_chip_info),
				  GFP_KERNEL);

	if (!priv->adsp)
		return -ENOMEM;

	adsp_info = priv->adsp;

	ret = platform_parse_resource(pdev, adsp_info);
	if (ret)
		return ret;

	ret = platform_parse_clock(&pdev->dev);
	if (ret)
		return ret;

	pm_runtime_enable(&pdev->dev);
	pm_runtime_get_sync(&pdev->dev);

	ret = adsp_clock_on(&pdev->dev);
	if (ret) {
		dev_err(sdev->dev, "adsp_clock_on fail!\n");
		goto err_pm_disable;
	}

	ret = adsp_sram_power_on(sdev->dev, true);
	if (ret) {
		dev_err(sdev->dev, "adsp_sram_power_on fail!\n");
		goto err_adsp_clock_off;
	}

	ret = adsp_memory_remap_init(&pdev->dev);
	if (ret) {
		dev_err(sdev->dev, "adsp_memory_remap_init fail!\n");
		goto err_adsp_sram_power_off;
	}

	sdev->bar[SOF_FW_BLK_TYPE_IRAM] = devm_ioremap(sdev->dev,
						       priv->adsp->pa_sram,
						       priv->adsp->srammsize);
	if (!sdev->bar[SOF_FW_BLK_TYPE_IRAM]) {
		dev_err(sdev->dev, "failed to ioremap base %pa size 0x%x\n",
			&priv->adsp->pa_sram, priv->adsp->srammsize);
		ret = -EINVAL;
		goto err_adsp_sram_power_off;
	}

	sdev->bar[SOF_FW_BLK_TYPE_SRAM] = devm_ioremap_wc(sdev->dev,
							  priv->adsp->pa_dram,
							  priv->adsp->dramsize);
	if (!sdev->bar[SOF_FW_BLK_TYPE_SRAM]) {
		dev_err(sdev->dev, "failed to ioremap base %pa size 0x%x\n",
			&priv->adsp->pa_dram, priv->adsp->dramsize);
		ret = -EINVAL;
		goto err_adsp_sram_power_off;
	}
	adsp_info->va_dram = sdev->bar[SOF_FW_BLK_TYPE_SRAM];
	mailbox_type = SOF_FW_BLK_TYPE_SRAM;

	ret =  adsp_shared_base_ioremap(pdev, adsp_info);
	if (ret) {
		dev_err(sdev->dev, "adsp_shared_base_ioremap fail!\n");
		goto err_adsp_sram_power_off;
	}

	sdev->mmio_bar = mailbox_type;
	sdev->mailbox_bar = mailbox_type;

	return 0;

err_adsp_sram_power_off:
	adsp_sram_power_on(&pdev->dev, false);
err_adsp_clock_off:
	adsp_clock_off(&pdev->dev);
err_pm_disable:
	pm_runtime_disable(&pdev->dev);

	return ret;
}

static int mt8195_dsp_remove(struct snd_sof_dev *sdev)
{
	struct platform_device *pdev = container_of(sdev->dev, struct platform_device, dev);

	adsp_clock_off(&pdev->dev);
	adsp_sram_power_on(&pdev->dev, false);
	pm_runtime_disable(&pdev->dev);
	return 0;
}

static void mt8195_machine_select(struct snd_sof_dev *sdev)
{
	struct snd_sof_pdata *sof_pdata = sdev->pdata;
	struct snd_soc_acpi_mach *mach;

	mach = &sof_mt8195_mach;
	if (!mach) {
		dev_warn(sdev->dev, "No matching ASoC machine driver found\n");
		return;
	}

	sof_pdata->tplg_filename = mach->sof_tplg_filename;
	sof_pdata->machine = mach;

	/* get machine node and save it to mach->pdata */
	mach->pdata = of_get_child_by_name(sdev->dev->of_node, "sound");
	if (!mach->pdata)
		dev_warn(sdev->dev, "get child machine node \"sound\" failed\n");
}

static int mt8195_get_bar_index(struct snd_sof_dev *sdev, u32 type)
{
	return type;
}

/* mt8195 ops */
struct snd_sof_dsp_ops sof_mt8195_ops = {
	/* probe and remove */
	.probe		= mt8195_dsp_probe,
	.remove		= mt8195_dsp_remove,

	/* Block IO */
	.block_read	= sof_block_read,
	.block_write	= sof_block_write,

	/* Register IO */
	.write		= sof_io_write,
	.read		= sof_io_read,
	.write64	= sof_io_write64,
	.read64		= sof_io_read64,

	/* machine driver */
	.machine_select = mt8195_machine_select,
	.machine_register = sof_machine_register,

	/* misc */
	.get_bar_index	= mt8195_get_bar_index,

	/* Firmware ops */
	.dsp_arch_ops = &sof_xtensa_arch_ops,
};
EXPORT_SYMBOL(sof_mt8195_ops);

MODULE_IMPORT_NS(SND_SOC_SOF_XTENSA);
MODULE_LICENSE("Dual BSD/GPL");
