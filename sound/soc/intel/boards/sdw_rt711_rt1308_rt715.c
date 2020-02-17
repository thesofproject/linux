// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2019 Intel Corporation

/*
 *  sdw_rt711_rt1308_rt715 - ASOC Machine driver for Intel SoundWire platforms
 * connected to 3 Realtek devices
 */

#include <linux/acpi.h>
#include <linux/async.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/io.h>
#include <linux/dmi.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/soundwire/sdw.h>
#include <linux/soundwire/sdw_type.h>
#include <sound/jack.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-acpi.h>
#include "../../codecs/hdac_hdmi.h"
#include "../../codecs/rt1308.h"
#include "hda_dsp_common.h"

/* comment out this define for mono configurations */

#define MAX_NO_PROPS 2

enum {
	SOF_RT711_JD_SRC_JD1 = 1,
	SOF_RT711_JD_SRC_JD2 = 2,
};

enum {
	SOF_PRE_TGL_HDMI_COUNT = 3,
	SOF_TGL_HDMI_COUNT = 4,
};

enum {
	SOF_I2S_SSP0 = BIT(0),
	SOF_I2S_SSP1 = BIT(1),
	SOF_I2S_SSP2 = BIT(2),
	SOF_I2S_SSP3 = BIT(3),
	SOF_I2S_SSP4 = BIT(4),
	SOF_I2S_SSP5 = BIT(5),
};

#define SOF_RT711_JDSRC(quirk)		((quirk) & GENMASK(1, 0))
#define SOF_SDW_MONO_SPK		BIT(2)
#define SOF_SDW_TGL_HDMI		BIT(3)
#define SOF_SDW_PCH_DMIC		BIT(4)
#define SOF_SSP_PORT(x)		(((x) & GENMASK(5, 0)) << 5)
#define SOF_SSP_GET_PORT(quirk)	(((quirk) >> 5) & GENMASK(5, 0))

static unsigned long sof_rt711_rt1308_rt715_quirk = SOF_RT711_JD_SRC_JD1;

struct mc_private {
	struct list_head hdmi_pcm_list;
	bool common_hdmi_codec_drv;
	struct snd_soc_jack sdw_headset;
};

struct codec_info {
	const int id;
	int amp_num;
	const u8 acpi_id[ACPI_ID_LEN];
	const bool direction[2]; // playback & capture support
	const char *dai_name;
	const struct snd_soc_ops *ops;

	void (*init)(const struct snd_soc_acpi_link_adr *link,
		     struct snd_soc_dai_link *dai_links,
		     struct codec_info *info,
		     bool playback);
};

#if IS_ENABLED(CONFIG_SND_HDA_CODEC_HDMI)
static struct snd_soc_jack hdmi[3];

struct hdmi_pcm {
	struct list_head head;
	struct snd_soc_dai *codec_dai;
	int device;
};

static int hdmi_init(struct snd_soc_pcm_runtime *rtd)
{
	struct mc_private *ctx = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *dai = rtd->codec_dai;
	struct hdmi_pcm *pcm;

	pcm = devm_kzalloc(rtd->card->dev, sizeof(*pcm), GFP_KERNEL);
	if (!pcm)
		return -ENOMEM;

	/* dai_link id is 1:1 mapped to the PCM device */
	pcm->device = rtd->dai_link->id;
	pcm->codec_dai = dai;

	list_add_tail(&pcm->head, &ctx->hdmi_pcm_list);

	return 0;
}

