// SPDX-License-Identifier: GPL-2.0
//
// ALSA SoC Texas Instruments TAS2783 Audio Smart Amplifier
//
// Copyright (C) 2023 Texas Instruments Incorporated
// https://www.ti.com
//
// The TAS2783 driver implements a flexible and configurable
// algorithm coefficient setting for one, two or even multiple
// TAS2781 chips.
//
// Author: Baojun Xu <baojun.xu@ti.com>
// 			Kevin Lu <kevin-lu@ti.com>
// 			Shenghao Ding <shenghao-ding@ti.com>
//

#include <linux/crc32.h>
#include <linux/efi.h>
#include <linux/err.h>
#include <linux/firmware.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/of.h>
#include <sound/pcm_params.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/soundwire/sdw.h>
#include <linux/soundwire/sdw_registers.h>
#include <linux/soundwire/sdw_type.h>
#include <sound/sdw.h>
#include <sound/soc.h>
#include <sound/tlv.h>
#include <sound/tas2781-tlv.h>

#include "tas2783.h"

static const unsigned int tas2783_cali_reg[] = {
	TAS2783_CALIBRATION_RE,		/* Resistance */
	TAS2783_CALIBRATION_RE_LOW,	/* Low limitation of RE */
	TAS2783_CALIBRATION_INV_RE,	/* Invert of RE */
	TAS2783_CALIBRATION_POW,	/* RMS Power */
	TAS2783_CALIBRATION_TLIMIT	/* Temperature limitation */
};

static const struct reg_default tas2783_reg_defaults[] = {
	/* Default values for ROM mode. Activated. */
	{ 0x8002, 0x1a},	/* AMP inactive. */
	{ 0x8097, 0xc8},
	{ 0x80b5, 0x74},
	{ 0x8099, 0x20},
	{ 0xfe8d, 0x0d},
	{ 0xfebe, 0x4a},
	{ 0x8230, 0x00},
	{ 0x8231, 0x00},
	{ 0x8232, 0x00},
	{ 0x8233, 0x01},
	{ 0x8418, 0x00},	/* Set volume to 0 dB. */
	{ 0x8419, 0x00},
	{ 0x841a, 0x00},
	{ 0x841b, 0x00},
	{ 0x8428, 0x40},	/* Unmute channel */
	{ 0x8429, 0x00},
	{ 0x842a, 0x00},
	{ 0x842b, 0x00},
	{ 0x8548, 0x00},	/* Set volume to 0 dB. */
	{ 0x8549, 0x00},
	{ 0x854a, 0x00},
	{ 0x854b, 0x00},
	{ 0x8558, 0x40},	/* Unmute channel */
	{ 0x8559, 0x00},
	{ 0x855a, 0x00},
	{ 0x855b, 0x00},
	{ 0x800a, 0x3a},	/* Enable both channel */
	{ 0x800e, 0x44},
	{ 0x800f, 0x40},
	{ 0x805c, 0x99},
	{ 0x40400088, 0},	/* FUNC_1, FU21, SEL_1(Mute) */
	{ 0x40400090, 0},	/* FUNC_1, FU21, SEL_2(Channel volume) */
	{ 0x40400108, 0},	/* FUNC_1, FU23, MUTE */
};

static bool tas2783_readable_register(struct device *dev, unsigned int reg)
{
	switch (reg) {
		case 0x8000 ... 0xc000:	/* Page 0 ~ 127. */
		case 0xfe80 ... 0xfeff:	/* Page 253. */
			return true;
		default:
			return false;
	}
}

static bool tas2783_volatile_register(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case 0x8001:
		/* Only reset register was volatiled.
		 * Write any value into this register, mean RESET device.
		 */
		return true;
	default:
		return false;
	}
}

static const struct regmap_config tasdevice_regmap = {
	.reg_bits = 32,
	.val_bits = 8,
	.readable_reg = tas2783_readable_register,
	.volatile_reg = tas2783_volatile_register,
	.max_register = 0x41008000 + TASDEVICE_REG(0xa1, 0x60, 0x7f),
	.reg_defaults = tas2783_reg_defaults,
	.num_reg_defaults = ARRAY_SIZE(tas2783_reg_defaults),
	.cache_type = REGCACHE_RBTREE,
	.use_single_read = true,
	.use_single_write = true,
};

