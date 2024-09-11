/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CS42L43 CODEC driver internal data
 *
 * Copyright (C) 2022-2023 Cirrus Logic, Inc. and
 *                         Cirrus Logic International Semiconductor Ltd.
 */

#ifndef CS42L43_ASOC_INT_H
#define CS42L43_ASOC_INT_H

#include <linux/completion.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/workqueue.h>
#include <sound/pcm.h>

#define CS42L43_INTERNAL_SYSCLK		24576000
#define CS42L43_DEFAULT_SLOTS		0x3F

#define CS42L43_PLL_TIMEOUT_MS		200
#define CS42L43_SPK_TIMEOUT_MS		100
#define CS42L43_HP_TIMEOUT_MS		2000
#define CS42L43_LOAD_TIMEOUT_MS		1000

#define CS42L43_HP_ILIMIT_BACKOFF_MS	1000
#define CS42L43_HP_ILIMIT_DECAY_MS	300
#define CS42L43_HP_ILIMIT_MAX_COUNT	4

#define CS42L43_ASP_MAX_CHANNELS	6
#define CS42L43_N_EQ_COEFFS		15

#define CS42L43_N_BUTTONS	6

struct clk;
struct device;

struct snd_soc_component;
struct snd_soc_jack;

struct cs42l43;

struct cs42l43_codec {
	struct device *dev;
	struct cs42l43 *core;
	struct snd_soc_component *component;

	struct clk *mclk;

	int n_slots;
	int slot_width;
	int tx_slots[CS42L43_ASP_MAX_CHANNELS];
	int rx_slots[CS42L43_ASP_MAX_CHANNELS];
	struct snd_pcm_hw_constraint_list constraint;
};

#if IS_REACHABLE(CONFIG_SND_SOC_CS42L43_SDW)

int cs42l43_sdw_add_peripheral(struct snd_pcm_substream *substream,
			       struct snd_pcm_hw_params *params,
			       struct snd_soc_dai *dai);
int cs42l43_sdw_remove_peripheral(struct snd_pcm_substream *substream,
				  struct snd_soc_dai *dai);
int cs42l43_sdw_set_stream(struct snd_soc_dai *dai, void *sdw_stream, int direction);

#else

static inline int cs42l43_sdw_add_peripheral(struct snd_pcm_substream *substream,
					     struct snd_pcm_hw_params *params,
					     struct snd_soc_dai *dai)
{
	return -EINVAL;
}

#define cs42l43_sdw_remove_peripheral NULL
#define cs42l43_sdw_set_stream NULL

#endif

#endif /* CS42L43_ASOC_INT_H */