#define NAME_SIZE	32
static int card_late_probe(struct snd_soc_card *card)
{
	struct mc_private *ctx = snd_soc_card_get_drvdata(card);
	struct hdmi_pcm *pcm;
	struct snd_soc_component *component = NULL;
	int err, i = 0;
	char jack_name[NAME_SIZE];

	pcm = list_first_entry(&ctx->hdmi_pcm_list, struct hdmi_pcm,
			       head);
	component = pcm->codec_dai->component;

	if (ctx->common_hdmi_codec_drv)
		return hda_dsp_hdmi_build_controls(card, component);

	list_for_each_entry(pcm, &ctx->hdmi_pcm_list, head) {
		component = pcm->codec_dai->component;
		snprintf(jack_name, sizeof(jack_name),
			 "HDMI/DP, pcm=%d Jack", pcm->device);
		err = snd_soc_card_jack_new(card, jack_name,
					    SND_JACK_AVOUT, &hdmi[i],
					    NULL, 0);

		if (err)
			return err;

		err = snd_jack_add_new_kctl(hdmi[i].jack,
					    jack_name, SND_JACK_AVOUT);
		if (err)
			dev_warn(component->dev, "failed creating Jack kctl\n");

		err = hdac_hdmi_jack_init(pcm->codec_dai, pcm->device,
					  &hdmi[i]);
		if (err < 0)
			return err;

		i++;
	}

	if (!component)
		return -EINVAL;

	return hdac_hdmi_jack_port_init(component, &card->dapm);
}
#else
static int card_late_probe(struct snd_soc_card *card)
{
	return 0;
}
#endif

static struct snd_soc_jack_pin sdw_jack_pins[] = {
	{
		.pin    = "Headphone",
		.mask   = SND_JACK_HEADPHONE,
	},
	{
		.pin    = "Headset Mic",
		.mask   = SND_JACK_MICROPHONE,
	},
};

static int headset_init(struct snd_soc_pcm_runtime *rtd)
{
	struct mc_private *ctx = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_component *component = rtd->codec_dai->component;
	struct snd_soc_jack *jack;
	int ret;

	ret = snd_soc_card_jack_new(rtd->card, "Headset Jack",
				    SND_JACK_HEADSET | SND_JACK_BTN_0 |
				    SND_JACK_BTN_1 | SND_JACK_BTN_2 |
				    SND_JACK_BTN_3,
				    &ctx->sdw_headset,
				    sdw_jack_pins,
				    ARRAY_SIZE(sdw_jack_pins));
	if (ret) {
		dev_err(rtd->card->dev, "Headset Jack creation failed: %d\n",
			ret);
		return ret;
	}

	jack = &ctx->sdw_headset;

	snd_jack_set_key(jack->jack, SND_JACK_BTN_0, KEY_VOLUMEUP);
	snd_jack_set_key(jack->jack, SND_JACK_BTN_1, KEY_PLAYPAUSE);
	snd_jack_set_key(jack->jack, SND_JACK_BTN_2, KEY_VOLUMEDOWN);
	snd_jack_set_key(jack->jack, SND_JACK_BTN_3, KEY_VOICECOMMAND);

	ret = snd_soc_component_set_jack(component, jack, NULL);

	if (ret)
		dev_err(rtd->card->dev, "Headset Jack call-back failed: %d\n",
			ret);

	return ret;
}

static int sof_rt711_rt1308_rt715_quirk_cb(const struct dmi_system_id *id)
{
	sof_rt711_rt1308_rt715_quirk = (unsigned long)id->driver_data;
	return 1;
}

static const struct dmi_system_id sof_sdw_rt711_rt1308_rt715_quirk_table[] = {
	{
		.callback = sof_rt711_rt1308_rt715_quirk_cb,
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Latitude"),
		},
		.driver_data = (void *)(SOF_RT711_JD_SRC_JD2 |
					SOF_SDW_MONO_SPK),
	},
	{
		.callback = sof_rt711_rt1308_rt715_quirk_cb,
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc"),
			DMI_MATCH(DMI_PRODUCT_NAME, "XPS"),
		},
		.driver_data = (void *)(SOF_RT711_JD_SRC_JD2),
	},
	{
		.callback = sof_rt711_rt1308_rt715_quirk_cb,
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Intel Corporation"),
			DMI_MATCH(DMI_PRODUCT_NAME,
				  "Tiger Lake Client Platform"),
		},
		.driver_data = (void *)(SOF_RT711_JD_SRC_JD1 |
				SOF_SDW_TGL_HDMI | SOF_SDW_PCH_DMIC |
				SOF_SSP_PORT(SOF_I2S_SSP2)),
	},
	{}
};

