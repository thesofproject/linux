// SPDX-License-Identifier: GPL-2.0-only
// Copyright(c) 2024 Advanced Micro Devices, Inc.
/*
 *  amd_sof_sdw - ASoC Machine driver for AMD SoundWire platforms
 */

#include <linux/bitmap.h>
#include <linux/device.h>
#include <linux/dmi.h>
#include <linux/module.h>
#include <linux/soundwire/sdw.h>
#include <linux/soundwire/sdw_type.h>
#include <sound/soc.h>
#include <sound/soc-acpi.h>
#include "soc_amd_sdw_common.h"
#include "../../codecs/rt711.h"

unsigned long sof_sdw_quirk = RT711_JD1;
static int quirk_override = -1;
module_param_named(quirk, quirk_override, int, 0444);
MODULE_PARM_DESC(quirk, "Board-specific quirk override");

static void log_quirks(struct device *dev)
{
	if (SOC_JACK_JDSRC(sof_sdw_quirk))
		dev_dbg(dev, "quirk realtek,jack-detect-source %ld\n",
			SOC_JACK_JDSRC(sof_sdw_quirk));
	if (sof_sdw_quirk & SOC_SDW_FOUR_SPK)
		dev_dbg(dev, "quirk SOC_SDW_FOUR_SPK enabled\n");
	if (sof_sdw_quirk & SOC_SDW_ACP_DMIC)
		dev_dbg(dev, "quirk SOC_SDW_ACP_DMIC enabled\n");
	if (sof_sdw_quirk & SOC_SDW_NO_AGGREGATION)
		dev_dbg(dev, "quirk SOC_SDW_NO_AGGREGATION enabled\n");
}

static int sof_sdw_quirk_cb(const struct dmi_system_id *id)
{
	sof_sdw_quirk = (unsigned long)id->driver_data;
	return 1;
}

static const struct dmi_system_id sof_sdw_quirk_table[] = {
	{
		.callback = sof_sdw_quirk_cb,
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "AMD"),
		},
		.driver_data = (void *)RT711_JD2,
	},
	{}
};

static struct snd_soc_dai_link_component platform_component[] = {
	{
		/* name might be overridden during probe */
		.name = "0000:04:00.5",
	}
};

static const struct snd_soc_ops sdw_ops = {
	.startup = asoc_sdw_startup,
	.prepare = asoc_sdw_prepare,
	.trigger = asoc_sdw_trigger,
	.hw_params = asoc_sdw_hw_params,
	.hw_free = asoc_sdw_hw_free,
	.shutdown = asoc_sdw_shutdown,
};

/*
 * get BE dailink number and CPU DAI number based on sdw link adr.
 * Since some sdw slaves may be aggregated, the CPU DAI number
 * may be larger than the number of BE dailinks.
 */
static int get_dailink_info(struct device *dev,
			    const struct snd_soc_acpi_link_adr *adr_link,
			    int *sdw_be_num, int *codecs_num)
{
	bool group_visited[SDW_MAX_GROUPS];
	bool no_aggregation;
	int i;
	int j;

	no_aggregation = sof_sdw_quirk & SOC_SDW_NO_AGGREGATION;
	*sdw_be_num  = 0;

	if (!adr_link)
		return -EINVAL;

	for (i = 0; i < SDW_MAX_GROUPS; i++)
		group_visited[i] = false;

	for (; adr_link->num_adr; adr_link++) {
		const struct snd_soc_acpi_endpoint *endpoint;
		struct soc_sdw_codec_info *codec_info;
		int stream;
		u64 adr;

		/* make sure the link mask has a single bit set */
		if (!is_power_of_2(adr_link->mask))
			return -EINVAL;

		for (i = 0; i < adr_link->num_adr; i++) {
			adr = adr_link->adr_d[i].adr;
			codec_info = find_sdw_codec_info_part(adr);
			if (!codec_info)
				return -EINVAL;

			*codecs_num += codec_info->dai_num;

			if (!adr_link->adr_d[i].name_prefix) {
				dev_err(dev, "codec 0x%llx does not have a name prefix\n",
					adr_link->adr_d[i].adr);
				return -EINVAL;
			}

			endpoint = adr_link->adr_d[i].endpoints;
			if (endpoint->aggregated && !endpoint->group_id) {
				dev_err(dev, "invalid group id on link %x\n",
					adr_link->mask);
				return -EINVAL;
			}

			for (j = 0; j < codec_info->dai_num; j++) {
				/* count DAI number for playback and capture */
				for_each_pcm_streams(stream) {
					if (!codec_info->dais[j].direction[stream])
						continue;

					/* count BE for each non-aggregated slave or group */
					if (!endpoint->aggregated || no_aggregation ||
					    !group_visited[endpoint->group_id])
						(*sdw_be_num)++;
				}
			}

			if (endpoint->aggregated)
				group_visited[endpoint->group_id] = true;
		}
	}
	return 0;
}

