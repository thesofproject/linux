// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
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

	snd_sof_dsp_dbg_dump(sdev, SOF_DBG_DUMP_REGS | SOF_DBG_DUMP_MBOX);
	snd_sof_trace_notify_for_error(sdev);
}
EXPORT_SYMBOL(snd_sof_dsp_panic);

/* This is for getting ref count for a DSP core and power on it if needed */
int snd_sof_dsp_core_get(struct snd_sof_dev *sdev, u32 core_idx)
{
	u32 target_cores_mask;
	int ret = 0;

	mutex_lock(&sdev->cores_status_mutex);

	target_cores_mask = sdev->enabled_cores_mask | BIT(core_idx);

	/* return if already powered on */
	if (sdev->core_refs[core_idx] > 0) {
		dev_vdbg(sdev->dev, "core_get: enabled_cores_mask 0x%x, core_refs[%d] %d, no need to power up\n",
			 sdev->enabled_cores_mask, core_idx, sdev->core_refs[core_idx]);
		sdev->core_refs[core_idx]++;
		goto done;
	}

	dev_vdbg(sdev->dev, "core_get: enabled_cores_mask 0x%x, core_refs[%d] %d, powering it up...\n",
		 sdev->enabled_cores_mask, core_idx, sdev->core_refs[core_idx]);
	/* power up the core that this pipeline is scheduled on */
	ret = snd_sof_dsp_core_power_up(sdev, BIT(core_idx));
	if (ret < 0) {
		dev_err(sdev->dev, "error: powering up pipeline schedule core %d\n",
			core_idx);
		goto done;
	}

	/* Now notify DSP that the core power status changed */
	ret = snd_sof_ipc_core_enable(sdev, target_cores_mask);
	if (ret < 0) {
		/* power down the core to reflect the status */
		snd_sof_dsp_core_power_down(sdev, BIT(core_idx));
		goto done;
	}

	/* update core ref count and enabled_cores_mask */
	sdev->core_refs[core_idx]++;
	sdev->enabled_cores_mask = target_cores_mask;
done:
	mutex_unlock(&sdev->cores_status_mutex);

	return ret;
}
EXPORT_SYMBOL(snd_sof_dsp_core_get);

/* This is for putting ref count for a DSP core and power off it if needed */
int snd_sof_dsp_core_put(struct snd_sof_dev *sdev, u32 core_idx)
{
	u32 target_cores_mask;
	int ret = 0;

	mutex_lock(&sdev->cores_status_mutex);

	target_cores_mask = sdev->enabled_cores_mask & (~BIT(core_idx));

	/* return if the core is still in use */
	if (sdev->core_refs[core_idx] > 1) {
		dev_vdbg(sdev->dev, "core_put: enabled_cores_mask 0x%x, core_refs[%d] %d, no need to power down\n",
			 sdev->enabled_cores_mask, core_idx, sdev->core_refs[core_idx]);
		sdev->core_refs[core_idx]--;
		goto done;
	}

	dev_vdbg(sdev->dev, "core_put: enabled_cores_mask 0x%x, core_refs[%d] %d, powering it down...\n",
		 sdev->enabled_cores_mask, core_idx, sdev->core_refs[core_idx]);
	/* power down the pipeline schedule core */
	ret = snd_sof_dsp_core_power_down(sdev, BIT(core_idx));
	if (ret < 0) {
		dev_err(sdev->dev, "error: powering down pipeline schedule core %d\n",
			core_idx);
		goto done;
	}

	/* Now notify DSP that the core power status changed */
	ret = snd_sof_ipc_core_enable(sdev, target_cores_mask);
	if (ret < 0) {
		/* power up the core back to reflect the status */
		snd_sof_dsp_core_power_up(sdev, BIT(core_idx));
		goto done;
	}

	/* update core ref count and enabled_cores_mask */
	sdev->core_refs[core_idx]--;
	sdev->enabled_cores_mask = target_cores_mask;

done:
	mutex_unlock(&sdev->cores_status_mutex);

	return ret;
}
EXPORT_SYMBOL(snd_sof_dsp_core_put);
