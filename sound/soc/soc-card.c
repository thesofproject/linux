// SPDX-License-Identifier: GPL-2.0
//
// soc-card.c
//
// Copyright (C) 2019 Renesas Electronics Corp.
// Kuninori Morimoto <kuninori.morimoto.gx@renesas.com>
//
#include <sound/soc.h>

#define soc_card_err(dai, ret) _soc_card_err(dai, __func__, ret)
static inline int _soc_card_err(struct snd_soc_card *card,
				const char *func, int ret)
{
	dev_err(card->dev,
		"ASoC: error at %s on %s: %d\n",
		func, card->name, ret);

	return ret;
}
