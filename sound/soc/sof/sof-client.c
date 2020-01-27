// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
//
// Copyright(c) 2020 Intel Corporation. All rights reserved.
//
// Author: Ranjani Sridharan <ranjani.sridharan@linux.intel.com>
//
#include <linux/device.h>
#include "sof-priv.h"
#include "sof-client.h"
#include "ops.h"

void sof_client_register(struct device *dev)
{
	struct snd_sof_client *client = dev_get_platdata(dev);
	struct snd_sof_dev *sdev = dev_get_drvdata(dev->parent);

	mutex_lock(&sdev->client_mutex);
	list_add(&client->list, &sdev->client_list);
	sdev->num_clients++;
	mutex_unlock(&sdev->client_mutex);

	dev_dbg(sdev->dev, "%s client registered\n", dev_name(dev));
}
EXPORT_SYMBOL_NS(sof_client_register, SND_SOC_SOF_CLIENT);

void sof_client_unregister(struct snd_sof_client *client)
{
	struct device *dev = &client->pdev->dev;
	struct snd_sof_dev *sdev = dev_get_drvdata(dev->parent);

	mutex_lock(&sdev->client_mutex);
	list_del(&client->list);
	sdev->num_clients--;
	mutex_unlock(&sdev->client_mutex);

	dev_dbg(sdev->dev, "%s client unregistered\n", dev_name(dev));
}
EXPORT_SYMBOL_NS(sof_client_unregister, SND_SOC_SOF_CLIENT);

void *sof_get_client_data(struct device *dev)
{
	struct snd_sof_client *client = dev_get_platdata(dev);

	return client->client_data;
}
EXPORT_SYMBOL_NS(sof_get_client_data, SND_SOC_SOF_CLIENT);

const struct sof_dev_desc *sof_get_dev_desc(struct device *dev)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(dev->parent);
	struct snd_sof_pdata *sof_pdata = sdev->pdata;

	return sof_pdata->desc;
}
EXPORT_SYMBOL_NS(sof_get_dev_desc, SND_SOC_SOF_CLIENT);

void sof_client_machine_select(struct device *dev)
{
	snd_sof_machine_select(dev);
}
EXPORT_SYMBOL_NS(sof_client_machine_select, SND_SOC_SOF_CLIENT);

void sof_client_set_mach_params(const struct snd_soc_acpi_mach *mach,
				struct device *dev)
{
	snd_sof_set_mach_params(mach, dev);
}
EXPORT_SYMBOL_NS(sof_client_set_mach_params, SND_SOC_SOF_CLIENT);

struct snd_soc_dai_driver *sof_client_get_dai_drv(struct device *dev)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(dev->parent);

	return sof_ops(sdev)->drv;
}
EXPORT_SYMBOL_NS(sof_client_get_dai_drv, SND_SOC_SOF_CLIENT);

int sof_client_get_num_dai_drv(struct device *dev)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(dev->parent);

	return sof_ops(sdev)->num_drv;
}
EXPORT_SYMBOL_NS(sof_client_get_num_dai_drv, SND_SOC_SOF_CLIENT);

int sof_client_machine_register(struct device *dev, void *data)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(dev->parent);

	return snd_sof_machine_register(sdev, data);
}
EXPORT_SYMBOL_NS(sof_client_machine_register, SND_SOC_SOF_CLIENT);

int sof_client_ipc_tx_message(struct device *dev, u32 header,
			      void *msg_data, size_t msg_bytes,
			      void *reply_data, size_t reply_bytes)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(dev->parent);

	return sof_ipc_tx_message(sdev->ipc, header, msg_data, msg_bytes,
				  reply_data, reply_bytes);
}
EXPORT_SYMBOL_NS(sof_client_ipc_tx_message, SND_SOC_SOF_CLIENT);

/* host PCM ops */
int sof_client_pcm_platform_open(struct device *dev,
				 struct snd_pcm_substream *substream)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(dev->parent);

	return snd_sof_pcm_platform_open(sdev, substream);
}
EXPORT_SYMBOL_NS(sof_client_pcm_platform_open, SND_SOC_SOF_CLIENT);

/* disconnect pcm substream to a host stream */
int sof_client_pcm_platform_close(struct device *dev,
				  struct snd_pcm_substream *substream)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(dev->parent);

	return snd_sof_pcm_platform_close(sdev, substream);
}
EXPORT_SYMBOL_NS(sof_client_pcm_platform_close, SND_SOC_SOF_CLIENT);

/* host stream hw params */
int sof_client_pcm_platform_hw_params(struct device *dev,
				      struct snd_pcm_substream *substream,
				      struct snd_pcm_hw_params *params,
				      struct sof_ipc_stream_params *ipc_params)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(dev->parent);

	return snd_sof_pcm_platform_hw_params(sdev, substream,
					      params, ipc_params);
}
EXPORT_SYMBOL_NS(sof_client_pcm_platform_hw_params, SND_SOC_SOF_CLIENT);

/* host stream hw free */
int sof_client_pcm_platform_hw_free(struct device *dev,
				    struct snd_pcm_substream *substream)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(dev->parent);

	return snd_sof_pcm_platform_hw_free(sdev, substream);
}
EXPORT_SYMBOL_NS(sof_client_pcm_platform_hw_free, SND_SOC_SOF_CLIENT);

