// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
// Copyright(c) 2024 Intel Corporation.

/*
 * SDCA Function Device management
 */

#include <linux/acpi.h>
#include <linux/module.h>
#include <linux/auxiliary_bus.h>
#include <linux/regmap.h>
#include <linux/sort.h>
#include <linux/soundwire/sdw.h>
#include <linux/soundwire/sdw_registers.h>
#include <sound/sdca.h>
#include <sound/sdca_function.h>
#include "sdca_function_device.h"

/*
 * A SoundWire device can have multiple SDCA functions identified by
 * their type and ADR. there can be multiple SoundWire devices per
 * link, or multiple devices spread across multiple links. An IDA is
 * required to identify each instance.
 */
static DEFINE_IDA(sdca_function_ida);

static void sdca_dev_release(struct device *dev)
{
	struct auxiliary_device *auxdev = to_auxiliary_dev(dev);
	struct sdca_dev *sdev = auxiliary_dev_to_sdca_dev(auxdev);

	ida_free(&sdca_function_ida, auxdev->id);
	kfree(sdev);
}

/* alloc, init and add link devices */
static struct sdca_dev *sdca_dev_register(struct device *parent,
					  struct sdca_function_desc *function_desc,
					  struct regmap *regmap)
{
	struct sdca_dev *sdev;
	struct auxiliary_device *auxdev;
	int ret;
	int rc;

	sdev = kzalloc(sizeof(*sdev), GFP_KERNEL);
	if (!sdev)
		return ERR_PTR(-ENOMEM);

	auxdev = &sdev->auxdev;
	auxdev->name = function_desc->name;
	auxdev->dev.parent = parent;
	auxdev->dev.fwnode = function_desc->function_node;
	auxdev->dev.release = sdca_dev_release;

	sdev->regmap = regmap;
	sdev->function_desc = function_desc;

	rc = ida_alloc(&sdca_function_ida, GFP_KERNEL);
	if (rc < 0) {
		kfree(sdev);
		return ERR_PTR(rc);
	}
	auxdev->id = rc;

	/* now follow the two-step init/add sequence */
	ret = auxiliary_device_init(auxdev);
	if (ret < 0) {
		dev_err(parent, "failed to initialize SDCA function dev %s\n",
			function_desc->name);
		ida_free(&sdca_function_ida, auxdev->id);
		kfree(sdev);
		return ERR_PTR(ret);
	}

	ret = auxiliary_device_add(auxdev);
	if (ret < 0) {
		dev_err(parent, "failed to add SDCA function dev %s\n",
			sdev->auxdev.name);
		/* sdev will be freed with the put_device() and .release sequence */
		auxiliary_device_uninit(&sdev->auxdev);
		return ERR_PTR(ret);
	}

	return sdev;
}

static void sdca_dev_unregister(struct sdca_dev *sdev)
{
	auxiliary_device_delete(&sdev->auxdev);
	auxiliary_device_uninit(&sdev->auxdev);
}

int sdca_dev_parse_functions(struct sdw_slave *slave)
{
	struct sdca_device_data *sdca_data = &slave->sdca_data;
	struct device *dev = &slave->dev;
	struct sdca_function_desc *func_desc;
	struct sdca_function_data *func;
	int ret;
	int i;

	for (i = 0; i < sdca_data->num_functions; i++) {
		func_desc = &sdca_data->sdca_func[i];

		func = devm_kzalloc(dev, sizeof(*func), GFP_KERNEL);
		if (!func)
			return -ENOMEM;

		func_desc->function = func;

		ret = sdca_parse_function(dev, func_desc->function_node, func_desc);
		if (ret)
			return ret;
	}

	return 0;
}
EXPORT_SYMBOL_NS(sdca_dev_parse_functions, SND_SOC_SDCA);

int sdca_dev_register_functions(struct sdw_slave *slave, struct regmap *regmap)
{
	struct sdca_device_data *sdca_data = &slave->sdca_data;
	int i;

	for (i = 0; i < sdca_data->num_functions; i++) {
		struct sdca_dev *func_dev;

		func_dev = sdca_dev_register(&slave->dev,
					     &sdca_data->sdca_func[i], regmap);
		if (!func_dev)
			return -ENODEV;

		sdca_data->sdca_func[i].func_dev = func_dev;
	}

	return 0;
}
EXPORT_SYMBOL_NS(sdca_dev_register_functions, SND_SOC_SDCA);

