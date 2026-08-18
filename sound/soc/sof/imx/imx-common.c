// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
//
// Copyright 2020-2025 NXP
//
// Common helpers for the audio DSP on i.MX8

#include <linux/firmware/imx/dsp.h>
#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/pm_domain.h>
#include <sound/sof/ipc4/header.h>
#include <sound/sof/xtensa.h>

#include "../ops.h"
#include "../sof-priv.h"
#include "../ipc4-priv.h"
#include "../ipc4-fw-reg.h"

#include "imx-common.h"

/**
 * imx8_get_registers() - This function is called in case of DSP oops
 * in order to gather information about the registers, filename and
 * linenumber and stack.
 * @sdev: SOF device
 * @xoops: Stores information about registers.
 * @panic_info: Stores information about filename and line number.
 * @stack: Stores the stack dump.
 * @stack_words: Size of the stack dump.
 */
void imx8_get_registers(struct snd_sof_dev *sdev,
			struct sof_ipc_dsp_oops_xtensa *xoops,
			struct sof_ipc_panic_info *panic_info,
			u32 *stack, size_t stack_words)
{
	u32 offset = sdev->dsp_oops_offset;

	/* first read registers */
	sof_mailbox_read(sdev, offset, xoops, sizeof(*xoops));
	xoops->plat_hdr.numaregs = 0;

	/* then get panic info */
	if (xoops->arch_hdr.totalsize > EXCEPT_MAX_HDR_SIZE) {
		dev_err(sdev->dev, "invalid header size 0x%x. FW oops is bogus\n",
			xoops->arch_hdr.totalsize);
		return;
	}
	offset += xoops->arch_hdr.totalsize;
	sof_mailbox_read(sdev, offset, panic_info, sizeof(*panic_info));

	/* then get the stack */
	offset += sizeof(*panic_info);
	sof_mailbox_read(sdev, offset, stack, stack_words * sizeof(u32));
}

/**
 * imx8_dump() - This function is called when a panic message is
 * received from the firmware.
 * @sdev: SOF device
 * @flags: parameter not used but required by ops prototype
 */
void imx8_dump(struct snd_sof_dev *sdev, u32 flags)
{
	struct sof_ipc_dsp_oops_xtensa xoops;
	struct sof_ipc_panic_info panic_info;
	u32 stack[IMX8_STACK_DUMP_SIZE];
	u32 status;

	/* Get information about the panic status from the debug box area.
	 * Compute the trace point based on the status.
	 */
	sof_mailbox_read(sdev, sdev->debug_box.offset + 0x4, &status, 4);

	/* Get information about the registers, the filename and line
	 * number and the stack.
	 */
	imx8_get_registers(sdev, &xoops, &panic_info, stack,
			   IMX8_STACK_DUMP_SIZE);

	/* Print the information to the console */
	sof_print_oops_and_stack(sdev, KERN_ERR, status, status, &xoops,
				 &panic_info, stack, IMX8_STACK_DUMP_SIZE);
}
EXPORT_SYMBOL(imx8_dump);

static void imx_handle_reply(struct imx_dsp_ipc *ipc)
{
	struct snd_sof_dev *sdev = imx_dsp_get_data(ipc);

	/* IPC4 replies arrive through the request doorbell */
	if (sdev->pdata->ipc_type == SOF_IPC_TYPE_4)
		return;

	guard(spinlock_irqsave)(&sdev->ipc_lock);
	snd_sof_ipc_process_reply(sdev, 0);
}

#if IS_ENABLED(CONFIG_SND_SOC_SOF_IPC4)
static void imx_handle_ipc4_msg(struct snd_sof_dev *sdev)
{
	struct sof_ipc4_msg *ipc4_msg;
	u64 header;

	/* the 8-byte header precedes the DSP -> host box payload */
	sof_mailbox_read(sdev, sdev->dsp_box.offset - IMX_IPC4_HDR_SIZE,
			 &header, sizeof(header));

	if (lower_32_bits(header) & SOF_IPC4_MSG_DIR_MASK) {
		guard(spinlock_irqsave)(&sdev->ipc_lock);

		if (sdev->msg) {
			ipc4_msg = sdev->msg->reply_data;
			ipc4_msg->header_u64 = header;
		}

		snd_sof_ipc_process_reply(sdev, 0);
	} else {
		struct sof_ipc4_msg notification_data = {{ 0 }};

		notification_data.header_u64 = header;
		sdev->ipc->msg.rx_data = &notification_data;
		snd_sof_ipc_msgs_rx(sdev);
		sdev->ipc->msg.rx_data = NULL;
	}
}
#endif

