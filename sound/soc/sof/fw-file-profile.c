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

/**
 * enum sof_fw_layout_type - pre-defined file-layout for loadable fw files
 * @SOF_FW_LAYOUT_IPC3_SOF:
 *	firmware path:		<vendor>/sof</fw_path_postfix>
 *	firmware name:		sof-<platform>.ri
 *	topology path:		<vendor>/sof-tplg/
 * @SOF_FW_LAYOUT_IPC4_SOF:
 *	firmware path:		<vendor>/sof-ipc4/<platform></fw_path_postfix>
 *	firmware name:		sof-<platform>.ri
 *	firmware lib path:	<vendor>/sof-ipc4-lib/<platform></fw_path_postfix>
 *      topology path:		<vendor>/sof-ipc4-tplg/
 * @SOF_FW_LAYOUT_IPC4_INTEL_ACE:
 *	firmware path:		intel/sof-ipc4/<platform></fw_path_postfix>
 *	firmware name:		sof-<platform>.ri
 *	firmware lib path:	intel/sof-ipc4-lib/<platform></fw_path_postfix>
 *	topology path:		intel/sof-ace-tplg/
 * @SOF_FW_LAYOUT_IPC4_INTEL_AVS:
 *	firmware path:		intel/avs/<platform></fw_path_postfix>
 *	firmware name:		dsp_basefw.bin
 *	firmware lib path:	intel/avs/<platform></fw_path_postfix>
 *	topology path:		intel/avs-tplg/
 */
enum sof_fw_layout_type {
	SOF_FW_LAYOUT_IPC3_SOF,
	SOF_FW_LAYOUT_IPC4_SOF,
	SOF_FW_LAYOUT_IPC4_INTEL_ACE,
	SOF_FW_LAYOUT_IPC4_INTEL_AVS,
};

struct sof_fw_layout_map {
	enum sof_ipc_type ipc_type;
	char *layout_name;
};

static const struct sof_fw_layout_map fw_layouts[] = {
	{ .ipc_type = SOF_IPC_TYPE_3, .layout_name = "SOF IPC3", },
	{ .ipc_type = SOF_IPC_TYPE_4, .layout_name = "SOF IPC4", },
	{ .ipc_type = SOF_IPC_TYPE_4, .layout_name = "SOF IPC4 for Intel ACE platforms", },
	{ .ipc_type = SOF_IPC_TYPE_4, .layout_name = "Intel AVS IPC4", },
};

static int sof_test_firmware_file(struct device *dev,
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
		dev_dbg(dev, "Failed to open firmware file: %s\n", fw_filename);
		kfree(fw_filename);
		return ret;
	}

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

static int sof_test_topology_file(struct device *dev,
				  struct sof_loadable_file_profile *profile)
{
	const struct firmware *fw;
	const char *tplg_filename;
	int ret;

	if (!profile->tplg_path || !profile->tplg_name)
		return 0;

	tplg_filename = kasprintf(GFP_KERNEL, "%s/%s", profile->tplg_path,
				  profile->tplg_name);
	if (!tplg_filename)
		return -ENOMEM;

	ret = firmware_request_nowarn(&fw, tplg_filename, dev);
	if (!ret)
		release_firmware(fw);
	else
		dev_dbg(dev, "Failed to open topology file: %s\n", tplg_filename);

	kfree(tplg_filename);

	return ret;
}

static int
sof_create_fw_profile(struct snd_sof_dev *sdev, const struct sof_dev_desc *desc,
		      struct sof_loadable_file_profile *default_profile,
		      struct sof_loadable_file_profile *base_profile,
		      struct sof_loadable_file_profile *out_profile)
{
	enum sof_ipc_type ipc_type = default_profile->ipc_type;
	struct snd_sof_pdata *plat_data = sdev->pdata;
	struct device *dev = sdev->dev;
	int ret = 0;

	/* firmware path */
	if (base_profile->fw_path)
		out_profile->fw_path = base_profile->fw_path;
	else
		out_profile->fw_path = default_profile->fw_path;

	/* firmware filename */
	if (base_profile->fw_name)
		out_profile->fw_name = base_profile->fw_name;
	else
		out_profile->fw_name = default_profile->fw_name;

	/*
	 * Check the custom firmware path/filename and adjust the ipc_type to
	 * match with the existing file for the remaining path configuration.
	 *
	 * For default path and firmware name do a verification before
	 * continuing further.
	 */
	if (base_profile->fw_path || base_profile->fw_name) {
		ret = sof_test_firmware_file(dev, out_profile, &ipc_type);
		if (ret)
			return ret;

		if (!(desc->ipc_supported_mask & BIT(ipc_type))) {
			dev_err(dev, "Unsupported IPC type %d needed by %s/%s\n",
				ipc_type, out_profile->fw_path,
				out_profile->fw_name);
			return -EINVAL;
		}
	}

