// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
//
// This file is provided under a dual BSD/GPLv2 license.  When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2018 Intel Corporation. All rights reserved.
//
// Author: Liam Girdwood <liam.r.girdwood@linux.intel.com>
//

#include <linux/pci.h>
#include "ops.h"

static
bool snd_sof_pci_update_bits_unlocked(struct snd_sof_dev *sdev, u32 offset,
				      u32 mask, u32 value)
{
	struct pci_dev *pci = to_pci_dev(sdev->dev);
	unsigned int old, new;
	u32 ret = 0;

	pci_read_config_dword(pci, offset, &ret);
	old = ret;
	dev_dbg(sdev->dev, "Debug PCIR: %8.8x at  %8.8x\n", old & mask, offset);

	new = (old & ~mask) | (value & mask);

	if (old == new)
		return false;

	pci_write_config_dword(pci, offset, new);
	dev_dbg(sdev->dev, "Debug PCIW: %8.8x at  %8.8x\n", value,
		offset);

	return true;
}

bool snd_sof_pci_update_bits(struct snd_sof_dev *sdev, u32 offset,
			     u32 mask, u32 value)
{
	unsigned long flags;
	bool change;

	spin_lock_irqsave(&sdev->hw_lock, flags);
	change = snd_sof_pci_update_bits_unlocked(sdev, offset, mask, value);
	spin_unlock_irqrestore(&sdev->hw_lock, flags);
	return change;
}
EXPORT_SYMBOL(snd_sof_pci_update_bits);

bool snd_sof_dsp_update_bits_unlocked(struct snd_sof_dev *sdev, u32 bar,
				      u32 offset, u32 mask, u32 value)
{
	unsigned int old, new;
	u32 ret;

	ret = snd_sof_dsp_read(sdev, bar, offset);

	old = ret;
	new = (old & ~mask) | (value & mask);

	if (old == new)
		return false;

	snd_sof_dsp_write(sdev, bar, offset, new);

	return true;
}
EXPORT_SYMBOL(snd_sof_dsp_update_bits_unlocked);

bool snd_sof_dsp_update_bits64_unlocked(struct snd_sof_dev *sdev, u32 bar,
					u32 offset, u64 mask, u64 value)
{
	u64 old, new;

	old = snd_sof_dsp_read64(sdev, bar, offset);

	new = (old & ~mask) | (value & mask);

	if (old == new)
		return false;

	snd_sof_dsp_write64(sdev, bar, offset, new);

	return true;
}
EXPORT_SYMBOL(snd_sof_dsp_update_bits64_unlocked);

/* This is for registers bits with attribute RWC */
bool snd_sof_dsp_update_bits(struct snd_sof_dev *sdev, u32 bar, u32 offset,
			     u32 mask, u32 value)
{
	unsigned long flags;
	bool change;

	spin_lock_irqsave(&sdev->hw_lock, flags);
	change = snd_sof_dsp_update_bits_unlocked(sdev, bar, offset, mask,
						  value);
	spin_unlock_irqrestore(&sdev->hw_lock, flags);
	return change;
}
EXPORT_SYMBOL(snd_sof_dsp_update_bits);

bool snd_sof_dsp_update_bits64(struct snd_sof_dev *sdev, u32 bar, u32 offset,
			       u64 mask, u64 value)
{
	unsigned long flags;
	bool change;

	spin_lock_irqsave(&sdev->hw_lock, flags);
	change = snd_sof_dsp_update_bits64_unlocked(sdev, bar, offset, mask,
						    value);
	spin_unlock_irqrestore(&sdev->hw_lock, flags);
	return change;
}
EXPORT_SYMBOL(snd_sof_dsp_update_bits64);

static
void snd_sof_dsp_update_bits_forced_unlocked(struct snd_sof_dev *sdev, u32 bar,
					     u32 offset, u32 mask, u32 value)
{
	unsigned int old, new;
	u32 ret;

	ret = snd_sof_dsp_read(sdev, bar, offset);

	old = ret;
	new = (old & ~mask) | (value & mask);

	snd_sof_dsp_write(sdev, bar, offset, new);
}

