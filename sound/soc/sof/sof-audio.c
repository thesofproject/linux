// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
//
// Copyright(c) 2019 Intel Corporation. All rights reserved.
//
// Author: Ranjani Sridharan <ranjani.sridharan@linux.intel.com>
//

#include <linux/device.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <sound/sof.h>
#include "sof-priv.h"
#include "ops.h"

/*
 * SOF Driver enumeration.
 */
static int sof_machine_check(struct snd_sof_dev *sdev)
{
	struct snd_sof_pdata *plat_data = sdev->pdata;
#if IS_ENABLED(CONFIG_SND_SOC_SOF_NOCODEC)
	struct snd_soc_acpi_mach *machine;
	int ret;
#endif

	if (plat_data->machine)
		return 0;

#if !IS_ENABLED(CONFIG_SND_SOC_SOF_NOCODEC)
	dev_err(sdev->dev, "error: no matching ASoC machine driver found - aborting probe\n");
	return -ENODEV;
#else
	/* fallback to nocodec mode */
	dev_warn(sdev->dev, "No ASoC machine driver found - using nocodec\n");
	machine = devm_kzalloc(sdev->dev, sizeof(*machine), GFP_KERNEL);
	if (!machine)
		return -ENOMEM;

	ret = sof_nocodec_setup(sdev->dev, plat_data, machine,
				plat_data->desc, plat_data->desc->ops);
	if (ret < 0)
		return ret;

	plat_data->machine = machine;

	return 0;
#endif
}

static int sof_audio_probe(struct platform_device *pdev)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(pdev->dev.parent);
	struct snd_sof_pdata *plat_data = sdev->pdata;
	struct snd_soc_acpi_mach *machine =
		(struct snd_soc_acpi_mach *)plat_data->machine;
	const char *drv_name;
	int size;
	int ret;

	/* check machine info */
	ret = sof_machine_check(sdev);
	if (ret < 0) {
		dev_err(&pdev->dev, "error: failed to get machine info %d\n",
			ret);
		goto err;
	}

	/* set platform name */
	machine->mach_params.platform = dev_name(&pdev->dev);
	plat_data->platform = dev_name(&pdev->dev);

	/* set up platform component driver */
	snd_sof_new_platform_drv(sdev);

	/* now register audio DSP platform driver and dai */
	ret = devm_snd_soc_register_component(&pdev->dev, &sdev->plat_drv,
					      sof_ops(sdev)->drv,
					      sof_ops(sdev)->num_drv);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"error: failed to register DSP DAI driver %d\n", ret);
		goto err;
	}

	drv_name = plat_data->machine->drv_name;
	size = sizeof(*plat_data->machine);

	/* register machine driver, pass machine info as pdata */
	plat_data->pdev_mach =
		platform_device_register_data(&pdev->dev, drv_name,
					      PLATFORM_DEVID_NONE,
					      (const void *)machine, size);

	if (IS_ERR(plat_data->pdev_mach)) {
		ret = PTR_ERR(plat_data->pdev_mach);
		goto err;
	}

	dev_dbg(&pdev->dev, "created machine %s\n",
		dev_name(&plat_data->pdev_mach->dev));

	/* enable runtime PM */
	pm_runtime_set_autosuspend_delay(&pdev->dev, SND_SOF_SUSPEND_DELAY_MS);
	pm_runtime_use_autosuspend(&pdev->dev);
	pm_runtime_enable(&pdev->dev);
	pm_runtime_mark_last_busy(&pdev->dev);
	pm_runtime_put_noidle(&pdev->dev);

	return 0;

err:
	snd_sof_remove(sdev);
	snd_sof_fw_unload(sdev);
	snd_sof_ipc_free(sdev);
	snd_sof_free_debug(sdev);
	return ret;
}

static int sof_audio_remove(struct platform_device *pdev)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(pdev->dev.parent);
	struct snd_sof_pdata *pdata = sdev->pdata;

	pm_runtime_disable(&pdev->dev);

	if (!IS_ERR_OR_NULL(pdata->pdev_mach))
		platform_device_unregister(pdata->pdev_mach);

	return 0;
}

static struct platform_driver sof_audio_driver = {
	.driver = {
		.name = "sof-audio",
	},

	.probe = sof_audio_probe,
	.remove = sof_audio_remove,
};

module_platform_driver(sof_audio_driver);

MODULE_DESCRIPTION("SOF Audio Client Platform Driver");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_ALIAS("platform:sof-audio");
