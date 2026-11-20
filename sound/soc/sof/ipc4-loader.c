// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
//
// This file is provided under a dual BSD/GPLv2 license.  When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2022 Intel Corporation

#include <linux/firmware.h>
#include <sound/sof/ext_manifest4.h>
#include <sound/sof/ipc4/header.h>
#include <trace/events/sof.h>
#include "ipc4-priv.h"
#include "sof-audio.h"
#include "sof-priv.h"
#include "ops.h"

/* The module ID includes the id of the library it is part of at offset 12 */
#define SOF_IPC4_MOD_LIB_ID_SHIFT	12

static ssize_t sof_ipc4_fw_parse_ext_man(struct snd_sof_dev *sdev,
					 struct sof_ipc4_fw_library *fw_lib)
{
	struct sof_ipc4_fw_data *ipc4_data = sdev->private;
	const struct firmware *fw = fw_lib->sof_fw.fw;
	struct sof_man4_fw_binary_header *fw_header;
	struct sof_ext_manifest4_hdr *ext_man_hdr;
	struct sof_man4_module_config *fm_config;
	struct sof_ipc4_fw_module *fw_module;
	struct sof_man4_module *fm_entry;
	ssize_t remaining;
	u32 fw_hdr_offset;
	int i;

	if (!ipc4_data) {
		dev_err(sdev->dev, "%s: ipc4_data is not available\n", __func__);
		return -EINVAL;
	}

	remaining = fw->size;
	if (remaining <= sizeof(*ext_man_hdr)) {
		dev_err(sdev->dev, "Firmware size is too small: %zu\n", remaining);
		return -EINVAL;
	}

	ext_man_hdr = (struct sof_ext_manifest4_hdr *)fw->data;

	/*
	 * At the start of the firmware image we must have an extended manifest.
	 * Verify that the magic number is correct.
	 */
	if (ext_man_hdr->id != SOF_EXT_MAN4_MAGIC_NUMBER) {
		dev_err(sdev->dev,
			"Unexpected extended manifest magic number: %#x\n",
			ext_man_hdr->id);
		return -EINVAL;
	}

	fw_hdr_offset = ipc4_data->manifest_fw_hdr_offset;
	if (!fw_hdr_offset)
		return -EINVAL;

	if (remaining <= ext_man_hdr->len + fw_hdr_offset + sizeof(*fw_header)) {
		dev_err(sdev->dev, "Invalid firmware size %zu, should be at least %zu\n",
			remaining, ext_man_hdr->len + fw_hdr_offset + sizeof(*fw_header));
		return -EINVAL;
	}

	fw_header = (struct sof_man4_fw_binary_header *)
				(fw->data + ext_man_hdr->len + fw_hdr_offset);
	remaining -= (ext_man_hdr->len + fw_hdr_offset);

	if (remaining <= fw_header->len) {
		dev_err(sdev->dev, "Invalid fw_header->len %u\n", fw_header->len);
		return -EINVAL;
	}

	dev_info(sdev->dev, "Loaded firmware library: %s, version: %u.%u.%u.%u\n",
		 fw_header->name, fw_header->major_version, fw_header->minor_version,
		 fw_header->hotfix_version, fw_header->build_version);
	dev_dbg(sdev->dev, "Header length: %u, module count: %u\n",
		fw_header->len, fw_header->num_module_entries);

	/* copy the fw_version of basefw into debugfs at first boot */
	if (fw == sdev->basefw.fw) {
		sdev->fw_version.major = fw_header->major_version;
		sdev->fw_version.minor = fw_header->minor_version;
		sdev->fw_version.micro = fw_header->hotfix_version;
		sdev->fw_version.build = fw_header->build_version;
	}

	fw_lib->modules = devm_kmalloc_array(sdev->dev, fw_header->num_module_entries,
					     sizeof(*fw_module), GFP_KERNEL);
	if (!fw_lib->modules)
		return -ENOMEM;

	fw_lib->name = fw_header->name;
	fw_lib->num_modules = fw_header->num_module_entries;
	fw_module = fw_lib->modules;

	fm_entry = (struct sof_man4_module *)((u8 *)fw_header + fw_header->len);
	remaining -= fw_header->len;

	if (remaining < fw_header->num_module_entries * sizeof(*fm_entry)) {
		dev_err(sdev->dev, "Invalid num_module_entries %u\n",
			fw_header->num_module_entries);
		return -EINVAL;
	}

