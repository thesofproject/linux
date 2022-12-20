// SPDX-License-Identifier: GPL-2.0-only
//
// Copyright (c) Intel Corporation, 2022
// Copyright Everest Semiconductor Co.,Ltd

#include <linux/module.h>
#include <linux/acpi.h>
#include "es83xx-dsm-common.h"

/* UUID ("a9800c04-e016-343e-41f4-6bcce70f4332") */
static const guid_t es83xx_dsm_guid =
	GUID_INIT(0xa9800c04, 0xe016, 0x343e,
		  0x41, 0xf4, 0x6b, 0xcc, 0xe7, 0x0f, 0x43, 0x32);

#define ES83xx_DSM_REVID 1

static int es83xx_dsm(struct device *dev, int arg, int *value)
{
	acpi_handle dhandle;
	union acpi_object *obj;
	int ret = 0;

	dhandle = ACPI_HANDLE(dev);
	if (!dhandle)
		return -ENOENT;

	obj = acpi_evaluate_dsm(dhandle, &es83xx_dsm_guid, ES83xx_DSM_REVID,
				arg, NULL);
	if (!obj) {
		dev_err(dev, "%s: acpi_evaluate_dsm() failed\n", __func__);
		ret = -EINVAL;
		goto out;
	}

	if (obj->type != ACPI_TYPE_INTEGER) {
		dev_err(dev, "%s: object is not ACPI_TYPE_INTEGER\n", __func__);
		ret = -EINVAL;
		goto err;
	}

	*value = obj->integer.value;
err:
	ACPI_FREE(obj);
out:
	return ret;
}

