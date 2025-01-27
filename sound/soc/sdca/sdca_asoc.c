// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Cirrus Logic, Inc. and
//                    Cirrus Logic International Semiconductor Ltd.

/*
 * The MIPI SDCA specification is available for public downloads at
 * https://www.mipi.org/mipi-sdca-v1-0-download
 */

#include <linux/bitmap.h>
#include <linux/delay.h>
#include <linux/dev_printk.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/soundwire/sdw_registers.h>
#include <linux/string_helpers.h>
#include <sound/control.h>
#include <sound/sdca.h>
#include <sound/sdca_asoc.h>
#include <sound/sdca_function.h>
#include <sound/soc.h>
#include <sound/soc-component.h>
#include <sound/soc-dapm.h>
#include <sound/tlv.h>

static struct sdca_control *selector_find_control(struct sdca_entity *entity,
						  const int sel)
{
	int i;

	for (i = 0; i < entity->num_controls; i++) {
		struct sdca_control *control = &entity->controls[i];

		if (control->sel == sel)
			return control;
	}

	return NULL;
}

static struct sdca_control_range *control_find_range(struct device *dev,
						     struct sdca_entity *entity,
						     struct sdca_control *control,
						     int cols, int rows)
{
	struct sdca_control_range *range = &control->range;

	if ((cols && range->cols != cols) || (rows && range->rows != rows) ||
	    !range->data) {
		dev_err(dev, "%s: control %#x: ranges invalid (%d,%d)\n",
			entity->label, control->sel, range->cols, range->rows);
		return NULL;
	}

	return range;
}

static struct sdca_control_range *selector_find_range(struct device *dev,
						      struct sdca_entity *entity,
						      int sel, int cols, int rows)
{
	struct sdca_control *control;

	control = selector_find_control(entity, sel);
	if (!control) {
		dev_err(dev, "%s: control %#x: missing\n", entity->label, sel);
		return NULL;
	}

	return control_find_range(dev, entity, control, cols, rows);
}

static bool exported_control(struct sdca_control *control)
{
	/* No need to export control for something that only has one value */
	if (control->has_fixed || control->mode == SDCA_ACCESS_MODE_DC)
		return false;

	return control->layers & (SDCA_ACCESS_LAYER_USER |
				  SDCA_ACCESS_LAYER_APPLICATION);
}

int sdca_asoc_count_component(struct device *dev, struct sdca_function_data *function,
			      int *num_widgets, int *num_routes, int *num_controls,
			      int *num_dais)
{
	int i, j;

	*num_widgets = function->num_entities - 1;
	*num_routes = 0;
	*num_controls = 0;
	*num_dais = 0;

	for (i = 0; i < function->num_entities - 1; i++) {
		struct sdca_entity *entity = &function->entities[i];

		switch (entity->type) {
		case SDCA_ENTITY_TYPE_IT:
		case SDCA_ENTITY_TYPE_OT:
			*num_routes += !!entity->iot.clock;
			*num_routes += !!entity->iot.is_dataport;
			*num_dais += !!entity->iot.is_dataport;
			break;
		case SDCA_ENTITY_TYPE_PDE:
			*num_routes += entity->pde.num_managed;
			break;
		default:
			break;
		}

		*num_routes += entity->num_sources;

		for (j = 0; j < entity->num_controls; j++) {
			if (exported_control(&entity->controls[j]))
				(*num_controls)++;
		}
	}

	return 0;
}
EXPORT_SYMBOL_NS(sdca_asoc_count_component, "SND_SOC_SDCA");

static const char *get_terminal_name(enum sdca_terminal_type type)
{
	switch (type) {
	case SDCA_TERM_TYPE_LINEIN_STEREO:
		return SDCA_TERM_TYPE_LINEIN_STEREO_NAME;
	case SDCA_TERM_TYPE_LINEIN_FRONT_LR:
		return SDCA_TERM_TYPE_LINEIN_FRONT_LR_NAME;
	case SDCA_TERM_TYPE_LINEIN_CENTER_LFE:
		return SDCA_TERM_TYPE_LINEIN_CENTER_LFE_NAME;
	case SDCA_TERM_TYPE_LINEIN_SURROUND_LR:
		return SDCA_TERM_TYPE_LINEIN_SURROUND_LR_NAME;
	case SDCA_TERM_TYPE_LINEIN_REAR_LR:
		return SDCA_TERM_TYPE_LINEIN_REAR_LR_NAME;
	case SDCA_TERM_TYPE_LINEOUT_STEREO:
		return SDCA_TERM_TYPE_LINEOUT_STEREO_NAME;
	case SDCA_TERM_TYPE_LINEOUT_FRONT_LR:
		return SDCA_TERM_TYPE_LINEOUT_FRONT_LR_NAME;
	case SDCA_TERM_TYPE_LINEOUT_CENTER_LFE:
		return SDCA_TERM_TYPE_LINEOUT_CENTER_LFE_NAME;
	case SDCA_TERM_TYPE_LINEOUT_SURROUND_LR:
		return SDCA_TERM_TYPE_LINEOUT_SURROUND_LR_NAME;
	case SDCA_TERM_TYPE_LINEOUT_REAR_LR:
		return SDCA_TERM_TYPE_LINEOUT_REAR_LR_NAME;
	case SDCA_TERM_TYPE_MIC_JACK:
		return SDCA_TERM_TYPE_MIC_JACK_NAME;
	case SDCA_TERM_TYPE_STEREO_JACK:
		return SDCA_TERM_TYPE_STEREO_JACK_NAME;
	case SDCA_TERM_TYPE_FRONT_LR_JACK:
		return SDCA_TERM_TYPE_FRONT_LR_JACK_NAME;
	case SDCA_TERM_TYPE_CENTER_LFE_JACK:
		return SDCA_TERM_TYPE_CENTER_LFE_JACK_NAME;
	case SDCA_TERM_TYPE_SURROUND_LR_JACK:
		return SDCA_TERM_TYPE_SURROUND_LR_JACK_NAME;
	case SDCA_TERM_TYPE_REAR_LR_JACK:
		return SDCA_TERM_TYPE_REAR_LR_JACK_NAME;
	case SDCA_TERM_TYPE_HEADPHONE_JACK:
		return SDCA_TERM_TYPE_HEADPHONE_JACK_NAME;
	case SDCA_TERM_TYPE_HEADSET_JACK:
		return SDCA_TERM_TYPE_HEADSET_JACK_NAME;
	default:
		return NULL;
	}
}