static int tasdevice_clamp(int val, int max, unsigned int invert)
{
	/* Keep in valid area, out of range value don't care. */
	if (val > max)
		val = max;
	if (invert)
		val = max - val;
	if (val < 0)
		val = 0;
	return val;
}

static int tas2783_digital_getvol(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component
		= snd_soc_kcontrol_component(kcontrol);
	struct tasdevice_priv *tas_dev =
		snd_soc_component_get_drvdata(component);
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	struct regmap *map = tas_dev->regmap;
	int val = 0, ret;

	if (!ucontrol) {
		dev_err(tas_dev->dev, "%s, wrong parameter.\n", __func__);
		return -EINVAL;
	}
	/* Read current volume from the device. */
	ret = regmap_read(map, mc->reg, &val);
	if (ret) {
		dev_err(tas_dev->dev, "%s, get digital vol error %x.\n",
			__func__, ret);
		return ret;
	}
	ucontrol->value.integer.value[0] =
		tasdevice_clamp(val, mc->max, mc->invert);

	return ret;
}

static int tas2783_digital_putvol(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component
		= snd_soc_kcontrol_component(kcontrol);
	struct tasdevice_priv *tas_dev =
		snd_soc_component_get_drvdata(component);
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	struct regmap *map = tas_dev->regmap;
	int val, ret;

	if (!ucontrol) {
		dev_err(tas_dev->dev, "%s, wrong parameter.\n", __func__);
		return -EINVAL;
	}
	val = tasdevice_clamp(ucontrol->value.integer.value[0],
		mc->max, mc->invert);

	ret = regmap_write(map, mc->reg, val);
	if (ret != 0) {
		dev_dbg(tas_dev->dev, "%s, Put vol %d into %x %x.\n",
		__func__, val, mc->reg, ret);
	}

	return ret;
}

static int tas2783_amp_getvol(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component
		= snd_soc_kcontrol_component(kcontrol);
	struct tasdevice_priv *tas_dev =
		snd_soc_component_get_drvdata(component);
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	struct regmap *map = tas_dev->regmap;
	unsigned char mask = 0;
	int ret, val = 0;

	if (!ucontrol) {
		dev_err(tas_dev->dev, "%s, wrong parameter.\n", __func__);
		return -EINVAL;
	}
	/* Read current volume from the device. */
	ret = regmap_read(map, mc->reg, &val);
	if (ret != 0) {
		dev_err(tas_dev->dev, "%s get AMP vol from %x with %d.\n",
			__func__, mc->reg, ret);
		return ret;
	}

	mask = (1 << fls(mc->max)) - 1;
	mask <<= mc->shift;
	val = (val & mask) >> mc->shift;
	ucontrol->value.integer.value[0] = tasdevice_clamp(val, mc->max,
		mc->invert);

	return ret;
}

static int tas2783_amp_putvol(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component
		= snd_soc_kcontrol_component(kcontrol);
	struct tasdevice_priv *tas_dev =
		snd_soc_component_get_drvdata(component);
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	struct regmap *map = tas_dev->regmap;
	unsigned char mask;
	int val, ret;

	if (!ucontrol) {
		dev_err(tas_dev->dev, "%s, wrong parameter.\n", __func__);
		return -EINVAL;
	}
	mask = (1 << fls(mc->max)) - 1;
	mask <<= mc->shift;
	val = tasdevice_clamp(ucontrol->value.integer.value[0], mc->max,
		mc->invert);
	ret = regmap_update_bits(map, mc->reg, mask, val << mc->shift);
	if (ret != 0)
		dev_err(tas_dev->dev, "Write @%#x..%#x:%d\n",
			mc->reg, val, ret);

	return ret;
}

