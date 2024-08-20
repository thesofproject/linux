/* SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause) */
/* Copyright(c) 2024 Intel Corporation. */

#ifndef __SDCA_FUNCTION_DEVICE_H
#define __SDCA_FUNCTION_DEVICE_H

struct regmap;

struct sdca_dev {
	struct auxiliary_device auxdev;
	struct sdca_function_desc *function_desc;
	struct regmap *regmap;
};

#define auxiliary_dev_to_sdca_dev(auxiliary_dev)		\
	container_of(auxiliary_dev, struct sdca_dev, auxdev)

#endif
