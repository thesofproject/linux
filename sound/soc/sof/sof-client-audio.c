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

static void sof_audio_client_ipc_rx_handler(struct sof_client_dev *cdev, void *msg_buf)
{
	struct sof_audio_client_pdata *pdata = dev_get_platdata(&cdev->auxdev.dev);

	if (pdata->ipc_ops && pdata->ipc_ops->rx_notification)
		pdata->ipc_ops->rx_notification(cdev, msg_buf);
}

static int sof_audio_client_get_module_name(struct sof_client_dev *cdev,
					    u32 module_id, int instance_id,
					    char *name, size_t size)
{
	struct sof_audio_client_pdata *pdata = dev_get_platdata(&cdev->auxdev.dev);

	if (!pdata->ipc_ops || !pdata->ipc_ops->get_module_name)
		return -EOPNOTSUPP;

	return pdata->ipc_ops->get_module_name(cdev, module_id, instance_id,
					       name, size);
}

static int sof_audio_client_fw_booted(struct sof_client_dev *cdev)
{
	struct sof_audio_client_pdata *pdata = dev_get_platdata(&cdev->auxdev.dev);

	if (!pdata->component)
		return 0;

	return sof_audio_instance_restore(pdata->component);
}

/* Streams which ignored the suspend trigger are left running in the DSP */
static bool sof_audio_client_keep_dsp_in_d0(struct sof_client_dev *cdev)
{
	struct sof_audio_client_pdata *pdata = dev_get_platdata(&cdev->auxdev.dev);

	if (!pdata->component)
		return false;

	return sof_audio_instance_suspend_ignored(pdata->component);
}

static const struct sof_client_ops sof_audio_client_ops = {
	.ipc_rx_handler = sof_audio_client_ipc_rx_handler,
	.fw_booted = sof_audio_client_fw_booted,
	.keep_dsp_in_d0 = sof_audio_client_keep_dsp_in_d0,
	.get_module_name = sof_audio_client_get_module_name,
};

static const struct sof_audio_client_ipc_ops *
sof_audio_client_get_ipc_ops(struct sof_client_dev *cdev)
{
	switch (sof_client_get_ipc_type(cdev)) {
#if IS_ENABLED(CONFIG_SND_SOC_SOF_IPC3)
	case SOF_IPC_TYPE_3:
		return &sof_audio_client_ipc3_ops;
#endif
#if IS_ENABLED(CONFIG_SND_SOC_SOF_IPC4)
	case SOF_IPC_TYPE_4:
		return &sof_audio_client_ipc4_ops;
#endif
	default:
		return NULL;
	}
}

static int sof_audio_client_probe(struct auxiliary_device *auxdev,
				  const struct auxiliary_device_id *id)
{
	struct sof_client_dev *cdev = auxiliary_dev_to_sof_client_dev(auxdev);
	struct sof_audio_client_pdata *pdata = dev_get_platdata(&auxdev->dev);
	int ret;

	auxiliary_set_drvdata(auxdev, cdev);

	pdata->ipc_ops = sof_audio_client_get_ipc_ops(cdev);

	ret = sof_client_register_ops(cdev, &sof_audio_client_ops);
	if (ret < 0)
		return ret;

	snd_sof_new_platform_drv(cdev, &pdata->plat_drv);

	ret = snd_soc_register_component(&auxdev->dev, &pdata->plat_drv,
					 pdata->drv, pdata->num_drv);
	if (ret < 0) {
		sof_client_unregister_ops(cdev);
		return ret;
	}

	ret = sof_client_machine_register(cdev);
	if (ret < 0) {
		snd_soc_unregister_component(&auxdev->dev);
		sof_client_unregister_ops(cdev);
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

	sof_client_unregister_ops(cdev);

	if (pdata && pdata->debugfs_root)
		debugfs_remove_recursive(pdata->debugfs_root);

	pm_runtime_disable(&auxdev->dev);

	sof_client_machine_unregister(cdev);
	snd_soc_unregister_component(&auxdev->dev);
}

/*
 * Tear down the topology pipelines owned by this audio client.
 *
 * The audio clients are children of the SOF device, so the PM core invokes
 * their suspend callbacks before the SOF device is suspended. Only the
 * pipelines of the calling instance are freed, other audio clients of the same
 * DSP are left untouched.
 */
static int sof_audio_client_suspend(struct device *dev)
{
	struct auxiliary_device *auxdev = to_auxiliary_dev(dev);
	struct sof_client_dev *cdev = auxiliary_dev_to_sof_client_dev(auxdev);
	struct sof_audio_client_pdata *pdata = dev_get_platdata(dev);

	if (!pdata || !pdata->component)
		return 0;

	/*
	 * The pipelines need to be torn down only if the DSP hardware is
	 * active. If it is already suspended there is nothing to free up.
	 */
	if (!sof_client_is_dsp_in_d0(cdev))
		return 0;

	return sof_audio_instance_suspend(pdata->component);
}

/*
 * Set up the static topology pipelines owned by this audio client.
 *
 * The audio clients are children of the SOF device, so the PM core invokes
 * their resume callbacks after the SOF device has been resumed. If the DSP was
 * powered down the firmware boot has already restored all instances, this only
 * covers the case when the instance was suspended while the DSP remained in D0.
 */
static int sof_audio_client_resume(struct device *dev)
{
	struct auxiliary_device *auxdev = to_auxiliary_dev(dev);
	struct sof_client_dev *cdev = auxiliary_dev_to_sof_client_dev(auxdev);
	struct sof_audio_client_pdata *pdata = dev_get_platdata(dev);

	if (!pdata || !pdata->component)
		return 0;

	/*
	 * With on-demand DSP boot the firmware is not running at this point,
	 * the pipelines are set up when the DSP is booted.
	 */
	if (sof_client_get_fw_state(cdev) != SOF_FW_BOOT_COMPLETE)
		return 0;

	return sof_audio_instance_resume(pdata->component);
}

static const struct dev_pm_ops sof_audio_client_pm = {
	SYSTEM_SLEEP_PM_OPS(sof_audio_client_suspend, sof_audio_client_resume)
	RUNTIME_PM_OPS(sof_audio_client_suspend, sof_audio_client_resume, NULL)
};

static const struct auxiliary_device_id sof_audio_client_id_table[] = {
	{ .name = "snd_sof.audio", },
	{},
};
MODULE_DEVICE_TABLE(auxiliary, sof_audio_client_id_table);

static struct auxiliary_driver sof_audio_client_drv = {
	.probe = sof_audio_client_probe,
	.remove = sof_audio_client_remove,
	.driver = {
		/* auxiliary_driver_register() sets .name to be the modname */
		.pm = pm_ptr(&sof_audio_client_pm),
	},
	.id_table = sof_audio_client_id_table,
};

module_auxiliary_driver(sof_audio_client_drv);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("SOF Audio Client Driver");
MODULE_IMPORT_NS("SND_SOC_SOF_CLIENT");