void sdca_dev_unregister_functions(struct sdw_slave *slave)
{
	struct sdca_device_data *sdca_data = &slave->sdca_data;
	int i;

	for (i = 0; i < sdca_data->num_functions; i++)
		sdca_dev_unregister(sdca_data->sdca_func[i].func_dev);
}
EXPORT_SYMBOL_NS(sdca_dev_unregister_functions, SND_SOC_SDCA);

int sdca_function_for_each_control(struct sdca_function_desc *func_desc,
				   int (*callback)(struct sdca_function_desc *,
						   struct sdca_entity *,
						   struct sdca_control *,
						   void *),
				   void *cookie)
{
	struct sdca_entity *entity;
	int i, j;
	int ret;

	for (i = 0; i < func_desc->function->num_entities; i++) {
		entity = &func_desc->function->entities[i];

		for (j = 0; j < entity->num_controls; j++) {
			ret = callback(func_desc, entity,
				       &entity->controls[j], cookie);
			if (ret)
				return ret;
		}
	}

	return 0;
}

int sdca_data_for_each_control(struct sdca_device_data *sdca_data,
			       int (*callback)(struct sdca_function_desc *,
					       struct sdca_entity *,
					       struct sdca_control *,
					       void *),
			       void *cookie)
{
	int ret;
	int i;

	for (i = 0; i < sdca_data->num_functions; i++) {
		ret = sdca_function_for_each_control(&sdca_data->sdca_func[i],
						     callback, cookie);
		if (ret)
			return ret;
	}

	return 0;
}

static int sdca_constants_count(struct sdca_function_desc *func_desc,
				struct sdca_entity *entity,
				struct sdca_control *control,
				void *cookie)
{
	int *count = cookie;

	if (control->mode == SDCA_CONTROL_ACCESS_MODE_DC)
		(*count)++;

	return 0;
}

static int sdca_constants_save(struct sdca_function_desc *func_desc,
			      struct sdca_entity *entity,
			      struct sdca_control *control,
			      void *cookie)
{
	struct reg_default **values = cookie;

	if (control->mode == SDCA_CONTROL_ACCESS_MODE_DC) {
		(*values)->reg = SDW_SDCA_CTL(func_desc->adr, entity->id,
					      control->id, 0);
		(*values)->def = control->value;
		(*values)++;
	}

	return 0;
}

static void sdca_constants_swap(void *a, void *b, int size)
{
	struct reg_default *x = a;
	struct reg_default *y = b;
	struct reg_default tmp;

	tmp = *x;
	*x = *y;
	*y = tmp;
}

static int sdca_constants_cmp(const void *a, const void *b)
{
	const struct reg_default *x = a;
	const struct reg_default *y = b;

	if (x->reg > y->reg)
		return 1;
	else if (x->reg < y->reg)
		return -1;
	else
		return 0;
}

int sdca_dev_populate_constants(struct sdw_slave *slave,
				struct regmap_config *config)
{
	struct sdca_device_data *sdca_data = &slave->sdca_data;
	struct reg_default *values, *tmp;
	int nvalues = 0;
	int ret;

	ret = sdca_data_for_each_control(sdca_data, sdca_constants_count, &nvalues);
	if (ret)
		return ret;

	values = devm_kcalloc(&slave->dev, nvalues, sizeof(*values), GFP_KERNEL);
	if (!values)
		return -ENOMEM;

	tmp = values;

	ret = sdca_data_for_each_control(sdca_data, sdca_constants_save, &tmp);
	if (ret)
		return ret;

	sort(values, nvalues, sizeof(*values), sdca_constants_cmp, sdca_constants_swap);

	config->reg_defaults = values;
	config->num_reg_defaults = nvalues;

	return 0;
}
EXPORT_SYMBOL_NS(sdca_dev_populate_constants, SND_SOC_SDCA);