static void init_dai_link(struct device *dev, struct snd_soc_dai_link *dai_links,
			  int *be_id, char *name, int playback, int capture,
			  struct snd_soc_dai_link_component *cpus, int cpus_num,
			  struct snd_soc_dai_link_component *codecs, int codecs_num,
			  int (*init)(struct snd_soc_pcm_runtime *rtd),
			  const struct snd_soc_ops *ops)
{
	dev_dbg(dev, "create dai link %s, id %d\n", name, *be_id);
	dai_links->id = (*be_id)++;
	dai_links->name = name;
	dai_links->platforms = platform_component;
	dai_links->num_platforms = ARRAY_SIZE(platform_component);
	dai_links->no_pcm = 1;
	dai_links->cpus = cpus;
	dai_links->num_cpus = cpus_num;
	dai_links->codecs = codecs;
	dai_links->num_codecs = codecs_num;
	dai_links->dpcm_playback = playback;
	dai_links->dpcm_capture = capture;
	dai_links->init = init;
	dai_links->ops = ops;
}

static int init_simple_dai_link(struct device *dev, struct snd_soc_dai_link *dai_links,
				int *be_id, char *name, int playback, int capture,
				const char *cpu_dai_name,
				const char *codec_name, const char *codec_dai_name,
				int (*init)(struct snd_soc_pcm_runtime *rtd),
				const struct snd_soc_ops *ops)
{
	struct snd_soc_dai_link_component *dlc;

	/* Allocate two DLCs one for the CPU, one for the CODEC */
	dlc = devm_kcalloc(dev, 2, sizeof(*dlc), GFP_KERNEL);
	if (!dlc || !name || !cpu_dai_name || !codec_name || !codec_dai_name)
		return -ENOMEM;

	dlc[0].dai_name = cpu_dai_name;

	dlc[1].name = codec_name;
	dlc[1].dai_name = codec_dai_name;

	init_dai_link(dev, dai_links, be_id, name, playback, capture,
		      &dlc[0], 1, &dlc[1], 1, init, ops);

	return 0;
}

static int fill_sdw_codec_dlc(struct device *dev,
			      const struct snd_soc_acpi_link_adr *adr_link,
			      struct snd_soc_dai_link_component *codec,
			      int adr_index, int dai_index)
{
	u64 adr = adr_link->adr_d[adr_index].adr;
	struct soc_sdw_codec_info *codec_info;

	codec_info = find_sdw_codec_info_part(adr);
	if (!codec_info)
		return -EINVAL;

	codec->name = get_sdw_codec_name(dev, codec_info, adr_link, adr_index);
	if (!codec->name)
		return -ENOMEM;

	codec->dai_name = codec_info->dais[dai_index].dai_name;
	dev_err(dev, "codec->dai_name:%s\n", codec->dai_name);
	return 0;
}

