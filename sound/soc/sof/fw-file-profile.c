// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
//
// This file is provided under a dual BSD/GPLv2 license.  When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2023 Intel Corporation. All rights reserved.
//

#include <linux/firmware.h>
#include <sound/sof.h>
#include <sound/sof/ext_manifest4.h>
#include "sof-priv.h"

static int sof_test_file_profile(struct device *dev,
				 struct sof_loadable_file_profile *profile,
				 enum sof_ipc_type *ipc_type_to_adjust)
{
	enum sof_ipc_type fw_ipc_type;
	const struct firmware *fw;
	const char *fw_filename;
	const u32 *magic;
	int ret;

	fw_filename = kasprintf(GFP_KERNEL, "%s/%s", profile->fw_path,
				profile->fw_name);
	if (!fw_filename)
		return -ENOMEM;

	ret = firmware_request_nowarn(&fw, fw_filename, dev);
	if (ret < 0) {
		kfree(fw_filename);
		return ret;
	}

	/* firmware file exists, check the magic number */
	magic = (const u32 *)fw->data;
	switch (*magic) {
	case SOF_EXT_MAN_MAGIC_NUMBER:
		fw_ipc_type = SOF_IPC;
		break;
	case SOF_EXT_MAN4_MAGIC_NUMBER:
		fw_ipc_type = SOF_INTEL_IPC4;
		break;
	default:
		dev_err(dev, "Invalid firmware magic: %#x\n", *magic);
		ret = -EINVAL;
		goto out;
	}

	if (ipc_type_to_adjust) {
		*ipc_type_to_adjust = fw_ipc_type;
	} else if (fw_ipc_type != profile->ipc_type) {
		dev_err(dev,
			"ipc type mismatch between %s and expected: %d vs %d\n",
			fw_filename, fw_ipc_type, profile->ipc_type);
		ret = -EINVAL;
	}
out:
	release_firmware(fw);
	kfree(fw_filename);

	return ret;
}

static int
sof_file_profile_for_ipc_type(struct device *dev,
			      enum sof_ipc_type ipc_type,
			      const struct sof_dev_desc *desc,
			      struct sof_loadable_file_profile *base_profile,
			      struct sof_loadable_file_profile *out_profile)
{
	bool fw_path_allocated = false;
	bool fw_lib_path_allocated = false;
	int ret = 0;

	/* firmware path */
	if (base_profile->fw_path) {
		out_profile->fw_path = base_profile->fw_path;
	} else if (base_profile->fw_path_postfix) {
		out_profile->fw_path = devm_kasprintf(dev, GFP_KERNEL, "%s/%s",
							desc->default_fw_path[ipc_type],
							base_profile->fw_path_postfix);
		if (!out_profile->fw_path)
			return -ENOMEM;

		fw_path_allocated = true;
	} else {
		out_profile->fw_path = desc->default_fw_path[ipc_type];
	}

	/* firmware filename */
	if (base_profile->fw_name)
		out_profile->fw_name = base_profile->fw_name;
	else
		out_profile->fw_name = desc->default_fw_filename[ipc_type];

	/*
	 * Check the custom firmware path/filename and adjust the ipc_type to
	 * match with the existing file for the remaining path configuration.
	 *
	 * For default path and firmware name do a verification before
	 * continuing further.
	 */
	if (base_profile->fw_path || base_profile->fw_name) {
		ret = sof_test_file_profile(dev, out_profile, &ipc_type);
		if (ret)
			return ret;

		if (!(desc->ipc_supported_mask & BIT(ipc_type))) {
			dev_err(dev, "Unsupported IPC type %d needed by %s/%s\n",
				ipc_type, base_profile->fw_path,
				base_profile->fw_name);
			return -EINVAL;
		}
	}

	/* firmware library path */
	if (base_profile->fw_lib_path) {
		out_profile->fw_lib_path = base_profile->fw_lib_path;
	} else if (desc->default_lib_path[ipc_type]) {
		if (base_profile->fw_path_postfix) {
			out_profile->fw_lib_path = devm_kasprintf(dev,
							GFP_KERNEL, "%s/%s",
							desc->default_lib_path[ipc_type],
							base_profile->fw_path_postfix);
			if (!out_profile->fw_lib_path) {
				ret = -ENOMEM;
				goto out;
			}

			fw_lib_path_allocated = true;
		} else {
			out_profile->fw_lib_path = desc->default_lib_path[ipc_type];
		}
	}

	if (base_profile->fw_path_postfix)
		out_profile->fw_path_postfix = base_profile->fw_path_postfix;

	/* topology path */
	if (base_profile->tplg_path)
		out_profile->tplg_path = base_profile->tplg_path;
	else
		out_profile->tplg_path = desc->default_tplg_path[ipc_type];

	/* topology name */
	if (base_profile->tplg_name)
		out_profile->tplg_name = base_profile->tplg_name;

	out_profile->ipc_type = ipc_type;

