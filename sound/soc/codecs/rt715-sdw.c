// SPDX-License-Identifier: GPL-2.0
/*
 * rt715-sdw.c -- rt715 ALSA SoC audio driver
 *
 * Copyright(c) 2019 Realtek Semiconductor Corp.
 *
 * ALC715 ASoC Codec Driver based Intel Dummy SdW codec driver
 *
 */
#define DEBUG

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/mod_devicetable.h>
#include <linux/soundwire/sdw.h>
#include <linux/soundwire/sdw_type.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <sound/soc.h>
#include "rt715.h"
#include "rt715-sdw.h"

static bool rt715_readable_register(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case 0x0020:
	case 0x0030:
	case 0x0060:
	case 0x0070:
	case 0x0080:
	case 0x02e0:
	case 0x02f0:
	case 0x04e0:
	case 0x04f0:
	case 0x06e0:
	case 0x06f0:
	case 0x0000 ... 0x0005:
	case 0x0022 ... 0x0026:
	case 0x0032 ... 0x0036:
	case 0x0040 ... 0x0046:
	case 0x0050 ... 0x0055:
	case 0x00e0 ... 0x00e5:
	case 0x00ee ... 0x00ef:
	case 0x00f0 ... 0x00f5:
	case 0x00fe ... 0x00ff:
	case 0x0200 ... 0x0205:
	case 0x0220 ... 0x0227:
	case 0x0230 ... 0x0237:
	case 0x0400 ... 0x0405:
	case 0x0420 ... 0x0427:
	case 0x0430 ... 0x0437:
	case 0x0600 ... 0x0605:
	case 0x0620 ... 0x0627:
	case 0x0630 ... 0x0637:
	case 0x0f00 ... 0x0f27:
	case 0x0f30 ... 0x0f37:
	case 0x2000 ... 0x2027:
	case 0x2029 ... 0x202a:
	case 0x202d ... 0x2034:
	case 0x2200 ... 0x2204:
	case 0x2206 ... 0x2212:
	case 0x2220 ... 0x2223:
	case 0x2230 ... 0x2239:
	case 0x22f0 ... 0x22f3:
		return true;
	default:
		return false;
	}
}

static bool rt715_volatile_register(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case 0x0000:
	case 0x0001:
	case 0x0004:
	case 0x0005:
	case 0x0040:
	case 0x0042:
	case 0x0043:
	case 0x0044:
	case 0x0045:
	case 0x00e5:
	case 0x00f0:
	case 0x00f3:
	case 0x00f5:
	case 0x0202:
	case 0x0204:
	case 0x0205:
	case 0x0402:
	case 0x0404:
	case 0x0405:
	case 0x0602:
	case 0x0604:
	case 0x0605:
	case 0x0f04:
	case 0x0f05:
	case 0x2009:
	case 0x2016:
	case 0x201b:
	case 0x201c:
	case 0x201d:
	case 0x201f:
	case 0x2023:
	case 0x2230:
	case 0x0050 ... 0x0055: /* device id */
	case 0x200b ... 0x200e: /* i2c read */
	case 0x2012 ... 0x2015: /* HD-A read */
	case 0x202d ... 0x202f: /* BRA */
	case 0x2201 ... 0x2212: /* i2c debug */
	case 0x2220 ... 0x2223: /* decoded HD-A */
		return true;
	case 0x3000 ... 0xffff: /* HD-A command */
		return false; /* should always read from cache */
	default:
		return true; /*for debug*/
	}
}

const struct regmap_config rt715_sdw_regmap = {
	.reg_bits = 32, /* Total register space for SDW */
	.val_bits = 8, /* Total number of bits in register */

	.readable_reg = rt715_readable_register, /* Readable registers */
	.volatile_reg = rt715_volatile_register, /* volatile register */

	.max_register = 0xffff, /* Maximum number of register */
	.reg_defaults = rt715_reg_defaults, /* Defaults */
	.num_reg_defaults = ARRAY_SIZE(rt715_reg_defaults),
	.cache_type = REGCACHE_RBTREE,
};

