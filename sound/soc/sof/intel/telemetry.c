// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
//
// This file is provided under a dual BSD/GPLv2 license.  When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2023 Intel Corporation. All rights reserved.
//
//

#include <sound/sof/ipc4/header.h>
#include <sound/sof/xtensa.h>
#include "../ipc4-priv.h"
#include "../sof-priv.h"
#include "hda.h"
#include "telemetry.h"

static u32 find_telemetry_slots(struct snd_sof_dev *sdev)
{
	u32 slot_desc_type_offset;
	u32 type;
	int i;

	slot_desc_type_offset = sdev->debug_box.offset + sizeof(u32);
	for (i = 0; i < SOF_IPC4_MAX_DEBUG_SLOTS; i++) {
		sof_mailbox_read(sdev, slot_desc_type_offset, &type, sizeof(type));

		if (type == SOF_IPC4_DEBUG_SLOT_TELEMETRY)
			return sdev->debug_box.offset + (i + 1) * SOF_IPC4_DEBUG_SLOT_SIZE;

		/* The type is the second u32 in the slot descriptor */
		slot_desc_type_offset += SOF_IPC4_DEBUG_DESCRIPTOR_SIZE;
	}

	dev_warn(sdev->dev, "Can't find telemetry in debug window\n");
	return 0;
}

static bool validate_telemetry_data(struct snd_sof_dev *sdev, void *telemetry_data)
{
	u32 seperator;

	seperator = *(u32 *)(telemetry_data + SOF_TELEMETRY_SEPARATOR_OFFSET);
	if (seperator != SOF_TELEMETRY_SEPARATOR) {
		dev_err(sdev->dev, "error: seperator: %#x is not matched with %#x", seperator,
					SOF_TELEMETRY_SEPARATOR);
		return false;
	}

	return true;
}

void sof_ipc4_intel_dump_telemetry_exception_state(struct snd_sof_dev *sdev, u32 flags)
{
	char *level = (flags & SOF_DBG_DUMP_OPTIONAL) ? KERN_DEBUG : KERN_ERR;
	struct core_exception_record *exception;
	struct sof_ipc_dsp_oops_xtensa *xoops;
	void *telemetry_data;
	u32 slot_offset;
	size_t size;
	u32 core;

	slot_offset = find_telemetry_slots(sdev);
	if (!slot_offset)
		return;

	telemetry_data = kmalloc(SOF_IPC4_DEBUG_SLOT_SIZE, GFP_KERNEL);
	if (!telemetry_data)
		return;

	size = sizeof(*xoops) + SOF_IPC4_FW_AR_REGS_COUNT * sizeof(int);
	xoops = kzalloc(size, GFP_KERNEL);
	if (!xoops)
		goto free;

	sof_mailbox_read(sdev, slot_offset, telemetry_data, SOF_IPC4_DEBUG_SLOT_SIZE);
	if (!validate_telemetry_data(sdev, telemetry_data))
		goto free;

	core = FIELD_GET(SOF_DBG_DUMP_CORE_MASK, flags);

	exception = (struct core_exception_record *)(telemetry_data +
				SOF_TELEMETRY_SEPARATOR_OFFSET + sizeof(u32));

	dev_dbg(sdev->dev, "Core exception record version %#x\n",
		exception[core].version);

	xoops->exccause = exception[core].exccause;
	xoops->excvaddr = exception[core].excvaddr;
	xoops->interrupt = exception[core].interrupt;
	xoops->excsave1 = exception[core].excsave;

	xoops->windowbase = exception[core].windowbase;
	xoops->windowstart = exception[core].windowstart;

	xoops->depc = exception[core].depc;
	xoops->epc1 = exception[core].epc_1;
	xoops->epc2 = exception[core].epc_2;
	xoops->eps2 = exception[core].eps_2;

	xoops->plat_hdr.stackptr = exception[core].stack_base_addr;
	xoops->plat_hdr.numaregs = SOF_IPC4_FW_AR_REGS_COUNT;

	memcpy((void *)xoops + sizeof(*xoops), exception[core].ar,
	       SOF_IPC4_FW_AR_REGS_COUNT * sizeof(int));

	sof_oops(sdev, level, xoops);
	sof_stack(sdev, level, xoops, NULL, 0);

free:
	kfree(telemetry_data);
	kfree(xoops);
}
