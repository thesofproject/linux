// SPDX-License-Identifier: GPL-2.0-only
// This file incorporates work covered by the following copyright notice:
// Copyright (c) 2020 Intel Corporation
// Copyright(c) 2024 Advanced Micro Devices, Inc.
/*
 *  soc-sdw-utils.c - common SoundWire machine driver helper functions
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/soundwire/sdw.h>
#include <linux/soundwire/sdw_type.h>
#include "soc_sdw_utils.h"

struct soc_sdw_codec_info codec_info_list[] = {
	{
		.part_id = 0x700,
		.dais = {
			{
				.direction = {true, true},
				.dai_name = "rt700-aif1",
				.dai_type = SOC_SDW_DAI_TYPE_JACK,
				.dailink = {SDW_JACK_OUT_DAI_ID, SDW_JACK_IN_DAI_ID},
				.rtd_init = rt700_sdw_rtd_init,
			},
		},
		.dai_num = 1,
	},
	{
		.part_id = 0x711,
		.version_id = 3,
		.dais = {
			{
				.direction = {true, true},
				.dai_name = "rt711-sdca-aif1",
				.dai_type = SOC_SDW_DAI_TYPE_JACK,
				.dailink = {SDW_JACK_OUT_DAI_ID, SDW_JACK_IN_DAI_ID},
				.init = soc_sdw_rt_sdca_jack_init,
				.exit = soc_sdw_rt_sdca_jack_exit,
				.rtd_init = rt_sdca_jack_rtd_init,
			},
		},
		.dai_num = 1,
	},
	{
		.part_id = 0x711,
		.version_id = 2,
		.dais = {
			{
				.direction = {true, true},
				.dai_name = "rt711-aif1",
				.dai_type = SOC_SDW_DAI_TYPE_JACK,
				.dailink = {SDW_JACK_OUT_DAI_ID, SDW_JACK_IN_DAI_ID},
				.init = soc_sdw_rt711_init,
				.exit = soc_sdw_rt711_exit,
				.rtd_init = rt711_sdw_rtd_init,
			},
		},
		.dai_num = 1,
	},
	{
		.part_id = 0x712,
		.version_id = 3,
		.dais =	{
			{
				.direction = {true, true},
				.dai_name = "rt712-sdca-aif1",
				.dai_type = SOC_SDW_DAI_TYPE_JACK,
				.dailink = {SDW_JACK_OUT_DAI_ID, SDW_JACK_IN_DAI_ID},
				.init = soc_sdw_rt_sdca_jack_init,
				.exit = soc_sdw_rt_sdca_jack_exit,
				.rtd_init = rt_sdca_jack_rtd_init,
			},
			{
				.direction = {true, false},
				.dai_name = "rt712-sdca-aif2",
				.dai_type = SOC_SDW_DAI_TYPE_AMP,
				.dailink = {SDW_AMP_OUT_DAI_ID, SDW_UNUSED_DAI_ID},
				.init = soc_sdw_rt_amp_init,
				.exit = soc_sdw_rt_amp_exit,
				.rtd_init = rt712_sdw_spk_rtd_init,
			},
		},
		.dai_num = 2,
	},
	{
		.part_id = 0x1712,
		.version_id = 3,
		.dais =	{
			{
				.direction = {false, true},
				.dai_name = "rt712-sdca-dmic-aif1",
				.dai_type = SOC_SDW_DAI_TYPE_MIC,
				.dailink = {SDW_UNUSED_DAI_ID, SDW_DMIC_DAI_ID},
				.rtd_init = rt_sdw_dmic_rtd_init,
			},
		},
		.dai_num = 1,
	},
	{
		.part_id = 0x713,
		.version_id = 3,
		.dais =	{
			{
				.direction = {true, true},
				.dai_name = "rt712-sdca-aif1",
				.dai_type = SOC_SDW_DAI_TYPE_JACK,
				.dailink = {SDW_JACK_OUT_DAI_ID, SDW_JACK_IN_DAI_ID},
				.init = soc_sdw_rt_sdca_jack_init,
				.exit = soc_sdw_rt_sdca_jack_exit,
				.rtd_init = rt_sdca_jack_rtd_init,
			},
		},
		.dai_num = 1,
	},
	{
		.part_id = 0x1713,
		.version_id = 3,
		.dais =	{
			{
				.direction = {false, true},
				.dai_name = "rt712-sdca-dmic-aif1",
				.dai_type = SOC_SDW_DAI_TYPE_MIC,
				.dailink = {SDW_UNUSED_DAI_ID, SDW_DMIC_DAI_ID},
				.rtd_init = rt_sdw_dmic_rtd_init,
			},
		},
		.dai_num = 1,
	},
	{
		.part_id = 0x1308,
		.acpi_id = "10EC1308",
		.dais = {
			{
				.direction = {true, false},
				.dai_name = "rt1308-aif",
				.dai_type = SOC_SDW_DAI_TYPE_AMP,
				.dailink = {SDW_AMP_OUT_DAI_ID, SDW_UNUSED_DAI_ID},
				.init = soc_sdw_rt_amp_init,
				.exit = soc_sdw_rt_amp_exit,
				.rtd_init = rt_amp_spk_rtd_init,
			},
		},
		.dai_num = 1,
		.ops = &soc_sdw_rt1308_i2s_ops,
	},
	{
		.part_id = 0x1316,
		.dais = {
			{
				.direction = {true, true},
				.dai_name = "rt1316-aif",
				.dai_type = SOC_SDW_DAI_TYPE_AMP,
				.dailink = {SDW_AMP_OUT_DAI_ID, SDW_AMP_IN_DAI_ID},
				.init = soc_sdw_rt_amp_init,
				.exit = soc_sdw_rt_amp_exit,
				.rtd_init = rt_amp_spk_rtd_init,
			},
		},
		.dai_num = 1,
	},
	{
		.part_id = 0x1318,
		.dais = {
			{
				.direction = {true, true},
				.dai_name = "rt1318-aif",
				.dai_type = SOC_SDW_DAI_TYPE_AMP,
				.dailink = {SDW_AMP_OUT_DAI_ID, SDW_AMP_IN_DAI_ID},
				.init = soc_sdw_rt_amp_init,
				.exit = soc_sdw_rt_amp_exit,
				.rtd_init = rt_amp_spk_rtd_init,
			},
		},
		.dai_num = 1,
	},
	{
		.part_id = 0x714,
		.version_id = 3,
		.ignore_internal_dmic = true,
		.dais = {
			{
				.direction = {false, true},
				.dai_name = "rt715-sdca-aif2",
				.dai_type = SOC_SDW_DAI_TYPE_MIC,
				.dailink = {SDW_UNUSED_DAI_ID, SDW_DMIC_DAI_ID},
				.rtd_init = rt_sdw_dmic_rtd_init,
			},
		},
		.dai_num = 1,
	},
	{
		.part_id = 0x715,
		.version_id = 3,
		.ignore_internal_dmic = true,
		.dais = {
			{
				.direction = {false, true},
				.dai_name = "rt715-sdca-aif2",
				.dai_type = SOC_SDW_DAI_TYPE_MIC,
				.dailink = {SDW_UNUSED_DAI_ID, SDW_DMIC_DAI_ID},
				.rtd_init = rt_sdw_dmic_rtd_init,
			},
		},
		.dai_num = 1,
	},
	{
		.part_id = 0x714,
		.version_id = 2,
		.ignore_internal_dmic = true,
		.dais = {
			{
				.direction = {false, true},
				.dai_name = "rt715-aif2",
				.dai_type = SOC_SDW_DAI_TYPE_MIC,
				.dailink = {SDW_UNUSED_DAI_ID, SDW_DMIC_DAI_ID},
				.rtd_init = rt_sdw_dmic_rtd_init,
			},
		},
		.dai_num = 1,
	},
	{
		.part_id = 0x715,
		.version_id = 2,
		.ignore_internal_dmic = true,
		.dais = {
			{
				.direction = {false, true},
				.dai_name = "rt715-aif2",
				.dai_type = SOC_SDW_DAI_TYPE_MIC,
				.dailink = {SDW_UNUSED_DAI_ID, SDW_DMIC_DAI_ID},
				.rtd_init = rt_sdw_dmic_rtd_init,
			},
		},
		.dai_num = 1,
	},
	{
		.part_id = 0x722,
		.version_id = 3,
		.dais = {
			{
				.direction = {true, true},
				.dai_name = "rt722-sdca-aif1",
				.dai_type = SOC_SDW_DAI_TYPE_JACK,
				.dailink = {SDW_JACK_OUT_DAI_ID, SDW_JACK_IN_DAI_ID},
				.init = soc_sdw_rt_sdca_jack_init,
				.exit = soc_sdw_rt_sdca_jack_exit,
				.rtd_init = rt_sdca_jack_rtd_init,
			},
			{
				.direction = {true, false},
				.dai_name = "rt722-sdca-aif2",
				.dai_type = SOC_SDW_DAI_TYPE_AMP,
				/* No feedback capability is provided by rt722-sdca codec driver*/
				.dailink = {SDW_AMP_OUT_DAI_ID, SDW_UNUSED_DAI_ID},
				.init = soc_sdw_rt_amp_init,
				.exit = soc_sdw_rt_amp_exit,
				.rtd_init = rt722_sdw_spk_rtd_init,
			},
			{
				.direction = {false, true},
				.dai_name = "rt722-sdca-aif3",
				.dai_type = SOC_SDW_DAI_TYPE_MIC,
				.dailink = {SDW_UNUSED_DAI_ID, SDW_DMIC_DAI_ID},
				.rtd_init = rt_sdw_dmic_rtd_init,
			},
		},
		.dai_num = 3,
	},
	{
		.part_id = 0x8373,
		.dais = {
			{
				.direction = {true, true},
				.dai_name = "max98373-aif1",
				.dai_type = SOC_SDW_DAI_TYPE_AMP,
				.dailink = {SDW_AMP_OUT_DAI_ID, SDW_AMP_IN_DAI_ID},
				.init = soc_sdw_maxim_init,
				.rtd_init = maxim_sdw_spk_rtd_init,
			},
		},
		.dai_num = 1,
	},
	{
		.part_id = 0x8363,
		.dais = {
			{
				.direction = {true, false},
				.dai_name = "max98363-aif1",
				.dai_type = SOC_SDW_DAI_TYPE_AMP,
				.dailink = {SDW_AMP_OUT_DAI_ID, SDW_UNUSED_DAI_ID},
				.init = soc_sdw_maxim_init,
				.rtd_init = maxim_sdw_spk_rtd_init,
			},
		},
		.dai_num = 1,
	},
	{
		.part_id = 0x5682,
		.dais = {
			{
				.direction = {true, true},
				.dai_name = "rt5682-sdw",
				.dai_type = SOC_SDW_DAI_TYPE_JACK,
				.dailink = {SDW_JACK_OUT_DAI_ID, SDW_JACK_IN_DAI_ID},
				.rtd_init = rt5682_sdw_rtd_init,
			},
		},
		.dai_num = 1,
	},
	{
		.part_id = 0x3556,
		.dais = {
			{
				.direction = {true, true},
				.dai_name = "cs35l56-sdw1",
				.dai_type = SOC_SDW_DAI_TYPE_AMP,
				.dailink = {SDW_AMP_OUT_DAI_ID, SDW_AMP_IN_DAI_ID},
				.init = soc_sdw_cs_amp_init,
				.rtd_init = cs_sdw_spk_rtd_init,
			},
		},
		.dai_num = 1,
	},
	{
		.part_id = 0x4242,
		.dais = {
			{
				.direction = {true, true},
				.dai_name = "cs42l42-sdw",
				.dai_type = SOC_SDW_DAI_TYPE_JACK,
				.dailink = {SDW_JACK_OUT_DAI_ID, SDW_JACK_IN_DAI_ID},
				.rtd_init = cs42l42_sdw_rtd_init,
			},
		},
		.dai_num = 1,
	},
	{
		.part_id = 0x4243,
		.codec_name = "cs42l43-codec",
		.dais = {
			{
				.direction = {true, false},
				.dai_name = "cs42l43-dp5",
				.dai_type = SOC_SDW_DAI_TYPE_JACK,
				.dailink = {SDW_JACK_OUT_DAI_ID, SDW_UNUSED_DAI_ID},
				.rtd_init = cs42l43_sdw_hs_rtd_init,
			},
			{
				.direction = {false, true},
				.dai_name = "cs42l43-dp1",
				.dai_type = SOC_SDW_DAI_TYPE_MIC,
				.dailink = {SDW_UNUSED_DAI_ID, SDW_DMIC_DAI_ID},
				.rtd_init = cs42l43_sdw_dmic_rtd_init,
			},
			{
				.direction = {false, true},
				.dai_name = "cs42l43-dp2",
				.dai_type = SOC_SDW_DAI_TYPE_JACK,
				.dailink = {SDW_UNUSED_DAI_ID, SDW_JACK_IN_DAI_ID},
			},
			{
				.direction = {true, false},
				.dai_name = "cs42l43-dp6",
				.dai_type = SOC_SDW_DAI_TYPE_AMP,
				.dailink = {SDW_AMP_OUT_DAI_ID, SDW_UNUSED_DAI_ID},
				.init = soc_sdw_cs42l43_spk_init,
				.rtd_init = cs42l43_sdw_spk_rtd_init,
				.quirk = SOC_SDW_CODEC_SPKR,
			},
		},
		.dai_num = 4,
	},
	{
		.part_id = 0xaaaa, /* generic codec mockup */
		.version_id = 0,
		.dais = {
			{
				.direction = {true, true},
				.dai_name = "sdw-mockup-aif1",
				.dai_type = SOC_SDW_DAI_TYPE_JACK,
				.dailink = {SDW_JACK_OUT_DAI_ID, SDW_JACK_IN_DAI_ID},
			},
		},
		.dai_num = 1,
	},
	{
		.part_id = 0xaa55, /* headset codec mockup */
		.version_id = 0,
		.dais = {
			{
				.direction = {true, true},
				.dai_name = "sdw-mockup-aif1",
				.dai_type = SOC_SDW_DAI_TYPE_JACK,
				.dailink = {SDW_JACK_OUT_DAI_ID, SDW_JACK_IN_DAI_ID},
			},
		},
		.dai_num = 1,
	},
	{
		.part_id = 0x55aa, /* amplifier mockup */
		.version_id = 0,
		.dais = {
			{
				.direction = {true, true},
				.dai_name = "sdw-mockup-aif1",
				.dai_type = SOC_SDW_DAI_TYPE_AMP,
				.dailink = {SDW_AMP_OUT_DAI_ID, SDW_AMP_IN_DAI_ID},
			},
		},
		.dai_num = 1,
	},
	{
		.part_id = 0x5555,
		.version_id = 0,
		.dais = {
			{
				.dai_name = "sdw-mockup-aif1",
				.direction = {false, true},
				.dai_type = SOC_SDW_DAI_TYPE_MIC,
				.dailink = {SDW_UNUSED_DAI_ID, SDW_DMIC_DAI_ID},
			},
		},
		.dai_num = 1,
	},
};
EXPORT_SYMBOL_NS(codec_info_list, SND_SOC_SDW_UTILS);

