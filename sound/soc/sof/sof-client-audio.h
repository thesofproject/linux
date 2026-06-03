/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __SOF_CLIENT_AUDIO_H
#define __SOF_CLIENT_AUDIO_H

#include <sound/soc.h>

struct dentry;

/**
 * struct sof_audio_client_pdata - platform data for the audio sof-client
 * @plat_drv:	Pre-built ASoC component driver
 * @drv:	Array of DAI drivers to register
 * @num_drv:	Number of DAI drivers
 * @machine:	Per-instance copy of the machine descriptor
 * @debugfs_root:	Per-audio-client debugfs directory
 * @debug_topology_name:	Topology loaded by this audio client
 * @debug_card_name:	Card name for this audio client
 * @debug_machine_driver:	Machine driver bound to this audio client
 */
struct sof_audio_client_pdata {
	struct snd_soc_component_driver plat_drv;
	struct snd_soc_dai_driver *drv;
	int num_drv;
	struct snd_soc_acpi_mach machine;
	struct dentry *debugfs_root;
	const char *debug_topology_name;
	const char *debug_card_name;
	const char *debug_machine_driver;
};

#endif /* __SOF_CLIENT_AUDIO_H */