static int sdca_asoc_early_ge(struct device *dev,
			      struct sdca_function_data *function,
			      struct sdca_entity *entity)
{
	struct sdca_control_range *range;
	struct sdca_control *control;
	struct snd_kcontrol_new *kctl;
	struct soc_enum *soc_enum;
	const char *control_name;
	unsigned int *values;
	const char **texts;
	int i;

	control = selector_find_control(entity, SDCA_CTL_GE_SELECTED_MODE);
	if (!control) {
		dev_err(dev, "%s: no selected mode control\n", entity->label);
		return -EINVAL;
	}

	if (control->layers != SDCA_ACCESS_LAYER_CLASS)
		dev_warn(dev, "%s: unexpected access layer: %x\n",
			 entity->label, control->layers);

	range = control_find_range(dev, entity, control, SDCA_SELECTED_MODE_NCOLS, 0);
	if (!range)
		return -EINVAL;

	control_name = devm_kasprintf(dev, GFP_KERNEL, "%s %s",
				      entity->label, control->label);
	if (!control_name)
		return -ENOMEM;

	kctl = devm_kmalloc(dev, sizeof(*kctl), GFP_KERNEL);
	if (!kctl)
		return -ENOMEM;

	soc_enum = devm_kmalloc(dev, sizeof(*soc_enum), GFP_KERNEL);
	if (!soc_enum)
		return -ENOMEM;

	texts = devm_kcalloc(dev, range->rows + 3, sizeof(*texts), GFP_KERNEL);
	if (!texts)
		return -ENOMEM;

	values = devm_kcalloc(dev, range->rows + 3, sizeof(*values), GFP_KERNEL);
	if (!values)
		return -ENOMEM;

	texts[0] = "No Jack";
	texts[1] = "Jack Unknown";
	texts[2] = "Detection in Progress";
	values[0] = 0;
	values[1] = 1;
	values[2] = 2;
	for (i = 0; i < range->rows; i++) {
		enum sdca_terminal_type type;

		type = sdca_range(range, SDCA_SELECTED_MODE_TERM_TYPE, i);

		values[i + 3] = sdca_range(range, SDCA_SELECTED_MODE_INDEX, i);
		texts[i + 3] = get_terminal_name(type);
		if (!texts[i + 3]) {
			dev_err(dev, "%s: Unrecognised terminal type: %#x\n",
				entity->label, type);
			return -EINVAL;
		}
	}

	soc_enum->reg = SDW_SDCA_CTL(function->desc->adr, entity->id, control->sel, 0);
	soc_enum->items = range->rows + 3;
	soc_enum->mask = roundup_pow_of_two(soc_enum->items) - 1;
	soc_enum->texts = texts;

	kctl->iface = SNDRV_CTL_ELEM_IFACE_MIXER;
	kctl->name = control_name;
	kctl->info = snd_soc_info_enum_double;
	kctl->get = snd_soc_dapm_get_enum_double;
	kctl->put = snd_soc_dapm_put_enum_double;
	kctl->private_value = (unsigned long)soc_enum;

	entity->ge.kctl = kctl;

	return 0;
}

static void add_route(struct snd_soc_dapm_route **route, const char *sink,
		      const char *control, const char *source)
{
	(*route)->sink = sink;
	(*route)->control = control;
	(*route)->source = source;
	(*route)++;
}

static int sdca_asoc_parse_simple(struct device *dev,
				  struct sdca_function_data *function,
				  struct sdca_entity *entity,
				  struct snd_soc_dapm_widget **widget,
				  struct snd_soc_dapm_route **route,
				  enum snd_soc_dapm_type id)
{
	int i;

	(*widget)->id = id;
	(*widget)++;

	for (i = 0; i < entity->num_sources; i++)
		add_route(route, entity->label, NULL, entity->sources[i]->label);

	return 0;
}