int get_sdw_codec_info_list_size(void)
{
	return ARRAY_SIZE(codec_info_list);
};
EXPORT_SYMBOL_NS(get_sdw_codec_info_list_size, SND_SOC_SDW_UTILS);

struct soc_sdw_codec_info *find_sdw_codec_info_part(const u64 adr)
{
	unsigned int part_id, sdw_version;
	int i;

	part_id = SDW_PART_ID(adr);
	sdw_version = SDW_VERSION(adr);
	for (i = 0; i < ARRAY_SIZE(codec_info_list); i++)
		/*
		 * A codec info is for all sdw version with the part id if
		 * version_id is not specified in the codec info.
		 */
		if (part_id == codec_info_list[i].part_id &&
		    (!codec_info_list[i].version_id ||
		     sdw_version == codec_info_list[i].version_id))
			return &codec_info_list[i];

	return NULL;
}
EXPORT_SYMBOL_NS(find_sdw_codec_info_part, SND_SOC_SDW_UTILS);

struct soc_sdw_codec_info *find_sdw_codec_info_acpi(const u8 *acpi_id)
{
	int i;

	if (!acpi_id[0])
		return NULL;

	for (i = 0; i < ARRAY_SIZE(codec_info_list); i++)
		if (!memcmp(codec_info_list[i].acpi_id, acpi_id, ACPI_ID_LEN))
			return &codec_info_list[i];

