// SPDX-License-Identifier: GPL-2.0-only
//
// Copyright(c) 2026 Intel Corporation
//
// Authors: Peter Ujfalusi <peter.ujfalusi@linux.intel.com>
//

#include <linux/auxiliary_bus.h>
#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <sound/soc.h>

#include "sof-client.h"
#include "sof-client-audio.h"

#define SOF_AUDIO_SUSPEND_DELAY_MS 3000

static void sof_audio_client_init_debugfs(struct sof_client_dev *cdev,
					  struct sof_audio_client_pdata *pdata)
{
	struct dentry *debugfs_root = sof_client_get_debugfs_root(cdev);
	char *debugfs_dir;

	debugfs_dir = devm_kasprintf(&cdev->auxdev.dev, GFP_KERNEL, "audio.%u",
				     cdev->auxdev.id);
	if (!debugfs_dir)
		return;

	pdata->debugfs_root = debugfs_create_dir(debugfs_dir, debugfs_root);
	if (IS_ERR_OR_NULL(pdata->debugfs_root))
		return;

	pdata->debug_topology_name = pdata->machine.sof_tplg_filename ?
				     pdata->machine.sof_tplg_filename :
				     sof_client_get_topology_name(cdev);
	pdata->debug_card_name = pdata->machine.mach_params.card_name;
	pdata->debug_machine_driver = pdata->machine.drv_name;

	debugfs_create_str("topology_name", 0444, pdata->debugfs_root,
			   (char **)&pdata->debug_topology_name);

	if (pdata->debug_card_name)
		debugfs_create_str("card_name", 0444, pdata->debugfs_root,
				   (char **)&pdata->debug_card_name);

	if (pdata->debug_machine_driver)
		debugfs_create_str("machine_driver", 0444, pdata->debugfs_root,
				   (char **)&pdata->debug_machine_driver);
}

static int sof_audio_client_probe(struct auxiliary_device *auxdev,
				  const struct auxiliary_device_id *id)
{
	struct sof_client_dev *cdev = auxiliary_dev_to_sof_client_dev(auxdev);
	struct sof_audio_client_pdata *pdata = dev_get_platdata(&auxdev->dev);
	int ret;

	auxiliary_set_drvdata(auxdev, cdev);

	ret = snd_soc_register_component(&auxdev->dev, &pdata->plat_drv,
					 pdata->drv, pdata->num_drv);
	if (ret < 0)
		return ret;

	ret = sof_client_machine_register(cdev);
	if (ret < 0) {
		snd_soc_unregister_component(&auxdev->dev);
		return ret;
	}

	sof_audio_client_init_debugfs(cdev, pdata);

	pm_runtime_set_autosuspend_delay(&auxdev->dev, SOF_AUDIO_SUSPEND_DELAY_MS);
	pm_runtime_use_autosuspend(&auxdev->dev);
	pm_runtime_enable(&auxdev->dev);
	pm_runtime_mark_last_busy(&auxdev->dev);
	pm_runtime_idle(&auxdev->dev);

	return 0;
}

static void sof_audio_client_remove(struct auxiliary_device *auxdev)
{
	struct sof_client_dev *cdev = auxiliary_dev_to_sof_client_dev(auxdev);
	struct sof_audio_client_pdata *pdata = dev_get_platdata(&auxdev->dev);

	if (pdata && pdata->debugfs_root)
		debugfs_remove_recursive(pdata->debugfs_root);

	pm_runtime_disable(&auxdev->dev);

	sof_client_machine_unregister(cdev);
	snd_soc_unregister_component(&auxdev->dev);
}

static const struct auxiliary_device_id sof_audio_client_id_table[] = {
	{ .name = "snd_sof.audio", },
	{},
};
MODULE_DEVICE_TABLE(auxiliary, sof_audio_client_id_table);

static struct auxiliary_driver sof_audio_client_drv = {
	.probe = sof_audio_client_probe,
	.remove = sof_audio_client_remove,
	.id_table = sof_audio_client_id_table,
};

module_auxiliary_driver(sof_audio_client_drv);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("SOF Audio Client Driver");
MODULE_IMPORT_NS("SND_SOC_SOF_CLIENT");