	fm_config = (struct sof_man4_module_config *)
				(fm_entry + fw_header->num_module_entries);
	remaining -= (fw_header->num_module_entries * sizeof(*fm_entry));
	for (i = 0; i < fw_header->num_module_entries; i++) {
		memcpy(&fw_module->man4_module_entry, fm_entry, sizeof(*fm_entry));

		if (fm_entry->cfg_count) {
			if (remaining < (fm_entry->cfg_offset + fm_entry->cfg_count) *
			    sizeof(*fm_config)) {
				dev_err(sdev->dev, "Invalid module cfg_offset %u\n",
					fm_entry->cfg_offset);
				return -EINVAL;
			}

			fw_module->fw_mod_cfg = &fm_config[fm_entry->cfg_offset];

			dev_dbg(sdev->dev,
				"module %s: UUID %pUL cfg_count: %u, bss_size: %#x\n",
				fm_entry->name, &fm_entry->uuid, fm_entry->cfg_count,
				fm_config[fm_entry->cfg_offset].is_bytes);
		} else {
			dev_dbg(sdev->dev, "module %s: UUID %pUL\n", fm_entry->name,
				&fm_entry->uuid);
		}

		fw_module->man4_module_entry.id = i;
		ida_init(&fw_module->m_ida);
		fw_module->private = NULL;

		fw_module++;
		fm_entry++;
	}

	return ext_man_hdr->len;
}

static size_t sof_ipc4_fw_parse_basefw_ext_man(struct snd_sof_dev *sdev)
{
	struct sof_ipc4_fw_data *ipc4_data = sdev->private;
	struct sof_ipc4_fw_library *fw_lib;
	ssize_t payload_offset;
	int ret;

	if (sdev->dsp_test_mode_enabled)
		fw_lib = devm_kzalloc(sdev->dev, sizeof(*fw_lib), GFP_KERNEL);
	else
		fw_lib = devm_kzalloc(sdev->dev, sizeof(*fw_lib), GFP_KERNEL);
	if (!fw_lib)
		return -ENOMEM;

	fw_lib->sof_fw.fw = sdev->basefw.fw;

	payload_offset = sof_ipc4_fw_parse_ext_man(sdev, fw_lib);
	if (sdev->dsp_test_mode_enabled) {
		devm_kfree(sdev->dev, fw_lib->modules);
		devm_kfree(sdev->dev, fw_lib);
		return payload_offset;
	}

	if (payload_offset > 0) {
		fw_lib->sof_fw.payload_offset = payload_offset;

		/* basefw ID is 0 */
		fw_lib->id = 0;
		ret = xa_insert(&ipc4_data->fw_lib_xa, 0, fw_lib, GFP_KERNEL);
		if (ret)
			return ret;
	}

	return payload_offset;
}

static int sof_ipc4_load_library(struct snd_sof_dev *sdev, unsigned long lib_id,
				 const char *lib_filename, bool optional,
				 bool unloadable)
{
	struct sof_ipc4_fw_data *ipc4_data = sdev->private;
	struct sof_ipc4_fw_library *fw_lib;
	ssize_t payload_offset;
	int ret, i;

	if (!ipc4_data->load_library) {
		dev_err(sdev->dev, "Library loading is not supported on this platform\n");
		return -EOPNOTSUPP;
	}

	fw_lib = devm_kzalloc(sdev->dev, sizeof(*fw_lib), GFP_KERNEL);
	if (!fw_lib)
		return -ENOMEM;

	if (optional) {
		ret = firmware_request_nowarn(&fw_lib->sof_fw.fw, lib_filename,
					      sdev->dev);
		if (ret < 0) {
			/* optional library, override the error */
			ret = 0;
			goto free_fw_lib;
		}
	} else {
		ret = request_firmware(&fw_lib->sof_fw.fw, lib_filename,
				       sdev->dev);
		if (ret < 0) {
			dev_err(sdev->dev, "Library file '%s' is missing\n",
				lib_filename);
			goto free_fw_lib;
		}
	}

	dev_dbg(sdev->dev, "Library file '%s' loaded\n", lib_filename);

	payload_offset = sof_ipc4_fw_parse_ext_man(sdev, fw_lib);
	if (payload_offset <= 0) {
		if (!payload_offset)
			ret = -EINVAL;
		else
			ret = payload_offset;

		goto release;
	}

	fw_lib->sof_fw.payload_offset = payload_offset;
	fw_lib->id = lib_id;
	fw_lib->unloadable = unloadable;

	/* Fix up the module ID numbers within the library */
	for (i = 0; i < fw_lib->num_modules; i++)
		fw_lib->modules[i].man4_module_entry.id |= (lib_id << SOF_IPC4_MOD_LIB_ID_SHIFT);

	ret = ipc4_data->load_library(sdev, fw_lib, false);
	if (ret)
		goto release;

	ret = xa_insert(&ipc4_data->fw_lib_xa, lib_id, fw_lib, GFP_KERNEL);
	if (unlikely(ret))
		goto release;

	ret = sof_ipc4_query_sof_info(sdev);
	if (ret)
		dev_warn(sdev->dev,
			 "%s: Failed to refresh runtime SOF info after loading %s: %d\n",
			 __func__, lib_filename, ret);

	return 0;

release:
	release_firmware(fw_lib->sof_fw.fw);
	/* Allocated within sof_ipc4_fw_parse_ext_man() */
	devm_kfree(sdev->dev, fw_lib->modules);
free_fw_lib:
	devm_kfree(sdev->dev, fw_lib);

	return ret;
}