static void imx_handle_request(struct imx_dsp_ipc *ipc)
{
	struct snd_sof_dev *sdev;
	u32 panic_code;

	sdev = imx_dsp_get_data(ipc);

	if (get_chip_info(sdev)->ipc_info.has_panic_code) {
		sof_mailbox_read(sdev, sdev->debug_box.offset + 0x4,
				 &panic_code,
				 sizeof(panic_code));

		if ((panic_code & SOF_IPC_PANIC_MAGIC_MASK) == SOF_IPC_PANIC_MAGIC) {
			snd_sof_dsp_panic(sdev, panic_code, true);
			return;
		}
	}

#if IS_ENABLED(CONFIG_SND_SOC_SOF_IPC4)
	if (sdev->pdata->ipc_type == SOF_IPC_TYPE_4) {
		imx_handle_ipc4_msg(sdev);
		return;
	}
#endif

	snd_sof_ipc_msgs_rx(sdev);
}

static struct imx_dsp_ops imx_ipc_ops = {
	.handle_reply = imx_handle_reply,
	.handle_request = imx_handle_request,
};

static int imx_send_msg(struct snd_sof_dev *sdev, struct snd_sof_ipc_msg *msg)
{
	struct imx_common_data *common = sdev->pdata->hw_pdata;

	if (sdev->pdata->ipc_type == SOF_IPC_TYPE_4) {
		struct sof_ipc4_msg *msg_data = msg->msg_data;

		/* the 8-byte header precedes the host -> DSP box payload */
		sof_mailbox_write(sdev,
				  sdev->host_box.offset - IMX_IPC4_HDR_SIZE,
				  &msg_data->header_u64,
				  sizeof(msg_data->header_u64));

		if (msg_data->data_size)
			sof_mailbox_write(sdev, sdev->host_box.offset,
					  msg_data->data_ptr,
					  msg_data->data_size);
	} else {
		sof_mailbox_write(sdev, sdev->host_box.offset,
				  msg->msg_data, msg->msg_size);
	}

	imx_dsp_ring_doorbell(common->ipc_handle, 0x0);

	return 0;
}

static int imx_get_bar_index(struct snd_sof_dev *sdev, u32 type)
{
	switch (type) {
	case SOF_FW_BLK_TYPE_IRAM:
	case SOF_FW_BLK_TYPE_DRAM:
	case SOF_FW_BLK_TYPE_SRAM:
		/* i.MX95 maps IRAM/DRAM/SRAM through a single window */
		if (!sdev->bar[type])
			return sdev->mailbox_bar;
		return type;
	default:
		return -EINVAL;
	}
}

static int imx_get_mailbox_offset(struct snd_sof_dev *sdev)
{
	if (sdev->pdata->ipc_type == SOF_IPC_TYPE_4)
		return get_chip_info(sdev)->ipc_info.ipc4_inbox_offset;

	return get_chip_info(sdev)->ipc_info.boot_mbox_offset;
}

static int imx_get_window_offset(struct snd_sof_dev *sdev, u32 id)
{
	if (sdev->pdata->ipc_type == SOF_IPC_TYPE_4) {
		const struct imx_ipc_info *ipc_info = &get_chip_info(sdev)->ipc_info;

		switch (id) {
		case SOF_IPC4_INBOX_WINDOW_IDX:
			/* fw_info_box lives in the SW register window */
			return ipc_info->ipc4_fw_regs_offset;
		case SOF_IPC4_OUTBOX_WINDOW_IDX:
			return ipc_info->ipc4_outbox_offset;
		case SOF_IPC4_DEBUG_WINDOW_IDX:
			return ipc_info->ipc4_debug_offset;
		default:
			return -EINVAL;
		}
	}

	return get_chip_info(sdev)->ipc_info.window_offset;
}

static int imx_set_power_state(struct snd_sof_dev *sdev,
			       const struct sof_dsp_power_state *target)
{
	sdev->dsp_power_state = *target;

	return 0;
}