/*
 * Note this MUST be called before snd_soc_register_card(), so that the props
 * are in place before the codec component driver's probe function parses them.
 */
static int sof_rt711_add_codec_device_props(const char *sdw_dev_name)
{
	struct property_entry props[MAX_NO_PROPS] = {};
	struct device *sdw_dev;
	int ret, cnt = 0;

	sdw_dev = bus_find_device_by_name(&sdw_bus_type, NULL, sdw_dev_name);
	if (!sdw_dev)
		return -EPROBE_DEFER;

	if (SOF_RT711_JDSRC(sof_rt711_rt1308_rt715_quirk)) {
		props[cnt++] = PROPERTY_ENTRY_U32(
			       "realtek,jd-src",
			       SOF_RT711_JDSRC(sof_rt711_rt1308_rt715_quirk));
	}

	ret = device_add_properties(sdw_dev, props);
	put_device(sdw_dev);

	return ret;
}

static const struct snd_soc_dapm_widget widgets[] = {
	SND_SOC_DAPM_HP("Headphone", NULL),
	SND_SOC_DAPM_MIC("Headset Mic", NULL),
	SND_SOC_DAPM_SPK("Speaker", NULL),
};

static const struct snd_soc_dapm_route map[] = {
	/* Headphones */
	{ "Headphone", NULL, "rt711 HP" },
	{ "rt711 MIC2", NULL, "Headset Mic" },
};

/*
 * dapm routes for rt1308 will be registered dynamically according
 * to the number of rt1308 used. The first two entries will be registered
 * for one codec case, and the last two entries are also registered
 * if two 1308s are used.
 */
static const struct snd_soc_dapm_route rt1308_speaker_map[] = {
	{ "Speaker", NULL, "rt1308-1 SPOL" },
	{ "Speaker", NULL, "rt1308-1 SPOR" },
	{ "Speaker", NULL, "rt1308-2 SPOL" },
	{ "Speaker", NULL, "rt1308-2 SPOR" },
};

static const struct snd_kcontrol_new controls[] = {
	SOC_DAPM_PIN_SWITCH("Headphone"),
	SOC_DAPM_PIN_SWITCH("Headset Mic"),
	SOC_DAPM_PIN_SWITCH("Speaker"),
};

static int first_spk_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_card *card = rtd->card;
	int ret;

	ret = snd_soc_dapm_add_routes(&card->dapm, rt1308_speaker_map, 2);
	if (ret)
		dev_err(rtd->dev, "failed to add first SPK map: %d\n", ret);

	return ret;
}

static int second_spk_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_card *card = rtd->card;
	int ret;

	ret = snd_soc_dapm_add_routes(&card->dapm, rt1308_speaker_map + 2, 2);
	if (ret)
		dev_err(rtd->dev, "failed to add second SPK map: %d\n", ret);

	return ret;
}

static int all_spk_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_card *card = rtd->card;
	int ret;

	ret = snd_soc_dapm_add_routes(&card->dapm, rt1308_speaker_map, 4);
	if (ret)
		dev_err(rtd->dev, "failed to add all SPK map: %d\n", ret);

	return ret;
}

static const struct snd_soc_dapm_widget dmic_widgets[] = {
	SND_SOC_DAPM_MIC("SoC DMIC", NULL),
};

static const struct snd_soc_dapm_route dmic_map[] = {
	/* digital mics */
	{"DMic", NULL, "SoC DMIC"},
};

static int dmic_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_card *card = rtd->card;
	int ret;

	ret = snd_soc_dapm_new_controls(&card->dapm, dmic_widgets,
					ARRAY_SIZE(dmic_widgets));
	if (ret) {
		dev_err(card->dev, "DMic widget addition failed: %d\n", ret);
		/* Don't need to add routes if widget addition failed */
		return ret;
	}

	ret = snd_soc_dapm_add_routes(&card->dapm, dmic_map,
				      ARRAY_SIZE(dmic_map));

	if (ret)
		dev_err(card->dev, "DMic map addition failed: %d\n", ret);

	return ret;
}

