// SPDX-License-Identifier: GPL-2.0
//
// rt711-sdw.c -- rt711 ALSA SoC audio driver
//
// Copyright(c) 2019 Realtek Semiconductor Corp.
//
//

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/mod_devicetable.h>
#include <linux/soundwire/sdw.h>
#include <linux/soundwire/sdw_type.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <sound/soc.h>
#include "rt711.h"
#include "rt711-sdw.h"

static bool rt711_readable_register(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case 0x00e0:
	case 0x00f0:
	case 0x2012 ... 0x2016:
	case 0x201a ... 0x2027:
	case 0x2029 ... 0x202a:
	case 0x202d ... 0x2034:
	case 0x2201 ... 0x2204:
	case 0x2206 ... 0x2212:
	case 0x2220 ... 0x2223:
	case 0x2230 ... 0x2239:
	case 0x2f01 ... 0x2f0f:
		return true;
	default:
		return false;
	}
}

static bool rt711_volatile_register(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case 0x2016:
	case 0x201b:
	case 0x201c:
	case 0x201d:
	case 0x201f:
	case 0x2021:
	case 0x2023:
	case 0x2230:
	case 0x2012 ... 0x2015: /* HD-A read */
	case 0x202d ... 0x202f: /* BRA */
	case 0x2201 ... 0x2212: /* i2c debug */
	case 0x2220 ... 0x2223: /* decoded HD-A */
		return true;
	default:
		return false;
	}
}

const struct regmap_config rt711_sdw_regmap = {
	.reg_bits = 32, /* Total register space for SDW */
	.val_bits = 8, /* Total number of bits in register */
	.readable_reg = rt711_readable_register, /* Readable registers */
	.volatile_reg = rt711_volatile_register, /* volatile register */
	.max_register = 0xff01, /* Maximum number of register */
	.reg_defaults = rt711_reg_defaults, /* Defaults */
	.num_reg_defaults = ARRAY_SIZE(rt711_reg_defaults),
	.cache_type = REGCACHE_RBTREE,
	.use_single_read = true,
	.use_single_write = true,
};

static int rt711_update_status(struct sdw_slave *slave,
			       enum sdw_slave_status status)
{
	struct rt711_priv *rt711 = dev_get_drvdata(&slave->dev);

	/* Update the status */
	rt711->status = status;

	if (status == SDW_SLAVE_UNATTACHED)
		rt711->hw_init = false;

	/*
	 * Perform initialization only if slave status is present and
	 * hw_init flag is false
	 */
	if (rt711->hw_init || rt711->status != SDW_SLAVE_ATTACHED)
		return 0;

	/* perform I/O transfers required for Slave initialization */
	return rt711_io_init(&slave->dev, slave);
}

static int rt711_read_prop(struct sdw_slave *slave)
{
	struct sdw_slave_prop *prop = &slave->prop;
	int nval, i, num_of_ports = 1;
	u32 bit;
	unsigned long addr;
	struct sdw_dpn_prop *dpn;

	prop->paging_support = false;

	/* first we need to allocate memory for set bits in port lists */
	prop->source_ports = 0x14;	/* BITMAP: 00010100 */
	prop->sink_ports = 0x8;	/* BITMAP:  00001000 */

	nval = hweight32(prop->source_ports);
	num_of_ports += nval;
	prop->src_dpn_prop = devm_kcalloc(&slave->dev, nval,
					  sizeof(*prop->src_dpn_prop),
					  GFP_KERNEL);
	if (!prop->src_dpn_prop)
		return -ENOMEM;

	i = 0;
	dpn = prop->src_dpn_prop;
	addr = prop->source_ports;
	for_each_set_bit(bit, &addr, 32) {
		dpn[i].num = bit;
		dpn[i].type = SDW_DPN_FULL;
		dpn[i].simple_ch_prep_sm = true;
		dpn[i].ch_prep_timeout = 10;
		i++;
	}

	/* do this again for sink now */
	nval = hweight32(prop->sink_ports);
	num_of_ports += nval;
	prop->sink_dpn_prop = devm_kcalloc(&slave->dev, nval,
					   sizeof(*prop->sink_dpn_prop),
					   GFP_KERNEL);
	if (!prop->sink_dpn_prop)
		return -ENOMEM;

	i = 0;
	dpn = prop->sink_dpn_prop;
	addr = prop->sink_ports;
	for_each_set_bit(bit, &addr, 32) {
		dpn[i].num = bit;
		dpn[i].type = SDW_DPN_FULL;
		dpn[i].simple_ch_prep_sm = true;
		dpn[i].ch_prep_timeout = 10;
		i++;
	}

	/* Allocate port_ready based on num_of_ports */
	slave->port_ready = devm_kcalloc(&slave->dev, num_of_ports,
					 sizeof(*slave->port_ready),
					 GFP_KERNEL);
	if (!slave->port_ready)
		return -ENOMEM;

	/* Initialize completion */
	for (i = 0; i < num_of_ports; i++)
		init_completion(&slave->port_ready[i]);

	/* set the timeout values */
	prop->clk_stop_timeout = 20;

	return 0;
}