	return NULL;
}
EXPORT_SYMBOL_NS(find_sdw_codec_info_acpi, SND_SOC_SDW_UTILS);

struct soc_sdw_codec_info *find_sdw_codec_info_dai(const char *dai_name,
						   int *dai_index)
{
	int i, j;

	for (i = 0; i < ARRAY_SIZE(codec_info_list); i++) {
		for (j = 0; j < codec_info_list[i].dai_num; j++) {
			if (!strcmp(codec_info_list[i].dais[j].dai_name, dai_name)) {
				*dai_index = j;
				return &codec_info_list[i];
			}
		}
	}

	return NULL;
}
EXPORT_SYMBOL_NS(find_sdw_codec_info_dai, SND_SOC_SDW_UTILS);

int soc_sdw_rtd_init(struct snd_soc_pcm_runtime *rtd)
{
	struct soc_sdw_codec_info *codec_info;
	struct snd_soc_dai *dai;
	int dai_index;
	int ret;
	int i;

	for_each_rtd_codec_dais(rtd, i, dai) {
		codec_info = find_sdw_codec_info_dai(dai->name, &dai_index);
		if (!codec_info)
			return -EINVAL;

		/*
		 * A codec dai can be connected to different dai links for capture and playback,
		 * but we only need to call the rtd_init function once.
		 * The rtd_init for each codec dai is independent. So, the order of rtd_init
		 * doesn't matter.
		 */
		if (codec_info->dais[dai_index].rtd_init_done)
			continue;
		if (codec_info->dais[dai_index].rtd_init) {
			ret = codec_info->dais[dai_index].rtd_init(rtd);
			if (ret)
				return ret;
		}
		codec_info->dais[dai_index].rtd_init_done = true;
	}

	return 0;
}
EXPORT_SYMBOL_NS(soc_sdw_rtd_init, SND_SOC_SDW_UTILS);