static struct snd_soc_codec_conf codec_conf[] = {
	{
		.dlc = COMP_CODEC_CONF("sdw:0:25d:711:0"),
		.name_prefix = "rt711",
	},
	{
		.dlc = COMP_CODEC_CONF("i2c-10EC1308:00"),
		.name_prefix = "rt1308-1",
	},
	{
		.dlc = COMP_CODEC_CONF("sdw:1:25d:1308:0"),
		.name_prefix = "rt1308-1",
	},
	/* two 1308s on link1 with different unique id */
	{
		.dlc = COMP_CODEC_CONF("sdw:1:25d:1308:0:0"),
		.name_prefix = "rt1308-1",
	},
	{
		.dlc = COMP_CODEC_CONF("sdw:1:25d:1308:0:2"),
		.name_prefix = "rt1308-2",
	},
	{
		.dlc = COMP_CODEC_CONF("sdw:2:25d:1308:0"),
		.name_prefix = "rt1308-2",
	},
	{
		.dlc = COMP_CODEC_CONF("sdw:3:25d:715:0"),
		.name_prefix = "rt715",
	},
};

static struct snd_soc_dai_link_component dmic_component[] = {
	{
		.name = "dmic-codec",
		.dai_name = "dmic-hifi",
	}
};

static struct snd_soc_dai_link_component platform_component[] = {
	{
		/* name might be overridden during probe */
		.name = "0000:00:1f.3"
	}
};

static int rt1308_i2s_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	int clk_id, clk_freq, pll_out;
	int err;

	clk_id = RT1308_PLL_S_MCLK;
	clk_freq = 38400000;

	pll_out = params_rate(params) * 512;

	/* Set rt1308 pll */
	err = snd_soc_dai_set_pll(codec_dai, 0, clk_id, clk_freq, pll_out);
	if (err < 0) {
		dev_err(card->dev, "Failed to set RT1308 PLL: %d\n", err);
		return err;
	}

	/* Set rt1308 sysclk */
	err = snd_soc_dai_set_sysclk(codec_dai, RT1308_FS_SYS_S_PLL, pll_out,
				     SND_SOC_CLOCK_IN);
	if (err < 0) {
		dev_err(card->dev, "Failed to set RT1308 SYSCLK: %d\n", err);
		return err;
	}

	return 0;
}

/* machine stream operations */
static struct snd_soc_ops rt1308_i2s_ops = {
	.hw_params = rt1308_i2s_hw_params,
};

static void rt711_init(const struct snd_soc_acpi_link_adr *link,
		       struct snd_soc_dai_link *dai_links,
		       struct codec_info *info,
		       bool playback)
{
	/*
	 * headset should be initialized once.
	 * Do it with dai link for playback.
	 */
	if (!playback)
		return;

	dai_links->init = headset_init;
}

static void rt1308_init(const struct snd_soc_acpi_link_adr *link,
			struct snd_soc_dai_link *dai_links,
			struct codec_info *info,
			bool playback)
{
	info->amp_num++;
	if (info->amp_num == 1)
		dai_links->init = first_spk_init;

	if (info->amp_num == 2) {
		/*
		 * if two 1308s are in one dai link, the init function
		 * in this dai link will be first set for the first speaker,
		 * and it should be reset to initialize all speakers when
		 * the second speaker is found.
		 */
		if (dai_links->init)
			dai_links->init = all_spk_init;
		else
			dai_links->init = second_spk_init;
	}
}

static struct codec_info codec_info_list[] = {
	{
		.id = 0x711,
		.direction = {true, true},
		.dai_name = "rt711-aif1",
		.init = rt711_init,
	},
	{
		.id = 0x1308,
		.acpi_id = "10EC1308",
		.direction = {true, false},
		.dai_name = "rt1308-aif",
		.ops = &rt1308_i2s_ops,
		.init = rt1308_init,
	},
	{
		.id = 0x715,
		.direction = {false, true},
		.dai_name = "rt715-aif2"
	},
};

