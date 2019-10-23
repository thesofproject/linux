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
#include "../sof-priv.h"
#include "../sof-client.h"
#include "../sof-audio.h"
#include "../audio-ops.h"
#include "../ops.h"
#include "shim.h"

/*
 * SOF Driver enumeration.
 */
static int sof_machine_check(struct platform_device *pdev,
			     const struct sof_dev_desc *desc)
{
	struct snd_sof_client *audio_client = dev_get_platdata(&pdev->dev);
	struct sof_audio_dev *sof_audio = audio_client->client_data;

#if IS_ENABLED(CONFIG_SND_SOC_SOF_NOCODEC)
	struct snd_soc_acpi_mach *machine;
	int ret;
#endif

	if (sof_audio->machine)
		return 0;

#if !IS_ENABLED(CONFIG_SND_SOC_SOF_NOCODEC)
	dev_warn(&pdev->dev, "error: no matching ASoC machine driver found - aborting probe\n");
	return -ENODEV;
#else
	/* fallback to nocodec mode */
	dev_warn(&pdev->dev, "No ASoC machine driver found - using nocodec\n");
	machine = devm_kzalloc(&pdev->dev, sizeof(*machine), GFP_KERNEL);
	if (!machine)
		return -ENOMEM;

	machine->drv_name = "sof-nocodec";
	sof_audio->machine = machine;

	ret = sof_nocodec_setup(&pdev->dev, sof_audio, desc);
	if (ret < 0)
		return ret;

	machine->mach_params.platform = dev_name(&pdev->dev);

	return 0;
#endif
}

static int sof_audio_select_machine(struct platform_device *pdev,
				    const struct sof_dev_desc *desc)
{
	struct sof_audio_dev *sof_audio = sof_get_client_data(&pdev->dev);
	struct snd_soc_acpi_mach *mach;
	int ret;

#if IS_ENABLED(CONFIG_SND_SOC_SOF_FORCE_NOCODEC_MODE)
	/* force nocodec mode */
	dev_warn(&pdev->dev, "Force to use nocodec mode\n");
	mach = devm_kzalloc(&pdev->dev, sizeof(*mach), GFP_KERNEL);
	if (!mach)
		return -ENOMEM;

	sof_audio->machine = mach;

	mach->drv_name = "sof-nocodec";

	ret = sof_nocodec_setup(&pdev->dev, sof_audio, desc);
	if (ret < 0)
		return ret;
	mach->mach_params.platform = dev_name(&pdev->dev);
#else
	/* find machine */
	mach = snd_soc_acpi_find_machine(desc->machines);
	if (!mach) {
		dev_warn(&pdev->dev, "warning: No matching ASoC machine driver found\n");
	} else {
		mach->mach_params.platform = dev_name(&pdev->dev);
		sof_audio->tplg_filename = mach->sof_tplg_filename;
		sof_audio->machine = mach;
	}
#endif /* CONFIG_SND_SOC_SOF_FORCE_NOCODEC_MODE */

	sof_audio->tplg_filename_prefix = desc->default_tplg_path;

	/* check machine info */
	ret = sof_machine_check(pdev, desc);
	if (ret < 0)
		dev_warn(&pdev->dev, "warning: failed to get machine info %d\n",
			 ret);

	return ret;
}

