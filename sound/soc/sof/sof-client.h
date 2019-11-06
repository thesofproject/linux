/* SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause) */
/*
 * Copyright(c) 2019 Intel Corporation. All rights reserved.
 *
 * Author: Ranjani Sridharan <ranjani.sridharan@linux.intel.com>
 */
#include <linux/platform_device.h>
#include <sound/memalloc.h>
#include <sound/soc.h>
#include <sound/sof/stream.h>

#ifndef __SOUND_SOC_SOF_CLIENT_H
#define __SOUND_SOC_SOF_CLIENT_H

/* SOF client device */
struct snd_sof_client {
	struct platform_device *pdev;

	/* IPC RX callback */
	void (*sof_client_rx_cb)(struct snd_sof_client *client,
				 u32 msg_cmd);

	struct list_head list;	/* list in sdev ipc_rx_list */

	void *client_data; /* core does not touch this */
};

int snd_sof_create_page_table(struct device *dev,
			      struct snd_dma_buffer *dmab,
			      unsigned char *page_table, size_t size);

void *sof_get_client_data(struct device *dev);

/* IPC TX/RX */
int sof_client_tx_message(struct device *dev, u32 header,
			  void *msg_data, size_t msg_bytes, void *reply_data,
			  size_t reply_bytes);
void snd_sof_ipc_rx_register(struct snd_sof_client *client,
			     struct device *dev);

/* client API's */
inline int
snd_sof_client_hw_params_upon_resume(struct device *dev);
inline int snd_sof_client_core_power_up(struct device *dev,
					       unsigned int core_mask);
inline int snd_sof_client_core_power_down(struct device *dev,
						 unsigned int core_mask);
inline void
snd_sof_client_ipc_msg_data(struct device *dev,
			    struct snd_pcm_substream *substream,
			    void *p, size_t sz);

/* PCM ops */
int snd_sof_client_pcm_platform_open(struct device *dev,
				     struct snd_pcm_substream *substream);
int snd_sof_client_pcm_platform_close(struct device *dev,
				      struct snd_pcm_substream *substream);
int snd_sof_client_pcm_platform_hw_params(struct device *dev,
					  struct snd_pcm_substream *substream,
					  struct snd_pcm_hw_params *params,
					  struct sof_ipc_stream_params *ipc_params);
int snd_sof_client_pcm_platform_hw_free(struct device *dev,
					struct snd_pcm_substream *substream);
int snd_sof_client_pcm_platform_trigger(struct device *dev,
					struct snd_pcm_substream *substream,
					int cmd);
long snd_sof_client_pcm_platform_pointer(struct device *dev,
					 struct snd_pcm_substream *substream);

int snd_sof_client_ipc_pcm_params(struct device *dev,
				  struct snd_pcm_substream *substream,
				  const struct sof_ipc_pcm_params_reply *reply);

/* access methods for snd_sof_dev members */
u32 snd_sof_dsp_get_enabled_cores_mask(struct device *dev);
void snd_sof_dsp_set_enabled_cores_mask(struct device *dev, u32 core_mask);
bool snd_sof_is_s0_suspend(struct device *dev);
int snd_sof_get_next_comp_id(struct device *dev);
void snd_sof_inc_next_comp_id(struct device *dev);
struct sof_ipc_fw_ready *snd_sof_get_fw_ready(struct device *dev);
struct snd_sof_pdata *snd_sof_get_pdata(struct device *dev);
u32 snd_sof_get_hw_info(struct device *dev);

/* dai drv */
int snd_sof_get_num_drv(struct device *dev);
struct snd_soc_dai_driver *snd_sof_get_dai_drv(struct device *dev);

/* machine driver select */
struct snd_soc_acpi_mach *
snd_sof_client_machine_driver_select(struct device *dev);

#endif
