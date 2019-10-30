/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright 2020 NXP
 */

#ifndef __LINUX_SND_SOC_OF_H
#define __LINUX_SND_SOC_OF_H

#include <linux/of.h>

/**
 * snd_soc_of_mach: DT-based machine driver descriptor
 *
 * @drv_name: machine driver name
 * @of: DT node providing machine driver description
 */
struct snd_soc_of_mach {
	const char *drv_name;
	struct device_node *of;
};

#endif
