/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (c) 2021 MediaTek Corporation. All rights reserved.
 *
 *  Header file for the mt8195 DSP clock  definition
 */

#ifndef __MT8195_CLK_H
#define __MT8195_CLK_H

/*DSP clock*/
enum ADSP_CLK_ID {
	CLK_TOP_DSP_SEL,
	CLK_TOP_CLK26M,
	CLK_TOP_AUDIO_LOCAL_BUS_SEL,
	CLK_TOP_MAINPLL_D7_D2,
	CLK_SCP_ADSP_AUDIODSP,
	CLK_TOP_AUDIO_H_SEL,
	ADSP_CLK_NUM
};

int platform_parse_clock(struct device *dev);
int adsp_default_clk_init(struct device *dev, int enable);
int adsp_enable_clock(struct device *dev);
void adsp_disable_clock(struct device *dev);
int adsp_clock_on(struct device *dev);
int adsp_clock_off(struct device *dev);
int adsp_sram_power_on(struct device *dev, bool val);
#endif
