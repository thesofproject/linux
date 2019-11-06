// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
//
// This file is provided under a dual BSD/GPLv2 license.  When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2019 Intel Corporation. All rights reserved.
//
// Author: Ranjani Sridharan <ranjani.sridharan@linux.intel.com>
//

#include "sof-client.h"
#include "sof-priv-core.h"
#include "ops.h"

void *sof_get_client_data(struct device *dev)
{
	struct snd_sof_client *client = dev_get_platdata(dev);

	return client->client_data;
}
EXPORT_SYMBOL(sof_get_client_data);

struct snd_sof_dev *snd_sof_get_sof_dev(struct device *dev)
{
	return dev_get_drvdata(dev->parent);
}
EXPORT_SYMBOL(snd_sof_get_sof_dev);

/* get/set cores mask */
u32 snd_sof_dsp_get_enabled_cores_mask(struct device *dev)
{
	struct snd_sof_dev *sdev = snd_sof_get_sof_dev(dev);

	return sdev->enabled_cores_mask;
}
EXPORT_SYMBOL(snd_sof_dsp_get_enabled_cores_mask);

void snd_sof_dsp_set_enabled_cores_mask(struct device *dev, u32 core_mask)
{
	struct snd_sof_dev *sdev = snd_sof_get_sof_dev(dev);

	sdev->enabled_cores_mask = core_mask;
}
EXPORT_SYMBOL(snd_sof_dsp_set_enabled_cores_mask);

bool snd_sof_is_s0_suspend(struct device *dev)
{
	struct snd_sof_dev *sdev = snd_sof_get_sof_dev(dev);

	return sdev->s0_suspend;
}
EXPORT_SYMBOL(snd_sof_is_s0_suspend);

/* get next comp id */
int snd_sof_get_next_comp_id(struct device *dev)
{
	struct snd_sof_dev *sdev = snd_sof_get_sof_dev(dev);

	return sdev->next_comp_id;
}
EXPORT_SYMBOL(snd_sof_get_next_comp_id);

/* increment next comp id */
void snd_sof_inc_next_comp_id(struct device *dev)
{
	struct snd_sof_dev *sdev = snd_sof_get_sof_dev(dev);

	sdev->next_comp_id++;
}
EXPORT_SYMBOL(snd_sof_inc_next_comp_id);

struct sof_ipc_fw_ready *snd_sof_get_fw_ready(struct device *dev)
{
	struct snd_sof_dev *sdev = snd_sof_get_sof_dev(dev);

	return &sdev->fw_ready;
}
EXPORT_SYMBOL(snd_sof_get_fw_ready);

struct snd_sof_pdata *snd_sof_get_pdata(struct device *dev)
{
	struct snd_sof_dev *sdev = snd_sof_get_sof_dev(dev);

	return sdev->pdata;
}
EXPORT_SYMBOL(snd_sof_get_pdata);

u32 snd_sof_get_hw_info(struct device *dev)
{
	struct snd_sof_dev *sdev = snd_sof_get_sof_dev(dev);
	const struct snd_sof_dsp_ops *ops = sof_ops(sdev);

	return ops->hw_info;
}
EXPORT_SYMBOL(snd_sof_get_hw_info);

int snd_sof_get_num_drv(struct device *dev)
{
	struct snd_sof_dev *sdev = snd_sof_get_sof_dev(dev);

	return sof_ops(sdev)->num_drv;
}
EXPORT_SYMBOL(snd_sof_get_num_drv);

struct snd_soc_dai_driver *snd_sof_get_dai_drv(struct device *dev)
{
	struct snd_sof_dev *sdev = snd_sof_get_sof_dev(dev);

	return sof_ops(sdev)->drv;
}
EXPORT_SYMBOL(snd_sof_get_dai_drv);

inline int
snd_sof_client_hw_params_upon_resume(struct device *dev)
{
	struct snd_sof_dev *sdev = snd_sof_get_sof_dev(dev);

	return snd_sof_dsp_hw_params_upon_resume(sdev);
}
EXPORT_SYMBOL(snd_sof_client_hw_params_upon_resume);