static const struct snd_kcontrol_new tas2783_snd_controls[] = {
	SOC_SINGLE_RANGE_EXT_TLV("Amp Gain Volume", TAS2783_AMP_LEVEL,
		1, 0, 20, 0, tas2783_amp_getvol,
		tas2783_amp_putvol, amp_vol_tlv),
	SOC_SINGLE_RANGE_EXT_TLV("Digital Volume", TAS2783_DVC_LVL,
		0, 0, 200, 1, tas2783_digital_getvol,
		tas2783_digital_putvol, dvc_tlv),
};

static void tas2783_apply_calib(
	struct tasdevice_priv *tas_dev, unsigned int *cali_data)
{
	struct regmap *map = tas_dev->regmap;
	u8 *reg_start;
	int ret;

	if (!tas_dev->sdw_peripheral) {
		dev_err(tas_dev->dev, "%s, slaver doesn't exist.\n",
			__func__);
		return;
	}
	if ((tas_dev->sdw_peripheral->id.unique_id < TAS2783_ID_MIN) ||
		(tas_dev->sdw_peripheral->id.unique_id > TAS2783_ID_MAX))
		return;
	reg_start = (u8 *)(cali_data+(tas_dev->sdw_peripheral->id.unique_id
		- TAS2783_ID_MIN)*sizeof(tas2783_cali_reg));
	for (int i = 0; i < ARRAY_SIZE(tas2783_cali_reg); i++) {
		ret = regmap_bulk_write(map, tas2783_cali_reg[i],
			reg_start + i, 4);
		if (ret != 0) {
			dev_err(tas_dev->dev, "Cali failed %x:%d\n",
			tas2783_cali_reg[i], ret);
			break;
		}
	}
}

static int tas2783_calibration(struct tasdevice_priv *tas_dev)
{
	efi_guid_t efi_guid = EFI_GUID(0x1f52d2a1, 0xbb3a, 0x457d, 0xbc,
			0x09, 0x43, 0xa3, 0xf4, 0x31, 0x0a, 0x92);
	static efi_char16_t efi_name[] = L"CALI_DATA";
	struct tm *tm = &tas_dev->tm;
	unsigned int attr = 0, crc;
	unsigned int *tmp_val;
	efi_status_t status;

	tas_dev->cali_data.total_sz = 128;
	/* Sometimes, calibration was performed from Windows,
	 * and data was saved in UEFI.
	 * So we can share it from linux, and data size is variable.
	 * Get real size and read it from UEFI.
	 */
	status = efi.get_variable(efi_name, &efi_guid, &attr,
		&tas_dev->cali_data.total_sz, tas_dev->cali_data.data);
	if (status == EFI_BUFFER_TOO_SMALL) {
		status = efi.get_variable(efi_name, &efi_guid, &attr,
			&tas_dev->cali_data.total_sz,
			tas_dev->cali_data.data);
		dev_dbg(tas_dev->dev, "cali get %lx bytes result:%ld\n",
			tas_dev->cali_data.total_sz, status);
	}
	if (status != 0) {
		/* Failed got calibration data from EFI. */
		dev_dbg(tas_dev->dev, "No calibration data in UEFI.");
		return 0;
	}

	tmp_val = (unsigned int *)tas_dev->cali_data.data;

	crc = crc32(~0, tas_dev->cali_data.data, 84) ^ ~0;

	if (crc == tmp_val[21]) {
		/* Date and time of calibration was done. */
		time64_to_tm(tmp_val[20], 0, tm);
		dev_dbg(tas_dev->dev, "%4ld-%2d-%2d, %2d:%2d:%2d\n",
			tm->tm_year, tm->tm_mon, tm->tm_mday,
			tm->tm_hour, tm->tm_min, tm->tm_sec);
		tas2783_apply_calib(tas_dev, tmp_val);
	} else {
		dev_dbg(tas_dev->dev, "CRC 0x%08x not match 0x%08x\n",
			crc, tmp_val[21]);
		tas_dev->cali_data.total_sz = 0;
	}

	return 0;
}

