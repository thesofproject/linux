/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __SOF_CLIENT_AUDIO_H
#define __SOF_CLIENT_AUDIO_H

#include <sound/soc.h>

struct dentry;
struct sof_client_dev;

/**
 * struct sof_audio_client_ipc_ops - IPC version specific ops of the audio client
 * @rx_notification:	Handle a firmware initiated notification. The message and
 *			its payload is owned by the SOF core, it must not be
 *			modified or freed by the client.
 * @get_module_name:	Copy the topology name of a module instance owned by this
 *			audio client. Returns -ENOENT if the module is not part of
 *			the topology of this client.
 *
 * Collects the IPC version specific glue of the audio client, most notably the
 * demultiplexing of the firmware notifications the client is interested in.
 */
struct sof_audio_client_ipc_ops {
	void (*rx_notification)(struct sof_client_dev *cdev, void *msg_buf);
	int (*get_module_name)(struct sof_client_dev *cdev, u32 module_id,
			       int instance_id, char *name, size_t size);
};

/**
 * struct sof_audio_client_pdata - platform data for the audio sof-client
 * @plat_drv:	Pre-built ASoC component driver
 * @drv:	Array of DAI drivers to register
 * @num_drv:	Number of DAI drivers
 * @machine:	Per-instance copy of the machine descriptor
 * @component:	The ASoC component registered by this audio client instance
 * @ipc_ops:	IPC version specific ops of this audio client instance
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
	struct snd_soc_component *component;
	const struct sof_audio_client_ipc_ops *ipc_ops;
	struct dentry *debugfs_root;
	const char *debug_topology_name;
	const char *debug_card_name;
	const char *debug_machine_driver;
};

extern const struct sof_audio_client_ipc_ops sof_audio_client_ipc3_ops;
extern const struct sof_audio_client_ipc_ops sof_audio_client_ipc4_ops;

/* power management of a single audio instance */
int sof_audio_instance_suspend(struct snd_soc_component *component);
int sof_audio_instance_resume(struct snd_soc_component *component);
int sof_audio_instance_restore(struct snd_soc_component *component);
bool sof_audio_instance_suspend_ignored(struct snd_soc_component *component);
enum sof_d0i3_vote sof_audio_instance_d0i3_vote(struct snd_soc_component *component);

void snd_sof_new_platform_drv(struct sof_client_dev *cdev,
			      struct snd_soc_component_driver * const pd);

#endif /* __SOF_CLIENT_AUDIO_H */
