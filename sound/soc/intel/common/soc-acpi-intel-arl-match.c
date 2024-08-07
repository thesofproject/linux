// SPDX-License-Identifier: GPL-2.0-only
/*
 * soc-apci-intel-arl-match.c - tables and support for ARL ACPI enumeration.
 *
 * Copyright (c) 2023 Intel Corporation.
 */

#include <sound/soc-acpi.h>
#include <sound/soc-acpi-intel-match.h>

static const struct snd_soc_acpi_endpoint single_endpoint = {
	.num = 0,
	.aggregated = 0,
	.group_position = 0,
	.group_id = 0,
};

static const struct snd_soc_acpi_endpoint spk_l_endpoint = {
	.num = 0,
	.aggregated = 1,
	.group_position = 0,
	.group_id = 1,
};

static const struct snd_soc_acpi_endpoint spk_r_endpoint = {
	.num = 0,
	.aggregated = 1,
	.group_position = 1,
	.group_id = 1,
};

static const struct snd_soc_acpi_endpoint spk_2_endpoint = {
	.num = 0,
	.aggregated = 1,
	.group_position = 2,
	.group_id = 1,
};

static const struct snd_soc_acpi_endpoint spk_3_endpoint = {
	.num = 0,
	.aggregated = 1,
	.group_position = 3,
	.group_id = 1,
};

static const struct snd_soc_acpi_adr_device cs35l56_2_lr_adr[] = {
	{
		.adr = 0x00023001FA355601ull,
		.num_endpoints = 1,
		.endpoints = &spk_l_endpoint,
		.name_prefix = "AMP1"
	},
	{
		.adr = 0x00023101FA355601ull,
		.num_endpoints = 1,
		.endpoints = &spk_r_endpoint,
		.name_prefix = "AMP2"
	}
};

static const struct snd_soc_acpi_adr_device cs35l56_3_lr_adr[] = {
	{
		.adr = 0x00033001FA355601ull,
		.num_endpoints = 1,
		.endpoints = &spk_l_endpoint,
		.name_prefix = "AMP1"
	},
	{
		.adr = 0x00033401FA355601ull,
		.num_endpoints = 1,
		.endpoints = &spk_r_endpoint,
		.name_prefix = "AMP2"
	}
};

static const struct snd_soc_acpi_adr_device cs35l56_2_r_adr[] = {
	{
		.adr = 0x00023201FA355601ull,
		.num_endpoints = 1,
		.endpoints = &spk_r_endpoint,
		.name_prefix = "AMP3"
	},
	{
		.adr = 0x00023301FA355601ull,
		.num_endpoints = 1,
		.endpoints = &spk_3_endpoint,
		.name_prefix = "AMP4"
	}
};

static const struct snd_soc_acpi_adr_device cs35l56_3_l_adr[] = {
	{
		.adr = 0x00033001fa355601ull,
		.num_endpoints = 1,
		.endpoints = &spk_l_endpoint,
		.name_prefix = "AMP1"
	},
	{
		.adr = 0x00033101fa355601ull,
		.num_endpoints = 1,
		.endpoints = &spk_2_endpoint,
		.name_prefix = "AMP2"
	}
};

static const struct snd_soc_acpi_adr_device cs35l56_2_r1_adr[] = {
	{
		.adr = 0x00023101FA355601ull,
		.num_endpoints = 1,
		.endpoints = &spk_r_endpoint,
		.name_prefix = "AMP2"
	},
};

static const struct snd_soc_acpi_adr_device cs35l56_3_l1_adr[] = {
	{
		.adr = 0x00033301fa355601ull,
		.num_endpoints = 1,
		.endpoints = &spk_l_endpoint,
		.name_prefix = "AMP1"
	},
};

static const struct snd_soc_acpi_endpoint cs42l43_endpoints[] = {
	{ /* Jack Playback Endpoint */
		.num = 0,
		.aggregated = 0,
		.group_position = 0,
		.group_id = 0,
	},
	{ /* DMIC Capture Endpoint */
		.num = 1,
		.aggregated = 0,
		.group_position = 0,
		.group_id = 0,
	},
	{ /* Jack Capture Endpoint */
		.num = 2,
		.aggregated = 0,
		.group_position = 0,
		.group_id = 0,
	},
	{ /* Speaker Playback Endpoint */
		.num = 3,
		.aggregated = 0,
		.group_position = 0,
		.group_id = 0,
	},
};

static const struct snd_soc_acpi_adr_device cs42l43_0_adr[] = {
	{
		.adr = 0x00003001FA424301ull,
		.num_endpoints = ARRAY_SIZE(cs42l43_endpoints),
		.endpoints = cs42l43_endpoints,
		.name_prefix = "cs42l43"
	}
};

static const struct snd_soc_acpi_adr_device cs42l43_2_adr[] = {
	{
		.adr = 0x00023001FA424301ull,
		.num_endpoints = ARRAY_SIZE(cs42l43_endpoints),
		.endpoints = cs42l43_endpoints,
		.name_prefix = "cs42l43"
	}
};

static const struct snd_soc_acpi_adr_device rt711_0_adr[] = {
	{
		.adr = 0x000020025D071100ull,
		.num_endpoints = 1,
		.endpoints = &single_endpoint,
		.name_prefix = "rt711"
	}
};

static const struct snd_soc_acpi_adr_device rt711_sdca_0_adr[] = {
	{
		.adr = 0x000030025D071101ull,
		.num_endpoints = 1,
		.endpoints = &single_endpoint,
		.name_prefix = "rt711"
	}
};