/* these wrappers are only needed to avoid typecast compilation errors */
int asoc_sdw_startup(struct snd_pcm_substream *substream)
{
	return sdw_startup_stream(substream);
}
EXPORT_SYMBOL_NS(asoc_sdw_startup, SND_SOC_SDW_UTILS);

int asoc_sdw_prepare(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct sdw_stream_runtime *sdw_stream;
	struct snd_soc_dai *dai;

	/* Find stream from first CPU DAI */
	dai = snd_soc_rtd_to_cpu(rtd, 0);

	sdw_stream = snd_soc_dai_get_stream(dai, substream->stream);
	if (IS_ERR(sdw_stream)) {
		dev_err(rtd->dev, "no stream found for DAI %s\n", dai->name);
		return PTR_ERR(sdw_stream);
	}

	return sdw_prepare_stream(sdw_stream);
}
EXPORT_SYMBOL_NS(asoc_sdw_prepare, SND_SOC_SDW_UTILS);

int asoc_sdw_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct sdw_stream_runtime *sdw_stream;
	struct snd_soc_dai *dai;
	int ret;

	/* Find stream from first CPU DAI */
	dai = snd_soc_rtd_to_cpu(rtd, 0);

	sdw_stream = snd_soc_dai_get_stream(dai, substream->stream);
	if (IS_ERR(sdw_stream)) {
		dev_err(rtd->dev, "no stream found for DAI %s\n", dai->name);
		return PTR_ERR(sdw_stream);
	}

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
	case SNDRV_PCM_TRIGGER_RESUME:
		ret = sdw_enable_stream(sdw_stream);
		break;

	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_STOP:
		ret = sdw_disable_stream(sdw_stream);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	if (ret)
		dev_err(rtd->dev, "%s trigger %d failed: %d\n", __func__, cmd, ret);

	return ret;
}
EXPORT_SYMBOL_NS(asoc_sdw_trigger, SND_SOC_SDW_UTILS);