static void tasdevice_rca_ready(const struct firmware *fmw,
	void *context)
{
	struct tasdevice_priv *tas_dev =
		(struct tasdevice_priv *) context;
	struct tas2783_firmware_node *p;
	struct regmap *map = tas_dev->regmap;
	unsigned char *buf = NULL;
	int offset = 0, img_sz;
	int ret, value_sdw;

	mutex_lock(&tas_dev->codec_lock);

	if (!fmw || !fmw->data) {
		/* No firmware binary, devices will work in ROM mode. */
		dev_err(tas_dev->dev,
		"Failed to read %s, no side-effect on driver running\n",
		tas_dev->rca_binaryname);
		ret = -EINVAL;
		goto out;
	}
	buf = (unsigned char *)fmw->data;

	img_sz = le32_to_cpup((__le32 *)&buf[offset]);
	offset  += sizeof(img_sz);
	if (img_sz != fmw->size) {
		dev_err(tas_dev->dev, "Size not matching, %d %u",
			(int)fmw->size, img_sz);
		ret = -EINVAL;
		goto out;
	}

	while (offset < img_sz) {
		p = (struct tas2783_firmware_node *)(buf + offset);
		if (p->length > 1) {
			ret = regmap_bulk_write(map, p->download_addr,
			buf + offset + sizeof(unsigned int)*5, p->length);
		} else
			ret = regmap_write(map, p->download_addr,
				*(buf + offset + sizeof(unsigned int) * 5));

		if (ret != 0) {
			dev_dbg(tas_dev->dev, "Load FW fail: %d.\n", ret);
			goto out;
		}
		offset += sizeof(unsigned int)*5 + p->length;
	}
	/* Select left-right channel based on unique id. */
	value_sdw = 0x1a;
	value_sdw += ((tas_dev->sdw_peripheral->dev_num & 1) << 4);
	dev_dbg(tas_dev->dev, "%s dev_num = %u", __func__,
		tas_dev->sdw_peripheral->dev_num);
	regmap_write(map, TASDEVICE_REG(0, 0, 0x0a), value_sdw);

	tas2783_calibration(tas_dev);

out:
	mutex_unlock(&tas_dev->codec_lock);
	if (fmw)
		release_firmware(fmw);
}