static inline int find_codec_info_part(unsigned int part_id)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(codec_info_list); i++)
		if (part_id == codec_info_list[i].id)
			break;

	if (i == ARRAY_SIZE(codec_info_list))
		return -EINVAL;

	return i;
}

static inline int find_codec_info_acpi(const u8 *acpi_id)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(codec_info_list); i++)
		if (!memcmp(codec_info_list[i].acpi_id, acpi_id,
			    ACPI_ID_LEN))
			break;

	if (i == ARRAY_SIZE(codec_info_list))
		return -EINVAL;

	return i;
}

static int get_sdw_dai_link_num(struct snd_soc_acpi_mach_params *mach_params)
{
	const struct snd_soc_acpi_link_adr *link;
	int num = 0;

	link = mach_params->links;
	if (!link)
		return -EINVAL;

	for (; link->num_adr; link++) {
		int part_id, id;

		part_id = SDW_PART_ID(link->adr[0]);
		id = find_codec_info_part(part_id);
		if (id < 0)
			return 0;

		/* support playback */
		if (codec_info_list[id].direction[0])
			num++;

		/* support capture */
		if (codec_info_list[id].direction[1])
			num++;
	}

	return num;
}

static void init_dai_link(struct snd_soc_dai_link *dai_links,
			  int id, char *name, int playback, int capture,
			  struct snd_soc_dai_link_component *cpus,
			  char *cpu_name,
			  struct snd_soc_dai_link_component *codecs,
			  int codecs_num,
			  int (*init)(struct snd_soc_pcm_runtime *rtd),
			  const struct snd_soc_ops *ops)
{
	dai_links[id].id = id;
	dai_links[id].name = name;
	dai_links[id].platforms = platform_component;
	dai_links[id].num_platforms = ARRAY_SIZE(platform_component);
	dai_links[id].nonatomic = true;
	dai_links[id].no_pcm = 1;
	dai_links[id].cpus = &cpus[id];
	dai_links[id].num_cpus = 1;
	dai_links[id].cpus->dai_name = cpu_name;
	dai_links[id].codecs = codecs;
	dai_links[id].num_codecs = codecs_num;
	dai_links[id].dpcm_playback = playback;
	dai_links[id].dpcm_capture = capture;
	dai_links[id].init = init;
	dai_links[id].ops = ops;
}

static bool is_unique_device(const struct snd_soc_acpi_link_adr *link,
			     unsigned int sdw_version,
			     unsigned int mfg_id,
			     unsigned int part_id,
			     unsigned int class_id,
			     int id
			    )
{
	int i;

	for (i = 0; i < link->num_adr; i++) {
		unsigned int sdw1_version, mfg1_id, part1_id, class1_id;

		/* skip itself */
		if (i == id)
			continue;

		sdw1_version = SDW_VERSION(link->adr[i]);
		mfg1_id = SDW_MFG_ID(link->adr[i]);
		part1_id = SDW_PART_ID(link->adr[i]);
		class1_id = SDW_CLASS_ID(link->adr[i]);

		if (sdw_version == sdw1_version &&
		    mfg_id == mfg1_id &&
		    part_id == part1_id &&
		    class_id == class1_id)
			return false;
	}

	return true;
}

static int create_codec_dai_name(struct device *dev,
				 const struct snd_soc_acpi_link_adr *link,
				 struct snd_soc_dai_link_component *codec)
{
	int i;

	for (i = 0; i < link->num_adr; i++) {
		unsigned int sdw_version, unique_id, mfg_id;
		unsigned int link_id, part_id, class_id;
		int id;

		sdw_version = SDW_VERSION(link->adr[i]);
		link_id = SDW_DISCO_LINK_ID(link->adr[i]);
		unique_id = SDW_UNIQUE_ID(link->adr[i]);
		mfg_id = SDW_MFG_ID(link->adr[i]);
		part_id = SDW_PART_ID(link->adr[i]);
		class_id = SDW_CLASS_ID(link->adr[i]);

		if (is_unique_device(link, sdw_version, mfg_id, part_id,
				     class_id, i))
			codec[i].name = devm_kasprintf(dev, GFP_KERNEL,
						       "sdw:%x:%x:%x:%x",
						       link_id, mfg_id, part_id,
						       class_id);
		else
			codec[i].name = devm_kasprintf(dev, GFP_KERNEL,
						       "sdw:%x:%x:%x:%x:%x",
						       link_id, mfg_id, part_id,
						       class_id, unique_id);

		if (!codec[i].name)
			return -ENOMEM;

		id = find_codec_info_part(part_id);
		if (id < 0)
			return id;

		codec[i].dai_name = codec_info_list[id].dai_name;
	}