void es83xx_dsm_dump(struct device *dev)
{
	int value;
	int ret;

	/*
	 * The _DSM method returns 0xff if the argument is not handled.
	 */
	dev_dbg(dev, "=========== EM83XX ACPI _DSM TABLE DUMP ===========");

	ret = es83xx_dsm(dev, PLATFORM_MAINMIC_TYPE_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "PLATFORM_MAINMIC_TYPE_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, PLATFORM_HPMIC_TYPE_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "PLATFORM_HPMIC_TYPE_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, PLATFORM_SPK_TYPE_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "PLATFORM_SPK_TYPE_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, PLATFORM_HPDET_INV_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "PLATFORM_HPDET_INV_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, PLATFORM_PCM_TYPE_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "PLATFORM_PCM_TYPE_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, PLATFORM_MIC_DE_POP_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "PLATFORM_MIC_DE_POP_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, PLATFORM_CODEC_TYPE_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "PLATFORM_CODEC_TYPE_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, PLATFORM_BUS_SLOT_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "PLATFORM_BUS_SLOT_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, HP_CODEC_LINEIN_PGA_GAIN_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "HP_CODEC_LINEIN_PGA_GAIN_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, MAIN_CODEC_LINEIN_PGA_GAIN_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "MAIN_CODEC_LINEIN_PGA_GAIN_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, HP_CODEC_D2SEPGA_GAIN_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "HP_CODEC_D2SEPGA_GAIN_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, MAIN_CODEC_D2SEPGA_GAIN_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "MAIN_CODEC_D2SEPGA_GAIN_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, HP_CODEC_ADC_VOLUME_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "HP_CODEC_ADC_VOLUME_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, MAIN_CODEC_ADC_VOLUME_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "MAIN_CODEC_ADC_VOLUME_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, HP_CODEC_ADC_ALC_ENABLE_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "HP_CODEC_ADC_ALC_ENABLE_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, MAIN_CODEC_ADC_ALC_ENABLE_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "MAIN_CODEC_ADC_ALC_ENABLE_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, HP_CODEC_ADC_ALC_TARGET_LEVEL_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "HP_CODEC_ADC_ALC_TARGET_LEVEL_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, MAIN_CODEC_ADC_ALC_TARGET_LEVEL_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "MAIN_CODEC_ADC_ALC_TARGET_LEVEL_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, HP_CODEC_ADC_ALC_MAXGAIN_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "HP_CODEC_ADC_ALC_MAXGAIN_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, MAIN_CODEC_ADC_ALC_MAXGAIN_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "MAIN_CODEC_ADC_ALC_MAXGAIN_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, HP_CODEC_ADC_ALC_MINGAIN_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "HP_CODEC_ADC_ALC_MINGAIN_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, MAIN_CODEC_ADC_ALC_MINGAIN_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "MAIN_CODEC_ADC_ALC_MINGAIN_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, HP_CODEC_ADC_ALC_HLDTIME_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "HP_CODEC_ADC_ALC_HLDTIME_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, MAIN_CODEC_ADC_ALC_HLDTIME_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "MAIN_CODEC_ADC_ALC_HLDTIME_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, HP_CODEC_ADC_ALC_DCYTIME_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "HP_CODEC_ADC_ALC_DCYTIME_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, MAIN_CODEC_ADC_ALC_DCYTIME_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "MAIN_CODEC_ADC_ALC_DCYTIME_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, HP_CODEC_ADC_ALC_ATKTIME_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "HP_CODEC_ADC_ALC_ATKTIME_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, MAIN_CODEC_ADC_ALC_ATKTIME_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "MAIN_CODEC_ADC_ALC_ATKTIME_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, HP_CODEC_ADC_ALC_NGTYPE_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "HP_CODEC_ADC_ALC_NGTYPE_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, MAIN_CODEC_ADC_ALC_NGTYPE_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "MAIN_CODEC_ADC_ALC_NGTYPE_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, HP_CODEC_ADC_ALC_NGTHLD_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "HP_CODEC_ADC_ALC_NGTHLD_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, MAIN_CODEC_ADC_ALC_NGTHLD_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "MAIN_CODEC_ADC_ALC_NGTHLD_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, MAIN_CODEC_ADC_GUI_STEP_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "MAIN_CODEC_ADC_GUI_STEP_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, MAIN_CODEC_ADC_GUI_GAIN_RANGE_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "MAIN_CODEC_ADC_GUI_GAIN_RANGE_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, HEADPHONE_DUMMY_REMOVE_ENABLE_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "HEADPHONE_DUMMY_REMOVE_ENABLE_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, HP_CODEC_DAC_HPMIX_HIGAIN_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "HP_CODEC_DAC_HPMIX_HIGAIN_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, SPK_CODEC_DAC_HPMIX_HIGAIN_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "SPK_CODEC_DAC_HPMIX_HIGAIN_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, HP_CODEC_DAC_HPMIX_VOLUME_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "HP_CODEC_DAC_HPMIX_VOLUME_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, SPK_CODEC_DAC_HPMIX_VOLUME_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "SPK_CODEC_DAC_HPMIX_VOLUME_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, HP_CODEC_DAC_HPOUT_VOLUME_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "HP_CODEC_DAC_HPOUT_VOLUME_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, SPK_CODEC_DAC_HPOUT_VOLUME_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "SPK_CODEC_DAC_HPOUT_VOLUME_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, HP_CODEC_LDAC_VOLUME_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "HP_CODEC_LDAC_VOLUME_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, HP_CODEC_RDAC_VOLUME_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "HP_CODEC_RDAC_VOLUME_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, SPK_CODEC_LDAC_VOLUME_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "SPK_CODEC_LDAC_VOLUME_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, SPK_CODEC_RDAC_VOLUME_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "SPK_CODEC_RDAC_VOLUME_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, HP_CODEC_DAC_AUTOMUTE_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "HP_CODEC_DAC_AUTOMUTE_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, SPK_CODEC_DAC_AUTOMUTE_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "SPK_CODEC_DAC_AUTOMUTE_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, HP_CODEC_DAC_MONO_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "HP_CODEC_DAC_MONO_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, SPK_CODEC_DAC_MONO_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "SPK_CODEC_DAC_MONO_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, HP_CTL_IO_LEVEL_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "HP_CTL_IO_LEVEL_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, SPK_CTL_IO_LEVEL_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "SPK_CTL_IO_LEVEL_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, CODEC_GPIO0_FUNC_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "CODEC_GPIO0_FUNC_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, CODEC_GPIO1_FUNC_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "CODEC_GPIO1_FUNC_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, CODEC_GPIO2_FUNC_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "CODEC_GPIO2_FUNC_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, CODEC_GPIO3_FUNC_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "CODEC_GPIO3_FUNC_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, CODEC_GPIO4_FUNC_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "CODEC_GPIO4_FUNC_ARG=%#x\n", value);

	ret = es83xx_dsm(dev, PLATFORM_MCLK_LRCK_FREQ_ARG, &value);
	if (ret >= 0 && value != 0xff)
		dev_dbg(dev, "PLATFORM_MCLK_LRCK_FREQ_ARG=%#x\n", value);

	dev_dbg(dev, "===================================================");
}
EXPORT_SYMBOL_GPL(es83xx_dsm_dump);

int es83xx_dsm_jack_inverted(struct device *dev)
{
	int value;
	int ret;

	ret = es83xx_dsm(dev,  PLATFORM_HPDET_INV_ARG, &value);
	if (ret < 0)
		return ret;
	dev_info(dev, "HP jack detect inverted %d\n", value);
	return value;
}
EXPORT_SYMBOL_GPL(es83xx_dsm_jack_inverted);

int es83xx_dsm_is_gpio_level_low(struct device *dev, bool is_spk)
{
	int value;
	int ret;
	const char *type;

	if (is_spk) {
		type = "Speaker";
		ret = es83xx_dsm(dev, SPK_CTL_IO_LEVEL_ARG, &value);
	} else {
		type = "Headphone";
		ret = es83xx_dsm(dev, HP_CTL_IO_LEVEL_ARG, &value);
	}

	if (ret < 0) {
		dev_err(dev, "%s: Can't read I/O ctl level argument\n", type);
		return ret;
	}

	switch (value) {
	case GPIO_CTL_IO_LEVEL_HIGH:
	case 0xff:
		return 0;
	case GPIO_CTL_IO_LEVEL_LOW:
		return 1;
	default:
		dev_err(dev, "%s: invalid I/O ctl level argument (%d)\n",
			type, value);
		return -EINVAL;
	}
}
EXPORT_SYMBOL_GPL(es83xx_dsm_is_gpio_level_low);

int es83xx_dsm_mic_type(struct device *dev, bool is_main)
{
	int value;
	int ret;
	const char *type;

	if (is_main) {
		type = "main";
		ret = es83xx_dsm(dev, PLATFORM_MAINMIC_TYPE_ARG, &value);
	} else {
		type = "headset";
		ret = es83xx_dsm(dev, PLATFORM_HPMIC_TYPE_ARG, &value);
	}

	if (ret < 0) {
		dev_err(dev, "Can't read %s microphone type\n", type);
		return ret;
	}

	switch (value) {
	case PLATFORM_MIC_DMIC_HIGH_LEVEL:
	case PLATFORM_MIC_DMIC_LOW_LEVEL:
	case PLATFORM_MIC_AMIC_LIN1RIN1:
	case PLATFORM_MIC_AMIC_LIN2RIN2:
		return value;
	default:
		dev_err(dev, "Invalid value for %s microphone type (%d)\n",
			type, value);
		return -EINVAL;
	}
}
EXPORT_SYMBOL_GPL(es83xx_dsm_mic_type);

MODULE_DESCRIPTION("Everest Semi ES83xx DSM helpers");
MODULE_LICENSE("GPL");
