/* SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause) */
/* Copyright(c) 2024 Intel Corporation. */

#ifndef __SDCA_FUNCTION_DEVICE_H
#define __SDCA_FUNCTION_DEVICE_H

#include <linux/auxiliary_bus.h>

struct sdca_device_data;
struct sdca_function_desc;
struct sdca_entity;
struct sdca_control;
struct regmap;

struct sdca_dev {
	struct auxiliary_device auxdev;
	struct sdca_function_desc *function_desc;
	struct regmap *regmap;
};

#define auxiliary_dev_to_sdca_dev(auxiliary_dev)		\
	container_of(auxiliary_dev, struct sdca_dev, auxdev)

int sdca_function_for_each_control(struct sdca_function_desc *func_desc,
				   int (*callback)(struct sdca_function_desc *,
						   struct sdca_entity *,
						   struct sdca_control *,
						   void *),
				   void *cookie);
int sdca_data_for_each_control(struct sdca_device_data *sdca_data,
			       int (*callback)(struct sdca_function_desc *,
					       struct sdca_entity *,
					       struct sdca_control *,
					       void *),
			       void *cookie);

#endif