	/* firmware library path */
	if (base_profile->fw_lib_path)
		out_profile->fw_lib_path = base_profile->fw_lib_path;
	else if (default_profile->fw_lib_path)
		out_profile->fw_lib_path = default_profile->fw_lib_path;

	if (base_profile->fw_path_postfix)
		out_profile->fw_path_postfix = base_profile->fw_path_postfix;

	/* topology path */
	if (base_profile->tplg_path)
		out_profile->tplg_path = base_profile->tplg_path;
	else
		out_profile->tplg_path = default_profile->tplg_path;

	/* topology name */
	out_profile->tplg_name = plat_data->tplg_filename;

	out_profile->ipc_type = ipc_type;

	/* Test only default firmware file */
	if (!base_profile->fw_path && !base_profile->fw_name)
		ret = sof_test_firmware_file(dev, out_profile, NULL);

	if (!ret)
		ret = sof_test_topology_file(dev, out_profile);

	if (ret)
		memset(out_profile, 0, sizeof(*out_profile));

	return ret;
}

static void sof_free_profile_strings(struct device *dev,
				     struct sof_loadable_file_profile *profile)
{
	devm_kfree(dev, profile->fw_path);
	devm_kfree(dev, profile->fw_lib_path);
	devm_kfree(dev, profile->fw_name);
	devm_kfree(dev, profile->tplg_path);

	memset(profile, 0, sizeof(*profile));
}

static int
sof_default_fw_layout(struct device *dev, const enum sof_fw_layout_type layout_type,
		      const char *vendor, const char *platform,
		      const char *fw_path_postfix,
		      struct sof_loadable_file_profile *default_profile)
{
	int ret = -ENOMEM;
	const char *str;

