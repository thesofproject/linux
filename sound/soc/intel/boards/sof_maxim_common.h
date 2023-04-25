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

#define MAX_98373_DEV0_NAME	"i2c-MX98373:00"
#define MAX_98373_DEV1_NAME	"i2c-MX98373:01"

extern const struct snd_kcontrol_new maxim_kcontrols[2];
extern const struct snd_soc_dapm_widget maxim_dapm_widgets[2];
extern const struct snd_soc_dapm_route maxim_dapm_routes[2];

void max_98373_dai_link(struct snd_soc_dai_link *link);
void sof_max98373_codec_conf(struct snd_soc_card *card);

/*
 * Maxim MAX98390
 */
#define MAX_98390_DEV0_NAME     "i2c-MX98390:00"
#define MAX_98390_DEV1_NAME     "i2c-MX98390:01"
#define MAX_98390_DEV2_NAME     "i2c-MX98390:02"
#define MAX_98390_DEV3_NAME     "i2c-MX98390:03"

void max_98390_dai_link(struct snd_soc_dai_link *link);
void sof_max98390_codec_conf(struct snd_soc_card *card);

/*
 * Maxim MAX98396
 */
#define MAX_98396_DEV0_NAME     "i2c-ADS8396-00"
#define MAX_98396_DEV1_NAME     "i2c-ADS8396-01"

void max_98396_dai_link(struct snd_soc_dai_link *link);
void sof_max98396_codec_conf(struct snd_soc_card *card);

/*
 * Maxim MAX98357A/MAX98360A
 */
#define MAX_98357A_CODEC_DAI	"HiFi"
#define MAX_98357A_DEV0_NAME	"MX98357A:00"
#define MAX_98360A_DEV0_NAME	"MX98360A:00"

void max_98357a_dai_link(struct snd_soc_dai_link *link);
void max_98360a_dai_link(struct snd_soc_dai_link *link);

#endif /* __SOF_MAXIM_COMMON_H */