static const struct snd_soc_acpi_link_adr arl_cs42l43_l0[] = {
	{
		.mask = BIT(0),
		.num_adr = ARRAY_SIZE(cs42l43_0_adr),
		.adr_d = cs42l43_0_adr,
	},
};

static const struct snd_soc_acpi_link_adr arl_cs42l43_l2[] = {
	{
		.mask = BIT(2),
		.num_adr = ARRAY_SIZE(cs42l43_2_adr),
		.adr_d = cs42l43_2_adr,
	},
};

static const struct snd_soc_acpi_link_adr arl_cs42l43_l2_cs35l56_l3[] = {
	{
		.mask = BIT(2),
		.num_adr = ARRAY_SIZE(cs42l43_2_adr),
		.adr_d = cs42l43_2_adr,
	},
	{
		.mask = BIT(3),
		.num_adr = ARRAY_SIZE(cs35l56_3_lr_adr),
		.adr_d = cs35l56_3_lr_adr,
	},
	{}
};

static const struct snd_soc_acpi_link_adr arl_cs42l43_l0_cs35l56_l2[] = {
	{
		.mask = BIT(0),
		.num_adr = ARRAY_SIZE(cs42l43_0_adr),
		.adr_d = cs42l43_0_adr,
	},
	{
		.mask = BIT(2),
		.num_adr = ARRAY_SIZE(cs35l56_2_lr_adr),
		.adr_d = cs35l56_2_lr_adr,
	},
	{}
};

static const struct snd_soc_acpi_link_adr arl_cs42l43_l0_cs35l56_l23[] = {
	{
		.mask = BIT(0),
		.num_adr = ARRAY_SIZE(cs42l43_0_adr),
		.adr_d = cs42l43_0_adr,
	},
	{
		.mask = BIT(2),
		.num_adr = ARRAY_SIZE(cs35l56_2_r_adr),
		.adr_d = cs35l56_2_r_adr,
	},
	{
		.mask = BIT(3),
		.num_adr = ARRAY_SIZE(cs35l56_3_l_adr),
		.adr_d = cs35l56_3_l_adr,
	},
	{}
};

static const struct snd_soc_acpi_link_adr arl_cs42l43_l0_cs35l56_2_l23[] = {
	{
		.mask = BIT(0),
		.num_adr = ARRAY_SIZE(cs42l43_0_adr),
		.adr_d = cs42l43_0_adr,
	},
	{
		.mask = BIT(2),
		.num_adr = ARRAY_SIZE(cs35l56_2_r1_adr),
		.adr_d = cs35l56_2_r1_adr,
	},
	{
		.mask = BIT(3),
		.num_adr = ARRAY_SIZE(cs35l56_3_l1_adr),
		.adr_d = cs35l56_3_l1_adr,
	},
	{}
};

static const struct snd_soc_acpi_link_adr arl_rvp[] = {
	{
		.mask = BIT(0),
		.num_adr = ARRAY_SIZE(rt711_0_adr),
		.adr_d = rt711_0_adr,
	},
	{}
};

static const struct snd_soc_acpi_link_adr arl_sdca_rvp[] = {
	{
		.mask = BIT(0),
		.num_adr = ARRAY_SIZE(rt711_sdca_0_adr),
		.adr_d = rt711_sdca_0_adr,
	},
	{}
};

struct snd_soc_acpi_mach snd_soc_acpi_intel_arl_machines[] = {
	{},
};
EXPORT_SYMBOL_GPL(snd_soc_acpi_intel_arl_machines);

/* this table is used when there is no I2S codec present */
struct snd_soc_acpi_mach snd_soc_acpi_intel_arl_sdw_machines[] = {
	{
		.link_mask = BIT(0) | BIT(2) | BIT(3),
		.links = arl_cs42l43_l0_cs35l56_l23,
		.drv_name = "sof_sdw",
		.sof_tplg_filename = "sof-arl-cs42l43-l0-cs35l56-l23.tplg",
	},
	{
		.link_mask = BIT(0) | BIT(2) | BIT(3),
		.links = arl_cs42l43_l0_cs35l56_2_l23,
		.drv_name = "sof_sdw",
		.sof_tplg_filename = "sof-arl-cs42l43-l0-cs35l56-l23.tplg",
	},
	{
		.link_mask = BIT(0) | BIT(2),
		.links = arl_cs42l43_l0_cs35l56_l2,
		.drv_name = "sof_sdw",
		.sof_tplg_filename = "sof-arl-cs42l43-l0-cs35l56-l2.tplg",
	},
	{
		.link_mask = BIT(0),
		.links = arl_cs42l43_l0,
		.drv_name = "sof_sdw",
		.sof_tplg_filename = "sof-arl-cs42l43-l0.tplg",
	},
	{
		.link_mask = BIT(2),
		.links = arl_cs42l43_l2,
		.drv_name = "sof_sdw",
		.sof_tplg_filename = "sof-arl-cs42l43-l2.tplg",
	},
	{
		.link_mask = BIT(2) | BIT(3),
		.links = arl_cs42l43_l2_cs35l56_l3,
		.drv_name = "sof_sdw",
		.sof_tplg_filename = "sof-arl-cs42l43-l2-cs35l56-l3.tplg",
	},
	{
		.link_mask = 0x1, /* link0 required */
		.links = arl_rvp,
		.drv_name = "sof_sdw",
		.sof_tplg_filename = "sof-arl-rt711.tplg",
	},
	{
		.link_mask = 0x1, /* link0 required */
		.links = arl_sdca_rvp,
		.drv_name = "sof_sdw",
		.sof_tplg_filename = "sof-arl-rt711-l0.tplg",
	},
	{},
};
EXPORT_SYMBOL_GPL(snd_soc_acpi_intel_arl_sdw_machines);