	return 0;
}

static void set_codec_init_func(const struct snd_soc_acpi_link_adr *link,
				struct snd_soc_dai_link *dai_links,
				bool playback)
{
	unsigned int part_id;
	int i, id;

	for (i = 0; i < link->num_adr; i++) {
		part_id = SDW_PART_ID(link->adr[i]);
		id = find_codec_info_part(part_id);

		if (codec_info_list[id].init)
			codec_info_list[id].init(link, dai_links,
						 &codec_info_list[id],
						 playback);
	}
}

static int create_sdw_codec_dai(struct device *dev,
				struct snd_soc_dai_link *dai_links, int id,
				struct snd_soc_dai_link_component *cpus,
				const struct snd_soc_acpi_link_adr *link)
{
	struct snd_soc_dai_link_component *codec;
	unsigned int part_id, link_id;
	int i, j = 0, idx;
	int ret;

	codec = devm_kcalloc(dev, link->num_adr,
			     sizeof(struct snd_soc_dai_link_component),
			     GFP_KERNEL);
	if (!codec)
		return -ENOMEM;

	ret = create_codec_dai_name(dev, link, codec);
	if (ret < 0)
		return ret;

	part_id = SDW_PART_ID(link->adr[0]);
	idx = find_codec_info_part(part_id);
	if (idx < 0)
		return idx;

	link_id = ffs(link->mask) - 1;

	/* playback & capture */
	for (i = 0; i < 2; i++) {
		char *name, *cpu_name;
		static const char * const sdw_stream_name[] = {
			"SDW%d-Playback",
			"SDW%d-Capture",
		};

		if (!codec_info_list[idx].direction[i])
			continue;

		name = devm_kasprintf(dev, GFP_KERNEL,
				      sdw_stream_name[i], link_id);
		if (!name)
			return -ENOMEM;

		cpu_name = devm_kasprintf(dev, GFP_KERNEL,
					  "SDW%d Pin%d", link_id, j + 2);
		if (!cpu_name)
			return -ENOMEM;

		init_dai_link(dai_links, id + j, name, 1 - i, i, cpus, cpu_name,
			      codec, link->num_adr, NULL, NULL);
		set_codec_init_func(link, dai_links + id + j, 1 - i);
		j++;
	}

	return j;
}

static int sof_card_dai_links_create(struct device *dev,
				     struct snd_soc_acpi_mach *mach,
				     struct snd_soc_card *card)
{
	struct snd_soc_dai_link_component *idisp_components;
	struct snd_soc_dai_link_component *ssp_components;
	struct snd_soc_acpi_mach_params *mach_params;
	int ssp_num, sdw_num, hdmi_num = 0, dmic_num;
	const struct snd_soc_acpi_link_adr *adr_link;
	struct snd_soc_dai_link_component *cpus;
	struct snd_soc_dai_link *links;
	int num_links, ssp_mask;
	char *name, *cpu_name;
	int i, j, index, id = 0;
	int val;

	/* reset amp_num to ensure amp_num++ starts from 0 in each probe */
	for (i = 0; i < ARRAY_SIZE(codec_info_list); i++)
		codec_info_list[i].amp_num = 0;

#if IS_ENABLED(CONFIG_SND_HDA_CODEC_HDMI)
	hdmi_num = sof_rt711_rt1308_rt715_quirk & SOF_SDW_TGL_HDMI ?
				SOF_TGL_HDMI_COUNT : SOF_PRE_TGL_HDMI_COUNT;
#endif