int hda_to_sdw(unsigned int nid, unsigned int verb, unsigned int payload,
	       unsigned int *sdw_addr_h, unsigned int *sdw_data_h,
	       unsigned int *sdw_addr_l, unsigned int *sdw_data_l)
{
	unsigned int offset_h, offset_l, e_verb;

	if (((verb & 0xff) != 0) || verb == 0xf00) { /* 12 bits command */
		if (verb == 0x7ff) /* special case */
			offset_h = 0;
		else
			offset_h = 0x3000;

		if (verb & 0x800) /* get command */
			e_verb = (verb - 0xf00) | 0x80;
		else /* set command */
			e_verb = (verb - 0x700);

		*sdw_data_h = payload; /* 7 bits payload */
		*sdw_addr_l = *sdw_data_l = 0;
	} else { /* 4 bits command */
		if ((verb & 0x800) == 0x800) { /* read */
			offset_h = 0x9000;
			offset_l = 0xa000;
		} else { /* write */
			offset_h = 0x7000;
			offset_l = 0x8000;
		}
		e_verb = verb >> 8;
		*sdw_data_h = (payload >> 8); /* 16 bits payload [15:8] */
		*sdw_addr_l = (e_verb << 8) | nid | 0x80; /* 0x80: valid bit */
		*sdw_addr_l += offset_l;
		*sdw_data_l = payload & 0xff;
	}

	*sdw_addr_h = (e_verb << 8) | nid;
	*sdw_addr_h += offset_h;

	return 0;
}
EXPORT_SYMBOL(hda_to_sdw);

static int rt715_update_status(struct sdw_slave *slave,
			       enum sdw_slave_status status)
{
	struct rt715_priv *rt715 = dev_get_drvdata(&slave->dev);

	/* Update the status */
	rt715->status = status;
	/*
	 * Perform initialization only if slave status is present and
	 * hw_init flag is false
	 */
	if (rt715->hw_init || rt715->status != SDW_SLAVE_ATTACHED)
		return 0;

	/* perform I/O transfers required for Slave initialization */
	return rt715_io_init(&slave->dev, slave);
}

static int rt715_read_prop(struct sdw_slave *slave)
{
	struct sdw_slave_prop *prop = &slave->prop;
	int nval, i, num_of_ports = 1;
	u32 bit;
	unsigned long addr;
	struct sdw_dpn_prop *dpn;

	prop->paging_support = false;

	/* first we need to allocate memory for set bits in port lists */
	prop->source_ports = 0x50;	/* BITMAP: 01010000 */
	prop->sink_ports = 0x0;	/* BITMAP:  00000000 */

	nval = hweight32(prop->source_ports);
	num_of_ports += nval;
	prop->src_dpn_prop = devm_kcalloc(&slave->dev, nval,
					  sizeof(*prop->src_dpn_prop),
					  GFP_KERNEL);
	if (!prop->src_dpn_prop)
		return -ENOMEM;

	dpn = prop->src_dpn_prop;
	i = 0;
	addr = prop->source_ports;
	for_each_set_bit(bit, &addr, 32) {
		dpn[i].num = bit;
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

	dpn = prop->sink_dpn_prop;
	i = 0;
	addr = prop->sink_ports;
	for_each_set_bit(bit, &addr, 32) {
		dpn[i].num = bit;
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

static int rt715_bus_config(struct sdw_slave *slave,
			    struct sdw_bus_params *params)
{
	struct rt715_priv *rt715 = dev_get_drvdata(&slave->dev);
	int ret;

	memcpy(&rt715->params, params, sizeof(*params));

	ret = rt715_clock_config(&slave->dev);
	if (ret < 0)
		dev_err(&slave->dev, "Invalid clk config");

	return 0;
}

static struct sdw_slave_ops rt715_slave_ops = {
	.read_prop = rt715_read_prop,
	.update_status = rt715_update_status,
	.bus_config = rt715_bus_config,
	.port_prep = NULL,
};

static int rt715_sdw_probe(struct sdw_slave *slave,
			   const struct sdw_device_id *id)
{
	struct regmap *regmap;
	int ret = 0;

	/* Assign ops */
	slave->ops = &rt715_slave_ops;

	/* Regmap Initialization */
	regmap = devm_regmap_init_sdw(slave, &rt715_sdw_regmap);
	if (!regmap)
		return -EINVAL;

	rt715_init(&slave->dev, regmap, slave);

	/* Perform IO operations only if slave is in ATTACHED state */
	if (slave->status == SDW_SLAVE_ATTACHED)
		rt715_io_init(&slave->dev, slave);

	return ret;
}

static int rt715_sdw_remove(struct sdw_slave *slave)
{
	return rt715_remove(&slave->dev);
}

static const struct sdw_device_id rt715_id[] = {
	SDW_SLAVE_ENTRY(0x025d, 0x715, 0),
	{},
};
MODULE_DEVICE_TABLE(sdw, rt715_id);

static struct sdw_driver rt715_sdw_driver = {
	.driver = {
		   .name = "rt715",
		   .owner = THIS_MODULE,
		   },
	.probe = rt715_sdw_probe,
	.remove = rt715_sdw_remove,
	.ops = &rt715_slave_ops,
	.id_table = rt715_id,
};
module_sdw_driver(rt715_sdw_driver);

MODULE_DESCRIPTION("ASoC RT715 driver SDW");
MODULE_AUTHOR("Jack Yu <jack.yu@realtek.com>");
MODULE_LICENSE("GPL v2");