static int rt711_bus_config(struct sdw_slave *slave,
			    struct sdw_bus_params *params)
{
	struct rt711_priv *rt711 = dev_get_drvdata(&slave->dev);
	int ret;

	memcpy(&rt711->params, params, sizeof(*params));

	ret = rt711_clock_config(&slave->dev);
	if (ret < 0)
		dev_err(&slave->dev, "Invalid clk config");

	return ret;
}

static int rt711_interrupt_callback(struct sdw_slave *slave,
				    struct sdw_slave_intr_status *status)
{
	struct rt711_priv *rt711 = dev_get_drvdata(&slave->dev);

	dev_dbg(&slave->dev,
		"%s control_port_stat=%x", __func__, status->control_port);

	if (status->control_port & 0x4) {
		mod_delayed_work(system_power_efficient_wq,
			&rt711->jack_detect_work, msecs_to_jiffies(250));
	}

	return 0;
}

static struct sdw_slave_ops rt711_slave_ops = {
	.read_prop = rt711_read_prop,
	.interrupt_callback = rt711_interrupt_callback,
	.update_status = rt711_update_status,
	.bus_config = rt711_bus_config,
};

static int rt711_sdw_probe(struct sdw_slave *slave,
			   const struct sdw_device_id *id)
{
	struct regmap *regmap;

	/* Assign ops */
	slave->ops = &rt711_slave_ops;

	/* Regmap Initialization */
	regmap = devm_regmap_init_sdw(slave, &rt711_sdw_regmap);
	if (!regmap)
		return -EINVAL;

	rt711_init(&slave->dev, regmap, slave);

	/* Perform IO operations only if slave is in ATTACHED state */
	if (slave->status == SDW_SLAVE_ATTACHED)
		rt711_io_init(&slave->dev, slave);

	return 0;
}

static int rt711_sdw_remove(struct sdw_slave *slave)
{
	struct rt711_priv *rt711 = dev_get_drvdata(&slave->dev);

	if (rt711 && rt711->hw_init) {
		cancel_delayed_work(&rt711->jack_detect_work);
		cancel_delayed_work(&rt711->jack_btn_check_work);
	}

	return 0;
}

static const struct sdw_device_id rt711_id[] = {
	SDW_SLAVE_ENTRY(0x025d, 0x711, 0),
	{},
};
MODULE_DEVICE_TABLE(sdw, rt711_id);

static struct sdw_driver rt711_sdw_driver = {
	.driver = {
		.name = "rt711",
		.owner = THIS_MODULE,
	},
	.probe = rt711_sdw_probe,
	.remove = rt711_sdw_remove,
	.ops = &rt711_slave_ops,
	.id_table = rt711_id,
};
module_sdw_driver(rt711_sdw_driver);

MODULE_DESCRIPTION("ASoC RT711 SDW driver");
MODULE_AUTHOR("Shuming Fan <shumingf@realtek.com>");
MODULE_LICENSE("GPL");
