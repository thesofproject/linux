/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The MIPI SDCA specification is available for public downloads at
 * https://www.mipi.org/mipi-sdca-v1-0-download
 *
 * Copyright (C) 2026 Cirrus Logic, Inc. and
 *                    Cirrus Logic International Semiconductor Ltd.
 */

#ifndef __SDCA_WOV_H__
#define __SDCA_WOV_H__

#include <linux/kconfig.h>
#include <linux/types.h>

#define SDCA_WOV_SMPU_LABEL		"SMPU 16"
#define SDCA_WOV_MAX_DRAIN_PASSES	32
#define SDCA_WOV_READY_POLL_US		100

/* SDCA v1.0 Table 103: algorithms required for proper operation. */
#define SDCA_ALGORITHM_CBN_REQUIRED	300

/* SDCA v1.0 Table 124: Voice and Speech trigger behaviors. */
#define SDCA_TRIGGER_CBN_VOICE		2000
#define SDCA_TRIGGER_CBN_SPEECH		3000

struct device;
struct sdca_entity;
struct sdca_function_data;
struct sdca_interrupt;
struct sdca_interrupt_info;
struct snd_soc_component_driver;
struct snd_soc_dai_driver;
struct snd_soc_dapm_route;
struct snd_soc_pcm_stream;

int sdca_wov_alloc_state(struct sdca_interrupt *interrupt);
void sdca_wov_free_state(struct sdca_interrupt *interrupt);

int sdca_wov_process_voice_trigger(struct sdca_interrupt *interrupt);

#if IS_ENABLED(CONFIG_SND_SOC_SDCA_IRQ)
bool sdca_wov_is_terminal(struct device *dev, struct sdca_function_data *function,
			  struct sdca_entity *entity);

int sdca_wov_populate_dapm(struct device *dev, struct sdca_entity *entity,
			   struct snd_soc_dapm_route **route);

int sdca_wov_populate_dai(struct device *dev,
			  struct sdca_function_data *function,
			  struct snd_soc_dai_driver *dai,
			  const struct snd_soc_pcm_stream *stream,
			  int entity_index);

int sdca_wov_init_component(struct device *dev,
			    struct sdca_function_data *function,
			    struct sdca_interrupt_info *info,
			    struct snd_soc_component_driver *component_drv);
#else
static inline bool sdca_wov_is_terminal(struct device *dev,
					struct sdca_function_data *function,
					struct sdca_entity *entity)
{
	return false;
}

static inline int sdca_wov_populate_dapm(struct device *dev, struct sdca_entity *entity,
					 struct snd_soc_dapm_route **route)
{
	return 0;
}

static inline int sdca_wov_populate_dai(struct device *dev,
					struct sdca_function_data *function,
					struct snd_soc_dai_driver *dai,
					const struct snd_soc_pcm_stream *stream,
					int entity_index)
{
	return 0;
}

static inline int sdca_wov_init_component(struct device *dev,
					  struct sdca_function_data *function,
					  struct sdca_interrupt_info *info,
					  struct snd_soc_component_driver *component_drv)
{
	return 0;
}
#endif

#endif /* __SDCA_WOV_H__ */
