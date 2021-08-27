// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
//
// Copyright(c) 2021 Mediatek Corporation. All rights reserved.
//
// Author: YC Hung <yc.hung@mediatek.com>
//
// Hardware interface for mt8195 DSP clock

#include <linux/clk.h>
#include <linux/pm_runtime.h>
#include <linux/io.h>
#include "mt8195.h"
#include "mt8195-clk.h"

struct clk *clk_handle[ADSP_CLK_NUM];

int platform_parse_clock(struct device *dev)
{
	clk_handle[CLK_TOP_DSP_SEL] = devm_clk_get(dev, "dsp_sel");
	if (IS_ERR(clk_handle[CLK_TOP_DSP_SEL])) {
		dev_err(dev, "clk_get(\"dsp_sel\") failed\n");
		return PTR_ERR(clk_handle[CLK_TOP_DSP_SEL]);
	}

	clk_handle[CLK_TOP_CLK26M] = devm_clk_get(dev, "clk26m_ck");
	if (IS_ERR(clk_handle[CLK_TOP_CLK26M])) {
		dev_err(dev, "clk_get(\"clk26m_ck\") failed\n");
		return PTR_ERR(clk_handle[CLK_TOP_CLK26M]);
	}

	clk_handle[CLK_TOP_AUDIO_LOCAL_BUS_SEL] = devm_clk_get(dev, "audio_local_bus");
	if (IS_ERR(clk_handle[CLK_TOP_AUDIO_LOCAL_BUS_SEL])) {
		dev_err(dev, "clk_get(\"audio_local_bus\") failed\n");
		return PTR_ERR(clk_handle[CLK_TOP_AUDIO_LOCAL_BUS_SEL]);
	}

	clk_handle[CLK_TOP_MAINPLL_D7_D2] = devm_clk_get(dev, "mainpll_d7_d2");
	if (IS_ERR(clk_handle[CLK_TOP_MAINPLL_D7_D2])) {
		dev_err(dev, "clk_get(\"mainpll_d7_d2\") failed\n");
		return PTR_ERR(clk_handle[CLK_TOP_MAINPLL_D7_D2]);
	}

	clk_handle[CLK_SCP_ADSP_AUDIODSP] = devm_clk_get(dev, "scp_adsp_audiodsp");
	if (IS_ERR(clk_handle[CLK_SCP_ADSP_AUDIODSP])) {
		dev_err(dev, "clk_get(\"scp_adsp_audiodsp\") failed\n");
		return PTR_ERR(clk_handle[CLK_SCP_ADSP_AUDIODSP]);
	}

	clk_handle[CLK_TOP_AUDIO_H_SEL] = devm_clk_get(dev, "audio_h_sel");
	if (IS_ERR(clk_handle[CLK_TOP_AUDIO_H_SEL])) {
		dev_err(dev, "clk_get(\"audio_h_sel\") failed\n");
		return PTR_ERR(clk_handle[CLK_TOP_AUDIO_H_SEL]);
	}

	return 0;
}