	ssp_mask = SOF_SSP_GET_PORT(sof_rt711_rt1308_rt715_quirk);
	ssp_num = hweight_long(ssp_mask);

	mach_params = &mach->mach_params;
	sdw_num = get_sdw_dai_link_num(mach_params);

	/* enable dmic01 & dmic16k */
	dmic_num = sof_rt711_rt1308_rt715_quirk & SOF_SDW_PCH_DMIC ? 2 : 0;

	dev_dbg(dev, "sdw %d, ssp %d, dmic %d, hdmi %d", sdw_num, ssp_num,
		dmic_num, hdmi_num);

	num_links = ssp_num + sdw_num + dmic_num + hdmi_num;
	links = devm_kcalloc(dev, num_links, sizeof(struct snd_soc_dai_link),
			     GFP_KERNEL);
	cpus = devm_kcalloc(dev, num_links,
			    sizeof(struct snd_soc_dai_link_component),
			    GFP_KERNEL);
	if (!links || !cpus)
		return -ENOMEM;

	/* SDW */
	if (!sdw_num)
		goto SSP;

	adr_link = mach_params->links;
	if (!adr_link)
		return -EINVAL;

	for (; adr_link->num_adr; adr_link++) {
		val = create_sdw_codec_dai(dev, links, id, cpus, adr_link);
		if (val < 0) {
			dev_err(dev, "failed to create dai link %d", id);
			return -ENOMEM;
		}

		id += val;
	}

SSP:
	/* SSP */
	if (!ssp_num)
		goto DMIC;

	index = find_codec_info_acpi(mach->id);
	if (index < 0)
		return -EINVAL;

	for (i = 0, j = 0; ssp_mask; i++, ssp_mask >>= 1) {
		struct codec_info *info;
		char *codec_name;

		if (!(ssp_mask & 0x1))
			continue;

		name = devm_kasprintf(dev, GFP_KERNEL,
				      "SSP%d-Codec", i);
		if (!name)
			return -ENOMEM;

		cpu_name = devm_kasprintf(dev, GFP_KERNEL, "SSP%d Pin", i);
		if (!cpu_name)
			return -ENOMEM;

		ssp_components = devm_kzalloc(dev, sizeof(*ssp_components),
					      GFP_KERNEL);
		if (!ssp_components)
			return -ENOMEM;

		info = &codec_info_list[index];
		codec_name = devm_kasprintf(dev, GFP_KERNEL, "i2c-%s:0%d",
					    info->acpi_id, j++);
		if (!codec_name)
			return -ENOMEM;

		ssp_components->name = codec_name;
		ssp_components->dai_name = info->dai_name;

		init_dai_link(links, id++, name, info->direction[0],
			      info->direction[1], cpus, cpu_name,
			      ssp_components, 1, NULL, info->ops);

		info->init(NULL, links + id - 1, info, 0);
	}

DMIC:
	/* dmic */
	if (dmic_num > 0) {
		init_dai_link(links, id++, "dmic01", 0, 1, cpus, "DMIC01 Pin",
			      dmic_component, 1, dmic_init, NULL);

		init_dai_link(links, id++, "dmic16k", 0, 1, cpus, "DMIC16k Pin",
			      dmic_component, 1, dmic_init, NULL);
	}

#if IS_ENABLED(CONFIG_SND_HDA_CODEC_HDMI)
	/* HDMI */
	if (hdmi_num > 0) {
		idisp_components = devm_kcalloc(dev, hdmi_num,
						sizeof(*idisp_components),
						GFP_KERNEL);
		if (!idisp_components)
			return -ENOMEM;
	}

	for (i = 0; i < hdmi_num; i++) {
		name = devm_kasprintf(dev, GFP_KERNEL,
				      "iDisp%d", i + 1);
		if (!name)
			return -ENOMEM;

		idisp_components[i].name = "ehdaudio0D2";
		idisp_components[i].dai_name = devm_kasprintf(dev,
							      GFP_KERNEL,
							      "intel-hdmi-hifi%d",
							      i + 1);
		if (!idisp_components[i].dai_name)
			return -ENOMEM;

		cpu_name = devm_kasprintf(dev, GFP_KERNEL,
					  "iDisp%d Pin", i + 1);
		if (!cpu_name)
			return -ENOMEM;

		init_dai_link(links, id++, name, 1, 0, cpus, cpu_name,
			      idisp_components + i, 1, hdmi_init, NULL);
	}
#endif