static int sdca_asoc_parse_it(struct device *dev,
			      struct sdca_function_data *function,
			      struct sdca_entity *entity,
			      struct snd_soc_dapm_widget **widget,
			      struct snd_soc_dapm_route **route)
{
	int i;

	if (entity->iot.is_dataport) {
		const char *aif_name = devm_kasprintf(dev, GFP_KERNEL, "%s %s",
						      entity->label, "Playback");
		if (!aif_name)
			return -ENOMEM;

		(*widget)->id = snd_soc_dapm_aif_in;

		add_route(route, entity->label, NULL, aif_name);
	} else if (entity->iot.type == SDCA_TERM_TYPE_SENSE_DATA) {
		//FIXME: Is there a better solution to sense data?
		/*
		 * Sense data has to be a supply to stop everything
		 * powering up.
		 */
		(*widget)->id = snd_soc_dapm_supply;
	} else {
		(*widget)->id = snd_soc_dapm_input;
	}

	if (entity->iot.clock)
		add_route(route, entity->label, NULL, entity->iot.clock->label);

	for (i = 0; i < entity->num_sources; i++)
		add_route(route, entity->label, NULL, entity->sources[i]->label);

	(*widget)++;

	return 0;
}

static int sdca_asoc_parse_ot(struct device *dev,
			      struct sdca_function_data *function,
			      struct sdca_entity *entity,
			      struct snd_soc_dapm_widget **widget,
			      struct snd_soc_dapm_route **route)
{
	int i;

	if (entity->iot.is_dataport) {
		const char *aif_name = devm_kasprintf(dev, GFP_KERNEL, "%s %s",
						      entity->label, "Capture");
		if (!aif_name)
			return -ENOMEM;

		(*widget)->id = snd_soc_dapm_aif_out;

		add_route(route, aif_name, NULL, entity->label);
	} else {
		(*widget)->id = snd_soc_dapm_output;
	}

	if (entity->iot.clock)
		add_route(route, entity->label, NULL, entity->iot.clock->label);

	for (i = 0; i < entity->num_sources; i++)
		add_route(route, entity->label, NULL, entity->sources[i]->label);

	(*widget)++;

	return 0;
}

static int sdca_asoc_pde_event(struct snd_soc_dapm_widget *widget,
			       struct snd_kcontrol *kctl, int event)
{
	struct snd_soc_component *component = widget->dapm->component;
	struct sdca_entity *entity = widget->priv;
	static const int poll_us = 10000;
	int polls = 1;
	unsigned int reg, val;
	int from, to, i;
	int ret;

	if (!component)
		return -EIO;

	switch (event) {
	case SND_SOC_DAPM_POST_PMD:
		from = widget->on_val;
		to = widget->off_val;
		break;
	case SND_SOC_DAPM_POST_PMU:
		from = widget->off_val;
		to = widget->on_val;
		break;
	}

	for (i = 0; i < entity->pde.num_max_delay; i++) {
		struct sdca_pde_delay *delay = &entity->pde.max_delay[i];

		if (delay->from_ps == from && delay->to_ps == to) {
			polls = max(polls, delay->us / poll_us);
			break;
		}
	}

	reg = SDW_SDCA_CTL(SDW_SDCA_CTL_FUNC(widget->reg),
			   SDW_SDCA_CTL_ENT(widget->reg),
			   SDCA_CTL_PDE_ACTUAL_PS, 0);

	for (i = 0; i < polls; i++) {
		if (i)
			fsleep(poll_us);

		ret = regmap_read(component->regmap, reg, &val);
		if (ret)
			return ret;
		else if (val == to)
			return 0;
	}

	dev_err(component->dev, "%s: power transition failed: %x\n",
		entity->label, val);
	return -ETIMEDOUT;
}

static int sdca_asoc_parse_pde(struct device *dev,
			       struct sdca_function_data *function,
			       struct sdca_entity *entity,
			       struct snd_soc_dapm_widget **widget,
			       struct snd_soc_dapm_route **route)
{
	unsigned int target = (1 << SDCA_PDE_PS0) | (1 << SDCA_PDE_PS3);
	struct sdca_control_range *range;
	struct sdca_control *control;
	unsigned int mask = 0;
	int i;

	control = selector_find_control(entity, SDCA_CTL_PDE_REQUESTED_PS);
	if (!control) {
		dev_err(dev, "%s: no power control\n", entity->label);
		return -EINVAL;
	}

	/* Power should only be controlled by the driver */
	if (control->layers != SDCA_ACCESS_LAYER_CLASS)
		dev_warn(dev, "%s: unexpected access layer: %x\n",
			 entity->label, control->layers);

	range = control_find_range(dev, entity, control, SDCA_REQUESTED_PS_NCOLS, 0);
	if (!range)
		return -EINVAL;

	for (i = 0; i < range->rows; i++)
		mask |= 1 << sdca_range(range, SDCA_REQUESTED_PS_STATE, i);

	if ((mask & target) != target) {
		dev_err(dev, "%s: power control missing states\n", entity->label);
		return -EINVAL;
	}