static int imx_common_resume(struct snd_sof_dev *sdev)
{
	struct imx_common_data *common;
	int ret, i;

	common = sdev->pdata->hw_pdata;

	ret = clk_bulk_prepare_enable(common->clk_num, common->clks);
	if (ret)
		dev_err(sdev->dev, "failed to enable clocks: %d\n", ret);

	for (i = 0; i < DSP_MU_CHAN_NUM; i++)
		imx_dsp_request_channel(common->ipc_handle, i);

	/* done. If need be, core will be started by SOF core immediately after */
	return 0;
}

static int imx_common_suspend(struct snd_sof_dev *sdev)
{
	struct imx_common_data *common;
	int i, ret;

	common = sdev->pdata->hw_pdata;

	ret = imx_chip_core_shutdown(sdev);
	if (ret < 0) {
		dev_err(sdev->dev, "failed to shutdown core: %d\n", ret);
		return ret;
	}

	for (i = 0; i < DSP_MU_CHAN_NUM; i++)
		imx_dsp_free_channel(common->ipc_handle, i);

	clk_bulk_disable_unprepare(common->clk_num, common->clks);

	return 0;
}

static int imx_runtime_resume(struct snd_sof_dev *sdev)
{
	const struct sof_dsp_power_state target_state = {
		.state = SOF_DSP_PM_D0,
	};
	int ret;

	ret = imx_common_resume(sdev);
	if (ret < 0) {
		dev_err(sdev->dev, "failed to runtime common resume: %d\n", ret);
		return ret;
	}

	return snd_sof_dsp_set_power_state(sdev, &target_state);
}

static int imx_resume(struct snd_sof_dev *sdev)
{
	const struct sof_dsp_power_state target_state = {
		.state = SOF_DSP_PM_D0,
	};
	int ret;

	ret = imx_common_resume(sdev);
	if (ret < 0) {
		dev_err(sdev->dev, "failed to common resume: %d\n", ret);
		return ret;
	}

	if (pm_runtime_suspended(sdev->dev)) {
		pm_runtime_disable(sdev->dev);
		pm_runtime_set_active(sdev->dev);
		pm_runtime_mark_last_busy(sdev->dev);
		pm_runtime_enable(sdev->dev);
		pm_runtime_idle(sdev->dev);
	}

	return snd_sof_dsp_set_power_state(sdev, &target_state);
}

static int imx_runtime_suspend(struct snd_sof_dev *sdev)
{
	const struct sof_dsp_power_state target_state = {
		.state = SOF_DSP_PM_D3,
	};
	int ret;

	ret = imx_common_suspend(sdev);
	if (ret < 0)
		dev_err(sdev->dev, "failed to runtime common suspend: %d\n", ret);

	return snd_sof_dsp_set_power_state(sdev, &target_state);
}

static int imx_suspend(struct snd_sof_dev *sdev, unsigned int target_state)
{
	const struct sof_dsp_power_state target_power_state = {
		.state = target_state,
	};
	int ret;

	if (!pm_runtime_suspended(sdev->dev)) {
		ret = imx_common_suspend(sdev);
		if (ret < 0) {
			dev_err(sdev->dev, "failed to common suspend: %d\n", ret);
			return ret;
		}
	}

	return snd_sof_dsp_set_power_state(sdev, &target_power_state);
}

static int imx_region_name_to_blk_type(const char *region_name)
{
	if (!strcmp(region_name, "iram"))
		return SOF_FW_BLK_TYPE_IRAM;
	else if (!strcmp(region_name, "dram"))
		return SOF_FW_BLK_TYPE_DRAM;
	else if (!strcmp(region_name, "sram"))
		return SOF_FW_BLK_TYPE_SRAM;
	else
		return -EINVAL;
}