	/* Test only default firmware file */
	if (!base_profile->fw_path && !base_profile->fw_name)
		ret = sof_test_file_profile(dev, out_profile, NULL);

out:
	if (ret) {
		/* Free up path strings created with devm_kasprintf */
		if (fw_path_allocated)
			devm_kfree(dev, out_profile->fw_path);
		if (fw_lib_path_allocated)
			devm_kfree(dev, out_profile->fw_lib_path);

		memset(out_profile, 0, sizeof(*out_profile));
	}

	return ret;
}

static void
sof_missing_firmware_notification(struct device *dev,
				  enum sof_ipc_type ipc_type,
				  const struct sof_dev_desc *desc,
				  struct sof_loadable_file_profile *base_profile)
{
	char *marker;
	int i;

	dev_err(dev, "No SOF firmware file was found.\n");

	for (i = 0; i < SOF_IPC_TYPE_COUNT; i++) {
		if (!(desc->ipc_supported_mask & BIT(i)))
			continue;

		if (i == ipc_type)
			marker = "Requested";
		else
			marker = "Fallback";

		if (base_profile->fw_path_postfix)
			dev_err(dev, " %s file: %s/%s/%s\n", marker,
				desc->default_fw_path[i],
				base_profile->fw_path_postfix,
				desc->default_fw_filename[i]);
		else
			dev_err(dev, " %s file: %s/%s\n", marker,
				desc->default_fw_path[i],
				desc->default_fw_filename[i]);
	}

	dev_err(dev, "Check if you have 'sof-firmware' package installed.\n");
	dev_err(dev, "Optionally it can be manually downloaded from:\n");
	dev_err(dev, "   https://github.com/thesofproject/sof-bin/\n");
}

static void sof_print_profile_info(struct device *dev,
				   enum sof_ipc_type ipc_type,
				   struct sof_loadable_file_profile *profile)
{
	if (ipc_type != profile->ipc_type)
		dev_info(dev,
			 "Using fallback IPC type %d (requested type was %d)\n",
			 profile->ipc_type, ipc_type);

	dev_dbg(dev, "Loadable file profile%s for ipc type %d:\n",
		ipc_type != profile->ipc_type ? " (fallback)" : "",
		profile->ipc_type);

	dev_dbg(dev, " Firmware file: %s/%s\n", profile->fw_path, profile->fw_name);
	if (profile->fw_lib_path)
		dev_dbg(dev, " Firmware lib path: %s\n", profile->fw_lib_path);
	dev_dbg(dev, " Topology path: %s\n", profile->tplg_path);
	if (profile->tplg_name)
		dev_dbg(dev, " Topology name: %s\n", profile->tplg_name);
}

int sof_create_ipc_file_profile(struct snd_sof_dev *sdev,
				struct sof_loadable_file_profile *base_profile,
				struct sof_loadable_file_profile *out_profile)
{
	const struct sof_dev_desc *desc = sdev->pdata->desc;
	struct device *dev = sdev->dev;
	int ret = -ENOENT;
	int i;

	memset(out_profile, 0, sizeof(*out_profile));

	if (base_profile->fw_path)
		dev_dbg(dev, "Module parameter used, changed fw path to %s\n",
			base_profile->fw_path);
	else if (base_profile->fw_path_postfix)
		dev_dbg(dev, "Path postfix appended to default fw path: %s\n",
			base_profile->fw_path_postfix);

	if (base_profile->fw_lib_path)
		dev_dbg(dev, "Module parameter used, changed fw_lib path to %s\n",
			base_profile->fw_lib_path);
	else if (base_profile->fw_path_postfix)
		dev_dbg(dev, "Path postfix appended to default fw_lib path: %s\n",
			base_profile->fw_path_postfix);

	if (base_profile->fw_name)
		dev_dbg(dev, "Module parameter used, changed fw filename to %s\n",
			base_profile->fw_name);

	if (base_profile->tplg_path)
		dev_dbg(dev, "Module parameter used, changed tplg path to %s\n",
			base_profile->tplg_path);

	if (base_profile->tplg_name)
		dev_dbg(dev, "Module parameter used, changed tplg name to %s\n",
			base_profile->tplg_name);

	ret = sof_file_profile_for_ipc_type(dev, base_profile->ipc_type, desc,
					    base_profile, out_profile);
	if (!ret)
		goto out;

	/*
	 * No firmware file was found for the requested IPC type, check all
	 * IPC types as fallback
	 */
	for (i = 0; i < SOF_IPC_TYPE_COUNT; i++) {
		if (i == base_profile->ipc_type ||
		    !(desc->ipc_supported_mask & BIT(i)))
			continue;

		ret = sof_file_profile_for_ipc_type(dev, i, desc, base_profile,
						    out_profile);
		if (!ret)
			break;
	}

out:
	if (ret)
		sof_missing_firmware_notification(dev, base_profile->ipc_type,
						  desc, base_profile);
	else
		sof_print_profile_info(dev, base_profile->ipc_type, out_profile);

	return ret;
}
EXPORT_SYMBOL(sof_create_ipc_file_profile);
