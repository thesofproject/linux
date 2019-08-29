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

int snd_soc_link_startup(struct snd_soc_pcm_runtime *rtd,
			 struct snd_pcm_substream *substream)
{
	if (rtd->dai_link->ops &&
	    rtd->dai_link->ops->startup) {
		int ret = rtd->dai_link->ops->startup(substream);
		if (ret < 0)
			return soc_link_err(rtd, ret);
	}

	rtd->started = 1;

	return 0;
}

void snd_soc_link_shutdown(struct snd_soc_pcm_runtime *rtd,
			   struct snd_pcm_substream *substream)
{
	if (rtd->started &&
	    rtd->dai_link->ops &&
	    rtd->dai_link->ops->shutdown)
		rtd->dai_link->ops->shutdown(substream);

	rtd->started = 0;
}

int snd_soc_link_prepare(struct snd_soc_pcm_runtime *rtd,
			 struct snd_pcm_substream *substream)
{
	if (rtd->dai_link->ops &&
	    rtd->dai_link->ops->prepare) {
		int ret = rtd->dai_link->ops->prepare(substream);
		if (ret < 0)
			return soc_link_err(rtd, ret);
	}

	return 0;
}

int snd_soc_link_hw_params(struct snd_soc_pcm_runtime *rtd,
			   struct snd_pcm_substream *substream,
			   struct snd_pcm_hw_params *params)
{
	if (rtd->dai_link->ops &&
	    rtd->dai_link->ops->hw_params) {
		int ret = rtd->dai_link->ops->hw_params(substream, params);
		if (ret < 0)
			return soc_link_err(rtd, ret);
	}

	rtd->hw_paramed = 1;

	return 0;
}

void snd_soc_link_hw_free(struct snd_soc_pcm_runtime *rtd,
			  struct snd_pcm_substream *substream)
{
	if (rtd->hw_paramed &&
	    rtd->dai_link->ops &&
	    rtd->dai_link->ops->hw_free)
		rtd->dai_link->ops->hw_free(substream);

	rtd->hw_paramed = 0;
}

int snd_soc_link_trigger(struct snd_soc_pcm_runtime *rtd,
			 struct snd_pcm_substream *substream, int cmd)
{
	if (rtd->dai_link->ops &&
	    rtd->dai_link->ops->trigger) {
		int ret = rtd->dai_link->ops->trigger(substream, cmd);
		if (ret < 0)
			return soc_link_err(rtd, ret);
	}

	return 0;
}
