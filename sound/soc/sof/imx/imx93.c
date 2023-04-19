// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
/*
 * Copyright 2023 NXP
 *
 * Author: Laurentiu Mihalcea <laurentiu.mihalcea@nxp.com>
 */

#include <sound/sof.h>
#include <linux/of_address.h>
#include <linux/firmware/imx/dsp.h>
#include <linux/clk.h>

#include "../sof-priv.h"
#include "../sof-of-dev.h"
#include "../ops.h"

/* since sdev->bar[SOF_FW_BLK_TYPE_SRAM] holds the base address
 * of the mailbox regions, the mailbox offset is 0.
 */
#define MBOX_OFFSET 0

struct imx93_priv {
	struct snd_sof_dev *sdev;
	struct platform_device *ipc_dev;
	struct imx_dsp_ipc *dummy_dsp_ipc;
	struct clk_bulk_data *clks;
	int num_clks;
};

static void imx93_dummy_dsp_handle_reply(struct imx_dsp_ipc *ipc)
{
	struct imx93_priv *priv;
	unsigned long flags;

	priv = imx_dsp_get_data(ipc);

	spin_lock_irqsave(&priv->sdev->ipc_lock, flags);
	snd_sof_ipc_process_reply(priv->sdev, 0);
	spin_unlock_irqrestore(&priv->sdev->ipc_lock, flags);
}

static void imx93_dummy_dsp_handle_request(struct imx_dsp_ipc *ipc)
{
	struct imx93_priv *priv = imx_dsp_get_data(ipc);

	/* TODO: handle panic case here if need be */

	snd_sof_ipc_msgs_rx(priv->sdev);
}

static struct imx_dsp_ops dummy_dsp_ops = {
	.handle_reply = imx93_dummy_dsp_handle_reply,
	.handle_request = imx93_dummy_dsp_handle_request,
};

static int imx93_get_mailbox_offset(struct snd_sof_dev *sdev)
{
	return MBOX_OFFSET;
}

static int imx93_get_window_offset(struct snd_sof_dev *sdev, u32 id)
{
	return MBOX_OFFSET;
}