static int imx_parse_ioremap_memory(struct snd_sof_dev *sdev)
{
	const struct imx_chip_info *chip_info;
	struct platform_device *pdev;
	struct resource *res, _res;
	int i, blk_type, ret;

	pdev = to_platform_device(sdev->dev);
	chip_info = get_chip_info(sdev);

	for (i = 0; chip_info->memory[i].name; i++) {
		blk_type = imx_region_name_to_blk_type(chip_info->memory[i].name);
		if (blk_type < 0)
			return dev_err_probe(sdev->dev, blk_type,
					     "no blk type for region %s\n",
					     chip_info->memory[i].name);

		if (!chip_info->memory[i].reserved) {
			res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
							   chip_info->memory[i].name);
			if (!res)
				return dev_err_probe(sdev->dev, -ENODEV,
						     "failed to fetch %s resource\n",
						     chip_info->memory[i].name);

		} else {
			ret = of_reserved_mem_region_to_resource_byname(pdev->dev.of_node,
									chip_info->memory[i].name,
									&_res);
			if (ret < 0)
				return dev_err_probe(sdev->dev, ret,
						     "no valid entry for %s\n",
						     chip_info->memory[i].name);
			res = &_res;
		}

		sdev->bar[blk_type] = devm_ioremap_resource(sdev->dev, res);
		if (IS_ERR(sdev->bar[blk_type]))
			return dev_err_probe(sdev->dev,
					     PTR_ERR(sdev->bar[blk_type]),
					     "failed to ioremap %s region\n",
					     chip_info->memory[i].name);
	}

	return 0;
}

static void imx_unregister_action(void *data)
{
	struct imx_common_data *common;
	struct snd_sof_dev *sdev;

	sdev = data;
	common = sdev->pdata->hw_pdata;

	if (get_chip_info(sdev)->has_dma_reserved)
		of_reserved_mem_device_release(sdev->dev);

	platform_device_unregister(common->ipc_dev);
}

static int imx_probe(struct snd_sof_dev *sdev)
{
	struct dev_pm_domain_attach_data domain_data = {
		.pd_names = NULL, /* no filtering */
		.pd_flags = PD_FLAG_DEV_LINK_ON,
	};
	struct imx_common_data *common;
	struct platform_device *pdev;
	int ret;

	pdev = to_platform_device(sdev->dev);

	common = devm_kzalloc(sdev->dev, sizeof(*common), GFP_KERNEL);
	if (!common)
		return -ENOMEM;

	sdev->pdata->hw_pdata = common;

	common->ipc_dev = platform_device_register_data(sdev->dev, "imx-dsp",
							PLATFORM_DEVID_NONE,
							pdev, sizeof(*pdev));
	if (IS_ERR(common->ipc_dev))
		return dev_err_probe(sdev->dev, PTR_ERR(common->ipc_dev),
				     "failed to create IPC device\n");

	if (get_chip_info(sdev)->has_dma_reserved) {
		ret = of_reserved_mem_device_init_by_name(sdev->dev,
							  pdev->dev.of_node,
							  "dma");
		if (ret) {
			platform_device_unregister(common->ipc_dev);

			return dev_err_probe(sdev->dev, ret,
					     "failed to bind DMA region\n");
		}
	}

	/* let the devres API take care of the cleanup */
	ret = devm_add_action_or_reset(sdev->dev,
				       imx_unregister_action,
				       sdev);
	if (ret)
		return ret;

	common->ipc_handle = dev_get_drvdata(&common->ipc_dev->dev);
	if (!common->ipc_handle)
		return dev_err_probe(sdev->dev, -EPROBE_DEFER,
				     "failed to fetch IPC handle\n");

	ret = imx_parse_ioremap_memory(sdev);
	if (ret < 0)
		return dev_err_probe(sdev->dev, ret,
				     "failed to parse/ioremap memory regions\n");

	if (!sdev->dev->pm_domain) {
		ret = devm_pm_domain_attach_list(sdev->dev,
						 &domain_data, &common->pd_list);
		if (ret < 0)
			return dev_err_probe(sdev->dev, ret, "failed to attach PDs\n");
	}

	ret = devm_clk_bulk_get_all(sdev->dev, &common->clks);
	if (ret < 0)
		return dev_err_probe(sdev->dev, ret, "failed to fetch clocks\n");
	common->clk_num = ret;

	ret = clk_bulk_prepare_enable(common->clk_num, common->clks);
	if (ret < 0)
		return dev_err_probe(sdev->dev, ret, "failed to enable clocks\n");

	common->ipc_handle->ops = &imx_ipc_ops;
	imx_dsp_set_data(common->ipc_handle, sdev);

	sdev->num_cores = 1;
	sdev->mailbox_bar = SOF_FW_BLK_TYPE_SRAM;

	/* IPC4 box offsets are set by sof_ipc4_init() */
	if (sdev->pdata->ipc_type == SOF_IPC_TYPE_3)
		sdev->dsp_box.offset = get_chip_info(sdev)->ipc_info.boot_mbox_offset;

	return imx_chip_probe(sdev);
}

