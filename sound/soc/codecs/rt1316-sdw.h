/* SPDX-License-Identifier: GPL-2.0 */
/*
 * rt1316-sdw.h -- RT1316 ALSA SoC audio driver header
 *
 * Copyright(c) 2020 Realtek Semiconductor Corp.
 */

#ifndef __RT1316_SDW_H__
#define __RT1316_SDW_H__

/* v1.2 device - SDCA address mapping */
#define RT1316_SDCA_CTL(fun, ent, ctl, ch) (BIT(30) |	\
					    (((fun) & 0x7) << 22) |	\
					    (((ent) & 0x40) << 15) |	\
					    (((ent) & 0x3f) << 7) |	\
					    (((ctl) & 0x30) << 15) |	\
					    (((ctl) & 0x0f) << 3) |	\
					    (((ch) & 0x38) << 12) |	\
					    ((ch) & 0x07))

/* SDCA function */
#define FUN_SMART_AMP 0x04

/* SDCA entity */
#define ENT_PDE23 0x31
#define ENT_PDE27 0x32
#define ENT_PDE22 0x33
#define ENT_PDE24 0x34
#define ENT_XU24 0x24
#define ENT_FU21 0x03
#define ENT_UDMPU21 0x02

/* SDCA control */
#define CTL_SAMPLE_FREQ_INDEX 0x10
#define CTL_REQ_POWER_STATE 0x01
#define CTL_BYPASS 0x01
#define CTL_FU_MUTE 0x01
#define CTL_FU_VOLUME 0x02
#define CTL_UDMPU_CLUSTER 0x10

/* SDCA channel */
#define CH_L 0x01
#define CH_R 0x02

/* Power State */
#define PS0 0x00
#define PS3 0x03

/* Mute Control */
#define UNMUTE 0x00
#define MUTE 0x01

static const struct reg_default rt1316_reg_defaults[] = {
	{ 0x3201, 0x00 },
	{ 0x3202, 0x00 },
	{ 0x3203, 0x01 },
	{ 0x3204, 0x07 },
	{ 0x3205, 0x00 },
	{ 0x3206, 0x00 },
	{ 0x3207, 0x00 },
	{ 0x3208, 0x09 },
	{ 0x3209, 0x09 },
	{ 0x320a, 0x00 },
	{ 0x320b, 0x00 },
	{ 0x320c, 0x00 },
	{ 0x320d, 0x00 },
	{ 0x320e, 0x00 },

	{ 0xc000, 0x00 },
	{ 0xc001, 0x00 },
	{ 0xc002, 0x00 },
	{ 0xc003, 0x00 },
	{ 0xc004, 0x00 },
	{ 0xc005, 0x00 },
	{ 0xc006, 0x00 },
	{ 0xc007, 0x00 },
	{ 0xc008, 0x00 },
	{ 0xc009, 0x00 },
	{ 0xc00a, 0x00 },
	{ 0xc00b, 0x00 },
	{ 0xc00c, 0x00 },
	{ 0xc00d, 0x00 },
	{ 0xc00e, 0x00 },
	{ 0xc00f, 0x00 },
	{ 0xc010, 0xa5 },
	{ 0xc011, 0x00 },
	{ 0xc012, 0xff },
	{ 0xc013, 0xff },
	{ 0xc014, 0x40 },
	{ 0xc015, 0x00 },
	{ 0xc016, 0x00 },
	{ 0xc017, 0x00 },

	{ 0xc605, 0x30 },
	{ 0xc700, 0x0a },
	{ 0xc701, 0xaa },
	{ 0xc702, 0x1a },
	{ 0xc703, 0x0a },
	{ 0xc710, 0x80 },
	{ 0xc711, 0x00 },
	{ 0xc712, 0x3e },
	{ 0xc713, 0x80 },
	{ 0xc714, 0x80 },
	{ 0xc715, 0x06 },
	{ 0xd101, 0x00 },
	{ 0xd102, 0x30 },
	{ 0xd103, 0x00 },

	{ RT1316_SDCA_CTL(FUN_SMART_AMP,
		ENT_UDMPU21, CTL_UDMPU_CLUSTER, 0), 0x00 },
	{ RT1316_SDCA_CTL(FUN_SMART_AMP, ENT_FU21, CTL_FU_MUTE, CH_L), 0x01 },
	{ RT1316_SDCA_CTL(FUN_SMART_AMP, ENT_FU21, CTL_FU_MUTE, CH_R), 0x01 },
	{ RT1316_SDCA_CTL(FUN_SMART_AMP, ENT_XU24, CTL_BYPASS, 0), 0x01 },
	{ RT1316_SDCA_CTL(FUN_SMART_AMP,
		ENT_PDE23, CTL_REQ_POWER_STATE, 0), 0x03 },
	{ RT1316_SDCA_CTL(FUN_SMART_AMP,
		ENT_PDE22, CTL_REQ_POWER_STATE, 0), 0x03 },
	{ RT1316_SDCA_CTL(FUN_SMART_AMP,
		ENT_PDE24, CTL_REQ_POWER_STATE, 0), 0x03 },
};

struct rt1316_sdw_priv {
	struct snd_soc_component *component;
	struct regmap *regmap;
	struct sdw_slave *sdw_slave;
	enum sdw_slave_status status;
	struct sdw_bus_params params;
	bool hw_init;
	bool first_hw_init;
	int rx_mask;
	int slots;
};

struct sdw_stream_data {
	struct sdw_stream_runtime *sdw_stream;
};

#endif /* __RT1316_SDW_H__ */