/**
 * sof_ipc4_complete_split_release - loads the library parts of a split firmware
 * @sdev: SOF device
 *
 * With IPC4 the firmware can be a single binary or a split release.
 * - single binary: only the basefw
 * - split release: basefw and two libraries (openmodules, debug)
 *
 * With split firmware release it is also allowed that for example only the
 * debug library is present (the openmodules content is built in the basefw).
 *
 * To handle the permutations try to load the openmodules then the debug
 * libraries as optional ones after the basefw boot.
 *
 * The libraries for the split release are stored alongside the basefw on the
 * filesystem.
 */
int sof_ipc4_complete_split_release(struct snd_sof_dev *sdev)
{
	static const char * const lib_bundle[] = { "openmodules", "debug" };
	const char *fw_filename = sdev->pdata->fw_filename;
	const char *lib_filename, *p;
	size_t lib_name_base_size;
	unsigned long lib_id = 1;
	char *lib_name_base;
	int i;

	p = strstr(fw_filename, ".ri");
	if (!p || strlen(p) != 3) {
		dev_info(sdev->dev,
			 "%s: Firmware name '%s' is missing .ri extension\n",
			 __func__, fw_filename);
		return 0;
	}

	/* Space for the firmware basename + '\0', without the extension */
	lib_name_base_size = strlen(fw_filename) - 2;
	lib_name_base = kzalloc(lib_name_base_size, GFP_KERNEL);
	if (!lib_name_base)
		return -ENOMEM;

	/*
	 * strscpy will 0 terminate the copied string, removing the '.ri' from
	 * the end of the fw_filename, for example:
	 * fw_filename:		"sof-ptl.ri\0"
	 * lib_name_base:	"sof-ptl\0"
	 */
	strscpy(lib_name_base, fw_filename, lib_name_base_size);

	for (i = 0; i < ARRAY_SIZE(lib_bundle); i++) {
		int ret;

		lib_filename = kasprintf(GFP_KERNEL, "%s/%s-%s.ri",
					 sdev->pdata->fw_filename_prefix,
					 lib_name_base, lib_bundle[i]);
		if (!lib_filename) {
			kfree(lib_name_base);
			return -ENOMEM;
		}

		ret = sof_ipc4_load_library(sdev, lib_id, lib_filename, true, false);
		if (ret)
			dev_warn(sdev->dev, "%s: Failed to load %s: %d\n",
				 __func__, lib_filename, ret);
		else
			lib_id++;

		kfree(lib_filename);
	}

	kfree(lib_name_base);

	return 0;
}

static int sof_ipc4_load_library_by_uuid(struct snd_sof_dev *sdev,
					 unsigned long lib_id, const guid_t *uuid)
{
	const char *lib_filename;
	int ret;

	if (!sdev->pdata->fw_lib_prefix) {
		dev_err(sdev->dev,
			"Library loading is not supported due to not set library path\n");
		return -EINVAL;
	}

	lib_filename = kasprintf(GFP_KERNEL, "%s/%pUL.bin",
				 sdev->pdata->fw_lib_prefix, uuid);
	if (!lib_filename)
		return -ENOMEM;

	ret = sof_ipc4_load_library(sdev, lib_id, lib_filename, false, true);

	kfree(lib_filename);

	return ret;
}