int asoc_sdw_hw_params(struct snd_pcm_substream *substream,
		       struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai_link_ch_map *ch_maps;
	int ch = params_channels(params);
	unsigned int ch_mask;
	int num_codecs;
	int step;
	int i;

	if (!rtd->dai_link->ch_maps)
		return 0;

	/* Identical data will be sent to all codecs in playback */
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		ch_mask = GENMASK(ch - 1, 0);
		step = 0;
	} else {
		num_codecs = rtd->dai_link->num_codecs;

		if (ch < num_codecs || ch % num_codecs != 0) {
			dev_err(rtd->dev, "Channels number %d is invalid when codec number = %d\n",
				ch, num_codecs);
			return -EINVAL;
		}

		ch_mask = GENMASK(ch / num_codecs - 1, 0);
		step = hweight_long(ch_mask);
	}

	/*
	 * The captured data will be combined from each cpu DAI if the dai
	 * link has more than one codec DAIs. Set codec channel mask and
	 * ASoC will set the corresponding channel numbers for each cpu dai.
	 */
	for_each_link_ch_maps(rtd->dai_link, i, ch_maps)
		ch_maps->ch_mask = ch_mask << (i * step);

	return 0;
}
EXPORT_SYMBOL_NS(asoc_sdw_hw_params, SND_SOC_SDW_UTILS);