	switch (layout_type) {
	case SOF_FW_LAYOUT_IPC3_SOF:
		default_profile->ipc_type = SOF_IPC_TYPE_3;
		/*
		 * firmware path: <vendor>/sof</fw_path_postfix>
		 * firmware name: sof-<platform>.ri
		 * topology path: <vendor>/sof-tplg/
		 */
		if (fw_path_postfix)
			str = devm_kasprintf(dev, GFP_KERNEL, "%s/sof/%s",
					     vendor, fw_path_postfix);
		else
			str = devm_kasprintf(dev, GFP_KERNEL, "%s/sof", vendor);
		if (!str)
			return -ENOMEM;

		default_profile->fw_path = str;

		default_profile->fw_name = devm_kasprintf(dev, GFP_KERNEL,
							  "sof-%s.ri", platform);
		if (!default_profile->fw_name)
			goto err;

		default_profile->tplg_path = devm_kasprintf(dev, GFP_KERNEL,
							    "%s/sof-tplg", vendor);
		if (!default_profile->tplg_path)
			goto err;
		break;
	case SOF_FW_LAYOUT_IPC4_SOF:
		default_profile->ipc_type = SOF_IPC_TYPE_4;
		/*
		 * firmware path: <vendor>/sof-ipc4/<platform></fw_path_postfix>
		 * firmware name: sof-<platform>.ri
		 * firmware lib path: <vendor>/sof-ipc4-lib/<platform></fw_path_postfix>
		 * topology path: <vendor>/sof-ipc4-tplg/
		 */
		if (fw_path_postfix)
			str = devm_kasprintf(dev, GFP_KERNEL, "%s/sof-ipc4/%s/%s",
					     vendor, platform, fw_path_postfix);
		else
			str = devm_kasprintf(dev, GFP_KERNEL, "%s/sof-ipc4/%s",
					     vendor, platform);
		if (!str)
			return -ENOMEM;

		default_profile->fw_path = str;

		default_profile->fw_name = devm_kasprintf(dev, GFP_KERNEL,
							  "sof-%s.ri", platform);
		if (!default_profile->fw_name)
			goto err;

		if (fw_path_postfix)
			str = devm_kasprintf(dev, GFP_KERNEL, "%s/sof-ipc4-lib/%s/%s",
					     vendor, platform, fw_path_postfix);
		else
			str = devm_kasprintf(dev, GFP_KERNEL, "%s/sof-ipc4-lib/%s",
					     vendor, platform);
		if (!str)
			goto err;

		default_profile->fw_lib_path = str;

		default_profile->tplg_path = devm_kasprintf(dev, GFP_KERNEL,
							    "%s/sof-ipc4-tplg", vendor);
		if (!default_profile->tplg_path)
			goto err;
		break;
	case SOF_FW_LAYOUT_IPC4_INTEL_ACE:
		default_profile->ipc_type = SOF_IPC_TYPE_4;
		/*
		 * firmware path: intel/sof-ipc4/<platform></fw_path_postfix>
		 * firmware name: sof-<platform>.ri
		 * firmware lib path: intel/sof-ipc4-lib/<platform></fw_path_postfix>
		 * topology path: intel/sof-ace-tplg/
		 */
		if (fw_path_postfix)
			str = devm_kasprintf(dev, GFP_KERNEL, "intel/sof-ipc4/%s/%s",
					     platform, fw_path_postfix);
		else
			str = devm_kasprintf(dev, GFP_KERNEL, "intel/sof-ipc4/%s",
					     platform);
		if (!str)
			return -ENOMEM;

		default_profile->fw_path = str;

		default_profile->fw_name = devm_kasprintf(dev, GFP_KERNEL,
							  "sof-%s.ri", platform);
		if (!default_profile->fw_name)
			goto err;

		if (fw_path_postfix)
			str = devm_kasprintf(dev, GFP_KERNEL, "intel/sof-ipc4-lib/%s/%s",
					     platform, fw_path_postfix);
		else
			str = devm_kasprintf(dev, GFP_KERNEL, "intel/sof-ipc4-lib/%s",
					     platform);
		if (!str)
			goto err;

		default_profile->fw_lib_path = str;

		default_profile->tplg_path = devm_kstrdup(dev, "intel/sof-ace-tplg",
							  GFP_KERNEL);
		if (!default_profile->tplg_path)
			goto err;
		break;
	case SOF_FW_LAYOUT_IPC4_INTEL_AVS:
		default_profile->ipc_type = SOF_IPC_TYPE_4;
		/*
		 * firmware path: intel/avs/<platform></fw_path_postfix>
		 * firmware name: dsp_basefw.bin
		 * firmware lib path: intel/avs-lib/<platform></fw_path_postfix>
		 * topology path: intel/avs-tplg/
		 */
		if (fw_path_postfix)
			str = devm_kasprintf(dev, GFP_KERNEL, "intel/avs/%s/%s",
					     platform, fw_path_postfix);
		else
			str = devm_kasprintf(dev, GFP_KERNEL, "intel/avs/%s",
					     platform);
		if (!str)
			return -ENOMEM;

		default_profile->fw_path = str;

		default_profile->fw_name = devm_kstrdup(dev, "dsp_basefw.bin",
							GFP_KERNEL);
		if (!default_profile->fw_name)
			goto err;

		if (fw_path_postfix)
			str = devm_kasprintf(dev, GFP_KERNEL, "intel/avs/%s/%s",
					     platform, fw_path_postfix);
		else
			str = devm_kasprintf(dev, GFP_KERNEL, "intel/avs/%s",
					     platform);
		if (!str)
			goto err;

		default_profile->fw_lib_path = str;

		default_profile->tplg_path = devm_kstrdup(dev, "intel/avs-tplg",
							  GFP_KERNEL);
		if (!default_profile->tplg_path)
			goto err;
		break;
	default:
		dev_err(dev, "Invalid firmware layout type: %d\n", layout_type);
		return -EINVAL;
	}

	return 0;

err:
	sof_free_profile_strings(dev, default_profile);

	return ret;
}

static int
sof_file_profile_for_ipc_type(struct snd_sof_dev *sdev,
			      enum sof_ipc_type ipc_type,
			      const struct sof_dev_desc *desc,
			      struct sof_loadable_file_profile *base_profile,
			      struct sof_loadable_file_profile *out_profile)
{
	struct sof_loadable_file_profile default_profile = { 0 };
	bool found = false;
	int i, ret;

	memset(out_profile, 0, sizeof(*out_profile));

	for (i = 0; i < ARRAY_SIZE(fw_layouts); i++) {
		if (fw_layouts[i].ipc_type != ipc_type)
			continue;

		ret = sof_default_fw_layout(sdev->dev, i, desc->vendor,
					    desc->platform,
					    base_profile->fw_path_postfix,
					    &default_profile);
		if (ret)
			return ret;

		ret = sof_create_fw_profile(sdev, desc, &default_profile,
					    base_profile, out_profile);
		if (!ret) {
			found = true;
			break;
		}

		sof_free_profile_strings(sdev->dev, &default_profile);
	}

	if (!found)
		return -ENOENT;

	return 0;
}

static void
sof_missing_firmware_notification(struct snd_sof_dev *sdev,
				  enum sof_ipc_type ipc_type,
				  struct sof_loadable_file_profile *base_profile)
{
	struct sof_loadable_file_profile default_profile = { 0 };
	struct snd_sof_pdata *plat_data = sdev->pdata;
	const struct sof_dev_desc *desc = plat_data->desc;
	struct device *dev = sdev->dev;
	int ipc_type_count, i, j, ret;
	char *marker;