/* This is for registers bits with attribute RWC */
void snd_sof_dsp_update_bits_forced(struct snd_sof_dev *sdev, u32 bar,
				    u32 offset, u32 mask, u32 value)
{
	unsigned long flags;

	spin_lock_irqsave(&sdev->hw_lock, flags);
	snd_sof_dsp_update_bits_forced_unlocked(sdev, bar, offset, mask, value);
	spin_unlock_irqrestore(&sdev->hw_lock, flags);
}
EXPORT_SYMBOL(snd_sof_dsp_update_bits_forced);

void snd_sof_dsp_panic(struct snd_sof_dev *sdev, u32 offset)
{
	dev_err(sdev->dev, "error : DSP panic!\n");

	/*
	 * check if DSP is not ready and did not set the dsp_oops_offset.
	 * if the dsp_oops_offset is not set, set it from the panic message.
	 * Also add a check to memory window setting with panic message.
	 */
	if (!sdev->dsp_oops_offset)
		sdev->dsp_oops_offset = offset;
	else
		dev_dbg(sdev->dev, "panic: dsp_oops_offset %zu offset %d\n",
			sdev->dsp_oops_offset, offset);

	snd_sof_dsp_dbg_dump(sdev, SOF_DBG_REGS | SOF_DBG_MBOX);
	snd_sof_trace_notify_for_error(sdev);
}
EXPORT_SYMBOL(snd_sof_dsp_panic);

/* This is for getting ref count for a DSP core and power on it if needed */
int snd_sof_dsp_core_get(struct snd_sof_dev *sdev, u32 core_idx)
{
	int ret;

	mutex_lock(&sdev->cores_status_mutex);

	/* already powered on, return */
	if (sdev->core_refs[core_idx] > 0) {
		sdev->core_refs[core_idx]++;
		dev_vdbg(sdev->dev, "core_get: core_refs[%d] %d, no need power up\n",
			 core_idx, sdev->core_refs[core_idx]);
		mutex_unlock(&sdev->cores_status_mutex);
		return 0;/* core already enabled, return */
	}

	dev_vdbg(sdev->dev, "core_get: core_refs[%d] %d, powering it up...\n",
		 core_idx, sdev->core_refs[core_idx]);
	/* power up the core that this pipeline is scheduled on */
	ret = snd_sof_dsp_core_power_up(sdev, BIT(core_idx));
	if (ret < 0) {
		dev_err(sdev->dev, "error: powering up pipeline schedule core %d\n",
			core_idx);
		mutex_unlock(&sdev->cores_status_mutex);
		return ret;
	}

	/* update core ref count and enabled_cores_mask */
	sdev->core_refs[core_idx]++;
	sdev->enabled_cores_mask |= BIT(core_idx);

	/* Now notify DSP that the core power status changed */
	snd_sof_ipc_core_enable(sdev);

	mutex_unlock(&sdev->cores_status_mutex);

	return 0;
}
EXPORT_SYMBOL(snd_sof_dsp_core_get);

/* This is for putting ref count for a DSP core and power off it if needed */
int snd_sof_dsp_core_put(struct snd_sof_dev *sdev, u32 core_idx)
{
	int ret;

	mutex_lock(&sdev->cores_status_mutex);

	/* return if the core is still in use */
	if (sdev->core_refs[core_idx] > 1) {
		sdev->core_refs[core_idx]--;
		dev_vdbg(sdev->dev, "core_put: core_refs[%d] %d, no need power down\n",
			 core_idx, sdev->core_refs[core_idx]);
		mutex_unlock(&sdev->cores_status_mutex);
		return 0;
	}

	dev_vdbg(sdev->dev, "core_put: core_refs[%d] %d, powering it down...\n",
		 core_idx, sdev->core_refs[core_idx]);
	/* power down the pipeline schedule core */
	ret = snd_sof_dsp_core_power_down(sdev, BIT(core_idx));
	if (ret < 0) {
		dev_err(sdev->dev, "error: powering down pipeline schedule core %d\n",
			core_idx);
		mutex_unlock(&sdev->cores_status_mutex);
		return ret;
	}

	/* update core ref count and enabled_cores_mask */
	sdev->core_refs[core_idx]--;
	sdev->enabled_cores_mask &= ~BIT(core_idx);

	/* Now notify DSP that the core power status changed */
	snd_sof_ipc_core_enable(sdev);

	mutex_unlock(&sdev->cores_status_mutex);

	return 0;
}
EXPORT_SYMBOL(snd_sof_dsp_core_put);
