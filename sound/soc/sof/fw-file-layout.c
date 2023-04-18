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

struct sof_profile_ipc_type_table {
	enum sof_fw_layout_type layout_type;
	enum sof_ipc_type ipc_type;
};

static const enum sof_ipc_type layout_to_ipc_type_map[] = {
	[SOF_FW_LAYOUT_VENDOR_IPC3] = SOF_IPC_TYPE_3,
	[SOF_FW_LAYOUT_VENDOR_IPC4_SOF] = SOF_IPC_TYPE_4,
	[SOF_FW_LAYOUT_VENDOR_IPC4_INTEL_AVS] = SOF_IPC_TYPE_4,
	[SOF_FW_LAYOUT_VENDOR_IPC4_INTEL_ACE] = SOF_IPC_TYPE_4,
};

static void sof_free_profile_strings(struct device *dev,
				     struct sof_fw_layout_profile *fw_layout)
{
	devm_kfree(dev, fw_layout->fw_path);
	devm_kfree(dev, fw_layout->fw_lib_path);
	devm_kfree(dev, fw_layout->fw_name);
	devm_kfree(dev, fw_layout->tplg_path);

	fw_layout->fw_path = NULL;
	fw_layout->fw_lib_path = NULL;
	fw_layout->fw_name = NULL;
	fw_layout->tplg_path = NULL;
}

static int
setup_fw_layout_profile(struct device *dev,
			const enum sof_fw_layout_type layout_type,
			const char *vendor_name, const char *platform_name,
			const char *fw_path_postfix,
			struct sof_fw_layout_profile *out_layout)
{
	int ret = -ENOMEM;
	const char *str;