	dev_err(dev, "SOF firmware and/or topology file not found.\n");
	dev_info(dev, "Supported default profiles\n");

	if (IS_ENABLED(CONFIG_SND_SOC_SOF_ALLOW_FALLBACK_TO_NEWER_IPC_VERSION))
		ipc_type_count = SOF_IPC_TYPE_COUNT - 1;
	else
		ipc_type_count = base_profile->ipc_type;

	for (i = 0; i <= ipc_type_count; i++) {
		if (!(desc->ipc_supported_mask & BIT(i)))
			continue;

		if (i == ipc_type)
			marker = "Requested";
		else
			marker = "Fallback";

		dev_info(dev, "- ipc type %d (%s):\n", i, marker);
		for (j = 0; j < ARRAY_SIZE(fw_layouts); j++) {
			if (fw_layouts[j].ipc_type != i)
				continue;

			ret = sof_default_fw_layout(sdev->dev, j, desc->vendor,
						    desc->platform,
						    base_profile->fw_path_postfix,
						    &default_profile);
			if (ret)
				return;

			dev_info(dev, " Firmware layout: %s\n",
				 fw_layouts[j].layout_name);
			dev_info(dev, "  Firmware file: %s/%s\n",
				default_profile.fw_path, default_profile.fw_name);
			dev_info(dev, "  Topology file: %s/%s\n",
				 default_profile.tplg_path, plat_data->tplg_filename);

			sof_free_profile_strings(sdev->dev, &default_profile);
		}
	}

	if (base_profile->fw_path || base_profile->fw_name ||
	    base_profile->tplg_path || base_profile->tplg_name)
		dev_info(dev, "Verify the path/name override module parameters.\n");

	dev_info(dev, "Check if you have 'sof-firmware' package installed.\n");
	dev_info(dev, "Optionally it can be manually downloaded from:\n");
	dev_info(dev, "   https://github.com/thesofproject/sof-bin/\n");
}

static void sof_print_profile_info(struct snd_sof_dev *sdev,
				   enum sof_ipc_type ipc_type,
				   struct sof_loadable_file_profile *profile)
{
	struct device *dev = sdev->dev;

	if (ipc_type != profile->ipc_type)
		dev_info(dev,
			 "Using fallback IPC type %d (requested type was %d)\n",
			 profile->ipc_type, ipc_type);

	dev_info(dev, "Firmware paths/files for ipc type %d:\n", profile->ipc_type);

	dev_info(dev, " Firmware file:     %s/%s\n", profile->fw_path, profile->fw_name);
	if (profile->fw_lib_path)
		dev_info(dev, " Firmware lib path: %s\n", profile->fw_lib_path);
	dev_info(dev, " Topology file:     %s/%s\n", profile->tplg_path, profile->tplg_name);
}

int sof_create_ipc_file_profile(struct snd_sof_dev *sdev,
				struct sof_loadable_file_profile *base_profile,
				struct sof_loadable_file_profile *out_profile)
{
	const struct sof_dev_desc *desc = sdev->pdata->desc;
	int ipc_fallback_start, ret, i;

	ret = sof_file_profile_for_ipc_type(sdev, base_profile->ipc_type, desc,
					    base_profile, out_profile);
	if (!ret)
		goto out;

	/*
	 * No firmware file was found for the requested IPC type, as fallback
	 * if SND_SOC_SOF_ALLOW_FALLBACK_TO_NEWER_IPC_VERSION is selected, check
	 * all IPC versions in a backwards direction (from newer to older)
	 * if SND_SOC_SOF_ALLOW_FALLBACK_TO_NEWER_IPC_VERSION is not selected,
	 * check only older IPC versions than the selected/default version
	 */
	if (IS_ENABLED(CONFIG_SND_SOC_SOF_ALLOW_FALLBACK_TO_NEWER_IPC_VERSION))
		ipc_fallback_start = SOF_IPC_TYPE_COUNT - 1;
	else
		ipc_fallback_start = (int)base_profile->ipc_type - 1;

	for (i = ipc_fallback_start; i >= 0 ; i--) {
		if (i == base_profile->ipc_type ||
		    !(desc->ipc_supported_mask & BIT(i)))
			continue;

		ret = sof_file_profile_for_ipc_type(sdev, i, desc, base_profile,
						    out_profile);
		if (!ret)
			break;
	}

out:
	if (ret)
		sof_missing_firmware_notification(sdev, base_profile->ipc_type,
						  base_profile);
	else
		sof_print_profile_info(sdev, base_profile->ipc_type, out_profile);

	return ret;
}
EXPORT_SYMBOL(sof_create_ipc_file_profile);
