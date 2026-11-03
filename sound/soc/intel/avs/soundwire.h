/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright(c) 2021-2026 Intel Corporation
 *
 * Authors: Cezary Rojewski <cezary.rojewski@intel.com>
 *          Amadeusz Slawinski <amade@asmblr.net>
 */

#ifndef __SOUND_SOC_INTEL_AVS_SOUNDWIRE_H
#define __SOUND_SOC_INTEL_AVS_SOUNDWIRE_H

#include <linux/irqreturn.h>

struct avs_dev;

#if IS_ENABLED(CONFIG_SND_SOC_INTEL_AVS_SOUNDWIRE)

int avs_sdw_probe(struct avs_dev *adev);
int avs_sdw_startup(struct avs_dev *adev);
void avs_sdw_exit(struct avs_dev *adev);
void avs_sdw_enable_irq(struct avs_dev *adev, bool enable);
void avs_sdw_process_wakeen_event(struct avs_dev *adev);
irqreturn_t avs_sdw_thread(int irq, struct avs_dev *adev);

int avs_register_sdw_component(struct avs_dev *adev, const char *name, u32 link_id);

#else

static inline int avs_sdw_probe(struct avs_dev *adev)
{
	return 0;
}

static inline int avs_sdw_startup(struct avs_dev *adev)
{
	return 0;
}

static inline void avs_sdw_exit(struct avs_dev *adev)
{
}

static inline void avs_sdw_enable_irq(struct avs_dev *adev, bool enable)
{
}

static inline void avs_sdw_process_wakeen_event(struct avs_dev *adev)
{
}

static inline irqreturn_t avs_sdw_thread(int irq, struct avs_dev *adev)
{
	return IRQ_HANDLED;
}

static inline int avs_register_sdw_component(struct avs_dev *adev, const char *name, u32 link_id)
{
	return 0;
}

#endif

#endif /* __SOUND_SOC_INTEL_AVS_SOUNDWIRE_H */