static bool sof_ipc4_fw_library_is_idle(struct sof_ipc4_fw_library *fw_lib)
{
	int i;

	if (!fw_lib || !fw_lib->id || !fw_lib->unloadable)
		return false;

	for (i = 0; i < fw_lib->num_modules; i++) {
		if (!ida_is_empty(&fw_lib->modules[i].m_ida))
			return false;
	}

	return true;
}

static void sof_ipc4_detach_library_widgets(struct snd_sof_dev *sdev,
					    struct sof_ipc4_fw_library *fw_lib)
{
	struct sof_ipc4_fw_module *mods_end = fw_lib->modules + fw_lib->num_modules;
	struct snd_sof_widget *swidget;

	if (!fw_lib->modules || !fw_lib->num_modules)
		return;

	list_for_each_entry(swidget, &sdev->widget_list, list) {
		struct sof_ipc4_fw_module *fw_module = swidget->module_info;

		if (fw_module >= fw_lib->modules && fw_module < mods_end)
			swidget->module_info = NULL;
	}
}

int sof_ipc4_unload_library(struct snd_sof_dev *sdev, unsigned long lib_id)
{
	struct sof_ipc4_fw_data *ipc4_data = sdev->private;
	const struct sof_ipc_ops *iops = sdev->ipc->ops;
	struct sof_ipc4_fw_library *fw_lib;
	struct sof_ipc4_msg msg = {{ 0 }};
	u32 id_payload;
	int ret;

	if (!ipc4_data->library_unload_supported)
		return -EOPNOTSUPP;

	if (!lib_id)
		return -EINVAL;

	fw_lib = xa_load(&ipc4_data->fw_lib_xa, lib_id);
	if (!fw_lib)
		return -ENOENT;

	if (!sof_ipc4_fw_library_is_idle(fw_lib))
		return -EBUSY;

	id_payload = lib_id;
	msg.primary = SOF_IPC4_MSG_TARGET(SOF_IPC4_MODULE_MSG);
	msg.primary |= SOF_IPC4_MSG_DIR(SOF_IPC4_MSG_REQUEST);
	msg.primary |= SOF_IPC4_MOD_ID(SOF_IPC4_MOD_INIT_BASEFW_MOD_ID);
	msg.primary |= SOF_IPC4_MOD_INSTANCE(SOF_IPC4_MOD_INIT_BASEFW_INSTANCE_ID);
	msg.extension = SOF_IPC4_MOD_EXT_MSG_PARAM_ID(SOF_IPC4_FW_PARAM_SOF_LIBRARY_UNLOAD);
	msg.data_ptr = &id_payload;
	msg.data_size = sizeof(id_payload);

	ret = iops->set_get_data(sdev, &msg, msg.data_size, true);
	if (ret) {
		dev_warn(sdev->dev, "Failed to unload library id %lu: %d\n", lib_id, ret);
		return ret;
	}

	dev_dbg(sdev->dev, "Unloaded library id %lu\n", lib_id);
	sof_ipc4_detach_library_widgets(sdev, fw_lib);
	xa_erase(&ipc4_data->fw_lib_xa, lib_id);
	release_firmware(fw_lib->sof_fw.fw);
	devm_kfree(sdev->dev, fw_lib->modules);
	devm_kfree(sdev->dev, fw_lib);

	ret = sof_ipc4_query_sof_info(sdev);
	if (ret)
		dev_warn(sdev->dev,
			 "Failed to refresh runtime SOF info after unloading lib %lu: %d\n",
			 lib_id, ret);

	return 0;
}

static int sof_ipc4_purge_idle_library(struct snd_sof_dev *sdev, unsigned long *freed_id)
{
	struct sof_ipc4_fw_data *ipc4_data = sdev->private;
	struct sof_ipc4_fw_library *fw_lib;
	unsigned long lib_id;

	if (!ipc4_data->library_unload_supported)
		return -EOPNOTSUPP;

	xa_for_each_start(&ipc4_data->fw_lib_xa, lib_id, fw_lib, 1) {
		if (!sof_ipc4_fw_library_is_idle(fw_lib))
			continue;

		if (!sof_ipc4_unload_library(sdev, lib_id)) {
			if (freed_id)
				*freed_id = lib_id;
			return 0;
		}
	}

	return -EBUSY;
}