static int sof_audio_probe(struct platform_device *pdev)
{
	struct snd_sof_client *audio_client = dev_get_platdata(&pdev->dev);
	struct snd_sof_dev *sdev = dev_get_drvdata(pdev->dev.parent);
	struct snd_sof_pdata *plat_data = sdev->pdata;
	const struct sof_dev_desc *desc = plat_data->desc;
	struct sof_audio_dev *sof_audio;
	struct ipc_rx_client *audio_rx;
	const char *drv_name;
	const void *machine;
	int num_drv;
	int size;
	int ret;

	/* set IPC RX and TX reply callbacks */
	audio_client->sof_client_rx_cb = sof_audio_rx_message;
	audio_client->sof_ipc_reply_cb = NULL;

	/* create SOF audio device */
	sof_audio = devm_kzalloc(&pdev->dev, sizeof(*sof_audio), GFP_KERNEL);
	if (!sof_audio)
		return -ENOMEM;

	INIT_LIST_HEAD(&sof_audio->pcm_list);
	INIT_LIST_HEAD(&sof_audio->kcontrol_list);
	INIT_LIST_HEAD(&sof_audio->widget_list);
	INIT_LIST_HEAD(&sof_audio->dai_list);
	INIT_LIST_HEAD(&sof_audio->route_list);

	sof_audio->audio_ops = desc->audio_ops;
	sof_audio->platform = dev_name(&pdev->dev);

	/* check for mandatory audio ops */
	if (!sof_audio || !sof_audio->audio_ops->ipc_pcm_params)
		return -EINVAL;

	audio_client->client_data = sof_audio;

	/* select machine driver */
	ret = sof_audio_select_machine(pdev, desc);
	if (ret < 0)
		return ret;

	/* set up platform component driver */
	snd_sof_new_platform_drv(sof_audio);

	/* number of dai drv to register */
	num_drv = snd_sof_get_dai_drv_count(sof_audio->audio_ops,
					    SOF_DAI_INTEL_SSP);

	/* now register audio DSP platform driver and dai */
	ret = devm_snd_soc_register_component(&pdev->dev, &sof_audio->plat_drv,
					      sof_audio->audio_ops->drv,
					      num_drv);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"error: failed to register DSP DAI driver %d\n", ret);
		return ret;
	}

	machine = (const void *)sof_audio->machine;
	drv_name = sof_audio->machine->drv_name;
	size = sizeof(*sof_audio->machine);

	/* register machine driver, pass machine info as pdata */
	sof_audio->pdev_mach =
		platform_device_register_data(&pdev->dev, drv_name,
					      PLATFORM_DEVID_NONE,
					      machine, size);

	if (IS_ERR(sof_audio->pdev_mach)) {
		ret = PTR_ERR(sof_audio->pdev_mach);
		return ret;
	}

	dev_dbg(&pdev->dev, "created machine %s\n",
		dev_name(&sof_audio->pdev_mach->dev));

	/* register for stream message rx */
	audio_rx = devm_kzalloc(&pdev->dev, sizeof(*audio_rx), GFP_KERNEL);
	if (!audio_rx)
		return -ENOMEM;
	audio_rx->ipc_cmd = SOF_IPC_GLB_STREAM_MSG;
	audio_rx->dev = &pdev->dev;
	snd_sof_ipc_rx_register(sdev, audio_rx);

	/* enable runtime PM */
	pm_runtime_set_autosuspend_delay(&pdev->dev, SND_SOF_SUSPEND_DELAY_MS);
	pm_runtime_use_autosuspend(&pdev->dev);
	pm_runtime_set_active(&pdev->dev);
	pm_runtime_enable(&pdev->dev);
	pm_runtime_mark_last_busy(&pdev->dev);
	pm_runtime_put_noidle(&pdev->dev);

	return 0;
}

const struct dev_pm_ops sof_audio_pm = {
	SET_SYSTEM_SLEEP_PM_OPS(sof_audio_suspend, sof_audio_resume)
	SET_RUNTIME_PM_OPS(sof_audio_runtime_suspend, sof_audio_resume, NULL)
};

static int sof_audio_remove(struct platform_device *pdev)
{
	struct sof_audio_dev *sof_audio = sof_get_client_data(&pdev->dev);

	pm_runtime_disable(&pdev->dev);

	if (!IS_ERR_OR_NULL(sof_audio->pdev_mach))
		platform_device_unregister(sof_audio->pdev_mach);

	return 0;
}

static struct platform_driver sof_audio_driver = {
	.driver = {
		.name = "sof-ssp-audio",
		.pm = &sof_audio_pm,
	},

	.probe = sof_audio_probe,
	.remove = sof_audio_remove,
};

module_platform_driver(sof_audio_driver);

MODULE_DESCRIPTION("SOF Audio Client Platform Driver");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_ALIAS("platform:sof-ssp-audio");