static int set_codec_init_func(struct snd_soc_card *card,
			       const struct snd_soc_acpi_link_adr *adr_link,
			       struct snd_soc_dai_link *dai_links,
			       bool playback, int group_id, int adr_index, int dai_index)
{
	int i = adr_index;

	do {
		/*
		 * Initialize the codec. If codec is part of an aggregated
		 * group (group_id>0), initialize all codecs belonging to
		 * same group.
		 * The first link should start with adr_link->adr_d[adr_index]
		 * because that is the device that we want to initialize and
		 * we should end immediately if it is not aggregated (group_id=0)
		 */
		for ( ; i < adr_link->num_adr; i++) {
			struct soc_sdw_codec_info *codec_info;

			codec_info = find_sdw_codec_info_part(adr_link->adr_d[i].adr);
			if (!codec_info)
				return -EINVAL;

			/* The group_id is > 0 iff the codec is aggregated */
			if (adr_link->adr_d[i].endpoints->group_id != group_id)
				continue;
			if (codec_info->dais[dai_index].init)
				codec_info->dais[dai_index].init(card,
								 dai_links,
								 codec_info,
								 playback);

			if (!group_id)
				return 0;
		}

		i = 0;
		adr_link++;
	} while (adr_link->mask);

	return 0;
}

/*
 * check endpoint status in slaves and gather link ID for all slaves in
 * the same group to generate different CPU DAI. Now only support
 * one sdw link with all slaves set with only single group id.
 *
 * one slave on one sdw link with aggregated = 0
 * one sdw BE DAI <---> one-cpu DAI <---> one-codec DAI
 *
 * two or more slaves on one sdw link with aggregated = 1
 * one sdw BE DAI  <---> one-cpu DAI <---> multi-codec DAIs
 */
static int get_slave_info(const struct snd_soc_acpi_link_adr *adr_link,
			  struct device *dev, int *cpu_dai_id, int *cpu_dai_num,
			  int *codec_num, unsigned int *group_id,
			  int adr_index)
{
	bool no_aggregation = sof_sdw_quirk & SOC_SDW_NO_AGGREGATION;
	int i;

	if (!adr_link->adr_d[adr_index].endpoints->aggregated || no_aggregation) {
		cpu_dai_id[0] = ffs(adr_link->mask) - 1;
		*cpu_dai_num = 1;
		*codec_num = 1;
		*group_id = 0;
		return 0;
	}

	*codec_num = 0;
	*cpu_dai_num = 0;
	*group_id = adr_link->adr_d[adr_index].endpoints->group_id;

	/* Count endpoints with the same group_id in the adr_link */
	for (; adr_link && adr_link->num_adr; adr_link++) {
		unsigned int link_codecs = 0;

		for (i = 0; i < adr_link->num_adr; i++) {
			if (adr_link->adr_d[i].endpoints->aggregated &&
			    adr_link->adr_d[i].endpoints->group_id == *group_id)
				link_codecs++;
		}

		if (link_codecs) {
			*codec_num += link_codecs;

			if (*cpu_dai_num >= SDW_MAX_CPU_DAIS) {
				dev_err(dev, "cpu_dai_id array overflowed\n");
				return -EINVAL;
			}

			cpu_dai_id[(*cpu_dai_num)++] = ffs(adr_link->mask) - 1;
		}
	}

	return 0;
}

static int get_acp63_cpu_pin_id(u32 sdw_link_id, int be_id, int *cpu_pin_id, struct device *dev)
{
	switch (sdw_link_id) {
	case AMD_SDW0:
		switch (be_id) {
		case SDW_JACK_OUT_DAI_ID:
			*cpu_pin_id = SW0_AUDIO0_TX;
			break;
		case SDW_JACK_IN_DAI_ID:
			*cpu_pin_id = SW0_AUDIO0_RX;
			break;
		case SDW_AMP_OUT_DAI_ID:
			*cpu_pin_id = SW0_AUDIO1_TX;
			break;
		case SDW_AMP_IN_DAI_ID:
			*cpu_pin_id = SW0_AUDIO1_RX;
			break;
		case SDW_DMIC_DAI_ID:
			*cpu_pin_id = SW0_AUDIO2_RX;
			break;
		default:
			dev_err(dev, "Invalid be id:%d\n", be_id);
			return -EINVAL;
		}
		break;
	case AMD_SDW1:
		switch (be_id) {
		case SDW_JACK_OUT_DAI_ID:
		case SDW_AMP_OUT_DAI_ID:
			*cpu_pin_id = SW1_AUDIO0_TX;
			break;
		case SDW_JACK_IN_DAI_ID:
		case SDW_AMP_IN_DAI_ID:
		case SDW_DMIC_DAI_ID:
			*cpu_pin_id = SW1_AUDIO0_RX;
			break;
		default:
			dev_err(dev, "invalid be_id:%d\n", be_id);
			return -EINVAL;
		}
		break;
	default:
		dev_err(dev, "Invalid link id:%d\n", sdw_link_id);
		return -EINVAL;
	}
	return 0;
}