	(*widget)->id = snd_soc_dapm_supply;
	(*widget)->reg = SDW_SDCA_CTL(function->desc->adr, entity->id, control->sel, 0);
	(*widget)->mask = GENMASK(control->nbits - 1, 0);
	(*widget)->on_val = SDCA_PDE_PS0;
	(*widget)->off_val = SDCA_PDE_PS3;
	(*widget)->event_flags = SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD;
	(*widget)->event = sdca_asoc_pde_event;
	(*widget)->priv = entity;
	(*widget)++;

	for (i = 0; i < entity->pde.num_managed; i++)
		add_route(route, entity->pde.managed[i]->label, NULL, entity->label);

	for (i = 0; i < entity->num_sources; i++)
		add_route(route, entity->label, NULL, entity->sources[i]->label);

	return 0;
}

static int sdca_asoc_parse_su_device(struct device *dev,
				     struct sdca_function_data *function,
				     struct sdca_entity *entity,
				     struct snd_soc_dapm_widget **widget,
				     struct snd_soc_dapm_route **route)
{
	struct sdca_control_range *range;
	int num_routes = 0;
	int i, j;

	if (!entity->group) {
		dev_err(dev, "%s: device selector unit missing group\n", entity->label);
		return -EINVAL;
	}

	range = selector_find_range(dev, entity->group, SDCA_CTL_GE_SELECTED_MODE,
				    SDCA_SELECTED_MODE_NCOLS, 0);
	if (!range)
		return -EINVAL;

	(*widget)->id = snd_soc_dapm_mux;
	(*widget)->kcontrol_news = entity->group->ge.kctl;
	(*widget)->num_kcontrols = 1;
	(*widget)++;

	for (i = 0; i < entity->group->ge.num_modes; i++) {
		struct sdca_ge_mode *mode = &entity->group->ge.modes[i];

		for (j = 0; j < mode->num_controls; j++) {
			struct sdca_ge_control *affected = &mode->controls[j];
			int term;

			if (affected->id != entity->id ||
			    affected->sel != SDCA_CTL_SU_SELECTOR ||
			    !affected->val)
				continue;

			if (affected->val - 1 >= entity->num_sources) {
				dev_err(dev, "%s: Bad control value: %#x\n",
					entity->label, affected->val);
				return -EINVAL;
			}

			if (++num_routes > entity->num_sources) {
				dev_err(dev, "%s: Too many input routes\n", entity->label);
				return -EINVAL;
			}

			term = sdca_range_search(range, SDCA_SELECTED_MODE_INDEX,
						 mode->val, SDCA_SELECTED_MODE_TERM_TYPE);
			if (!term) {
				dev_err(dev, "%s: Mode not found: %#x\n",
					entity->label, mode->val);
				return -EINVAL;
			}

			add_route(route, entity->label, get_terminal_name(term),
				  entity->sources[affected->val - 1]->label);
		}
	}

	return 0;
}

static int sdca_asoc_parse_su_class(struct device *dev,
				    struct sdca_function_data *function,
				    struct sdca_entity *entity,
				    struct sdca_control *control,
				    struct snd_soc_dapm_widget **widget,
				    struct snd_soc_dapm_route **route)
{
	struct snd_kcontrol_new *kctl;
	struct soc_enum *soc_enum;
	const char **texts;
	int i;

	kctl = devm_kmalloc(dev, sizeof(*kctl), GFP_KERNEL);
	if (!kctl)
		return -ENOMEM;

	soc_enum = devm_kmalloc(dev, sizeof(*soc_enum), GFP_KERNEL);
	if (!soc_enum)
		return -ENOMEM;

	texts = devm_kcalloc(dev, entity->num_sources + 1, sizeof(*texts), GFP_KERNEL);
	if (!texts)
		return -ENOMEM;

	texts[0] = "No Signal";
	for (i = 0; i < entity->num_sources; i++)
		texts[i + 1] = entity->sources[i]->label;

	soc_enum->reg = SDW_SDCA_CTL(function->desc->adr, entity->id, control->sel, 0);
	soc_enum->items = entity->num_sources + 1;
	soc_enum->mask = roundup_pow_of_two(soc_enum->items) - 1;
	soc_enum->texts = texts;

	kctl->iface = SNDRV_CTL_ELEM_IFACE_MIXER;
	kctl->name = "Route";
	kctl->info = snd_soc_info_enum_double;
	kctl->get = snd_soc_dapm_get_enum_double;
	kctl->put = snd_soc_dapm_put_enum_double;
	kctl->private_value = (unsigned long)soc_enum;

	(*widget)->id = snd_soc_dapm_mux;
	(*widget)->kcontrol_news = kctl;
	(*widget)->num_kcontrols = 1;
	(*widget)++;

	for (i = 0; i < entity->num_sources; i++)
		add_route(route, entity->label, texts[i + 1], entity->sources[i]->label);

	return 0;
}

static int sdca_asoc_parse_su(struct device *dev,
			      struct sdca_function_data *function,
			      struct sdca_entity *entity,
			      struct snd_soc_dapm_widget **widget,
			      struct snd_soc_dapm_route **route)
{
	struct sdca_control *control;

	if (!entity->num_sources) {
		dev_err(dev, "%s: selector with no inputs\n", entity->label);
		return -EINVAL;
	}

