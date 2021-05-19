// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
//
// This file is provided under a dual BSD/GPLv2 license.  When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2021 Intel Corporation. All rights reserved.
//
// Author: Brent Lu <brent.lu@intel.com>
//
// DAI clock layer to support explicit clock control in machine driver.
//

#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include <sound/sof/dai.h>
#include "sof-priv.h"
#include "sof-dai-clk.h"

struct sof_clk {
	struct clk_hw hw;
	struct snd_sof_dev *sdev;
	struct sof_dai_clk *dai_clk;
};

static int sof_clk_ctrl(struct snd_sof_dev *sdev, struct sof_dai_clk *dai_clk, int enable)
{
	struct sof_ipc_dai_clkctrl clkctrl;
	struct sof_ipc_reply reply;
	int ret;

	memset(&clkctrl, 0, sizeof(clkctrl));

	/* set IPC DAI clock control */
	clkctrl.hdr.size = sizeof(clkctrl);
	clkctrl.hdr.cmd = SOF_IPC_GLB_DAI_MSG | SOF_IPC_DAI_CLKCTRL;

	clkctrl.type = dai_clk->type;
	clkctrl.dai_index = dai_clk->dai_index;

	clkctrl.clk_en = enable ? 1 : 0;
	clkctrl.clk_id = dai_clk->clk_id;

	dev_dbg(sdev->dev, "clkctrl: SSP%d clk_en %d clk_id %d\n",
		clkctrl.dai_index, clkctrl.clk_en, clkctrl.clk_id);

	/* send IPC to the DSP */
	ret = sof_ipc_tx_message(sdev->ipc, clkctrl.hdr.cmd, &clkctrl,
				 sizeof(clkctrl), &reply, sizeof(reply));

	if (ret < 0) {
		dev_err(sdev->dev,
			"failed to control DAI clock for type %d index %d\n",
			dai_clk->type, dai_clk->dai_index);
		return ret;
	}

	return 0;
}

static int sof_clk_prepare(struct clk_hw *hw)
{
	struct sof_clk *clkdev = container_of(hw, struct sof_clk, hw);
	struct snd_sof_dev *sdev = clkdev->sdev;

	return sof_clk_ctrl(sdev, clkdev->dai_clk, 1);
}

static void sof_clk_unprepare(struct clk_hw *hw)
{
	struct sof_clk *clkdev = container_of(hw, struct sof_clk, hw);
	struct snd_sof_dev *sdev = clkdev->sdev;

	sof_clk_ctrl(sdev, clkdev->dai_clk, 0);
}

static const struct clk_ops sof_clk_ops = {
	.prepare = sof_clk_prepare,
	.unprepare = sof_clk_unprepare,
};

int snd_sof_dai_clks_probe(struct snd_soc_component *component)
{
	struct snd_sof_dev *sdev = snd_soc_component_get_drvdata(component);
	struct snd_sof_pdata *pdata = sdev->pdata;
	struct device *dev = component->dev;
	int i, ret;

	sdev->num_clks = pdata->num_clks;

	if (sdev->num_clks > SOF_DAI_CLKS)
		sdev->num_clks = SOF_DAI_CLKS;

	for (i = 0; i < sdev->num_clks; ++i) {
		struct clk_init_data init = {};
		struct clk_lookup *dai_clk_lookup;
		struct sof_clk *clkdev;

		clkdev = devm_kzalloc(dev, sizeof(*clkdev), GFP_KERNEL);
		if (!clkdev) {
			ret = -ENOMEM;
			goto err;
		}

		init.name = pdata->dai_clks[i].name;
		init.ops = &sof_clk_ops;
		init.flags = CLK_GET_RATE_NOCACHE | CLK_SET_RATE_GATE;
		init.parent_names = NULL;
		init.num_parents = 0;

		clkdev->hw.init = &init;
		clkdev->sdev = sdev;
		clkdev->dai_clk = &pdata->dai_clks[i];

		ret = devm_clk_hw_register(dev, &clkdev->hw);
		if (ret) {
			dev_warn(dev, "failed to register %s: %d\n",
				 init.name, ret);
			goto err;
		}

		dai_clk_lookup = clkdev_hw_create(&clkdev->hw, init.name,
						  NULL);
		if (!dai_clk_lookup) {
			dev_warn(dev, "failed to create %s: %d\n",
				 init.name, ret);
			ret = -ENOMEM;
			goto err;
		}

		sdev->dai_clks_lookup[i] = dai_clk_lookup;
	}

	return 0;

err:
	do {
		if (sdev->dai_clks_lookup[i])
			clkdev_drop(sdev->dai_clks_lookup[i]);
	} while (i-- > 0);

	return ret;
}

void snd_sof_dai_clks_remove(struct snd_soc_component *component)
{
	struct snd_sof_dev *sdev = snd_soc_component_get_drvdata(component);
	int i;

	for (i = sdev->num_clks - 1; i >= 0; --i) {
		if (sdev->dai_clks_lookup[i])
			clkdev_drop(sdev->dai_clks_lookup[i]);
	}
}