int asoc_sdw_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct sdw_stream_runtime *sdw_stream;
	struct snd_soc_dai *dai;

	/* Find stream from first CPU DAI */
	dai = snd_soc_rtd_to_cpu(rtd, 0);

	sdw_stream = snd_soc_dai_get_stream(dai, substream->stream);
	if (IS_ERR(sdw_stream)) {
		dev_err(rtd->dev, "no stream found for DAI %s\n", dai->name);
		return PTR_ERR(sdw_stream);
	}

	return sdw_deprepare_stream(sdw_stream);
}
EXPORT_SYMBOL_NS(asoc_sdw_hw_free, SND_SOC_SDW_UTILS);

void asoc_sdw_shutdown(struct snd_pcm_substream *substream)
{
	sdw_shutdown_stream(substream);
}
EXPORT_SYMBOL_NS(asoc_sdw_shutdown, SND_SOC_SDW_UTILS);

struct snd_soc_dai *get_sdw_codec_dai_by_name(struct snd_soc_pcm_runtime *rtd,
					      const char * const dai_name[],
					      int num_dais)
{
	struct snd_soc_dai *dai;
	int index;
	int i;

	for (index = 0; index < num_dais; index++)
		for_each_rtd_codec_dais(rtd, i, dai)
			if (strstr(dai->name, dai_name[index])) {
				dev_dbg(rtd->card->dev, "get dai %s\n", dai->name);
				return dai;
			}

	return NULL;
}
EXPORT_SYMBOL_NS(get_sdw_codec_dai_by_name, SND_SOC_SDW_UTILS);

bool is_sdw_unique_device(const struct snd_soc_acpi_link_adr *adr_link,
			  unsigned int sdw_version,
			  unsigned int mfg_id,
			  unsigned int part_id,
			  unsigned int class_id,
			  int index_in_link)
{
	int i;

	for (i = 0; i < adr_link->num_adr; i++) {
		unsigned int sdw1_version, mfg1_id, part1_id, class1_id;
		u64 adr;

		/* skip itself */
		if (i == index_in_link)
			continue;

		adr = adr_link->adr_d[i].adr;

		sdw1_version = SDW_VERSION(adr);
		mfg1_id = SDW_MFG_ID(adr);
		part1_id = SDW_PART_ID(adr);
		class1_id = SDW_CLASS_ID(adr);

		if (sdw_version == sdw1_version &&
		    mfg_id == mfg1_id &&
		    part_id == part1_id &&
		    class_id == class1_id)
			return false;
	}

	return true;
}
EXPORT_SYMBOL_NS(is_sdw_unique_device, SND_SOC_SDW_UTILS);

const char *get_sdw_codec_name(struct device *dev,
			       const struct soc_sdw_codec_info *codec_info,
			       const struct snd_soc_acpi_link_adr *adr_link,
			       int adr_index)
{
	u64 adr = adr_link->adr_d[adr_index].adr;
	unsigned int sdw_version = SDW_VERSION(adr);
	unsigned int link_id = SDW_DISCO_LINK_ID(adr);
	unsigned int unique_id = SDW_UNIQUE_ID(adr);
	unsigned int mfg_id = SDW_MFG_ID(adr);
	unsigned int part_id = SDW_PART_ID(adr);
	unsigned int class_id = SDW_CLASS_ID(adr);

	if (codec_info->codec_name)
		return devm_kstrdup(dev, codec_info->codec_name, GFP_KERNEL);
	else if (is_sdw_unique_device(adr_link, sdw_version, mfg_id, part_id,
				      class_id, adr_index))
		return devm_kasprintf(dev, GFP_KERNEL, "sdw:0:%01x:%04x:%04x:%02x",
				      link_id, mfg_id, part_id, class_id);
	else
		return devm_kasprintf(dev, GFP_KERNEL, "sdw:0:%01x:%04x:%04x:%02x:%01x",
				      link_id, mfg_id, part_id, class_id, unique_id);

	return NULL;
}
EXPORT_SYMBOL_NS(get_sdw_codec_name, SND_SOC_SDW_UTILS);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("SoundWire ASoC helpers");
