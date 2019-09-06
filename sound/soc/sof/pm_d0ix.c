// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
//
// This file is provided under a dual BSD/GPLv2 license.  When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2019 Intel Corporation. All rights reserved.
//
// Author: Keyon Jie <yang.jie@linux.intel.com>
//

#include <linux/device.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <sound/sof.h>
#include "ops.h"

#ifdef CONFIG_PM
static int sof_d0ix_suspend(struct device *dev)
{
	struct snd_sof_dev *sdev = dev_get_platdata(dev);

	dev_dbg(dev, "Suspending to D0i3...\n");

	return snd_sof_set_dsp_state(sdev, SOF_DSP_D0I3);
}

static int sof_d0ix_resume(struct device *dev)
{
	struct snd_sof_dev *sdev = dev_get_platdata(dev);

	dev_dbg(dev, "Resuming from D0i3...\n");

	return snd_sof_set_dsp_state(sdev, SOF_DSP_D0I0);
}
#else
#define sof_d0ix_suspend NULL
#define sof_d0ix_resume NULL
#endif

static int pm_d0ix_probe(struct platform_device *pdev)
{
	/* allow runtime_pm */
	pm_runtime_set_autosuspend_delay(&pdev->dev, SND_SOF_D0I3_DELAY_MS);
	pm_runtime_use_autosuspend(&pdev->dev);

	pm_runtime_allow(&pdev->dev);

	pm_runtime_enable(&pdev->dev);

	pm_runtime_mark_last_busy(&pdev->dev);

	pm_runtime_put_noidle(&pdev->dev);

	dev_dbg(&pdev->dev, "%s done.\n", __func__);
	return 0;
}

static int pm_d0ix_remove(struct platform_device *pdev)
{
	return 0;
}

const struct dev_pm_ops d0ix_pm_ops = {
	SET_RUNTIME_PM_OPS(sof_d0ix_suspend, sof_d0ix_resume, NULL)
};

static struct platform_driver sof_d0ix_driver = {
	.driver = {
		.name = "sof_d0ix",
		.pm = &d0ix_pm_ops,
	},

	.probe = pm_d0ix_probe,
	.remove = pm_d0ix_remove,
};

module_platform_driver(sof_d0ix_driver);

MODULE_DESCRIPTION("SOF D0Ix driver");
MODULE_AUTHOR("Keyon Jie");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_ALIAS("platform:sof_d0ix");

