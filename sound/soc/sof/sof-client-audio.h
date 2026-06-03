/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __SOF_CLIENT_AUDIO_H
#define __SOF_CLIENT_AUDIO_H

#include <sound/soc.h>

/**
 * struct sof_audio_client_pdata - platform data for the audio sof-client
 * @plat_drv:	Pre-built ASoC component driver
 * @drv:	Array of DAI drivers to register
 * @num_drv:	Number of DAI drivers
 * @machine:	Per-instance copy of the machine descriptor
 */
struct sof_audio_client_pdata {
	const struct snd_soc_component_driver plat_drv;
	struct snd_soc_dai_driver *drv;
	int num_drv;
	struct snd_soc_acpi_mach machine;
};

#endif /* __SOF_CLIENT_AUDIO_H */