struct sof_ipc4_fw_module *sof_ipc4_find_loaded_module_by_uuid(struct snd_sof_dev *sdev,
								   const guid_t *uuid)
{
	struct sof_ipc4_fw_data *ipc4_data = sdev->private;
	struct sof_ipc4_fw_library *fw_lib;
	unsigned long lib_id;
	int i;

	if (guid_is_null(uuid))
		return NULL;

	xa_for_each(&ipc4_data->fw_lib_xa, lib_id, fw_lib) {
		for (i = 0; i < fw_lib->num_modules; i++) {
			if (guid_equal(uuid, &fw_lib->modules[i].man4_module_entry.uuid))
				return &fw_lib->modules[i];
		}
	}

	return NULL;
}

struct sof_ipc4_fw_module *sof_ipc4_find_module_by_uuid(struct snd_sof_dev *sdev,
							const guid_t *uuid)
{
	struct sof_ipc4_fw_data *ipc4_data = sdev->private;
	struct sof_ipc4_fw_library *fw_lib;
	struct sof_ipc4_fw_module *fw_module;
	unsigned long lib_id = 0;
	int i, ret;

	fw_module = sof_ipc4_find_loaded_module_by_uuid(sdev, uuid);
	if (fw_module || guid_is_null(uuid))
		return fw_module;

	for (lib_id = 1; lib_id < ipc4_data->max_libs_count; lib_id++) {
		if (!xa_load(&ipc4_data->fw_lib_xa, lib_id))
			break;
	}

	if (lib_id >= ipc4_data->max_libs_count) {
		ret = sof_ipc4_purge_idle_library(sdev, &lib_id);
		if (ret) {
			dev_err(sdev->dev,
				"%s: Maximum allowed number of libraries reached (%u)\n",
				__func__, ipc4_data->max_libs_count);
			return NULL;
		}
	}

	/* The module cannot be found, try to load it as a library */
	ret = sof_ipc4_load_library_by_uuid(sdev, lib_id, uuid);
	if (ret && !sof_ipc4_purge_idle_library(sdev, NULL))
		ret = sof_ipc4_load_library_by_uuid(sdev, lib_id, uuid);
	if (ret)
		return NULL;

	/* Look for the module in the newly loaded library, it should be available now */
	fw_lib = xa_load(&ipc4_data->fw_lib_xa, lib_id);
	if (!fw_lib)
		return NULL;

	for (i = 0; i < fw_lib->num_modules; i++) {
		if (guid_equal(uuid, &fw_lib->modules[i].man4_module_entry.uuid))
			return &fw_lib->modules[i];
	}

	return NULL;
}

static int sof_ipc4_validate_firmware(struct snd_sof_dev *sdev)
{
	struct sof_ipc4_fw_data *ipc4_data = sdev->private;
	u32 fw_hdr_offset = ipc4_data->manifest_fw_hdr_offset;
	struct sof_man4_fw_binary_header *fw_header;
	const struct firmware *fw = sdev->basefw.fw;
	struct sof_ext_manifest4_hdr *ext_man_hdr;

	ext_man_hdr = (struct sof_ext_manifest4_hdr *)fw->data;
	fw_header = (struct sof_man4_fw_binary_header *)
				(fw->data + ext_man_hdr->len + fw_hdr_offset);

	/* TODO: Add firmware verification code here */

	dev_dbg(sdev->dev, "Validated firmware version: %u.%u.%u.%u\n",
		fw_header->major_version, fw_header->minor_version,
		fw_header->hotfix_version, fw_header->build_version);

	return 0;
}

