/* SPDX-License-Identifier: GPL-2.0
 *
 * ALSA SoC Texas Instruments TAS2783 Audio Smart Amplifier
 *
 * Copyright (C) 2023 Texas Instruments Incorporated
 * https://www.ti.com
 *
 * The TAS2783 driver implements a flexible and configurable
 * algorithm coefficient setting for single TAS2783 chips.
 *
 * Author: Baojun Xu <baojun.xu@ti.com>
 * 			Shenghao Ding <shenghao-ding@ti.com>
 */

#ifndef __TAS2783_H__
#define __TAS2783_H__

#define TAS2783_DEVICE_RATES			(SNDRV_PCM_RATE_44100 | \
										SNDRV_PCM_RATE_48000 | \
										SNDRV_PCM_RATE_96000 | \
										SNDRV_PCM_RATE_88200)

#define TAS2783_DEVICE_FORMATS			(SNDRV_PCM_FMTBIT_S16_LE | \
										SNDRV_PCM_FMTBIT_S24_LE | \
										SNDRV_PCM_FMTBIT_S32_LE)

/* BOOK, PAGE Control Register */
#define TASDEVICE_REG(book, page, reg)	((book * 256 * 256) + 0x8000 +\
										(page * 128) + reg)

/*Software Reset */
#define TAS2873_REG_SWRESET				TASDEVICE_REG(0x0, 0X0, 0x01)

/* Volume control */
#define TAS2783_DVC_LVL					TASDEVICE_REG(0x0, 0x00, 0x1A)
#define TAS2783_AMP_LEVEL				TASDEVICE_REG(0x0, 0x00, 0x03)
#define TAS2783_AMP_LEVEL_MASK			GENMASK(5, 1)

/* Calibration data */
#define TAS2783_CALIBRATION_RE			TASDEVICE_REG(0x0, 0x17, 0x74)
#define TAS2783_CALIBRATION_RE_LOW		TASDEVICE_REG(0x0, 0x18, 0x14)
#define TAS2783_CALIBRATION_INV_RE		TASDEVICE_REG(0x0, 0x18, 0x0c)
#define TAS2783_CALIBRATION_POW			TASDEVICE_REG(0x0, 0x0d, 0x3c)
#define TAS2783_CALIBRATION_TLIMIT 		TASDEVICE_REG(0x0, 0x18, 0x7c)

#define TAS2783_ID_MIN					0x08	// Unique id start
#define TAS2783_ID_MAX					0x0F	// Unique id end

/* TAS2783 SDCA Control - function number */
#define FUNC_NUM_SMART_AMP				0x01

/* TAS2783 SDCA entity */
#define TAS2783_SDCA_ENT_PDE23			0x0C
#define TAS2783_SDCA_ENT_PDE22			0x0B
#define TAS2783_SDCA_ENT_FU21			0x01
#define TAS2783_SDCA_ENT_UDMPU21		0x10

/* TAS2783 SDCA control */
#define TAS2783_SDCA_CTL_REQ_POWER_STATE	0x01
#define TAS2783_SDCA_CTL_FU_MUTE		0x01
#define TAS2783_SDCA_CTL_UDMPU_CLUSTER	0x10

#define TAS2783_DEVICE_CHANNEL_LEFT		1
#define TAS2783_DEVICE_CHANNEL_RIGHT	2

#define TAS2783_MAX_CALIDATA_SIZE		252

struct tas2783_firmware_node {
	unsigned int vendor_id;
	unsigned int file_id;
	unsigned int version_id;
	unsigned int length;
	unsigned int download_addr;
};

struct calibration_data {
	unsigned long total_sz;
	unsigned char data[TAS2783_MAX_CALIDATA_SIZE];
};

struct tasdevice_priv {
	struct snd_soc_component *component;
	struct calibration_data cali_data;
	struct sdw_slave *sdw_peripheral;
	enum sdw_slave_status status;
	struct sdw_bus_params params;
	struct mutex codec_lock;
	struct regmap *regmap;
	struct device *dev;
	struct tm tm;
	unsigned char rca_binaryname[64];
	unsigned char dev_name[32];
	unsigned int chip_id;
	bool pstream;
	bool hw_init;
};

struct sdw_stream_data {
	struct sdw_stream_runtime *sdw_stream;
};

#endif /*__TAS2783_H__ */
