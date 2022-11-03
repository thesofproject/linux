/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright(c) 2020 Intel Corporation.
 */

/*
 * This file defines data structures used in Machine Driver for Intel
 * platforms with Maxim Codecs.
 */
#ifndef __SOF_MAXIM_COMMON_H
#define __SOF_MAXIM_COMMON_H

#include <sound/soc.h>

/*
 * Maxim codecs: MAX98373/MAX98390/MAX98396
 */
extern const struct snd_soc_ops maxim_ops;
extern const struct snd_soc_dapm_route maxim_dapm_routes[];

void maxim_dai_link(struct snd_soc_dai_link *link);
void maxim_set_codec_conf(struct snd_soc_card *card);
int maxim_spk_codec_init(struct snd_soc_pcm_runtime *rtd);
int maxim_trigger(struct snd_pcm_substream *substream, int cmd);

/*
 * Maxim MAX98357A/MAX98360A
 */
#define MAX_98357A_CODEC_DAI	"HiFi"
#define MAX_98357A_DEV0_NAME	"MX98357A:00"
#define MAX_98360A_DEV0_NAME	"MX98360A:00"

void max_98357a_dai_link(struct snd_soc_dai_link *link);
void max_98360a_dai_link(struct snd_soc_dai_link *link);

#endif /* __SOF_MAXIM_COMMON_H */