static const struct snd_soc_dapm_widget tasdevice_dapm_widgets[] = {
	SND_SOC_DAPM_AIF_IN("ASI", "ASI Playback", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("ASI OUT", "ASI Capture", 0, SND_SOC_NOPM,
		0, 0),
	SND_SOC_DAPM_OUTPUT("OUT"),
	SND_SOC_DAPM_INPUT("DMIC")
};

static const struct snd_soc_dapm_route tasdevice_audio_map[] = {
	{"OUT", NULL, "ASI"},
	{"ASI OUT", NULL, "DMIC"}
};

static int tasdevice_set_sdw_stream(struct snd_soc_dai *dai,
	void *sdw_stream, int direction)
{
	struct sdw_stream_data *stream;

	if (!sdw_stream)
		return 0;

	stream = kzalloc(sizeof(*stream), GFP_KERNEL);
	if (!stream)
		return -ENOMEM;

	stream->sdw_stream = sdw_stream;

	 snd_soc_dai_dma_data_set(dai, direction, stream);

	return 0;
}

static void tasdevice_sdw_shutdown(struct snd_pcm_substream *substream,
	struct snd_soc_dai *dai)
{
	struct sdw_stream_data *stream;

	stream = snd_soc_dai_get_dma_data(dai, substream);
	snd_soc_dai_set_dma_data(dai, substream, NULL);
	kfree(stream);
}

static int tasdevice_sdw_hw_params(struct snd_pcm_substream *substream,
	struct snd_pcm_hw_params *params, struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct tasdevice_priv *tas_priv =
		snd_soc_component_get_drvdata(component);
	struct sdw_stream_config stream_config = {0};
	struct sdw_port_config port_config = {0};
	struct sdw_stream_data *stream;
	int ret;

	dev_dbg(dai->dev, "%s %s", __func__, dai->name);
	stream = snd_soc_dai_get_dma_data(dai, substream);

	if (!stream)
		return -EINVAL;

	if (!tas_priv->sdw_peripheral)
		return -EINVAL;

	/* SoundWire specific configuration */
	snd_sdw_params_to_config(substream, params,
		&stream_config, &port_config);

	/* port 1 for playback */
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		port_config.num = 1;
	else
		port_config.num = 2;

	ret = sdw_stream_add_slave(tas_priv->sdw_peripheral,
		&stream_config, &port_config, 1, stream->sdw_stream);
	if (ret) {
		dev_err(dai->dev, "Unable to configure port\n");
		return ret;
	}

	dev_dbg(dai->dev, "%s fomrat: %d rate: %u\n", __func__,
		params_format(params), params_rate(params));

	return 0;
}

static int tasdevice_sdw_pcm_hw_free(struct snd_pcm_substream *substream,
	struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct tasdevice_priv *tas_priv =
		snd_soc_component_get_drvdata(component);
	struct sdw_stream_data *stream =
		snd_soc_dai_get_dma_data(dai, substream);

	if (!tas_priv->sdw_peripheral)
		return -EINVAL;

	sdw_stream_remove_slave(tas_priv->sdw_peripheral,
		stream->sdw_stream);

	return 0;
}

static int tasdevice_mute(struct snd_soc_dai *dai, int mute,
	int direction)
{
	struct snd_soc_component *component = dai->component;
	struct tasdevice_priv *tas_dev =
		snd_soc_component_get_drvdata(component);
	struct regmap *map = tas_dev->regmap;
	int ret;

	dev_info(tas_dev->dev, "Mute or unmute %d.\n", mute);

	if (mute) {
		/* Echo channel can't be shutdown while tas2783 must keep working state
		 * while playback is on */
		if (direction == SNDRV_PCM_STREAM_CAPTURE && tas_dev->pstream == true)
			return 0;
		/* FU23 mute (0x40400108) */
		ret = regmap_write(map, SDW_SDCA_CTL(1, 2, 1, 0), 1);
		ret |= regmap_write(map, TASDEVICE_REG(0, 0, 0x02), 0x1a);
		tas_dev->pstream = false;
	} else {
		/* FU23 Unmute, 0x40400108. */
		ret = regmap_write(map, SDW_SDCA_CTL(1, 2, 1, 0), 0);
		ret |= regmap_write(map, TASDEVICE_REG(0, 0, 0x02), 0x0);
		if (direction == SNDRV_PCM_STREAM_PLAYBACK)
			tas_dev->pstream = true;
	}

	if (ret)
		dev_err(tas_dev->dev, "Mute or unmute %d failed %d.\n", mute, ret);

	return ret;
}

static const struct snd_soc_dai_ops tasdevice_dai_ops = {
	.mute_stream	= tasdevice_mute,
	.hw_params	= tasdevice_sdw_hw_params,
	.hw_free	= tasdevice_sdw_pcm_hw_free,
	.set_stream	= tasdevice_set_sdw_stream,
	.shutdown	= tasdevice_sdw_shutdown,
};

static struct snd_soc_dai_driver tasdevice_dai_driver[] = {
	{
		.name = "tas2783-codec",
		.id = 0,
		.playback = {
			.stream_name	= "Playback",
			.channels_min	= 1,
			.channels_max	= 4,
			.rates		= TAS2783_DEVICE_RATES,
			.formats	= TAS2783_DEVICE_FORMATS,
		},
		.capture = {
			.stream_name	= "Capture",
			.channels_min	= 1,
			.channels_max	= 4,
			.rates		= TAS2783_DEVICE_RATES,
			.formats	= TAS2783_DEVICE_FORMATS,
		},
		.ops = &tasdevice_dai_ops,
		.symmetric_rate = 1,
	},
};

static void tas2783_reset(struct tasdevice_priv *tas_dev)
{
	struct regmap *map = tas_dev->regmap;
	int ret;

	ret = regmap_write(map, TAS2873_REG_SWRESET, 1);
	if (ret) {
		dev_err(tas_dev->dev, "Reset failed.\n");
		return;
	}
	usleep_range(1000, 1050);
}

static int tasdevice_component_probe(struct snd_soc_component *component)
{
	struct tasdevice_priv *tas_dev =
		snd_soc_component_get_drvdata(component);

	/* Codec Lock Hold */
	mutex_lock(&tas_dev->codec_lock);

	tas_dev->component = component;

	/* Codec Lock Release*/
	mutex_unlock(&tas_dev->codec_lock);

	dev_dbg(tas_dev->dev, "%s was called.\n", __func__);

	return 0;
}

static const struct snd_soc_component_driver
	soc_codec_driver_tasdevice = {
	.probe		= tasdevice_component_probe,
	.controls	= tas2783_snd_controls,
	.num_controls	= ARRAY_SIZE(tas2783_snd_controls),
	.dapm_widgets	= tasdevice_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(tasdevice_dapm_widgets),
	.dapm_routes	= tasdevice_audio_map,
	.num_dapm_routes = ARRAY_SIZE(tasdevice_audio_map),
	.idle_bias_on	= 1,
	.endianness	= 1,
};

static int tasdevice_init(struct tasdevice_priv *tas_dev)
{
	int ret;

	dev_set_drvdata(tas_dev->dev, tas_dev);

	mutex_init(&tas_dev->codec_lock);
	ret = devm_snd_soc_register_component(tas_dev->dev,
		&soc_codec_driver_tasdevice,
		tasdevice_dai_driver, ARRAY_SIZE(tasdevice_dai_driver));
	if (ret) {
		dev_err(tas_dev->dev, "%s: codec register error:%d.\n",
			__func__, ret);
	}

	tas2783_reset(tas_dev);
	/* tas2783-8[9,...,f].bin was copied into /lib/firmware/ */
	scnprintf(tas_dev->rca_binaryname, 64, "tas2783-%01x.bin",
		tas_dev->sdw_peripheral->id.unique_id);

	ret = request_firmware_nowait(THIS_MODULE, FW_ACTION_UEVENT,
		tas_dev->rca_binaryname, tas_dev->dev, GFP_KERNEL,
		tas_dev, tasdevice_rca_ready);
	if (ret) {
		dev_dbg(tas_dev->dev,
		"%s: request_firmware %x open status: %d.\n",
		__func__, tas_dev->sdw_peripheral->id.unique_id, ret);
	}

	/* set autosuspend parameters */
	pm_runtime_set_autosuspend_delay(tas_dev->dev, 3000);
	pm_runtime_use_autosuspend(tas_dev->dev);

	/* make sure the device does not suspend immediately */
	pm_runtime_mark_last_busy(tas_dev->dev);

	pm_runtime_enable(tas_dev->dev);

	dev_dbg(tas_dev->dev, "%s was called for TAS2783.\n",  __func__);

	return ret;
}

static int tasdevice_read_prop(struct sdw_slave *slave)
{
	struct sdw_slave_prop *prop = &slave->prop;
	int nval;
	int i, j;
	u32 bit;
	unsigned long addr;
	struct sdw_dpn_prop *dpn;

	prop->scp_int1_mask =
		SDW_SCP_INT1_BUS_CLASH | SDW_SCP_INT1_PARITY;
	prop->quirks = SDW_SLAVE_QUIRKS_INVALID_INITIAL_PARITY;

	prop->paging_support = true;

	/* first we need to allocate memory for set bits in port lists */
	prop->source_ports = 0x04; /* BITMAP: 00000100 */
	prop->sink_ports = 0x2; /* BITMAP:  00000010 */

	nval = hweight32(prop->source_ports);
	prop->src_dpn_prop = devm_kcalloc(&slave->dev, nval,
		sizeof(*prop->src_dpn_prop), GFP_KERNEL);
	if (!prop->src_dpn_prop)
		return -ENOMEM;

	i = 0;
	dpn = prop->src_dpn_prop;
	addr = prop->source_ports;
	for_each_set_bit(bit, &addr, 32) {
		dpn[i].num = bit;
		dpn[i].type = SDW_DPN_FULL;
		dpn[i].simple_ch_prep_sm = true;
		dpn[i].ch_prep_timeout = 10;
		i++;
	}

	/* do this again for sink now */
	nval = hweight32(prop->sink_ports);
	prop->sink_dpn_prop = devm_kcalloc(&slave->dev, nval,
		sizeof(*prop->sink_dpn_prop), GFP_KERNEL);
	if (!prop->sink_dpn_prop)
		return -ENOMEM;

	j = 0;
	dpn = prop->sink_dpn_prop;
	addr = prop->sink_ports;
	for_each_set_bit(bit, &addr, 32) {
		dpn[j].num = bit;
		dpn[j].type = SDW_DPN_FULL;
		dpn[j].simple_ch_prep_sm = true;
		dpn[j].ch_prep_timeout = 10;
		j++;
	}

	/* set the timeout values */
	prop->clk_stop_timeout = 20;

	return 0;
}

static int tasdevice_io_init(struct device *dev, struct sdw_slave *slave)
{
	struct tasdevice_priv *tas_priv = dev_get_drvdata(dev);

	if (tas_priv->hw_init)
		return 0;

	/* Mark Slave initialization complete */
	tas_priv->hw_init = true;

	return 0;
}

static int tasdevice_update_status(struct sdw_slave *slave,
	enum sdw_slave_status status)
{
	struct  tasdevice_priv *tas_priv = dev_get_drvdata(&slave->dev);

	/* Update the status */
	tas_priv->status = status;

	if (status == SDW_SLAVE_UNATTACHED)
		tas_priv->hw_init = false;

	/* Perform initialization only if slave status
	 * is present and hw_init flag is false
	 */
	if (tas_priv->hw_init || tas_priv->status != SDW_SLAVE_ATTACHED)
		return 0;

	/* perform I/O transfers required for Slave initialization */
	return tasdevice_io_init(&slave->dev, slave);
}

/*
 * slave_ops: callbacks for get_clock_stop_mode, clock_stop and
 * port_prep are not defined for now
 */
static const struct sdw_slave_ops tasdevice_sdw_ops = {
	.read_prop	= tasdevice_read_prop,
	.update_status	= tasdevice_update_status,
};

static void tasdevice_remove(struct tasdevice_priv *tas_dev)
{
	mutex_destroy(&tas_dev->codec_lock);
}

static int tasdevice_sdw_probe(struct sdw_slave *peripheral,
	const struct sdw_device_id *id)
{
	struct device *dev = &peripheral->dev;
	struct tasdevice_priv *tas_dev;
	int ret;

	tas_dev = devm_kzalloc(dev, sizeof(*tas_dev), GFP_KERNEL);
	if (!tas_dev)
		return -ENOMEM;

	tas_dev->dev = dev;
	tas_dev->chip_id = id->driver_data;
	tas_dev->sdw_peripheral = peripheral;
	tas_dev->hw_init = false;

	dev_set_drvdata(dev, tas_dev);

	tas_dev->regmap = devm_regmap_init_sdw(peripheral,
		&tasdevice_regmap);
	if (IS_ERR(tas_dev->regmap)) {
		ret = PTR_ERR(tas_dev->regmap);
		dev_err(dev, "Failed %d of devm_regmap_init_sdw.", ret);
	} else
		ret = tasdevice_init(tas_dev);

	if (ret < 0)
		tasdevice_remove(tas_dev);

	return ret;
}

static int tasdevice_sdw_remove(struct sdw_slave *peripheral)
{
	struct tasdevice_priv *tas_dev = dev_get_drvdata(&peripheral->dev);

	pm_runtime_disable(tas_dev->dev);
	tasdevice_remove(tas_dev);

	return 0;
}

static const struct sdw_device_id tasdevice_sdw_id[] = {
	SDW_SLAVE_ENTRY(0x0102, 0x0000, 0),
	{},
};

MODULE_DEVICE_TABLE(sdw, tasdevice_sdw_id);

static struct sdw_driver tasdevice_sdw_driver = {
	.driver = {
		.name = "slave-tas2783",
	},
	.probe = tasdevice_sdw_probe,
	.remove = tasdevice_sdw_remove,
	.ops = &tasdevice_sdw_ops,
	.id_table = tasdevice_sdw_id,
};

module_sdw_driver(tasdevice_sdw_driver);

MODULE_AUTHOR("Baojun Xu <baojun.xu@ti.com>");
MODULE_AUTHOR("Shenghao Ding <shenghao-ding@ti.com>");
MODULE_DESCRIPTION("ASoC TAS2783 SoundWire Driver");
MODULE_LICENSE("GPL");