	card->dai_link = links;
	card->num_links = num_links;

	return 0;
}

/* SoC card */
static char components_string[] = "cfg-spk:2"; /* cfg-spk:%d */
#if !IS_ENABLED(CONFIG_SND_SOC_INTEL_USER_FRIENDLY_LONG_NAMES)
/* Can also be sof-sdw-rt711-mono-rt1308-rt715 */
static char sdw_card_long_name[] = "sof-sdw-rt711-stereo-rt1308-rt715";
#endif
static struct snd_soc_card card_rt700_rt1308_rt715 = {
	.name = "sdw-rt711-1308-715",
	.controls = controls,
	.num_controls = ARRAY_SIZE(controls),
	.dapm_widgets = widgets,
	.num_dapm_widgets = ARRAY_SIZE(widgets),
	.dapm_routes = map,
	.num_dapm_routes = ARRAY_SIZE(map),
	.late_probe = card_late_probe,
	.codec_conf = codec_conf,
	.num_configs = ARRAY_SIZE(codec_conf),
	.components = components_string,
};

static int mc_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card = &card_rt700_rt1308_rt715;
	struct snd_soc_acpi_mach *mach;
	struct mc_private *ctx;
	int ret;

	dev_dbg(&pdev->dev, "Entry %s\n", __func__);

	ctx = devm_kzalloc(&pdev->dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	dmi_check_system(sof_sdw_rt711_rt1308_rt715_quirk_table);

#if IS_ENABLED(CONFIG_SND_HDA_CODEC_HDMI)
	INIT_LIST_HEAD(&ctx->hdmi_pcm_list);
#endif

	card->dev = &pdev->dev;

	mach = pdev->dev.platform_data;
	ret = sof_card_dai_links_create(&pdev->dev, mach,
					&card_rt700_rt1308_rt715);
	if (ret < 0)
		return ret;

	ctx->common_hdmi_codec_drv = mach->mach_params.common_hdmi_codec_drv;

	snd_soc_card_set_drvdata(card, ctx);

	sof_rt711_add_codec_device_props("sdw:0:25d:711:0");


	snprintf(components_string, sizeof(components_string),
		 "cfg-spk:%d",
		 (sof_rt711_rt1308_rt715_quirk & SOF_SDW_MONO_SPK) ? 2 : 4);
#if !IS_ENABLED(CONFIG_SND_SOC_INTEL_USER_FRIENDLY_LONG_NAMES)
	snprintf(sdw_card_long_name, sizeof(sdw_card_long_name),
		 "sof-sdw-rt711-%s-rt1308-rt715",
		 (sof_rt711_rt1308_rt715_quirk & SOF_SDW_MONO_SPK) ?
			"mono" : "stereo");
	card_rt700_rt1308_rt715.long_name = sdw_card_long_name;
#endif
	/* Register the card */
	ret = devm_snd_soc_register_card(&pdev->dev, card);
	if (ret) {
		dev_err(card->dev, "snd_soc_register_card failed %d\n", ret);
		return ret;
	}

	platform_set_drvdata(pdev, card);

	return ret;
}

static struct platform_driver sdw_rt711_rt1308_rt715_driver = {
	.driver = {
		.name = "sdw_rt711_rt1308_rt715",
		.pm = &snd_soc_pm_ops,
	},
	.probe = mc_probe,
};

module_platform_driver(sdw_rt711_rt1308_rt715_driver);

MODULE_DESCRIPTION("ASoC SoundWire RT711/1308/715 Machine driver");
MODULE_AUTHOR("Bard Liao <yung-chuan.liao@linux.intel.com>");
MODULE_AUTHOR("Pierre-Louis Bossart <pierre-louis.bossart@linux.intel.com>");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:sdw_rt711_rt1308_rt715");
