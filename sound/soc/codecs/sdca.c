// SPDX-License-Identifier: GPL-2.0
//
// sdca.c -- SoundWire Device Class support
//
// Copyright(c) 2020 Intel Corporation
//
//
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/pm_runtime.h>
#include <linux/mod_devicetable.h>
#include <linux/soundwire/sdw.h>
#include <linux/soundwire/sdw_type.h>
#include <linux/soundwire/sdw_registers.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/initval.h>

#include "sdca.h"

static int sdca_io_init(struct device *dev, struct sdw_slave *slave)
{
	struct sdca_priv *sdca = dev_get_drvdata(dev);
	int ret = 0;

	if (sdca->hw_init)
		return 0;

	if (sdca->first_hw_init) {
		/* TODO: support regmap bypass */
	}

	/*
	 * PM runtime is only enabled when a Slave reports as Attached
	 */
	if (!sdca->first_hw_init) {
		/* set autosuspend parameters */
		pm_runtime_set_autosuspend_delay(&slave->dev, 3000);
		pm_runtime_use_autosuspend(&slave->dev);

		/* update count of parent 'active' children */
		pm_runtime_set_active(&slave->dev);

		/* make sure the device does not suspend immediately */
		pm_runtime_mark_last_busy(&slave->dev);

		pm_runtime_enable(&slave->dev);
	}

	pm_runtime_get_noresume(&slave->dev);

	/* TODO: initialization and regmap setup */

	if (sdca->first_hw_init) {
		/* TODO: regmap sync */
	} else {
		sdca->first_hw_init = true;
	}

	/* Mark Slave initialization complete */
	sdca->hw_init = true;

	pm_runtime_mark_last_busy(&slave->dev);
	pm_runtime_put_autosuspend(&slave->dev);

	dev_dbg(&slave->dev, "%s hw_init complete\n", __func__);

	return ret;
}

static int sdca_update_status(struct sdw_slave *slave,
			      enum sdw_slave_status status)
{
	struct  sdca_priv *sdca = dev_get_drvdata(&slave->dev);

	/* Update the status */
	sdca->status = status;

	if (status == SDW_SLAVE_UNATTACHED)
		sdca->hw_init = false;

	/*
	 * Perform initialization only if slave status is present and
	 * hw_init flag is false
	 */
	if (sdca->hw_init || sdca->status != SDW_SLAVE_ATTACHED)
		return 0;

	/* perform I/O transfers required for Slave initialization */
	return sdca_io_init(&slave->dev, slave);
}

static int sdca_bus_config(struct sdw_slave *slave,
			   struct sdw_bus_params *params)
{
	int ret = 0;

	/* TODO: do bus config */

	return ret;
}

static int sdca_interrupt_callback(struct sdw_slave *slave,
				   struct sdw_slave_intr_status *status)
{
	dev_dbg(&slave->dev,
		"%s control_port_stat=%x", __func__, status->control_port);

	return 0;
}

/*
 * slave_ops: callbacks for read_prop, get_clock_stop_mode, clock_stop and
 * port_prep are not defined for now
 */
static struct sdw_slave_ops sdca_slave_ops = {
	.interrupt_callback = sdca_interrupt_callback,
	.update_status = sdca_update_status,
	.bus_config = sdca_bus_config,
};

static int sdca_sdw_init(struct device *dev, struct regmap *regmap,
			 struct sdw_slave *slave)
{
	struct sdca_priv *sdca;

	sdca = devm_kzalloc(dev, sizeof(*sdca), GFP_KERNEL);
	if (!sdca)
		return -ENOMEM;

	dev_set_drvdata(dev, sdca);
	sdca->sdw_slave = slave;

	/*
	 * Mark hw_init to false
	 * HW init will be performed when device reports present
	 */
	sdca->hw_init = false;
	sdca->first_hw_init = false;

	/* TODO: add devm_snd_soc_register_component */

	dev_dbg(&slave->dev, "%s\n", __func__);

	return 0;
}

static int sdca_sdw_probe(struct sdw_slave *slave,
			  const struct sdw_device_id *id)
{

	/* Assign ops */
	slave->ops = &sdca_slave_ops;

	/* TODO: add regmap init */

	sdca_sdw_init(&slave->dev, NULL, slave);

	return 0;
}

static const struct sdw_device_id sdca_id[] = {
	SDW_SLAVE_CLASS_ENTRY(0x1, 0),
	{},
};
MODULE_DEVICE_TABLE(sdw, sdca_id);

static int __maybe_unused sdca_dev_suspend(struct device *dev)
{
	struct sdca_priv *sdca = dev_get_drvdata(dev);

	if (!sdca->hw_init)
		return 0;

	/* TODO: add regmap support */

	return 0;
}

#define SDCA_PROBE_TIMEOUT 2000

static int __maybe_unused sdca_dev_resume(struct device *dev)
{
	struct sdw_slave *slave = dev_to_sdw_dev(dev);
	struct sdca_priv *sdca = dev_get_drvdata(dev);
	unsigned long time;

	if (!sdca->hw_init)
		return 0;

	if (!slave->unattach_request)
		goto regmap_sync;

	time = wait_for_completion_timeout(&slave->initialization_complete,
					   msecs_to_jiffies(SDCA_PROBE_TIMEOUT));
	if (!time) {
		dev_err(&slave->dev, "Initialization not complete, timed out\n");
		return -ETIMEDOUT;
	}

regmap_sync:
	slave->unattach_request = 0;
	/* TODO: regmap support */

	return 0;
}

static const struct dev_pm_ops sdca_pm = {
	SET_SYSTEM_SLEEP_PM_OPS(sdca_dev_suspend, sdca_dev_resume)
	SET_RUNTIME_PM_OPS(sdca_dev_suspend, sdca_dev_resume, NULL)
};

static struct sdw_driver sdw_sdca_driver = {
	.driver = {
		.name = "sdw-sdca",
		.owner = THIS_MODULE,
		.pm = &sdca_pm,
	},
	.probe = sdca_sdw_probe,
	.ops = &sdca_slave_ops,
	.id_table = sdca_id,
};
module_sdw_driver(sdw_sdca_driver);

MODULE_DESCRIPTION("ASoC SDCA driver");
MODULE_AUTHOR("Pierre-Louis Bossart <pierre-louis.bossart@linux.intel.com>");
MODULE_LICENSE("GPL v2");
