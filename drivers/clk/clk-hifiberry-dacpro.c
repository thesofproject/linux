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
#include <linux/gpio/consumer.h>
#include <linux/gpio/machine.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/platform_device.h>

/* Clock rate of CLK44EN attached to GPIO6 pin */
#define CLK_44EN_RATE 22579200UL
/* Clock rate of CLK48EN attached to GPIO3 pin */
#define CLK_48EN_RATE 24576000UL

static struct gpiod_lookup_table pcm512x_gpios_table = {
	/* .dev_id set during probe */
	.table = {
		GPIO_LOOKUP("pcm512x-gpio", 2, "PCM512x-GPIO3", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("pcm512x-gpio", 5, "PCM512x-GPIO6", GPIO_ACTIVE_HIGH),
		{ },
	},
};

/**
 * struct clk_hifiberry_hw - Common struct to the HiFiBerry DAC Pro
 * @dev: device
 * @hw: clk_hw for the common clk framework
 * @mode: 0 => CLK44EN, 1 => CLK48EN
 * @sclk_lookup: handle for "sclk"
 * @gpio_44: gpiod desc for 44.1kHz support
 * @gpio_48: gpiod desc for 48 kHz support
 * @prepared: boolean caching clock state
 * @gpio_initialized: boolean flag used to take gpio references.
 */
struct clk_hifiberry_hw {
	struct device *dev;
	struct clk_hw hw;
	u8 mode;
	struct clk_lookup *sclk_lookup;
	struct gpio_desc *gpio_44;
	struct gpio_desc *gpio_48;
	bool prepared;
	bool gpio_initialized;
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

static int clk_hifiberry_dacpro_is_prepared(struct clk_hw *hw)
{
	struct clk_hifiberry_hw *clk = to_hifiberry_clk(hw);

	return clk->prepared;
}

static int clk_hifiberry_dacpro_prepare(struct clk_hw *hw)
{
	struct clk_hifiberry_hw *clk = to_hifiberry_clk(hw);

	/*
	 * The gpios are handled here to avoid any dependencies on
	 * probe.
	 *
	 * The user of the clock should verify with the PCM512
	 * registers that the clock are actually present and stable.
	 * This driver only toggles the relevant GPIOs.
	 */
	if (!clk->gpio_initialized) {

		clk->gpio_44 = devm_gpiod_get(clk->dev,
					      "PCM512x-GPIO6",
					      GPIOD_OUT_LOW);
		if (IS_ERR(clk->gpio_44)) {
			dev_err(clk->dev, "gpio44 not found\n");
			return PTR_ERR(clk->gpio_44);
		}

		clk->gpio_48 = devm_gpiod_get(clk->dev,
					      "PCM512x-GPIO3",
					      GPIOD_OUT_LOW);
		if (IS_ERR(clk->gpio_48)) {
			dev_err(clk->dev, "gpio48 not found\n");
			return PTR_ERR(clk->gpio_48);
		}

		clk->gpio_initialized = true;
	}

	if (clk->prepared)
		return 0;

	switch (clk->mode) {
	case 0:
		/* 44.1 kHz */
		gpiod_set_value_cansleep(clk->gpio_44, 1);
		break;
	case 1:
		/* 48 kHz */
		gpiod_set_value_cansleep(clk->gpio_48, 1);
		break;
	default:
		return -EINVAL;
	}

	clk->prepared = 1;

	return 0;
}

static void clk_hifiberry_dacpro_unprepare(struct clk_hw *hw)
{
	struct clk_hifiberry_hw *clk = to_hifiberry_clk(hw);

	if (!clk->prepared)
		return;

	switch (clk->mode) {
	case 0:
		gpiod_set_value_cansleep(clk->gpio_44, 0);
		break;
	case 1:
		gpiod_set_value_cansleep(clk->gpio_48, 0);
		break;
	default:
		return;
	}

	clk->prepared = false;
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
	.is_prepared = clk_hifiberry_dacpro_is_prepared,
	.prepare = clk_hifiberry_dacpro_prepare,
	.unprepare = clk_hifiberry_dacpro_unprepare,
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

	pcm512x_gpios_table.dev_id = dev_name(dev);
	gpiod_add_lookup_table(&pcm512x_gpios_table);

	proclk = devm_kzalloc(dev, sizeof(*proclk), GFP_KERNEL);
	if (!proclk)
		return -ENOMEM;

	init.name = "clk-hifiberry-dacpro";
	init.ops = &clk_hifiberry_dacpro_rate_ops;
	init.flags = 0;
	init.parent_names = NULL;
	init.num_parents = 0;

	proclk->dev = dev;
	proclk->mode = 0;
	proclk->hw.init = &init;

	ret = devm_clk_hw_register(dev, &proclk->hw);
	if (ret) {
		dev_err(dev, "Fail to register clock driver\n");
		return ret;
	}

#ifndef CONFIG_ACPI
	ret = of_clk_add_hw_provider(dev->of_node, of_clk_hw_simple_get,
				     &proclk->hw);
#else
	ret = devm_clk_hw_register_clkdev(dev, &proclk->hw,
					  init.name, NULL);
#endif
	if (ret) {
		dev_err(dev, "Fail to add clock driver\n");
		return ret;
	}

	proclk->sclk_lookup = clkdev_hw_create(&proclk->hw, "sclk", NULL);
	if (!proclk->sclk_lookup) {
#ifndef CONFIG_ACPI
		of_clk_del_provider(dev->of_node);
#endif
		return -ENOMEM;
	}

	platform_set_drvdata(pdev, proclk);

	return ret;
}

static int clk_hifiberry_dacpro_remove(struct platform_device *pdev)
{
	struct clk_hifiberry_hw *proclk = platform_get_drvdata(pdev);

	clkdev_drop(proclk->sclk_lookup);

#ifndef CONFIG_ACPI
	of_clk_del_provider(pdev->dev.of_node);
#endif

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
