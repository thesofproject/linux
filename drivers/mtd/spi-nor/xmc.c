// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2005, Intec Automation Inc.
 * Copyright (C) 2014, Freescale Semiconductor, Inc.
 */

#include <linux/mtd/spi-nor.h>

#include "core.h"

static const struct flash_info xmc_nor_parts[] = {
	{
		.id = SNOR_ID(0x20, 0x70, 0x17),
		.name = "XM25QH64A",
		.size = SZ_8M,
		.no_sfdp_flags = SECT_4K | SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ,
	}, {
		.id = SNOR_ID(0x20, 0x70, 0x18),
		.name = "XM25QH128A",
		.size = SZ_16M,
		.no_sfdp_flags = SECT_4K | SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ,
	}, {
		.id = SNOR_ID(0x20, 0x41, 0x19),
		.name = "XM25QU256C",
		.size = SZ_32M,
		.no_sfdp_flags = SECT_4K | SPI_NOR_DUAL_READ | SPI_NOR_QUAD_READ | SPI_NOR_4B_OPCODES,
		.flags = SPI_NOR_HAS_LOCK | SPI_NOR_HAS_TB | SPI_NOR_TB_SR_BIT6 | SPI_NOR_4BIT_BP,
	}, {
		.id = SNOR_ID(0x20, 0x40, 0x18),
		.name = "XM25QH128C",
		.size = SZ_16M,
		.flags = SPI_NOR_HAS_LOCK | SPI_NOR_HAS_TB,
	}, {
		.id = SNOR_ID(0x20, 0x40, 0x19),
		.name = "XM25QH256C",
		.size = SZ_32M,
		.flags = SPI_NOR_HAS_LOCK | SPI_NOR_HAS_TB | SPI_NOR_4BIT_BP | SPI_NOR_TB_SR_BIT6,
	},
};

/* XMC (Wuhan Xinxin Semiconductor Manufacturing Corp.) */
const struct spi_nor_manufacturer spi_nor_xmc = {
	.name = "xmc",
	.parts = xmc_nor_parts,
	.nparts = ARRAY_SIZE(xmc_nor_parts),
};