int adsp_enable_clock(struct device *dev)
{
	int ret;

	ret = clk_prepare_enable(clk_handle[CLK_TOP_MAINPLL_D7_D2]);
	if (ret) {
		dev_err(dev, "%s clk_prepare_enable(mainpll_d7_d2) fail %d\n",
			__func__, ret);
		return ret;
	}

	ret = clk_prepare_enable(clk_handle[CLK_TOP_DSP_SEL]);
	if (ret) {
		dev_err(dev, "%s clk_prepare_enable(dsp_sel) fail %d\n",
			__func__, ret);
		goto disable_mainpll_d7_d2_clk;
	}

	ret = clk_prepare_enable(clk_handle[CLK_TOP_AUDIO_LOCAL_BUS_SEL]);
	if (ret) {
		dev_err(dev, "%s clk_prepare_enable(audio_local_bus) fail %d\n",
			__func__, ret);
		goto disable_dsp_sel_clk;
	}

	ret = clk_prepare_enable(clk_handle[CLK_SCP_ADSP_AUDIODSP]);
	if (ret) {
		dev_err(dev, "%s clk_prepare_enable(scp_adsp_audiodsp) fail %d\n",
			__func__, ret);
		goto disable_audio_local_bus_clk;
	}

	ret = clk_prepare_enable(clk_handle[CLK_TOP_AUDIO_H_SEL]);
	if (ret) {
		dev_err(dev, "%s clk_prepare_enable(audio_h_sel) fail %d\n",
			__func__, ret);
		goto disable_scp_adsp_audiodsp_clk;
	}

	return 0;

disable_scp_adsp_audiodsp_clk:
	clk_disable_unprepare(clk_handle[CLK_SCP_ADSP_AUDIODSP]);
disable_audio_local_bus_clk:
	clk_disable_unprepare(clk_handle[CLK_TOP_AUDIO_LOCAL_BUS_SEL]);
disable_dsp_sel_clk:
	clk_disable_unprepare(clk_handle[CLK_TOP_DSP_SEL]);
disable_mainpll_d7_d2_clk:
	clk_disable_unprepare(clk_handle[CLK_TOP_MAINPLL_D7_D2]);

	return ret;
}

int adsp_sram_power_on(struct device *dev, bool val)
{
	void __iomem **va_dspsysreg;

	va_dspsysreg = devm_ioremap(dev, ADSP_SRAM_POOL_CON, 0x4);
	if (!va_dspsysreg) {
		dev_err(dev, "error: failed to ioremap sram pool base 0x%x\n",
			ADSP_SRAM_POOL_CON);
		return -ENOMEM;
	}

	if (val)  /* Power on ADSP SRAM */
		writel(readl(va_dspsysreg) & ~DSP_SRAM_POOL_PD_MASK, va_dspsysreg);
	else /* Power down ADSP SRAM */
		writel(readl(va_dspsysreg) | DSP_SRAM_POOL_PD_MASK, va_dspsysreg);

	return 0;
}

void adsp_disable_clock(struct device *dev)
{
	clk_disable_unprepare(clk_handle[CLK_TOP_AUDIO_H_SEL]);
	clk_disable_unprepare(clk_handle[CLK_SCP_ADSP_AUDIODSP]);
	clk_disable_unprepare(clk_handle[CLK_TOP_AUDIO_LOCAL_BUS_SEL]);
	clk_disable_unprepare(clk_handle[CLK_TOP_DSP_SEL]);
	clk_disable_unprepare(clk_handle[CLK_TOP_MAINPLL_D7_D2]);
}

int adsp_default_clk_init(struct device *dev, int enable)
{
	int ret = 0;

	dev_dbg(dev, "%s: %s\n", __func__, enable ? "on" : "off");

	if (enable) {
		ret = clk_set_parent(clk_handle[CLK_TOP_DSP_SEL],
				     clk_handle[CLK_TOP_CLK26M]);
		if (ret) {
			dev_err(dev, "failed to set dsp_sel to clk26m: %d\n", ret);
			return ret;
		}

		ret = clk_set_parent(clk_handle[CLK_TOP_AUDIO_LOCAL_BUS_SEL],
				     clk_handle[CLK_TOP_MAINPLL_D7_D2]);
		if (ret) {
			dev_err(dev, "set audio_local_bus failed %d\n", ret);
			return ret;
		}

		ret = adsp_enable_clock(dev);
		if (ret)
			dev_err(dev, "failed to adsp_enable_clock: %d\n", ret);

		return ret;
	}

	adsp_disable_clock(dev);

	return ret;
}

int adsp_clock_on(struct device *dev)
{
	/* Open ADSP clock */
	return adsp_default_clk_init(dev, 1);
}

int adsp_clock_off(struct device *dev)
{
	/* Close ADSP clock */
	return adsp_default_clk_init(dev, 0);
}