static void imx_remove(struct snd_sof_dev *sdev)
{
	struct imx_common_data *common;
	int ret;

	common = sdev->pdata->hw_pdata;

	if (!pm_runtime_suspended(sdev->dev)) {
		ret = imx_chip_core_shutdown(sdev);
		if (ret < 0)
			dev_err(sdev->dev, "failed to shutdown core: %d\n", ret);

		clk_bulk_disable_unprepare(common->clk_num, common->clks);
	}
}

#if IS_ENABLED(CONFIG_SND_SOC_SOF_IPC4)
/* copy the Reef payload that follows the extended manifest and $AM1 header */
static int imx_ipc4_load_fw_to_dsp(struct snd_sof_dev *sdev)
{
	u32 payload_offset = sdev->basefw.payload_offset;
	const struct firmware *fw = sdev->basefw.fw;
	const struct sof_man4_fw_binary_header *man4_hdr;
	const struct snd_sof_fw_header *header;
	const struct snd_sof_mod_hdr *module;
	size_t remaining;
	u32 man4_hdr_offset;
	int count, ret;

	if (!fw)
		return -EINVAL;

	man4_hdr_offset = payload_offset + IMX_MAN4_FW_HDR_OFFSET;
	if (fw->size < man4_hdr_offset + sizeof(*man4_hdr)) {
		dev_err(sdev->dev, "firmware too small for $AM1 header (size %zu)\n",
			fw->size);
		return -EINVAL;
	}

	man4_hdr = (const struct sof_man4_fw_binary_header *)
		   (fw->data + man4_hdr_offset);

	payload_offset = man4_hdr_offset + man4_hdr->len +
			 man4_hdr->num_module_entries * sizeof(struct sof_man4_module);

	if (fw->size < payload_offset + sizeof(*header)) {
		dev_err(sdev->dev, "firmware too small for payload header\n");
		return -EINVAL;
	}

	header = (const struct snd_sof_fw_header *)(fw->data + payload_offset);

	if (memcmp(header->sig, SND_SOF_FW_SIG, SND_SOF_FW_SIG_SIZE)) {
		dev_err(sdev->dev, "invalid firmware payload signature\n");
		return -EINVAL;
	}

	module = (const struct snd_sof_mod_hdr *)(fw->data + payload_offset +
						  sizeof(*header));
	remaining = fw->size - sizeof(*header) - payload_offset;

	for (count = 0; count < header->num_modules; count++) {
		const struct snd_sof_blk_hdr *block;
		int blk;

		if (remaining < sizeof(*module)) {
			dev_err(sdev->dev, "not enough data for a module\n");
			return -EINVAL;
		}

		block = (const struct snd_sof_blk_hdr *)((const u8 *)module +
							 sizeof(*module));
		remaining -= sizeof(*module);

		for (blk = 0; blk < module->num_blocks; blk++) {
			u32 offset;

			if (remaining < sizeof(*block)) {
				dev_err(sdev->dev, "not enough data for block\n");
				return -EINVAL;
			}
			remaining -= sizeof(*block);

			switch (block->type) {
			case SOF_FW_BLK_TYPE_IRAM:
			case SOF_FW_BLK_TYPE_DRAM:
			case SOF_FW_BLK_TYPE_SRAM:
				offset = block->offset;
				break;
			default:
				/* skip anything the memcpy loader can't handle */
				if (remaining < block->size)
					return -EINVAL;
				remaining -= block->size;
				block = (const struct snd_sof_blk_hdr *)
					((const u8 *)block + sizeof(*block) +
					 block->size);
				continue;
			}

			if (block->size % sizeof(u32)) {
				dev_err(sdev->dev, "unaligned block size %#x\n",
					block->size);
				return -EINVAL;
			}

			ret = snd_sof_dsp_block_write(sdev, block->type, offset,
						      (void *)(block + 1),
						      block->size);
			if (ret < 0) {
				dev_err(sdev->dev,
					"failed to write block type %#x\n",
					block->type);
				return ret;
			}

			if (remaining < block->size)
				return -EINVAL;
			remaining -= block->size;

			block = (const struct snd_sof_blk_hdr *)
				((const u8 *)block + sizeof(*block) + block->size);
		}

		module = (const struct snd_sof_mod_hdr *)block;
	}

	return 0;
}

