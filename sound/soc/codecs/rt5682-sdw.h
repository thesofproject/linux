/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * rt5682-sdw.h  --  RT5682 SDW ALSA SoC audio driver
 *
 * Copyright 2019 Realtek Semiconductor Corp.
 * Author: Oder Chiou <oder_chiou@realtek.com>
 */

#ifndef __RT5682_SDW_H__
#define __RT5682_SDW_H__

#define RT5682_SDW_ADDR_L			0x3000
#define RT5682_SDW_ADDR_H			0x3001
#define RT5682_SDW_DATA_L			0x3004
#define RT5682_SDW_DATA_H			0x3005
#define RT5682_SDW_CMD				0x3008

int rt5682_sdw_read(void *context, unsigned int reg, unsigned int *val);
int rt5682_sdw_write(void *context, unsigned int reg, unsigned int val);
int rt5682_set_sdw_stream(struct snd_soc_dai *dai, void *sdw_stream,
				int direction);
void rt5682_sdw_shutdown(struct snd_pcm_substream *substream,
				struct snd_soc_dai *dai);
int rt5682_sdw_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params,
				struct snd_soc_dai *dai);
int rt5682_sdw_hw_free(struct snd_pcm_substream *substream,
				struct snd_soc_dai *dai);

#endif /* __RT5682_SDW_H__ */