	control = selector_find_control(entity, SDCA_CTL_SU_SELECTOR);
	if (!control) {
		dev_err(dev, "%s: no selector control\n", entity->label);
		return -EINVAL;
	}

	if (control->layers == SDCA_ACCESS_LAYER_DEVICE)
		return sdca_asoc_parse_su_device(dev, function, entity, widget, route);

	if (control->layers != SDCA_ACCESS_LAYER_CLASS)
		dev_warn(dev, "%s: unexpected access layer: %x\n",
			 entity->label, control->layers);

	return sdca_asoc_parse_su_class(dev, function, entity, control, widget, route);
}

static int sdca_asoc_parse_mu(struct device *dev,
			      struct sdca_function_data *function,
			      struct sdca_entity *entity,
			      struct snd_soc_dapm_widget **widget,
			      struct snd_soc_dapm_route **route)
{
	struct sdca_control *control;
	struct snd_kcontrol_new *kctl;
	int cn;
	int i;

	if (!entity->num_sources) {
		dev_err(dev, "%s: selector 1 or more inputs\n", entity->label);
		return -EINVAL;
	}

	control = selector_find_control(entity, SDCA_CTL_MU_MIXER);
	if (!control) {
		dev_err(dev, "%s: no mixer controls\n", entity->label);
		return -EINVAL;
	}

	/* MU control should be through DAPM */
	if (control->layers != SDCA_ACCESS_LAYER_CLASS)
		dev_warn(dev, "%s: unexpected access layer: %x\n",
			 entity->label, control->layers);

	if (entity->num_sources != hweight64(control->cn_list)) {
		dev_err(dev, "%s: mismatched control and sources\n", entity->label);
		return -EINVAL;
	}

	kctl = devm_kcalloc(dev, entity->num_sources, sizeof(*kctl), GFP_KERNEL);
	if (!kctl)
		return -ENOMEM;

	i = 0;
	for_each_set_bit(cn, (unsigned long *)&control->cn_list,
			 BITS_PER_TYPE(control->cn_list)) {
		const char *control_name;
		struct soc_mixer_control *mc;

		control_name = devm_kasprintf(dev, GFP_KERNEL, "%s %d",
					      control->label, i + 1);
		if (!control_name)
			return -ENOMEM;

		mc = devm_kmalloc(dev, sizeof(*mc), GFP_KERNEL);
		if (!mc)
			return -ENOMEM;

		mc->reg = SDW_SDCA_CTL(function->desc->adr, entity->id, control->sel, cn);
		mc->rreg = mc->reg;

		//FIXME: Should be treated as Q7.8dB
		mc->min = 0;
		mc->max = (0x1ull << control->nbits) - 1;
		//FIXME: Control currently hard coded to 0, this forces DAPM to
		// connect route
		mc->invert = 1;

		kctl[i].name = control_name;
		kctl[i].private_value = (unsigned long)mc;
		kctl[i].iface = SNDRV_CTL_ELEM_IFACE_MIXER;
		kctl[i].info = snd_soc_info_volsw;
		kctl[i].get = snd_soc_dapm_get_volsw;
		kctl[i].put = snd_soc_dapm_put_volsw;
		i++;
	}

	(*widget)->id = snd_soc_dapm_mixer;
	(*widget)->kcontrol_news = kctl;
	(*widget)->num_kcontrols = entity->num_sources;
	(*widget)++;

	for (i = 0; i < entity->num_sources; i++)
		add_route(route, entity->label, kctl[i].name, entity->sources[i]->label);

	return 0;
}

static int sdca_asoc_cs_event(struct snd_soc_dapm_widget *widget,
			      struct snd_kcontrol *kctl, int event)
{
	struct snd_soc_component *component = widget->dapm->component;
	struct sdca_entity *entity = widget->priv;

	if (!component)
		return -EIO;

	if (entity->cs.max_delay)
		fsleep(entity->cs.max_delay);

	return 0;
}

static int sdca_asoc_parse_cs(struct device *dev,
			      struct sdca_function_data *function,
			      struct sdca_entity *entity,
			      struct snd_soc_dapm_widget **widget,
			      struct snd_soc_dapm_route **route)
{
	int i;

	(*widget)->id = snd_soc_dapm_supply;
	(*widget)->subseq = 1; /* Ensure these run after PDEs */
	(*widget)->event_flags = SND_SOC_DAPM_POST_PMU;
	(*widget)->event = sdca_asoc_cs_event;
	(*widget)->priv = entity;
	(*widget)++;

	for (i = 0; i < entity->num_sources; i++)
		add_route(route, entity->label, NULL, entity->sources[i]->label);

	return 0;
}

int sdca_asoc_populate_dapm(struct device *dev, struct sdca_function_data *function,
			    struct snd_soc_dapm_widget *widget,
			    struct snd_soc_dapm_route *route)
{
	int ret;
	int i;

	/* Some entities need to add controls referenced by other entities */
	for (i = 0; i < function->num_entities - 1; i++) {
		struct sdca_entity *entity = &function->entities[i];

		if (entity->type == SDCA_ENTITY_TYPE_GE) {
			ret = sdca_asoc_early_ge(dev, function, entity);
			if (ret)
				return ret;
		}
	}