	switch (layout_type) {
	case SOF_FW_LAYOUT_VENDOR_IPC3:
		out_layout->ipc_type = SOF_IPC_TYPE_3;
		/*
		 * firmware path: <vendor>/sof</fw_path_postfix>
		 * firmware name: sof-<platform>.ri
		 * topology path: <vendor>/sof-tplg/
		 */
		if (fw_path_postfix)
			str = devm_kasprintf(dev, GFP_KERNEL, "%s/sof/%s",
					     vendor_name, fw_path_postfix);
		else
			str = devm_kasprintf(dev, GFP_KERNEL, "%s/sof",
					     vendor_name);
		if (!str)
			return -ENOMEM;

		out_layout->fw_path = str;

		out_layout->fw_name = devm_kasprintf(dev, GFP_KERNEL,
						     "sof-%s.ri",
						     platform_name);
		if (!out_layout->fw_name)
			goto err;

		out_layout->tplg_path = devm_kasprintf(dev, GFP_KERNEL,
						       "%s/sof-tplg",
						       vendor_name);
		if (!out_layout->tplg_path)
			goto err;
		break;
	case SOF_FW_LAYOUT_VENDOR_IPC4_SOF:
		out_layout->ipc_type = SOF_IPC_TYPE_4;
		/*
		 * firmware path: <vendor>/sof-ipc4/<platform></fw_path_postfix>
		 * firmware name: sof-<platform>.ri
		 * firmware lib path: <vendor>/sof-ipc4-lib/<platform></fw_path_postfix>
		 * topology path: <vendor>/sof-ipc4-tplg/
		 */
		if (fw_path_postfix)
			str = devm_kasprintf(dev, GFP_KERNEL,
					     "%s/sof-ipc4/%s/%s",
					     vendor_name, platform_name,
					     fw_path_postfix);
		else
			str = devm_kasprintf(dev, GFP_KERNEL, "%s/sof-ipc4/%s",
					     vendor_name, platform_name);
		if (!str)
			return -ENOMEM;

		out_layout->fw_path = str;

		out_layout->fw_name = devm_kasprintf(dev, GFP_KERNEL,
						     "sof-%s.ri",
						     platform_name);
		if (!out_layout->fw_name)
			goto err;

		if (fw_path_postfix)
			str = devm_kasprintf(dev, GFP_KERNEL,
					     "%s/sof-ipc4-lib/%s/%s",
					     vendor_name, platform_name,
					     fw_path_postfix);
		else
			str = devm_kasprintf(dev, GFP_KERNEL,
					     "%s/sof-ipc4-lib/%s",
					     vendor_name, platform_name);
		if (!str)
			goto err;

		out_layout->fw_lib_path = str;

		out_layout->tplg_path = devm_kasprintf(dev, GFP_KERNEL,
						       "%s/sof-ace-tplg",
						       vendor_name);
		if (!out_layout->tplg_path)
			goto err;
		break;
	case SOF_FW_LAYOUT_VENDOR_IPC4_INTEL_AVS:
		out_layout->ipc_type = SOF_IPC_TYPE_4;
		/*
		 * firmware path: intel/avs/<platform></fw_path_postfix>
		 * firmware name: dsp_basefw.bin
		 * firmware lib path: intel/avs-lib/<platform></fw_path_postfix>
		 * topology path: intel/avs-tplg/
		 */
		if (fw_path_postfix)
			str = devm_kasprintf(dev, GFP_KERNEL, "intel/avs/%s/%s",
					     platform_name, fw_path_postfix);
		else
			str = devm_kasprintf(dev, GFP_KERNEL, "intel/avs/%s",
					     platform_name);
		if (!str)
			return -ENOMEM;

		out_layout->fw_path = str;

		out_layout->fw_name = devm_kstrdup(dev, "dsp_basefw.bin",
						   GFP_KERNEL);
		if (!out_layout->fw_name)
			goto err;

		if (fw_path_postfix)
			str = devm_kasprintf(dev, GFP_KERNEL,
					     "intel/avs-lib/%s/%s",
					     platform_name, fw_path_postfix);
		else
			str = devm_kasprintf(dev, GFP_KERNEL,
					     "intel/avs-lib/%s",
					     platform_name);
		if (!str)
			goto err;

		out_layout->fw_lib_path = str;

		out_layout->tplg_path = devm_kstrdup(dev, "intel/avs-tplg",
						     GFP_KERNEL);
		if (!out_layout->tplg_path)
			goto err;
		break;
	case SOF_FW_LAYOUT_VENDOR_IPC4_INTEL_ACE:
		out_layout->ipc_type = SOF_IPC_TYPE_4;
		/*
		 * firmware path: intel/sof-ipc4/<platform></fw_path_postfix>
		 * firmware name: sof-<platform>.ri
		 * firmware lib path: intel/sof-ipc4-lib/<platform></fw_path_postfix>
		 * topology path: intel/sof-ace-tplg/
		 */
		if (fw_path_postfix)
			str = devm_kasprintf(dev, GFP_KERNEL,
					     "intel/sof-ipc4/%s/%s",
					     platform_name, fw_path_postfix);
		else
			str = devm_kasprintf(dev, GFP_KERNEL,
					     "intel/sof-ipc4/%s",
					     platform_name);
		if (!str)
			return -ENOMEM;

		out_layout->fw_path = str;

		out_layout->fw_name = devm_kasprintf(dev, GFP_KERNEL,
						     "sof-%s.ri",
						     platform_name);
		if (!out_layout->fw_name)
			goto err;

		if (fw_path_postfix)
			str = devm_kasprintf(dev, GFP_KERNEL,
					     "intel/sof-ipc4-lib/%s/%s",
					     platform_name, fw_path_postfix);
		else
			str = devm_kasprintf(dev, GFP_KERNEL,
					     "intel/sof-ipc4-lib/%s",
					     platform_name);
		if (!str)
			goto err;

		out_layout->fw_lib_path = str;

		out_layout->tplg_path = devm_kstrdup(dev, "intel/sof-ace-tplg",
						     GFP_KERNEL);
		if (!out_layout->tplg_path)
			goto err;
		break;
	default:
		dev_err(dev, "Invalid firmware layout type: %d\n", layout_type);
		return -EINVAL;
	}