static int sof_ipc4_parse_sof_info(struct snd_sof_dev *sdev,
				   void *sof_info_data, u32 sof_info_size,
				   bool replace)
{
	struct sof_ipc4_fw_data *ipc4_data = sdev->private;
	void *new_codec_info = NULL;
	struct sof_ipc4_tuple *tuple;
	size_t tuple_size;
	size_t offset = 0;
	int ret = 0;

	while (offset < sof_info_size) {
		if (sof_info_size - offset < sizeof(*tuple)) {
			dev_err(sdev->dev, "Invalid SOF info tuple header at offset %zu\n",
				offset);
			ret = -EINVAL;
			goto out_free;
		}

		tuple = (struct sof_ipc4_tuple *)((u8 *)sof_info_data + offset);
		tuple_size = sizeof(*tuple) + tuple->size;
		if (tuple_size < sizeof(*tuple) || tuple_size > sof_info_size - offset) {
			dev_err(sdev->dev, "Invalid SOF info tuple size %u at offset %zu\n",
				tuple->size, offset);
			ret = -EINVAL;
			goto out_free;
		}

		switch (tuple->type) {
		case SOF_IPC4_SOF_CODEC_INFO:
		{
			u32 count;

			if (tuple->size < sizeof(u32)) {
				dev_err(sdev->dev, "Invalid codec info tuple size %u\n",
					tuple->size);
				ret = -EINVAL;
				goto out_free;
			}

			count = *(const u32 *)tuple->value;
			if (count > (tuple->size - sizeof(u32)) / sizeof(u32)) {
				dev_err(sdev->dev,
					"Invalid codec info item count %u for size %u\n",
					count, tuple->size);
				ret = -EINVAL;
				goto out_free;
			}

			if (new_codec_info) {
				dev_err(sdev->dev, "Duplicate codec info tuple\n");
				ret = -EINVAL;
				goto out_free;
			}

			if (count) {
				new_codec_info = kmemdup(tuple->value, tuple->size, GFP_KERNEL);
				if (!new_codec_info) {
					ret = -ENOMEM;
					goto out_free;
				}
			}
			dev_dbg(sdev->dev, "SOF codec_info %s: %u items\n",
				replace ? "refreshed" : "parsed", count);
			break;
		}
		case SOF_IPC4_SOF_CAPS:
		{
			u32 caps;

			if (tuple->size < sizeof(u32)) {
				dev_err(sdev->dev, "Invalid SOF caps tuple size %u\n",
					tuple->size);
				ret = -EINVAL;
				goto out_free;
			}
			caps = *(const u32 *)tuple->value;
			ipc4_data->sof_info_runtime_supported =
				!!(caps & SOF_IPC4_SOF_CAP_RUNTIME_SOF_INFO);
			ipc4_data->library_unload_supported =
				!!(caps & SOF_IPC4_SOF_CAP_LIBRARY_UNLOAD);
			dev_dbg(sdev->dev, "SOF caps: 0x%x (runtime_info=%d, lib_unload=%d)\n",
				caps, ipc4_data->sof_info_runtime_supported,
				ipc4_data->library_unload_supported);
			break;
		}
		default:
			break;
		}

		offset += tuple_size;
	}

	if (replace || new_codec_info) {
		guard(mutex)(&ipc4_data->codec_info_mutex);
		kfree(ipc4_data->codec_info);
		ipc4_data->codec_info = new_codec_info;
		new_codec_info = NULL;
	}

out_free:
	kfree(new_codec_info);
	return ret;
}

int sof_ipc4_query_sof_info(struct snd_sof_dev *sdev)
{
	struct sof_ipc4_fw_data *ipc4_data = sdev->private;
	const struct sof_ipc_ops *iops = sdev->ipc->ops;
	struct sof_ipc4_msg msg = {{ 0 }};
	int ret;

	if (!ipc4_data->sof_info_runtime_supported)
		return 0;

	msg.primary = SOF_IPC4_MSG_TARGET(SOF_IPC4_MODULE_MSG);
	msg.primary |= SOF_IPC4_MSG_DIR(SOF_IPC4_MSG_REQUEST);
	msg.primary |= SOF_IPC4_MOD_ID(SOF_IPC4_MOD_INIT_BASEFW_MOD_ID);
	msg.primary |= SOF_IPC4_MOD_INSTANCE(SOF_IPC4_MOD_INIT_BASEFW_INSTANCE_ID);
	msg.extension = SOF_IPC4_MOD_EXT_MSG_PARAM_ID(SOF_IPC4_FW_PARAM_SOF_INFO_GET);

	msg.data_size = sdev->ipc->max_payload_size;
	msg.data_ptr = kzalloc(msg.data_size, GFP_KERNEL);
	if (!msg.data_ptr)
		return -ENOMEM;

	ret = iops->set_get_data(sdev, &msg, msg.data_size, false);
	if (!ret)
		ret = sof_ipc4_parse_sof_info(sdev, msg.data_ptr, msg.data_size, true);

	kfree(msg.data_ptr);
	return ret;
}