/* The generic IPC4 loader has no load_fw_to_dsp(), so copy before run */
static int imx_pre_fw_run(struct snd_sof_dev *sdev)
{
	if (sdev->pdata->ipc_type != SOF_IPC_TYPE_4)
		return 0;

	return imx_ipc4_load_fw_to_dsp(sdev);
}

int imx_ipc4_init_data(struct snd_sof_dev *sdev)
{
	struct sof_ipc4_fw_data *ipc4_data;

	if (sdev->pdata->ipc_type != SOF_IPC_TYPE_4)
		return 0;

	ipc4_data = devm_kzalloc(sdev->dev, sizeof(*ipc4_data), GFP_KERNEL);
	if (!ipc4_data)
		return -ENOMEM;

	ipc4_data->manifest_fw_hdr_offset = IMX_MAN4_FW_HDR_OFFSET;
	ipc4_data->mtrace_type = SOF_IPC4_MTRACE_NOT_AVAILABLE;
	ipc4_data->max_libs_count = 1;

	sdev->private = ipc4_data;

	return 0;
}
EXPORT_SYMBOL(imx_ipc4_init_data);
#else
int imx_ipc4_init_data(struct snd_sof_dev *sdev)
{
	return 0;
}
EXPORT_SYMBOL(imx_ipc4_init_data);

static int imx_pre_fw_run(struct snd_sof_dev *sdev)
{
	return 0;
}
#endif /* CONFIG_SND_SOC_SOF_IPC4 */

/* no hardware stream tags: derive the host gateway id from device and direction */
static int imx_pcm_hw_params(struct snd_sof_dev *sdev,
			     struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *params,
			     struct snd_sof_platform_stream_params *platform_params)
{
	if (sdev->pdata->ipc_type == SOF_IPC_TYPE_4) {
		platform_params->stream_tag = substream->pcm->device * 2 + substream->stream + 1;

		platform_params->use_phy_address = true;
		platform_params->phy_addr = substream->runtime->dma_addr;
	}

	return 0;
}

/* The firmware publishes a byte counter per host gateway in fw_registers */
static u64 imx_get_host_byte_counter(struct snd_sof_dev *sdev,
				     struct snd_soc_component *component,
				     struct snd_pcm_substream *substream)
{
	unsigned int host_dma_id = substream->pcm->device * 2 + substream->stream;
	u64 counter = 0;
	u32 offset;

	if (host_dma_id >= SOF_IPC4_MAX_HOST_BYTE_CNT_SLOTS)
		return 0;

	offset = sdev->fw_info_box.offset +
		 offsetof(struct sof_ipc4_fw_registers, host_byte_cnt) +
		 host_dma_id * sizeof(u64);

	sof_mailbox_read(sdev, offset, &counter, sizeof(counter));

	return counter;
}

const struct snd_sof_dsp_ops sof_imx_ops = {
	.probe = imx_probe,
	.remove = imx_remove,

	.pre_fw_run = imx_pre_fw_run,
	.run = imx_chip_core_kick,
	.reset = imx_chip_core_reset,

	.block_read = sof_block_read,
	.block_write = sof_block_write,

	.mailbox_read = sof_mailbox_read,
	.mailbox_write = sof_mailbox_write,

	.send_msg = imx_send_msg,
	.get_mailbox_offset = imx_get_mailbox_offset,
	.get_window_offset = imx_get_window_offset,

	.ipc_msg_data = sof_ipc_msg_data,
	.set_stream_data_offset = sof_set_stream_data_offset,

	.get_bar_index = imx_get_bar_index,
	.load_firmware = snd_sof_load_firmware_memcpy,

	.debugfs_add_region_item = snd_sof_debugfs_add_region_item_iomem,

	.pcm_open = sof_stream_pcm_open,
	.pcm_close = sof_stream_pcm_close,
	.pcm_hw_params = imx_pcm_hw_params,
	.get_host_byte_counter = imx_get_host_byte_counter,
	.pcm_contiguous_buffer = true,

	.runtime_suspend = imx_runtime_suspend,
	.runtime_resume = imx_runtime_resume,
	.suspend = imx_suspend,
	.resume = imx_resume,

	.set_power_state = imx_set_power_state,

	.hw_info = SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_PAUSE |
		SNDRV_PCM_INFO_BATCH |
		SNDRV_PCM_INFO_NO_PERIOD_WAKEUP,
};
EXPORT_SYMBOL(sof_imx_ops);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("SOF helpers for IMX platforms");
