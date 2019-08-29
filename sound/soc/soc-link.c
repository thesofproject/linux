// SPDX-License-Identifier: GPL-2.0
//
// soc-link.c
//
// Copyright (C) 2019 Renesas Electronics Corp.
// Kuninori Morimoto <kuninori.morimoto.gx@renesas.com>
//
#include <sound/soc.h>

#define soc_link_err(rtd, ret) _soc_link_err(rtd, __func__, ret)
static inline int _soc_link_err(struct snd_soc_pcm_runtime *rtd,
				const char *func, int ret)
{
	dev_err(rtd->dev,
		"ASoC: error at %s on %s: %d\n",
		func, rtd->dai_link->name, ret);

	return ret;
}

int snd_soc_link_init(struct snd_soc_pcm_runtime *rtd)
{
	if (rtd->dai_link->init) {
		int ret = rtd->dai_link->init(rtd);
		if (ret < 0)
			return soc_link_err(rtd, ret);
	}

	return 0;
}
