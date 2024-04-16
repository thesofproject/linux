/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause) */
/*
 * The MIPI SDCA specification is available for public downloads at
 * https://www.mipi.org/mipi-sdca-v1-0-download
 *
 * Copyright(c) 2024 Intel Corporation
 */

#ifndef __SDCA_H__
#define __SDCA_H__

struct sdca_data;
struct sdw_slave;

#if IS_ENABLED(CONFIG_ACPI) && IS_ENABLED(CONFIG_SND_SOC_SDCA)

int sdca_alloc(void **context,
	       struct device *parent,
	       struct sdw_slave *slave);

#else

static inline int sdca_alloc(void **context,
			     struct device *parent,
			     struct sdw_slave *slave)
{
	return 0;
}

#endif

#endif
