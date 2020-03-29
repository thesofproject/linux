// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2015 Stuart MacLean
 * Copyright (c) 2020 Intel Corporation
 *
 * Clock Driver for HiFiBerry DAC Pro
 *
 */

#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/platform_device.h>

/* Clock rate of CLK44EN attached to GPIO6 pin */
#define CLK_44EN_RATE 22579200UL
/* Clock rate of CLK48EN attached to GPIO3 pin */
#define CLK_48EN_RATE 24576000UL

/**
 * struct clk_hifiberry_hw - Common struct to the HiFiBerry DAC Pro
 * @hw: clk_hw for the common clk framework
 * @mode: 0 => CLK44EN, 1 => CLK48EN
 */
struct clk_hifiberry_hw {
	struct clk_hw hw;
	u8 mode;
};

#define to_hifiberry_clk(_hw) container_of(_hw, struct clk_hifiberry_hw, hw)

static const struct of_device_id clk_hifiberry_dacpro_dt_ids[] = {
	{ .compatible = "hifiberry,dacpro-clk",},
	{ }
};
MODULE_DEVICE_TABLE(of, clk_hifiberry_dacpro_dt_ids);

static unsigned long clk_hifiberry_dacpro_recalc_rate(struct clk_hw *hw,
						      unsigned long parent_rate)
{
	return (to_hifiberry_clk(hw)->mode == 0) ? CLK_44EN_RATE :
		CLK_48EN_RATE;
}

static long clk_hifiberry_dacpro_round_rate(struct clk_hw *hw,
					    unsigned long rate,
					    unsigned long *parent_rate)
{
	long actual_rate;

	if (rate <= CLK_44EN_RATE) {
		actual_rate = (long)CLK_44EN_RATE;
	} else if (rate >= CLK_48EN_RATE) {
		actual_rate = (long)CLK_48EN_RATE;
	} else {
		long diff44Rate = (long)(rate - CLK_44EN_RATE);
		long diff48Rate = (long)(CLK_48EN_RATE - rate);

		if (diff44Rate < diff48Rate)
			actual_rate = (long)CLK_44EN_RATE;
		else
			actual_rate = (long)CLK_48EN_RATE;
	}
	return actual_rate;
}

static int clk_hifiberry_dacpro_set_rate(struct clk_hw *hw,
					 unsigned long rate,
					 unsigned long parent_rate)
{
	struct clk_hifiberry_hw *clk = to_hifiberry_clk(hw);
	unsigned long actual_rate;

	actual_rate = (unsigned long)clk_hifiberry_dacpro_round_rate(hw, rate,
								&parent_rate);
	clk->mode = (actual_rate == CLK_44EN_RATE) ? 0 : 1;
	return 0;
}

static const struct clk_ops clk_hifiberry_dacpro_rate_ops = {
	.recalc_rate = clk_hifiberry_dacpro_recalc_rate,
	.round_rate = clk_hifiberry_dacpro_round_rate,
	.set_rate = clk_hifiberry_dacpro_set_rate,
};

static int clk_hifiberry_dacpro_probe(struct platform_device *pdev)
{
	struct clk_hifiberry_hw *proclk;
	struct clk_init_data init;
	struct device *dev;
	int ret;

	dev = &pdev->dev;

	proclk = devm_kzalloc(dev, sizeof(*proclk), GFP_KERNEL);
	if (!proclk)
		return -ENOMEM;

	init.name = "clk-hifiberry-dacpro";
	init.ops = &clk_hifiberry_dacpro_rate_ops;
	init.flags = 0;
	init.parent_names = NULL;
	init.num_parents = 0;

	proclk->mode = 0;
	proclk->hw.init = &init;

	ret = devm_clk_hw_register(dev, &proclk->hw);
	if (ret) {
		dev_err(dev, "Fail to register clock driver\n");
		return ret;
	}

	ret = of_clk_add_hw_provider(dev->of_node, of_clk_hw_simple_get,
				     &proclk->hw);

	return ret;
}

static int clk_hifiberry_dacpro_remove(struct platform_device *pdev)
{
	of_clk_del_provider(pdev->dev.of_node);
	return 0;
}

static struct platform_driver clk_hifiberry_dacpro_driver = {
	.probe = clk_hifiberry_dacpro_probe,
	.remove = clk_hifiberry_dacpro_remove,
	.driver = {
		.name = "clk-hifiberry-dacpro",
		.of_match_table = clk_hifiberry_dacpro_dt_ids,
	},
};
module_platform_driver(clk_hifiberry_dacpro_driver);

MODULE_DESCRIPTION("HiFiBerry DAC Pro clock driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:clk-hifiberry-dacpro");