	for (i = 0; i < function->num_entities - 1; i++) {
		struct sdca_entity *entity = &function->entities[i];

		widget->name = entity->label;
		widget->reg = SND_SOC_NOPM;

		switch (entity->type) {
		case SDCA_ENTITY_TYPE_IT:
			ret = sdca_asoc_parse_it(dev, function, entity, &widget, &route);
			break;
		case SDCA_ENTITY_TYPE_OT:
			ret = sdca_asoc_parse_ot(dev, function, entity, &widget, &route);
			break;
		case SDCA_ENTITY_TYPE_PDE:
			ret = sdca_asoc_parse_pde(dev, function, entity, &widget, &route);
			break;
		case SDCA_ENTITY_TYPE_SU:
			ret = sdca_asoc_parse_su(dev, function, entity, &widget, &route);
			break;
		case SDCA_ENTITY_TYPE_MU:
			ret = sdca_asoc_parse_mu(dev, function, entity, &widget, &route);
			break;
		case SDCA_ENTITY_TYPE_CS:
			ret = sdca_asoc_parse_cs(dev, function, entity, &widget, &route);
			break;
		case SDCA_ENTITY_TYPE_CX:
			/*
			 * FIXME: For now we will just treat these as a supply,
			 * meaning all options are enabled.
			 */
			dev_warn(dev, "%s: Clock Selectors not fully supported yet\n",
				 entity->label);
			ret = sdca_asoc_parse_simple(dev, function, entity, &widget,
						      &route, snd_soc_dapm_supply);
			break;
		case SDCA_ENTITY_TYPE_TG:
			ret = sdca_asoc_parse_simple(dev, function, entity, &widget,
						     &route, snd_soc_dapm_siggen);
			break;
		default:
			ret = sdca_asoc_parse_simple(dev, function, entity, &widget,
						     &route, snd_soc_dapm_pga);
			break;
		}
		if (ret)
			return ret;
	}

	return 0;
}
EXPORT_SYMBOL_NS(sdca_asoc_populate_dapm, "SND_SOC_SDCA");

static int sdca_asoc_limit_kctl(struct device *dev,
				struct sdca_entity *entity,
				struct sdca_control *control,
				struct snd_kcontrol_new *kctl)
{
	struct soc_mixer_control *mc = (struct soc_mixer_control *)kctl->private_value;
	struct sdca_control_range *range;
	int min, max, step;
	unsigned int *tlv;
	int shift;

	if (control->type != SDCA_CTL_DATATYPE_Q7P8DB)
		return 0;

	/*
	 * FIXME: For now only handle the simple case of a single linear range
	 */
	range = control_find_range(dev, entity, control, SDCA_VOLUME_LINEAR_NCOLS, 1);
	if (!range)
		return -EINVAL;

	min = sdca_range(range, SDCA_VOLUME_LINEAR_MIN, 0);
	max = sdca_range(range, SDCA_VOLUME_LINEAR_MAX, 0);
	step = sdca_range(range, SDCA_VOLUME_LINEAR_STEP, 0);

	min = sign_extend32(min, control->nbits - 1);
	max = sign_extend32(max, control->nbits - 1);

	/*
	 * FIXME: Only support power of 2 step sizes as this can be supported
	 * by a simple shift.
	 */
	if (hweight32(step) != 1) {
		dev_err(dev, "%s: %s: currently unsupported step size\n",
			entity->label, control->label);
		return -EINVAL;
	}

	/*
	 * The SDCA volumes are in steps of 1/256th of a dB, a step down of
	 * 64 (shift of 6) gives 1/4dB. 1/4dB is the smallest unit that is also
	 * representable in the ALSA TLVs which are in 1/100ths of a dB.
	 * FIXME: Even with this scheme we could accept non-power of 2 steps
	 * that are multiples of 64.
	 */
	shift = max(ffs(step) - 1, 6);

	tlv = devm_kcalloc(dev, 4, sizeof(*tlv), GFP_KERNEL);
	if (!tlv)
		return -ENOMEM;

	tlv[0] = SNDRV_CTL_TLVT_DB_SCALE;
	tlv[1] = 2 * sizeof(*tlv);
	tlv[2] = (min * 100) >> 8;
	tlv[3] = ((1 << shift) * 100) >> 8;

	mc->min = min >> shift;
	mc->max = max >> shift;
	mc->shift = shift;
	mc->rshift = shift;
	mc->sign_bit = 15 - shift;

	kctl->access = SNDRV_CTL_ELEM_ACCESS_TLV_READ | SNDRV_CTL_ELEM_ACCESS_READWRITE;
	kctl->tlv.p = tlv;

	return 0;
}

static int sdca_asoc_populate_control(struct device *dev,
				      struct sdca_function_data *function,
				      struct sdca_entity *entity,
				      struct sdca_control *control,
				      struct snd_kcontrol_new **kctl)
{
	const char *control_suffix = "";
	const char *control_name;
	struct soc_mixer_control *mc;
	int index = 0;
	int ret;
	int cn;

	if (!exported_control(control))
		return 0;

