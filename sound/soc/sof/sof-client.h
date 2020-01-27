/* SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause) */
/*
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * Copyright(c) 2020 Intel Corporation. All rights reserved.
 *
 * Author: Ranjani Sridharan <ranjani.sridharan@linux.intel.com>
 */
#ifndef __SOUND_SOC_SOF_CLIENT_H
#define __SOUND_SOC_SOF_CLIENT_H

#include <sound/sof/stream.h> /* needs to be included before control.h */
#include <sound/sof/control.h>
#include <sound/sof/dai.h>
#include <sound/sof/topology.h>
#include <sound/sof/header.h>
#include <sound/soc.h>
#include <sound/sof.h>

enum sof_client_type {
	SOF_CLIENT_AUDIO,
	SOF_CLIENT_IPC,
};

/* SOF client device */
struct snd_sof_client {
	struct platform_device *pdev;

	enum sof_client_type type;

	struct list_head list;	/* item in SOF client list */

	/* Optional IPC RX callback */
	void (*sof_client_ipc_rx)(struct device *dev, u32 msg_cmd);

	/*
	 * Optional callback to check if the client's current status allows the
	 * DSP to enter a low-power D0 substate when the system is in S0.
	 */
	bool (*allow_lp_d0_substate_in_s0)(struct device *dev);

	/*
	 * Optional callback to check if the client is requesting to remain in
	 * D0 when the system suspends to S0IX.
	 */
	bool (*request_d0_during_suspend)(struct device *dev);

	void *client_data; /* SOF core cannot touch this */
};

void sof_client_register(struct device *dev);
void sof_client_unregister(struct snd_sof_client *client);
void *sof_get_client_data(struct device *dev);
const struct sof_dev_desc *sof_get_dev_desc(struct device *dev);
int sof_client_machine_register(struct device *dev, void *data);
void sof_client_machine_select(struct device *dev);
void sof_client_set_mach_params(const struct snd_soc_acpi_mach *mach,
				struct device *dev);
struct snd_soc_dai_driver *sof_client_get_dai_drv(struct device *dev);
int sof_client_get_num_dai_drv(struct device *dev);
int sof_client_ipc_tx_message(struct device *dev, u32 header,
			      void *msg_data, size_t msg_bytes,
			      void *reply_data, size_t reply_bytes);
int sof_client_pcm_platform_open(struct device *dev,
				 struct snd_pcm_substream *substream);
int sof_client_pcm_platform_close(struct device *dev,
				  struct snd_pcm_substream *substream);
int sof_client_pcm_platform_hw_params(struct device *dev,
				      struct snd_pcm_substream *substream,
				      struct snd_pcm_hw_params *params,
				      struct sof_ipc_stream_params *ipc_params);
int sof_client_pcm_platform_hw_free(struct device *dev,
				    struct snd_pcm_substream *substream);
int sof_client_pcm_platform_trigger(struct device *dev,
				    struct snd_pcm_substream *substream,
				    int cmd);
void sof_client_ipc_msg_data(struct device *dev,
			     struct snd_pcm_substream *substream,
			     void *p, size_t sz);
int sof_client_ipc_pcm_params(struct device *dev,
			      struct snd_pcm_substream *substream,
			      const struct sof_ipc_pcm_params_reply *reply);
snd_pcm_uframes_t
sof_client_pcm_platform_pointer(struct device *dev,
				struct snd_pcm_substream *substream);
u32 sof_client_get_hw_info(struct device *dev);
int sof_client_dsp_core_power_up(struct device *dev, unsigned int core_mask);
int sof_client_dsp_core_power_down(struct device *dev, unsigned int core_mask);
u32 sof_client_get_enabled_cores(struct device *dev);
u32 sof_client_get_next_comp_id(struct device *dev);
u32 sof_client_inc_next_comp_id(struct device *dev);
int sof_client_get_mmio_bar(struct device *dev);
struct sof_ipc_fw_ready *sof_client_get_fw_ready(struct device *dev);
bool sof_client_is_s0ix_suspend(struct device *dev);
int sof_client_dsp_hw_params_upon_resume(struct device *dev);
int sof_client_create_page_table(struct device *dev,
				 struct snd_dma_buffer *dmab,
				 unsigned char *page_table, size_t size);
void sof_client_dsp_block_read(struct device *dev, u32 bar,
			       u32 offset, void *dest, size_t bytes);
void sof_client_dsp_block_write(struct device *dev, u32 bar,
				u32 offset, void *src, size_t bytes);
int sof_ipc_set_get_large_ctrl_data(struct device *dev,
				    struct sof_ipc_ctrl_data *cdata,
				    struct sof_ipc_ctrl_data_params *sparams,
				    bool send);
struct dentry *sof_client_get_debugfs_root(struct device *dev);

#endif
