// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
//
// This file is provided under a dual BSD/GPLv2 license.  When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2022 Intel Corporation. All rights reserved.

#include <linux/firmware.h>
#include <sound/sof/ext_manifest4.h>
#include <sound/sof/ipc4/header.h>
#include "ipc4-ops.h"
#include "ops.h"
#include "sof-priv.h"
#include "sof-audio.h"

static size_t sof_ipc4_fw_parse_ext_man(struct snd_sof_dev *sdev)
{
	struct sof_ipc4_data *ipc4_data = sdev->private;
	struct snd_sof_pdata *plat_data = sdev->pdata;
	struct sof_man4_fw_binary_header *fw_header;
	const struct firmware *fw = plat_data->fw;
	struct sof_man4_module_config *fm_config;
	struct sof_ipc4_fw_module *module_entry;
	struct sof_ext_manifest4_hdr *hdr;
	struct sof_man4_module *fm_entry;
	ssize_t remaining;
	u32 fw_hdr_offset;
	int i;

	remaining = fw->size;
	if (remaining <= sizeof(*hdr)) {
		dev_err(sdev->dev, "Invalid fw size\n");
		return -EINVAL;
	}

	hdr = (struct sof_ext_manifest4_hdr *)fw->data;

	fw_hdr_offset = ipc4_data->manifest_fw_hdr_offset;
	if (!fw_hdr_offset)
		return -EINVAL;

	if (remaining <= hdr->len + fw_hdr_offset + sizeof(*fw_header)) {
		dev_err(sdev->dev, "Invalid hdr->len %d\n", hdr->len);
		return -EINVAL;
	}

	fw_header = (struct sof_man4_fw_binary_header *)(fw->data + hdr->len + fw_hdr_offset);
	remaining -= (hdr->len + fw_hdr_offset);

	if (remaining <= fw_header->len) {
		dev_err(sdev->dev, "Invalid fw_header->len %d\n", fw_header->len);
		return -EINVAL;
	}

	dev_dbg(sdev->dev, " fw %s: header length %x, module num %d", fw_header->name,
		fw_header->len, fw_header->num_module_entries);

	ipc4_data->fw_modules = devm_kmalloc_array(sdev->dev, fw_header->num_module_entries,
						   sizeof(*module_entry), GFP_KERNEL);
	if (!ipc4_data->fw_modules)
		return -ENOMEM;

	ipc4_data->num_fw_modules = fw_header->num_module_entries;
	module_entry = ipc4_data->fw_modules;

	fm_entry = (struct sof_man4_module *)((u8 *)fw_header + fw_header->len);
	remaining -= fw_header->len;

	if (remaining < fw_header->num_module_entries * sizeof(*fm_entry)) {
		dev_err(sdev->dev, "Invalid num_module_entries %d\n",
			fw_header->num_module_entries);
		return -EINVAL;
	}

	fm_config = (struct sof_man4_module_config *)(fm_entry + fw_header->num_module_entries);
	remaining -= (fw_header->num_module_entries * sizeof(*fm_entry));
	for (i = 0; i < fw_header->num_module_entries; i++) {
		memcpy(&module_entry->man4_module_entry, fm_entry, sizeof(*fm_entry));

		if (fm_entry->cfg_count) {
			if (remaining < (fm_entry->cfg_offset + fm_entry->cfg_count) *
			    sizeof(*fm_config)) {
				dev_err(sdev->dev, "Invalid cfg_offset %d\n", fm_entry->cfg_offset);
				return -EINVAL;
			}

			module_entry->bss_size = fm_config[fm_entry->cfg_offset].is_bytes;
		}

		dev_dbg(sdev->dev, "module %s : UUID %pUL bss_size: %#x", fm_entry->name,
			fm_entry->uuid, module_entry->bss_size);

		module_entry->man4_module_entry.id = i;
		ida_init(&module_entry->m_ida);
		module_entry++;
		fm_entry++;
	}

	return hdr->len;
}

static int sof_ipc4_validate_firmware(struct snd_sof_dev *sdev)
{
	/* TODO: Add firmware verification code here */
	return 0;
}

static void sof_ipc4_query_fw_configuration(struct snd_sof_dev *sdev)
{
	const struct sof_ipc_ops *iops = sdev->ipc->ops;
	struct sof_ipc4_msg msg;
	int ret;

	msg.primary = SOF_IPC4_GLB_MSG_TARGET(SOF_IPC4_MODULE_MSG);
	msg.primary |= SOF_IPC4_GLB_MSG_DIR(SOF_IPC4_MSG_REQUEST);
	msg.primary |= SOF_IPC4_MOD_INSTANCE(0);
	msg.primary |= SOF_IPC4_MOD_ID(0);
	msg.extension = SOF_IPC4_MOD_EXT_MSG_PARAM_ID(7);

	msg.data_size = sdev->ipc->max_payload_size;
	msg.data_ptr = kzalloc(msg.data_size, GFP_KERNEL);
	if (!msg.data_ptr)
		return;

	ret = iops->set_get_data(sdev, &msg, msg.data_size, false);
	if (ret)
		return;

	/* TODO: parse the received information */

	kfree(msg.data_ptr);
}

const struct sof_ipc_fw_loader_ops ipc4_loader_ops = {
	.validate = sof_ipc4_validate_firmware,
	.parse_ext_manifest = sof_ipc4_fw_parse_ext_man,
	.query_fw_configuration = sof_ipc4_query_fw_configuration,
};