static const char * const type_strings[] = {"SimpleJack", "SmartAmp", "SmartMic"};
static int create_sdw_dailink(struct snd_soc_card *card,
			      struct snd_soc_dai_link **dai_links,
			      const struct snd_soc_acpi_link_adr *adr_link,
			      struct snd_soc_codec_conf **codec_conf,
			      int *be_id, int adr_index, int dai_index)
{
	struct mc_private *ctx = snd_soc_card_get_drvdata(card);
	struct amd_mc_ctx *amd_ctx = (struct amd_mc_ctx *)ctx->amd_mc_private;
	struct device *dev = card->dev;
	const struct snd_soc_acpi_link_adr *adr_link_next;
	struct snd_soc_dai_link_ch_map *sdw_codec_ch_maps;
	struct snd_soc_dai_link_component *codecs;
	struct snd_soc_dai_link_component *cpus;
	struct soc_sdw_codec_info *codec_info;
	int cpu_dai_id[SDW_MAX_CPU_DAIS];
	int cpu_dai_num;
	unsigned int group_id;
	unsigned int sdw_link_id;
	int codec_dlc_index = 0;
	int codec_num;
	int stream;
	int i = 0;
	int j, k;
	int ret;
	int cpu_pin_id;

	ret = get_slave_info(adr_link, dev, cpu_dai_id, &cpu_dai_num, &codec_num,
			     &group_id, adr_index);
	if (ret)
		return ret;
	codecs = devm_kcalloc(dev, codec_num, sizeof(*codecs), GFP_KERNEL);
	if (!codecs)
		return -ENOMEM;

	sdw_codec_ch_maps = devm_kcalloc(dev, codec_num,
					 sizeof(*sdw_codec_ch_maps), GFP_KERNEL);
	if (!sdw_codec_ch_maps)
		return -ENOMEM;

	/* generate codec name on different links in the same group */
	j = adr_index;
	for (adr_link_next = adr_link; adr_link_next && adr_link_next->num_adr &&
	     i < cpu_dai_num; adr_link_next++) {
		/* skip the link excluded by this processed group */
		if (cpu_dai_id[i] != ffs(adr_link_next->mask) - 1)
			continue;

		/* j reset after loop, adr_index only applies to first link */
		for (k = 0 ; (j < adr_link_next->num_adr) && (k < codec_num) ; j++, k++) {
			const struct snd_soc_acpi_endpoint *endpoints;

			endpoints = adr_link_next->adr_d[j].endpoints;
			if (group_id && (!endpoints->aggregated ||
					 endpoints->group_id != group_id))
				continue;

			/* sanity check */
			if (*codec_conf >= card->codec_conf + card->num_configs) {
				dev_err(dev, "codec_conf array overflowed\n");
				return -EINVAL;
			}

			ret = fill_sdw_codec_dlc(dev, adr_link_next,
						 &codecs[codec_dlc_index],
						 j, dai_index);
			if (ret)
				return ret;
			(*codec_conf)->dlc = codecs[codec_dlc_index];
			(*codec_conf)->name_prefix = adr_link_next->adr_d[j].name_prefix;

			sdw_codec_ch_maps[codec_dlc_index].cpu = i;
			sdw_codec_ch_maps[codec_dlc_index].codec = codec_dlc_index;

			codec_dlc_index++;
			(*codec_conf)++;
		}
		j = 0;

		/* check next link to create codec dai in the processed group */
		i++;
	}

	  /* find codec info to create BE DAI */
	codec_info = find_sdw_codec_info_part(adr_link->adr_d[adr_index].adr);
	if (!codec_info)
		return -EINVAL;

	ctx->ignore_internal_dmic |= codec_info->ignore_internal_dmic;

	sdw_link_id = (adr_link->adr_d[adr_index].adr) >> 48;
	for_each_pcm_streams(stream) {
		char *name, *cpu_name;
		int playback, capture;
		static const char * const sdw_stream_name[] = {
			"SDW%d-PIN%d-PLAYBACK",
			"SDW%d-PIN%d-CAPTURE",
			"SDW%d-PIN%d-PLAYBACK-%s",
			"SDW%d-PIN%d-CAPTURE-%s",
		};

		if (!codec_info->dais[dai_index].direction[stream])
			continue;

		*be_id = codec_info->dais[dai_index].dailink[stream];
		if (*be_id < 0) {
			dev_err(dev, "Invalid dailink id %d\n", *be_id);
			return -EINVAL;
		}
		switch (amd_ctx->acp_rev) {
		case ACP63_PCI_REV:
			ret = get_acp63_cpu_pin_id(sdw_link_id, *be_id, &cpu_pin_id, dev);
			if (ret)
				return ret;
			break;
		default:
			return -EINVAL;
		}
		/* create stream name according to first link id */
		if (ctx->append_dai_type) {
			name = devm_kasprintf(dev, GFP_KERNEL,
					      sdw_stream_name[stream + 2], sdw_link_id, cpu_pin_id,
					      type_strings[codec_info->dais[dai_index].dai_type]);
		} else {
			name = devm_kasprintf(dev, GFP_KERNEL,
					      sdw_stream_name[stream], sdw_link_id, cpu_pin_id);
		}
		if (!name)
			return -ENOMEM;

		cpus = devm_kcalloc(dev, cpu_dai_num, sizeof(*cpus), GFP_KERNEL);
		if (!cpus)
			return -ENOMEM;
		/*
		 * generate CPU DAI name base on the sdw link ID and
		 * cpu pin id according to sdw dai driver.
		 */
		for (k = 0; k < cpu_dai_num; k++) {
			cpu_name = devm_kasprintf(dev, GFP_KERNEL,
						  "SDW%d Pin%d", sdw_link_id, cpu_pin_id);
			if (!cpu_name)
				return -ENOMEM;

			cpus[k].dai_name = cpu_name;
		}

		playback = (stream == SNDRV_PCM_STREAM_PLAYBACK);
		capture = (stream == SNDRV_PCM_STREAM_CAPTURE);
		init_dai_link(dev, *dai_links, be_id, name,
			      playback, capture,
			      cpus, cpu_dai_num,
			      codecs, codec_num,
			      soc_sdw_rtd_init, &sdw_ops);
		/*
		 * SoundWire DAILINKs use 'stream' functions and Bank Switch operations
		 * based on wait_for_completion(), tag them as 'nonatomic'.
		 */
		(*dai_links)->nonatomic = true;
		(*dai_links)->ch_maps = sdw_codec_ch_maps;

		ret = set_codec_init_func(card, adr_link, *dai_links,
					  playback, group_id, adr_index, dai_index);
		if (ret < 0) {
			dev_err(dev, "failed to init codec 0x%x\n", codec_info->part_id);
			return ret;
		}

		(*dai_links)++;
	}
	return 0;
}