	if (control->type == SDCA_CTL_DATATYPE_ONEBIT)
		control_suffix = " Switch";

	control_name = devm_kasprintf(dev, GFP_KERNEL, "%s %s%s", entity->label,
				      control->label, control_suffix);
	if (!control_name)
		return -ENOMEM;

	mc = devm_kmalloc(dev, sizeof(*mc), GFP_KERNEL);
	if (!mc)
		return -ENOMEM;

	for_each_set_bit(cn, (unsigned long *)&control->cn_list,
			 BITS_PER_TYPE(control->cn_list)) {
		switch (index++) {
			case 0:
				mc->reg = SDW_SDCA_CTL(function->desc->adr, entity->id,
						       control->sel, cn);
				mc->rreg = mc->reg;
				break;
			case 1:
				mc->rreg = SDW_SDCA_CTL(function->desc->adr, entity->id,
							control->sel, cn);
				break;
			default:
				dev_err(dev, "%s: %s: only mono/stereo controls supported\n",
					entity->label, control->label);
				return -EINVAL;
		}
	}

	mc->min = 0;
	mc->max = (0x1ull << control->nbits) - 1;

	(*kctl)->name = control_name;
	(*kctl)->private_value = (unsigned long)mc;
	(*kctl)->iface = SNDRV_CTL_ELEM_IFACE_MIXER;
	(*kctl)->info = snd_soc_info_volsw;
	(*kctl)->get = snd_soc_get_volsw;
	(*kctl)->put = snd_soc_put_volsw;

	ret = sdca_asoc_limit_kctl(dev, entity, control, *kctl);
	if (ret)
		return ret;

	(*kctl)++;

	return 0;
}

int sdca_asoc_populate_controls(struct device *dev,
				struct sdca_function_data *function,
				struct snd_kcontrol_new *kctl)
{
	int i, j;
	int ret;

	for (i = 0; i < function->num_entities; i++) {
		struct sdca_entity *entity = &function->entities[i];

		for (j = 0; j < entity->num_controls; j++) {
			ret = sdca_asoc_populate_control(dev, function, entity,
							 &entity->controls[j], &kctl);
			if (ret)
				return ret;
		}
	}

	return 0;
}
EXPORT_SYMBOL_NS(sdca_asoc_populate_controls, "SND_SOC_SDCA");

static unsigned int rate_find_mask(unsigned int rate)
{
	switch (rate) {
	case 0:
		return SNDRV_PCM_RATE_8000_768000;
	case 5512:
		return SNDRV_PCM_RATE_5512;
	case 8000:
		return SNDRV_PCM_RATE_8000;
	case 11025:
		return SNDRV_PCM_RATE_11025;
	case 16000:
		return SNDRV_PCM_RATE_16000;
	case 22050:
		return SNDRV_PCM_RATE_22050;
	case 32000:
		return SNDRV_PCM_RATE_32000;
	case 44100:
		return SNDRV_PCM_RATE_44100;
	case 48000:
		return SNDRV_PCM_RATE_48000;
	case 64000:
		return SNDRV_PCM_RATE_64000;
	case 88200:
		return SNDRV_PCM_RATE_88200;
	case 96000:
		return SNDRV_PCM_RATE_96000;
	case 176400:
		return SNDRV_PCM_RATE_176400;
	case 192000:
		return SNDRV_PCM_RATE_192000;
	case 352800:
		return SNDRV_PCM_RATE_352800;
	case 384000:
		return SNDRV_PCM_RATE_384000;
	case 705600:
		return SNDRV_PCM_RATE_705600;
	case 768000:
		return SNDRV_PCM_RATE_768000;
	case 12000:
		return SNDRV_PCM_RATE_12000;
	case 24000:
		return SNDRV_PCM_RATE_24000;
	case 128000:
		return SNDRV_PCM_RATE_128000;
	default:
		return 0;
	}
}

static u64 width_find_mask(unsigned int bits)
{
	switch (bits) {
	case 0:
		return SNDRV_PCM_FMTBIT_S8 | SNDRV_PCM_FMTBIT_S16_LE |
		       SNDRV_PCM_FMTBIT_S20_LE | SNDRV_PCM_FMTBIT_S24_LE |
		       SNDRV_PCM_FMTBIT_S32_LE;
	case 8:
		return SNDRV_PCM_FMTBIT_S8;
	case 16:
		return SNDRV_PCM_FMTBIT_S16_LE;
	case 20:
		return SNDRV_PCM_FMTBIT_S20_LE;
	case 24:
		return SNDRV_PCM_FMTBIT_S24_LE;
	case 32:
		return SNDRV_PCM_FMTBIT_S32_LE;
	default:
		return 0;
	}
}

static int sdca_asoc_populate_rate_format(struct device *dev,
					  struct sdca_function_data *function,
					  struct sdca_entity *entity,
					  struct snd_soc_pcm_stream *stream)
{
	struct sdca_control_range *range;
	unsigned int sample_rate, sample_width;
	unsigned int clock_rates = 0;
	unsigned int rates = 0;
	u64 formats = 0;
	int sel, i;