int sof_ipc4_query_fw_configuration(struct snd_sof_dev *sdev)
{
	struct sof_ipc4_fw_data *ipc4_data = sdev->private;
	const struct sof_ipc_ops *iops = sdev->ipc->ops;
	struct sof_ipc4_fw_version *fw_ver;
	struct sof_ipc4_tuple *tuple;
	struct sof_ipc4_msg msg;
	size_t offset = 0;
	int ret;

	/* Get the firmware configuration */
	msg.primary = SOF_IPC4_MSG_TARGET(SOF_IPC4_MODULE_MSG);
	msg.primary |= SOF_IPC4_MSG_DIR(SOF_IPC4_MSG_REQUEST);
	msg.primary |= SOF_IPC4_MOD_ID(SOF_IPC4_MOD_INIT_BASEFW_MOD_ID);
	msg.primary |= SOF_IPC4_MOD_INSTANCE(SOF_IPC4_MOD_INIT_BASEFW_INSTANCE_ID);
	msg.extension = SOF_IPC4_MOD_EXT_MSG_PARAM_ID(SOF_IPC4_FW_PARAM_FW_CONFIG);

	msg.data_size = sdev->ipc->max_payload_size;
	msg.data_ptr = kzalloc(msg.data_size, GFP_KERNEL);
	if (!msg.data_ptr)
		return -ENOMEM;

	ret = iops->set_get_data(sdev, &msg, msg.data_size, false);
	if (ret)
		goto out;

	while (offset < msg.data_size) {
		tuple = (struct sof_ipc4_tuple *)((u8 *)msg.data_ptr + offset);

		switch (tuple->type) {
		case SOF_IPC4_FW_CFG_FW_VERSION:
			fw_ver = (struct sof_ipc4_fw_version *)tuple->value;

			dev_info(sdev->dev,
				 "Booted firmware version: %u.%u.%u.%u\n",
				 fw_ver->major, fw_ver->minor, fw_ver->hotfix,
				 fw_ver->build);
			break;
		case SOF_IPC4_FW_CFG_DL_MAILBOX_BYTES:
			trace_sof_ipc4_fw_config(sdev, "DL mailbox size", *tuple->value);
			break;
		case SOF_IPC4_FW_CFG_UL_MAILBOX_BYTES:
			trace_sof_ipc4_fw_config(sdev, "UL mailbox size", *tuple->value);
			break;
		case SOF_IPC4_FW_CFG_TRACE_LOG_BYTES:
			trace_sof_ipc4_fw_config(sdev, "Trace log size", *tuple->value);
			ipc4_data->mtrace_log_bytes = *tuple->value;
			break;
		case SOF_IPC4_FW_CFG_MAX_LIBS_COUNT:
			trace_sof_ipc4_fw_config(sdev, "maximum number of libraries",
						 *tuple->value);
			ipc4_data->max_libs_count = *tuple->value;
			if (!ipc4_data->max_libs_count)
				ipc4_data->max_libs_count = 1;
			break;
		case SOF_IPC4_FW_CFG_MAX_PPL_COUNT:
			ipc4_data->max_num_pipelines = *tuple->value;
			trace_sof_ipc4_fw_config(sdev, "Max PPL count %d",
						 ipc4_data->max_num_pipelines);
			if (ipc4_data->max_num_pipelines <= 0) {
				dev_err(sdev->dev, "Invalid max_num_pipelines %d",
					ipc4_data->max_num_pipelines);
				ret = -EINVAL;
				goto out;
			}
			break;
		case SOF_IPC4_FW_CONTEXT_SAVE:
			ipc4_data->fw_context_save = *tuple->value;
			/*
			 * Set the default libraries_restored value - if full
			 * context save is supported then it means that
			 * libraries are restored
			 */
			ipc4_data->libraries_restored = ipc4_data->fw_context_save;
			break;
		case SOF_IPC4_FW_CFG_SOF_INFO:
			ret = sof_ipc4_parse_sof_info(sdev, tuple->value, tuple->size, false);
			if (ret)
				goto out;
			break;
		default:
			break;
		}

		offset += sizeof(*tuple) + tuple->size;
	}

	/* Get the hardware configuration */
	msg.primary = SOF_IPC4_MSG_TARGET(SOF_IPC4_MODULE_MSG);
	msg.primary |= SOF_IPC4_MSG_DIR(SOF_IPC4_MSG_REQUEST);
	msg.primary |= SOF_IPC4_MOD_ID(SOF_IPC4_MOD_INIT_BASEFW_MOD_ID);
	msg.primary |= SOF_IPC4_MOD_INSTANCE(SOF_IPC4_MOD_INIT_BASEFW_INSTANCE_ID);
	msg.extension = SOF_IPC4_MOD_EXT_MSG_PARAM_ID(SOF_IPC4_FW_PARAM_HW_CONFIG_GET);

	msg.data_size = sdev->ipc->max_payload_size;

	ret = iops->set_get_data(sdev, &msg, msg.data_size, false);
	if (ret)
		goto out;

	offset = 0;
	while (offset < msg.data_size) {
		tuple = (struct sof_ipc4_tuple *)((u8 *)msg.data_ptr + offset);

		switch (tuple->type) {
		case SOF_IPC4_HW_CFG_INTEL_MIC_PRIVACY_CAPS:
			if (ipc4_data->intel_configure_mic_privacy) {
				struct sof_ipc4_intel_mic_privacy_cap *caps;

				caps = (struct sof_ipc4_intel_mic_privacy_cap *)tuple->value;
				ipc4_data->intel_configure_mic_privacy(sdev, caps);
			}
			break;
		default:
			break;
		}

		offset += sizeof(*tuple) + tuple->size;
	}

out:
	kfree(msg.data_ptr);

	return ret;
}