/* dsp core power up/power down */
inline int snd_sof_client_core_power_up(struct device *dev,
					       unsigned int core_mask)
{
	struct snd_sof_dev *sdev = snd_sof_get_sof_dev(dev);

	return snd_sof_dsp_core_power_up(sdev, core_mask);
}
EXPORT_SYMBOL(snd_sof_client_core_power_up);

inline int snd_sof_client_core_power_down(struct device *dev,
						 unsigned int core_mask)
{
	struct snd_sof_dev *sdev = snd_sof_get_sof_dev(dev);

	return snd_sof_dsp_core_power_down(sdev, core_mask);
}
EXPORT_SYMBOL(snd_sof_client_core_power_down);

inline void
snd_sof_client_ipc_msg_data(struct device *dev,
			    struct snd_pcm_substream *substream,
			    void *p, size_t sz)
{
	struct snd_sof_dev *sdev = snd_sof_get_sof_dev(dev);

	return snd_sof_ipc_msg_data(sdev, substream, p, sz);
}
EXPORT_SYMBOL(snd_sof_client_ipc_msg_data);

/* host PCM ops */
int snd_sof_client_pcm_platform_open(struct device *dev,
				     struct snd_pcm_substream *substream)
{
	struct snd_sof_dev *sdev = snd_sof_get_sof_dev(dev);

	return snd_sof_pcm_platform_open(sdev, substream);
}
EXPORT_SYMBOL(snd_sof_client_pcm_platform_open);

/* disconnect pcm substream to a host stream */
int snd_sof_client_pcm_platform_close(struct device *dev,
				      struct snd_pcm_substream *substream)
{
	struct snd_sof_dev *sdev = snd_sof_get_sof_dev(dev);

	return snd_sof_pcm_platform_close(sdev, substream);
}
EXPORT_SYMBOL(snd_sof_client_pcm_platform_close);

/* host stream hw params */
int snd_sof_client_pcm_platform_hw_params(struct device *dev,
					  struct snd_pcm_substream *substream,
					  struct snd_pcm_hw_params *params,
					  struct sof_ipc_stream_params *ipc_params)
{
	struct snd_sof_dev *sdev = snd_sof_get_sof_dev(dev);

	return snd_sof_pcm_platform_hw_params(sdev, substream, params,
					      ipc_params);
}
EXPORT_SYMBOL(snd_sof_client_pcm_platform_hw_params);

/* host stream hw free */
int snd_sof_client_pcm_platform_hw_free(struct device *dev,
					struct snd_pcm_substream *substream)
{
	struct snd_sof_dev *sdev = snd_sof_get_sof_dev(dev);

	return snd_sof_pcm_platform_hw_free(sdev, substream);
}
EXPORT_SYMBOL(snd_sof_client_pcm_platform_hw_free);

/* host stream trigger */
int snd_sof_client_pcm_platform_trigger(struct device *dev,
					struct snd_pcm_substream *substream,
					int cmd)
{
	struct snd_sof_dev *sdev = snd_sof_get_sof_dev(dev);

	return snd_sof_pcm_platform_trigger(sdev, substream, cmd);
}
EXPORT_SYMBOL(snd_sof_client_pcm_platform_trigger);

/* host stream pointer */
long snd_sof_client_pcm_platform_pointer(struct device *dev,
					 struct snd_pcm_substream *substream)
{
	struct snd_sof_dev *sdev = snd_sof_get_sof_dev(dev);

	if (sof_ops(sdev)->pcm_pointer)
		return sof_ops(sdev)->pcm_pointer(sdev, substream);

	return -ENOTSUPP;
}
EXPORT_SYMBOL(snd_sof_client_pcm_platform_pointer);

int snd_sof_client_ipc_pcm_params(struct device *dev,
				  struct snd_pcm_substream *substream,
				  const struct sof_ipc_pcm_params_reply *reply)
{
	struct snd_sof_dev *sdev = snd_sof_get_sof_dev(dev);

	return snd_sof_ipc_pcm_params(sdev, substream, reply);
}
EXPORT_SYMBOL(snd_sof_client_ipc_pcm_params);

struct snd_soc_acpi_mach *
snd_sof_client_machine_driver_select(struct device *dev)
{
	struct snd_sof_dev *sdev = snd_sof_get_sof_dev(dev);

	return snd_sof_machine_driver_select(sdev);
}
EXPORT_SYMBOL(snd_sof_client_machine_driver_select);