static int create_dmic_dailinks(struct snd_soc_card *card,
				struct snd_soc_dai_link **dai_links, int *be_id)
{
	struct device *dev = card->dev;
	int ret;

	ret = init_simple_dai_link(dev, *dai_links, be_id, "acp-dmic-codec",
				   0, 1, // DMIC only supports capture
				   "acp-sof-dmic", "dmic-codec", "dmic-hifi",
				   asoc_sdw_dmic_init, NULL);
	if (ret)
		return ret;

	(*dai_links)++;

	return 0;
}

static int sof_card_dai_links_create(struct snd_soc_card *card)
{
	struct device *dev = card->dev;
	struct snd_soc_acpi_mach *mach = dev_get_platdata(card->dev);
	int sdw_be_num = 0, dmic_num = 0;
	struct mc_private *ctx = snd_soc_card_get_drvdata(card);
	struct snd_soc_acpi_mach_params *mach_params = &mach->mach_params;
	const struct snd_soc_acpi_link_adr *adr_link = mach_params->links;
	bool aggregation = !(sof_sdw_quirk & SOC_SDW_NO_AGGREGATION);
	struct snd_soc_codec_conf *codec_conf;
	int codec_conf_num = 0;
	bool group_generated[SDW_MAX_GROUPS] = { };
	struct snd_soc_dai_link *dai_links;
	struct soc_sdw_codec_info *codec_info;
	int num_links;
	int i, j, be_id = 0;
	int ret;

	ret = get_dailink_info(dev, adr_link, &sdw_be_num, &codec_conf_num);
	if (ret < 0) {
		dev_err(dev, "failed to get sdw link info %d\n", ret);
		return ret;
	}

	/* enable dmic */
	if (sof_sdw_quirk & SOC_SDW_ACP_DMIC || mach_params->dmic_num)
		dmic_num = 1;

	dev_dbg(dev, "sdw %d, dmic %d", sdw_be_num, dmic_num);

	/* allocate BE dailinks */
	num_links = sdw_be_num + dmic_num;
	dai_links = devm_kcalloc(dev, num_links, sizeof(*dai_links), GFP_KERNEL);
	if (!dai_links)
		return -ENOMEM;

	/* allocate codec conf, will be populated when dailinks are created */
	codec_conf = devm_kcalloc(dev, codec_conf_num, sizeof(*codec_conf),
				  GFP_KERNEL);
	if (!codec_conf)
		return -ENOMEM;

	card->dai_link = dai_links;
	card->num_links = num_links;
	card->codec_conf = codec_conf;
	card->num_configs = codec_conf_num;

	/* SDW */
	if (!sdw_be_num)
		goto DMIC;

	for (; adr_link->num_adr; adr_link++) {
		/*
		 * If there are two or more different devices on the same sdw link, we have to
		 * append the codec type to the dai link name to prevent duplicated dai link name.
		 * The same type devices on the same sdw link will be in the same
		 * snd_soc_acpi_adr_device array. They won't be described in different adr_links.
		 */
		for (i = 0; i < adr_link->num_adr; i++) {
			/* find codec info to get dai_num */
			codec_info = find_sdw_codec_info_part(adr_link->adr_d[i].adr);
			if (!codec_info)
				return -EINVAL;
			if (codec_info->dai_num > 1) {
				ctx->append_dai_type = true;
				goto out;
			}
			for (j = 0; j < i; j++) {
				if ((SDW_PART_ID(adr_link->adr_d[i].adr) !=
				    SDW_PART_ID(adr_link->adr_d[j].adr)) ||
				    (SDW_MFG_ID(adr_link->adr_d[i].adr) !=
				    SDW_MFG_ID(adr_link->adr_d[j].adr))) {
					ctx->append_dai_type = true;
					goto out;
				}
			}
		}
	}
out:

	/* generate DAI links by each sdw link */
	for (adr_link = mach_params->links ; adr_link->num_adr; adr_link++) {
		for (i = 0; i < adr_link->num_adr; i++) {
			const struct snd_soc_acpi_endpoint *endpoint;

			endpoint = adr_link->adr_d[i].endpoints;

			/* this group has been generated */
			if (endpoint->aggregated &&
			    group_generated[endpoint->group_id])
				continue;

			/* find codec info to get dai_num */
			codec_info = find_sdw_codec_info_part(adr_link->adr_d[i].adr);
			if (!codec_info)
				return -EINVAL;

			for (j = 0; j < codec_info->dai_num ; j++) {
				int current_be_id;

				ret = create_sdw_dailink(card, &dai_links, adr_link,
							 &codec_conf, &current_be_id,
							 i, j);
				if (ret < 0) {
					dev_err(dev,
						"failed to create dai link %d on 0x%x\n",
						j, codec_info->part_id);
					return ret;
				}
			/* Update the be_id to match the highest ID used for SDW link */
				if (be_id < current_be_id)
					be_id = current_be_id;
			}

			if (aggregation && endpoint->aggregated)
				group_generated[endpoint->group_id] = true;
		}
	}

DMIC:
	/* dmic */
	if (dmic_num > 0) {
		if (ctx->ignore_internal_dmic) {
			dev_warn(dev, "Ignoring ACP DMIC\n");
		} else {
			be_id = SDW_DMIC_DAI_ID;
			ret = create_dmic_dailinks(card, &dai_links, &be_id);
			if (ret)
				return ret;
		}
	}

	WARN_ON(dai_links != card->dai_link + card->num_links);
	return 0;
}