/* host stream trigger */
int sof_client_pcm_platform_trigger(struct device *dev,
				    struct snd_pcm_substream *substream,
				    int cmd)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(dev->parent);

	return snd_sof_pcm_platform_trigger(sdev, substream, cmd);
}
EXPORT_SYMBOL_NS(sof_client_pcm_platform_trigger, SND_SOC_SOF_CLIENT);

/* host DSP message data */
void sof_client_ipc_msg_data(struct device *dev,
			     struct snd_pcm_substream *substream,
			     void *p, size_t sz)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(dev->parent);

	snd_sof_ipc_msg_data(sdev, substream, p, sz);
}
EXPORT_SYMBOL_NS(sof_client_ipc_msg_data, SND_SOC_SOF_CLIENT);

/* host configure DSP HW parameters */
int sof_client_ipc_pcm_params(struct device *dev,
			      struct snd_pcm_substream *substream,
			      const struct sof_ipc_pcm_params_reply *reply)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(dev->parent);

	return snd_sof_ipc_pcm_params(sdev, substream, reply);
}
EXPORT_SYMBOL_NS(sof_client_ipc_pcm_params, SND_SOC_SOF_CLIENT);

/* host stream pointer */
snd_pcm_uframes_t
sof_client_pcm_platform_pointer(struct device *dev,
				struct snd_pcm_substream *substream)
{
	return snd_sof_pcm_platform_pointer(dev, substream);
}
EXPORT_SYMBOL_NS(sof_client_pcm_platform_pointer, SND_SOC_SOF_CLIENT);

/* get hw info */
u32 sof_client_get_hw_info(struct device *dev)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(dev->parent);

	return sof_ops(sdev)->hw_info;
}
EXPORT_SYMBOL_NS(sof_client_get_hw_info, SND_SOC_SOF_CLIENT);

/* dsp core power up/power down */
int sof_client_dsp_core_power_up(struct device *dev, unsigned int core_mask)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(dev->parent);

	return snd_sof_dsp_core_power_up(sdev, core_mask);
}
EXPORT_SYMBOL_NS(sof_client_dsp_core_power_up, SND_SOC_SOF_CLIENT);

int sof_client_dsp_core_power_down(struct device *dev, unsigned int core_mask)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(dev->parent);

	return snd_sof_dsp_core_power_down(sdev, core_mask);
}
EXPORT_SYMBOL_NS(sof_client_dsp_core_power_down, SND_SOC_SOF_CLIENT);

/* get enabled cores mask */
u32 sof_client_get_enabled_cores(struct device *dev)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(dev->parent);

	return sdev->enabled_cores_mask;
}
EXPORT_SYMBOL_NS(sof_client_get_enabled_cores, SND_SOC_SOF_CLIENT);

/* get mmio bar */
int sof_client_get_mmio_bar(struct device *dev)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(dev->parent);

	return sdev->mmio_bar;
}
EXPORT_SYMBOL_NS(sof_client_get_mmio_bar, SND_SOC_SOF_CLIENT);

/* get enabled cores mask */
u32 sof_client_get_next_comp_id(struct device *dev)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(dev->parent);

	return sdev->next_comp_id;
}
EXPORT_SYMBOL_NS(sof_client_get_next_comp_id, SND_SOC_SOF_CLIENT);

/* increment enabled cores mask */
u32 sof_client_inc_next_comp_id(struct device *dev)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(dev->parent);

	return sdev->next_comp_id++;
}
EXPORT_SYMBOL_NS(sof_client_inc_next_comp_id, SND_SOC_SOF_CLIENT);

/* get fw_ready */
struct sof_ipc_fw_ready *sof_client_get_fw_ready(struct device *dev)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(dev->parent);

	return &sdev->fw_ready;
}
EXPORT_SYMBOL_NS(sof_client_get_fw_ready, SND_SOC_SOF_CLIENT);

bool sof_client_is_s0ix_suspend(struct device *dev)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(dev->parent);

	return sdev->system_suspend_target == SOF_SUSPEND_S0IX;
}
EXPORT_SYMBOL_NS(sof_client_is_s0ix_suspend, SND_SOC_SOF_CLIENT);

int sof_client_dsp_hw_params_upon_resume(struct device *dev)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(dev->parent);

	return snd_sof_dsp_hw_params_upon_resume(sdev);
}
EXPORT_SYMBOL_NS(sof_client_dsp_hw_params_upon_resume, SND_SOC_SOF_CLIENT);

/* block IO */
void sof_client_dsp_block_read(struct device *dev, u32 bar,
			       u32 offset, void *dest, size_t bytes)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(dev->parent);

	return snd_sof_dsp_block_read(sdev, bar, offset, dest, bytes);
}
EXPORT_SYMBOL_NS(sof_client_dsp_block_read, SND_SOC_SOF_CLIENT);

void sof_client_dsp_block_write(struct device *dev, u32 bar,
				u32 offset, void *src, size_t bytes)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(dev->parent);

	snd_sof_dsp_block_write(sdev, bar, offset, src, bytes);
}
EXPORT_SYMBOL_NS(sof_client_dsp_block_write, SND_SOC_SOF_CLIENT);

struct dentry *sof_client_get_debugfs_root(struct device *dev)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(dev->parent);

	return sdev->debugfs_root;
}
EXPORT_SYMBOL_NS(sof_client_get_debugfs_root, SND_SOC_SOF_CLIENT);