static int imx93_probe(struct snd_sof_dev *sdev)
{
	struct platform_device *pdev;
	struct resource res;
	struct imx93_priv *priv;
	struct device_node *res_node, *np;
	int ret;

	pdev = container_of(sdev->dev, struct platform_device, dev);
	np = pdev->dev.of_node;

	priv = devm_kzalloc(sdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	sdev->num_cores = 1;

	/* this will make host initiate SOF_IPC_FW_READY sequence */
	sdev->init_fw_ready = true;

	sdev->pdata->hw_pdata = priv;
	priv->sdev = sdev;

	res_node = of_parse_phandle(np, "mbox-base", 0);
	if (!res_node) {
		dev_err(sdev->dev, "failed to get mbox-base node.\n");
		return -ENODEV;
	}

	ret = of_address_to_resource(res_node, 0, &res);
	of_node_put(res_node);
	if (ret) {
		dev_err(sdev->dev, "failed to get mbox-base address.\n");
		return ret;
	}

	/* map mailbox region into the kernel space */
	sdev->bar[SOF_FW_BLK_TYPE_SRAM] = devm_ioremap_resource(sdev->dev, &res);
	if (IS_ERR(sdev->bar[SOF_FW_BLK_TYPE_SRAM])) {
		/* devm_ioremap_resource error message should be fine on
		 * its own but this additional error message will help
		 * debug cases in which memory isn't reserved properly
		 * at boot time.
		 */
		dev_err(sdev->dev, "failed to ioremap mailbox region. Are you sure you have reserved at least 800MB of memory using 'mem' boot arg?\n");
		return -ENOMEM;
	}

	sdev->mailbox_bar = SOF_FW_BLK_TYPE_SRAM;

	/* this is needed because SOF_IPC_FW_READY data will be sent
	 * as a reply by the FW. Because we don't know the offsets
	 * for the other mailbox regions in advance and we don't want
	 * to hard code them here just add the following restriction:
	 *
	 * host_box needs to be placed at the base of the mailbox
	 * region.
	 */
	sdev->host_box.offset = 0;

	/* initialize IPC driver */
	priv->ipc_dev = platform_device_register_data(sdev->dev,
						      "imx-dsp",
						      PLATFORM_DEVID_NONE,
						      pdev,
						      sizeof(*pdev));
	if (IS_ERR(priv->ipc_dev)) {
		dev_err(sdev->dev, "failed to register platform device data.\n");
		return PTR_ERR(priv->ipc_dev);
	}

	priv->dummy_dsp_ipc = dev_get_drvdata(&priv->ipc_dev->dev);
	if (!priv->dummy_dsp_ipc) {
		/* DSP driver not probed */
		dev_err(sdev->dev, "failed to get drvdata.\n");
		ret = -EPROBE_DEFER;
		goto err_pdev_unregister;
	}

	imx_dsp_set_data(priv->dummy_dsp_ipc, priv);
	priv->dummy_dsp_ipc->ops = &dummy_dsp_ops;

	ret = devm_clk_bulk_get_all(sdev->dev, &priv->clks);
	if (ret < 0) {
		dev_err(sdev->dev, "failed to get clocks.\n");
		goto err_pdev_unregister;
	}
	priv->num_clks = ret;

	ret = clk_bulk_prepare_enable(priv->num_clks, priv->clks);
	if (ret < 0) {
		dev_err(sdev->dev, "failed to enable clocks.\n");
		goto err_pdev_unregister;
	}

	return 0;

err_pdev_unregister:
	platform_device_unregister(priv->ipc_dev);

	return ret;
}

static int imx93_remove(struct snd_sof_dev *sdev)
{
	struct imx93_priv *priv = sdev->pdata->hw_pdata;

	platform_device_unregister(priv->ipc_dev);

	/* disable clocks */
	clk_bulk_disable_unprepare(priv->num_clks, priv->clks);

	return 0;
}

static int imx93_run(struct snd_sof_dev *sdev)
{
	/* nothing to be done here */
	return 0;
}

static int imx93_load_firmware(struct snd_sof_dev *sdev)
{
	/* nothing to be done here */
	return 0;
}

static int imx93_send_msg(struct snd_sof_dev *sdev, struct snd_sof_ipc_msg *msg)
{
	struct imx93_priv *priv = sdev->pdata->hw_pdata;

	sof_mailbox_write(sdev, sdev->host_box.offset, msg->msg_data,
			  msg->msg_size);

	imx_dsp_ring_doorbell(priv->dummy_dsp_ipc, 0);

	return 0;
}

static int imx93_get_bar_index(struct snd_sof_dev *sdev, u32 type)
{
	switch (type) {
	case SOF_FW_BLK_TYPE_SRAM:
		return type;
	default:
		return -EINVAL;
	}
}

/* the values in this structure are taken from fsl_sai.c */
static struct snd_soc_dai_driver imx93_dai[] = {
	{
		.name = "sai3",
		.playback = {
			.channels_min = 1,
			.channels_max = 32,
		},
		.capture = {
			.channels_min = 1,
			.channels_max = 32,
		},
	},
};

static struct snd_sof_dsp_ops sof_imx93_ops = {
	/* probe/remove operations */
	.probe = imx93_probe,
	.remove = imx93_remove,

	/* DSP core boot */
	.run = imx93_run,

	/* block I/O */
	.block_read = sof_block_read,
	.block_write = sof_block_write,

	/* mailbox I/O */
	.mailbox_read = sof_mailbox_read,
	.mailbox_write = sof_mailbox_write,

	/* IPC */
	.send_msg = imx93_send_msg,
	.get_mailbox_offset = imx93_get_mailbox_offset,
	.get_window_offset = imx93_get_window_offset,
	.ipc_msg_data = sof_ipc_msg_data,

	.set_stream_data_offset = sof_set_stream_data_offset,

	.get_bar_index = imx93_get_bar_index,

	/* firmware loading */
	.load_firmware = imx93_load_firmware,

	/* DAI drivers */
	.drv = imx93_dai,
	.num_drv = ARRAY_SIZE(imx93_dai),

	/* stream callbacks */
	.pcm_open = sof_stream_pcm_open,
	.pcm_close = sof_stream_pcm_close,

	/* ALSA HW info flags */
	.hw_info = SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_PAUSE |
		SNDRV_PCM_INFO_NO_PERIOD_WAKEUP,

};

static struct sof_dev_desc sof_of_imx93_desc = {
	.ipc_supported_mask = BIT(SOF_IPC),
	.ipc_default = SOF_IPC,
	.default_tplg_path = {
		[SOF_IPC] = "imx/sof-tplg",
	},
	.ops = &sof_imx93_ops,
};

static const struct of_device_id sof_of_imx93_ids[] = {
	{ .compatible = "fsl,imx93-dummy-dsp", .data = &sof_of_imx93_desc },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, sof_of_imx93_ids);

static struct platform_driver snd_sof_of_imx93_driver = {
	.probe = sof_of_probe,
	.remove = sof_of_remove,
	.driver = {
		.name = "sof-audio-of-imx93",
		/* for now, PM is not supported */
		.pm = NULL,
		.of_match_table = sof_of_imx93_ids,
	},
};

module_platform_driver(snd_sof_of_imx93_driver);

MODULE_LICENSE("Dual BSD/GPL");