	return 0;

err:
	sof_free_profile_strings(dev, out_layout);

	return ret;
}

static int sof_test_fw_layout(struct device *dev,
			      struct sof_fw_layout_profile *fw_layout)
{
	enum sof_ipc_type fw_ipc_type;
	const struct firmware *fw;
	const char *fw_filename;
	const u32 *magic;
	int ret;

	fw_filename = kasprintf(GFP_KERNEL, "%s/%s", fw_layout->fw_path,
				fw_layout->fw_name);
	if (!fw_filename)
		return -ENOMEM;

	ret = firmware_request_nowarn(&fw, fw_filename, dev);
	if (ret < 0)
		return ret;

	/* firmware file exists, check the magic number */
	magic = (const u32 *)fw->data;
	switch (*magic) {
	case SOF_EXT_MAN_MAGIC_NUMBER:
		fw_ipc_type = SOF_IPC_TYPE_3;
		break;
	case SOF_EXT_MAN4_MAGIC_NUMBER:
		fw_ipc_type = SOF_IPC_TYPE_4;
		break;
	default:
		dev_err(dev, "Invalid firmware magic: %#x\n", *magic);
		ret = -EINVAL;
		goto out;
	}

	if (fw_ipc_type != fw_layout->ipc_type) {
		dev_err(dev,
			"ipc type mismatch between file and profile: %d vs %d\n",
			fw_ipc_type, fw_layout->ipc_type);
		ret = -EINVAL;
	}
out:
	release_firmware(fw);

	return ret;
}

static int fw_layout_for_ipc_type(struct device *dev,
				  const enum sof_ipc_type ipc_type,
				  const struct sof_dev_desc *desc,
				  const char *fw_path_postfix,
				  struct sof_fw_layout_profile *out_layout)
{
	bool found = false;
	int i, ret;

	memset(out_layout, 0, sizeof(*out_layout));

	for (i = 0; i < ARRAY_SIZE(layout_to_ipc_type_map); i++) {
		if (layout_to_ipc_type_map[i] != ipc_type)
			continue;

		ret = setup_fw_layout_profile(dev, i, desc->vendor,
					      desc->platform, fw_path_postfix,
					      out_layout);
		if (ret)
			return ret;

		ret = sof_test_fw_layout(dev, out_layout);
		if (!ret) {
			found = true;
			break;
		}

		sof_free_profile_strings(dev, out_layout);
	}

	if (!found)
		return -ENOENT;

	dev_dbg(dev, "Default profile type for IPC type %d: %d\n", ipc_type, i);

	return 0;
}

int sof_create_default_fw_layout_profile(struct device *dev,
					 const enum sof_ipc_type ipc_type,
					 const struct sof_dev_desc *desc,
					 const char *fw_path_postfix,
					 struct sof_fw_layout_profile *out_layout)
{
	int ret = -ENOENT;
	int i;

	memset(out_layout, 0, sizeof(*out_layout));

	ret = fw_layout_for_ipc_type(dev, ipc_type, desc, fw_path_postfix,
				     out_layout);
	if (!ret)
		return 0;

	dev_warn(dev, "No default profile found for requested IPC type %d\n",
		 ipc_type);

	/*
	 * No firmware file was found for the requested IPC type, check all
	 * IPC types as fallback
	 */
	for (i = 0; i < SOF_IPC_TYPE_COUNT; i++) {
		if (i == ipc_type || !(desc->ipc_supported_mask & BIT(i)))
			continue;

		ret = fw_layout_for_ipc_type(dev, i, desc, fw_path_postfix,
					     out_layout);
		if (!ret)
			break;

		dev_info(dev, "No default profile found for fallback IPC type%d\n",
			 ipc_type);
	}

	if (ret) {
		dev_err(dev, "No sof firmware file was found, you might need to\n");
		dev_err(dev,
			"       download it from https://github.com/thesofproject/sof-bin/\n");
	}

	return ret;
}
EXPORT_SYMBOL(sof_create_default_fw_layout_profile);
