// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
//
// This file is provided under a dual BSD/GPLv2 license.  When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2018 Intel Corporation. All rights reserved.
//
// Author: Liam Girdwood <liam.r.girdwood@linux.intel.com>
//

#include "ops.h"
#include "sof-priv.h"

static int sof_send_pm_ipc(struct snd_sof_dev *sdev, int cmd)
{
	struct sof_ipc_pm_ctx pm_ctx;
	struct sof_ipc_reply reply;

	memset(&pm_ctx, 0, sizeof(pm_ctx));

	/* configure ctx save ipc message */
	pm_ctx.hdr.size = sizeof(pm_ctx);
	pm_ctx.hdr.cmd = SOF_IPC_GLB_PM_MSG | cmd;

	/* send ctx save ipc to dsp */
	return sof_ipc_tx_message(sdev->ipc, pm_ctx.hdr.cmd, &pm_ctx,
				 sizeof(pm_ctx), &reply, sizeof(reply));
}

#if IS_ENABLED(CONFIG_SND_SOC_SOF_DEBUG_ENABLE_DEBUGFS_CACHE)
static void sof_cache_debugfs(struct snd_sof_dev *sdev)
{
	struct snd_sof_dfsentry *dfse;

	list_for_each_entry(dfse, &sdev->dfsentry_list, list) {

		/* nothing to do if debugfs buffer is not IO mem */
		if (dfse->type == SOF_DFSENTRY_TYPE_BUF)
			continue;

		/* cache memory that is only accessible in D0 */
		if (dfse->access_type == SOF_DEBUGFS_ACCESS_D0_ONLY)
			memcpy_fromio(dfse->cache_buf, dfse->io_mem,
				      dfse->size);
	}
}
#endif

static int sof_resume(struct device *dev, bool runtime_resume)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(dev);
	int ret;

	/* do nothing if dsp resume callbacks are not set */
	if (!sof_ops(sdev)->resume || !sof_ops(sdev)->runtime_resume)
		return 0;

	/*
	 * if the runtime_resume flag is set, call the runtime_resume routine
	 * or else call the system resume routine
	 */
	if (runtime_resume)
		ret = snd_sof_dsp_runtime_resume(sdev);
	else
		ret = snd_sof_dsp_resume(sdev);
	if (ret < 0) {
		dev_err(sdev->dev,
			"error: failed to power up DSP after resume\n");
		return ret;
	}

	/* load the firmware */
	ret = snd_sof_load_firmware(sdev);
	if (ret < 0) {
		dev_err(sdev->dev,
			"error: failed to load DSP firmware after resume %d\n",
			ret);
		return ret;
	}

	/* boot the firmware */
	ret = snd_sof_run_firmware(sdev);
	if (ret < 0) {
		dev_err(sdev->dev,
			"error: failed to boot DSP firmware after resume %d\n",
			ret);
		return ret;
	}

	/* resume DMA trace, only need send ipc */
	ret = snd_sof_init_trace_ipc(sdev);
	if (ret < 0) {
		/* non fatal */
		dev_warn(sdev->dev,
			 "warning: failed to init trace after resume %d\n",
			 ret);
	}

	/* notify DSP of system resume */
	ret = sof_send_pm_ipc(sdev, SOF_IPC_PM_CTX_RESTORE);
	if (ret < 0)
		dev_err(sdev->dev,
			"error: ctx_restore ipc error during resume %d\n",
			ret);

	/* initialize default D0 sub-state */
	sdev->d0_substate = SOF_DSP_D0I0;

	return ret;
}

static int sof_suspend(struct device *dev, bool runtime_suspend)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(dev);
	int ret;

	/* do nothing if dsp suspend callback is not set */
	if (!sof_ops(sdev)->suspend)
		return 0;

	/* release trace */
	snd_sof_release_trace(sdev);

#if IS_ENABLED(CONFIG_SND_SOC_SOF_DEBUG_ENABLE_DEBUGFS_CACHE)
	/* cache debugfs contents during runtime suspend */
	if (runtime_suspend)
		sof_cache_debugfs(sdev);
#endif
	/* notify DSP of upcoming power down */
	ret = sof_send_pm_ipc(sdev, SOF_IPC_PM_CTX_SAVE);
	if (ret == -EBUSY || ret == -EAGAIN) {
		/*
		 * runtime PM has logic to handle -EBUSY/-EAGAIN so
		 * pass these errors up
		 */
		dev_err(sdev->dev,
			"error: ctx_save ipc error during suspend %d\n",
			ret);
		return ret;
	} else if (ret < 0) {
		/* FW in unexpected state, continue to power down */
		dev_warn(sdev->dev,
			 "ctx_save ipc error %d, proceeding with suspend\n",
			 ret);
	}

	/* power down all DSP cores */
	if (runtime_suspend)
		ret = snd_sof_dsp_runtime_suspend(sdev);
	else
		ret = snd_sof_dsp_suspend(sdev);
	if (ret < 0)
		dev_err(sdev->dev,
			"error: failed to power down DSP during suspend %d\n",
			ret);

	return ret;
}

int snd_sof_runtime_suspend(struct device *dev)
{
	return sof_suspend(dev, true);
}
EXPORT_SYMBOL(snd_sof_runtime_suspend);

int snd_sof_runtime_idle(struct device *dev)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(dev);

	return snd_sof_dsp_runtime_idle(sdev);
}
EXPORT_SYMBOL(snd_sof_runtime_idle);

int snd_sof_runtime_resume(struct device *dev)
{
	return sof_resume(dev, true);
}
EXPORT_SYMBOL(snd_sof_runtime_resume);

int snd_sof_resume(struct device *dev)
{
	return sof_resume(dev, false);
}
EXPORT_SYMBOL(snd_sof_resume);

int snd_sof_suspend(struct device *dev)
{
	return sof_suspend(dev, false);
}
EXPORT_SYMBOL(snd_sof_suspend);