	switch (entity->type) {
	case SDCA_ENTITY_TYPE_IT:
		sel = SDCA_CTL_IT_USAGE;
		break;
	case SDCA_ENTITY_TYPE_OT:
		sel = SDCA_CTL_OT_USAGE;
		break;
	default:
		dev_err(dev, "%s: entity type has no usage control\n",
			entity->label);
		return -EINVAL;
	}

	if (entity->iot.clock) {
		range = selector_find_range(dev, entity->iot.clock,
					    SDCA_CTL_CS_SAMPLERATEINDEX,
					    SDCA_SAMPLERATEINDEX_NCOLS, 0);
		if (!range)
			return -EINVAL;

		for (i = 0; i < range->rows; i++) {
			sample_rate = sdca_range(range, SDCA_SAMPLERATEINDEX_RATE, i);
			clock_rates |= rate_find_mask(sample_rate);
		}
	} else {
		clock_rates = UINT_MAX;
	}

	range = selector_find_range(dev, entity, sel, SDCA_USAGE_NCOLS, 0);
	if (!range)
		return -EINVAL;

	for (i = 0; i < range->rows; i++) {
		sample_rate = sdca_range(range, SDCA_USAGE_SAMPLE_RATE, i);
		sample_rate = rate_find_mask(sample_rate);

		if (sample_rate & clock_rates) {
			rates |= sample_rate;

			sample_width = sdca_range(range, SDCA_USAGE_SAMPLE_WIDTH, i);
			formats |= width_find_mask(sample_width);
		}
	}

	stream->formats = formats;
	stream->rates = rates;

	return 0;
}

int sdca_asoc_populate_dais(struct device *dev, struct sdca_function_data *function,
			    struct snd_soc_dai_driver *dais,
			    const struct snd_soc_dai_ops *ops)
{
	int i, j;
	int ret;

	for (i = 0, j = 0; i < function->num_entities - 1; i++) {
		struct sdca_entity *entity = &function->entities[i];
		struct snd_soc_pcm_stream *stream;
		const char *stream_suffix;

		switch (entity->type) {
		case SDCA_ENTITY_TYPE_IT:
			stream = &dais[j].playback;
			stream_suffix = "Playback";
			break;
		case SDCA_ENTITY_TYPE_OT:
			stream = &dais[j].capture;
			stream_suffix = "Capture";
			break;
		default:
			continue;
		}

		if (!entity->iot.is_dataport)
			continue;

		stream->stream_name = devm_kasprintf(dev, GFP_KERNEL, "%s %s",
						     entity->label, stream_suffix);
		if (!stream->stream_name)
			return -ENOMEM;
		/* Channels will be further limited by constraints */
		stream->channels_min = 1;
		stream->channels_max = SDCA_MAX_CHANNEL_COUNT;

		ret = sdca_asoc_populate_rate_format(dev, function, entity, stream);
		if (ret)
			return ret;

		dais[j].id = i;
		dais[j].name = entity->label;
		dais[j].ops = ops;
		j++;
	}

	return 0;
}
EXPORT_SYMBOL_NS(sdca_asoc_populate_dais, "SND_SOC_SDCA");

int sdca_asoc_populate_component(struct device *dev,
				 struct sdca_function_data *function,
				 struct snd_soc_component_driver *component_drv,
				 struct snd_soc_dai_driver **dai_drv, int *num_dai_drv,
				 const struct snd_soc_dai_ops *ops)
{
	struct snd_soc_dapm_widget *widgets;
	struct snd_soc_dapm_route *routes;
	struct snd_kcontrol_new *controls;
	struct snd_soc_dai_driver *dais;
	int num_widgets, num_routes, num_controls, num_dais;
	int ret;

	ret = sdca_asoc_count_component(dev, function, &num_widgets, &num_routes,
					&num_controls, &num_dais);
	if (ret)
		return ret;

	widgets = devm_kcalloc(dev, num_widgets, sizeof(*widgets), GFP_KERNEL);
	if (!widgets)
		return -ENOMEM;

	routes = devm_kcalloc(dev, num_routes, sizeof(*routes), GFP_KERNEL);
	if (!routes)
		return -ENOMEM;

	controls = devm_kcalloc(dev, num_controls, sizeof(*controls), GFP_KERNEL);
	if (!controls)
		return -ENOMEM;

	dais = devm_kcalloc(dev, num_dais, sizeof(*dais), GFP_KERNEL);
	if (!dais)
		return -ENOMEM;

	ret = sdca_asoc_populate_dapm(dev, function, widgets, routes);
	if (ret)
		return ret;

	ret = sdca_asoc_populate_controls(dev, function, controls);
	if (ret)
		return ret;

	ret = sdca_asoc_populate_dais(dev, function, dais, ops);
	if (ret)
		return ret;

	component_drv->dapm_widgets = widgets;
	component_drv->num_dapm_widgets = num_widgets;
	component_drv->dapm_routes = routes;
	component_drv->num_dapm_routes = num_routes;
	component_drv->controls = controls;
	component_drv->num_controls = num_controls;

	*dai_drv = dais;
	*num_dai_drv = num_dais;

	return 0;
}
EXPORT_SYMBOL_NS(sdca_asoc_populate_component, "SND_SOC_SDCA");

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("SDCA library");