int sof_ipc4_reload_fw_libraries(struct snd_sof_dev *sdev)
{
	struct sof_ipc4_fw_data *ipc4_data = sdev->private;
	struct sof_ipc4_fw_library *fw_lib;
	bool has_ext_libs = false;
	unsigned long lib_id;
	int ret = 0;

	xa_for_each_start(&ipc4_data->fw_lib_xa, lib_id, fw_lib, 1) {
		has_ext_libs = true;
		ret = ipc4_data->load_library(sdev, fw_lib, true);
		if (ret) {
			dev_err(sdev->dev, "%s: Failed to reload library: %s, %d\n",
				__func__, fw_lib->name, ret);
			break;
		}
	}

	if (!ret && has_ext_libs && !ipc4_data->libraries_restored &&
	    ipc4_data->sof_info_runtime_supported)
		ret = sof_ipc4_query_sof_info(sdev);

	return ret;
}

/**
 * sof_ipc4_update_cpc_from_manifest - Update the cpc in base config from manifest
 * @sdev: SOF device
 * @fw_module: pointer struct sof_ipc4_fw_module to parse
 * @basecfg: Pointer to the base_config to update
 */
void sof_ipc4_update_cpc_from_manifest(struct snd_sof_dev *sdev,
				       struct sof_ipc4_fw_module *fw_module,
				       struct sof_ipc4_base_module_cfg *basecfg)
{
	const struct sof_man4_module_config *fw_mod_cfg;
	u32 cpc_pick = 0;
	u32 max_cpc = 0;
	const char *msg;
	int i;

	if (!fw_module->fw_mod_cfg) {
		msg = "No mod_cfg available for CPC lookup in the firmware file's manifest";
		goto no_cpc;
	}

	/*
	 * Find the best matching (highest) CPC value based on the module's
	 * IBS/OBS configuration inferred from the audio format selection.
	 *
	 * The CPC value in each module config entry has been measured and
	 * recorded as a IBS/OBS/CPC triplet and stored in the firmware file's
	 * manifest
	 */
	fw_mod_cfg = fw_module->fw_mod_cfg;
	for (i = 0; i < fw_module->man4_module_entry.cfg_count; i++) {
		if (basecfg->obs == fw_mod_cfg[i].obs &&
		    basecfg->ibs == fw_mod_cfg[i].ibs &&
		    cpc_pick < fw_mod_cfg[i].cpc)
			cpc_pick = fw_mod_cfg[i].cpc;

		if (max_cpc < fw_mod_cfg[i].cpc)
			max_cpc = fw_mod_cfg[i].cpc;
	}

	basecfg->cpc = cpc_pick;

	/* We have a matching configuration for CPC */
	if (basecfg->cpc)
		return;

	/*
	 * No matching IBS/OBS found, the firmware manifest is missing
	 * information in the module's module configuration table.
	 */
	if (!max_cpc)
		msg = "No CPC value available in the firmware file's manifest";
	else if (!cpc_pick)
		msg = "No CPC match in the firmware file's manifest";

no_cpc:
	dev_dbg(sdev->dev, "%s (UUID: %pUL): %s (ibs/obs: %u/%u)\n",
		fw_module->man4_module_entry.name,
		&fw_module->man4_module_entry.uuid, msg, basecfg->ibs,
		basecfg->obs);
}

const struct sof_ipc_fw_loader_ops ipc4_loader_ops = {
	.validate = sof_ipc4_validate_firmware,
	.parse_ext_manifest = sof_ipc4_fw_parse_basefw_ext_man,
};