/* SoC card */
static const char sdw_card_long_name[] = "AMD Soundwire SOF";

static struct snd_soc_card card_sof_sdw = {
	.name = "amd-soundwire",
	.owner = THIS_MODULE,
	.late_probe = soc_sdw_card_late_probe,
};

static int mc_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card = &card_sof_sdw;
	struct snd_soc_acpi_mach *mach = dev_get_platdata(&pdev->dev);
	int codec_info_list_size = get_sdw_codec_info_list_size();
	struct amd_mc_ctx *amd_ctx;
	struct mc_private *ctx;
	int amp_num = 0, i;
	int ret;

	card->dev = &pdev->dev;

	dev_dbg(card->dev, "Entry\n");
	amd_ctx = devm_kzalloc(card->dev, sizeof(*amd_ctx), GFP_KERNEL);
	if (!amd_ctx)
		return -ENOMEM;

	amd_ctx->acp_rev = mach->mach_params.subsystem_rev;
	amd_ctx->max_sdw_links = SDW_MAX_LINKS;
	ctx = devm_kzalloc(card->dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->amd_mc_private = amd_ctx;
	card->dev = &pdev->dev;
	snd_soc_card_set_drvdata(card, ctx);

	dmi_check_system(sof_sdw_quirk_table);

	if (quirk_override != -1) {
		dev_info(card->dev, "Overriding quirk 0x%lx => 0x%x\n",
			 sof_sdw_quirk, quirk_override);
		sof_sdw_quirk = quirk_override;
	}

	log_quirks(card->dev);

	/* reset amp_num to ensure amp_num++ starts from 0 in each probe */
	for (i = 0; i < codec_info_list_size; i++)
		codec_info_list[i].amp_num = 0;

	ret = sof_card_dai_links_create(card);
	if (ret < 0)
		return ret;

	/*
	 * the default amp_num is zero for each codec and
	 * amp_num will only be increased for active amp
	 * codecs on used platform
	 */
	for (i = 0; i < codec_info_list_size; i++)
		amp_num += codec_info_list[i].amp_num;

	card->long_name = sdw_card_long_name;

	/* Register the card */
	ret = devm_snd_soc_register_card(card->dev, card);
	if (ret) {
		dev_err_probe(card->dev, ret, "snd_soc_register_card failed %d\n", ret);
		mc_dailink_exit_loop(card);
		return ret;
	}

	platform_set_drvdata(pdev, card);

	return ret;
}

static void mc_remove(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);

	mc_dailink_exit_loop(card);
}

static const struct platform_device_id mc_id_table[] = {
	{ "amd_sof_sdw", },
	{}
};
MODULE_DEVICE_TABLE(platform, mc_id_table);

static struct platform_driver sof_sdw_driver = {
	.driver = {
		.name = "amd_sof_sdw",
		.pm = &snd_soc_pm_ops,
	},
	.probe = mc_probe,
	.remove_new = mc_remove,
	.id_table = mc_id_table,
};

module_platform_driver(sof_sdw_driver);

MODULE_DESCRIPTION("ASoC AMD SoundWire Generic Machine driver");
MODULE_AUTHOR("Vijendar Mukunda <Vijendar.Mukunda@amd.com");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:amd_sof_sdw");
MODULE_IMPORT_NS(SND_SOC_SDW_UTILS);
