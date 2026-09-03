// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
/*
 * SoundWire AMD Manager driver
 *
 * Copyright 2023-24 Advanced Micro Devices, Inc.
 */

#include <linux/completion.h>
#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/dma-mapping.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/soundwire/sdw.h>
#include <linux/soundwire/sdw_registers.h>
#include <linux/pm_runtime.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include "bus.h"
#include "amd_init.h"
#include "amd_manager.h"

/* ATU register offsets */
#define ACPAXI2AXI_ATU_PAGE_SIZE_GRP_1		0x0000C00
#define ACPAXI2AXI_ATU_BASE_ADDR_GRP_1		0x0000C04
#define ACPAXI2AXI_ATU_CTRL			0x0000C40
#define ACP_SCRATCH_REG_0			0x0010000

#define DRV_NAME "amd_sdw_manager"

#define to_amd_sdw(b)	container_of(b, struct amd_sdw_manager, bus)

#define AMD_BPT_MSG_BYTE_MIN 16

/*
 * BRA PTE/ATU Configuration Constants
 *
 * BRA DMA uses ATU GRP_1 with 4KB pages. GRP_1 PTE table base sits at
 * ACP_SCRATCH_REG_0 (offset 0x03800000 from MMIO base). The AXI window
 * for GRP_1 starts at ACP_BRA_MEM_WINDOW_START (0x4000000). PTE entries
 * are 8 bytes each and start at scratch offset ACP_BRA_PTE_OFFSET (0x0).
 * ATU_PAGE_SIZE = 0x0 means DISABLED; set ACP_BRA_PAGE_SIZE_4K_ENABLE (0x2)
 * to enable 4KB page translation.
 */
#define ACP_BRA_SRAM_GRP1_BASE		0x03800000
#define ACP_BRA_PAGE_SIZE_4K_ENABLE	0x2
#define ACP_BRA_PTE_OFFSET		0x0
#define ACP_BRA_MEM_WINDOW_START	0x4000000
#define ACP_BRA_ATU_PTE_ENTRY_SIZE	8
#define ACP_BRA_MAX_PTE_ENTRIES		512
struct amd_bra_params {
	u32 sample_interval;
	u32 bytes_per_frame;
	u8 hstart;
	u8 hstop;
	u8 word_length;
	u8 dev_addr;
	bool write_mode;
	u32 peripheral_first_byte_addr;
	u32 dma_base_addr;
	u32 transfer_length;
};


static int amd_sdw_clk_init_ctrl(struct amd_sdw_manager *amd_manager)
{
	struct sdw_bus *bus = &amd_manager->bus;
	struct sdw_master_prop *prop = &bus->prop;
	u32 val;
	int divider;

	dev_dbg(amd_manager->dev, "mclk %d max %d row %d col %d frame_rate:%d\n",
		prop->mclk_freq, prop->max_clk_freq, prop->default_row,
		prop->default_col, prop->default_frame_rate);

	if (!prop->default_frame_rate || !prop->default_row) {
		dev_err(amd_manager->dev, "Default frame_rate %d or row %d is invalid\n",
			prop->default_frame_rate, prop->default_row);
		return -EINVAL;
	}

	/* Set clock divider */
	dev_dbg(amd_manager->dev, "bus params curr_dr_freq: %d\n",
		bus->params.curr_dr_freq);
	divider = (prop->mclk_freq / bus->params.curr_dr_freq);

	writel(divider, amd_manager->mmio + ACP_SW_CLK_FREQUENCY_CTRL);
	val = readl(amd_manager->mmio + ACP_SW_CLK_FREQUENCY_CTRL);
	dev_dbg(amd_manager->dev, "ACP_SW_CLK_FREQUENCY_CTRL:0x%x\n", val);

	/* Set frame shape base on the actual bus frequency. */
	prop->default_col = bus->params.curr_dr_freq /
			    prop->default_frame_rate / prop->default_row;

	dev_dbg(amd_manager->dev, "default_frame_rate:%d default_row: %d default_col: %d\n",
		prop->default_frame_rate, prop->default_row, prop->default_col);
	amd_manager->cols_index = sdw_find_col_index(prop->default_col);
	amd_manager->rows_index = sdw_find_row_index(prop->default_row);
	bus->params.col = prop->default_col;
	bus->params.row = prop->default_row;
	dev_dbg(amd_manager->dev, "rows_index: %d cols_index: %d\n",
		amd_manager->rows_index, amd_manager->cols_index);
	dev_dbg(amd_manager->dev, "params.col:0x%x params.row:0x%x\n",
		bus->params.col, bus->params.row);
	return 0;
}

static int amd_init_sdw_manager(struct amd_sdw_manager *amd_manager)
{
	u32 val;
	int ret;

	writel(AMD_SDW_ENABLE, amd_manager->mmio + ACP_SW_EN);
	ret = readl_poll_timeout(amd_manager->mmio + ACP_SW_EN_STATUS, val, val, ACP_DELAY_US,
				 AMD_SDW_TIMEOUT);
	if (ret)
		return ret;

	/* SoundWire manager bus reset */
	writel(AMD_SDW_BUS_RESET_REQ, amd_manager->mmio + ACP_SW_BUS_RESET_CTRL);
	ret = readl_poll_timeout(amd_manager->mmio + ACP_SW_BUS_RESET_CTRL, val,
				 (val & AMD_SDW_BUS_RESET_DONE), ACP_DELAY_US, AMD_SDW_TIMEOUT);
	if (ret)
		return ret;

	writel(AMD_SDW_BUS_RESET_CLEAR_REQ, amd_manager->mmio + ACP_SW_BUS_RESET_CTRL);
	ret = readl_poll_timeout(amd_manager->mmio + ACP_SW_BUS_RESET_CTRL, val, !val,
				 ACP_DELAY_US, AMD_SDW_TIMEOUT);
	if (ret) {
		dev_err(amd_manager->dev, "Failed to reset SoundWire manager instance%d\n",
			amd_manager->instance);
		return ret;
	}

	writel(AMD_SDW_DISABLE, amd_manager->mmio + ACP_SW_EN);
	return readl_poll_timeout(amd_manager->mmio + ACP_SW_EN_STATUS, val, !val, ACP_DELAY_US,
				  AMD_SDW_TIMEOUT);
}

static int amd_enable_sdw_manager(struct amd_sdw_manager *amd_manager)
{
	u32 val;

	writel(AMD_SDW_ENABLE, amd_manager->mmio + ACP_SW_EN);
	return readl_poll_timeout(amd_manager->mmio + ACP_SW_EN_STATUS, val, val, ACP_DELAY_US,
				  AMD_SDW_TIMEOUT);
}

static int amd_disable_sdw_manager(struct amd_sdw_manager *amd_manager)
{
	u32 val;

	writel(AMD_SDW_DISABLE, amd_manager->mmio + ACP_SW_EN);
	/*
	 * After invoking manager disable sequence, check whether
	 * manager has executed clock stop sequence. In this case,
	 * manager should ignore checking enable status register.
	 */
	val = readl(amd_manager->mmio + ACP_SW_CLK_RESUME_CTRL);
	if (val)
		return 0;
	return readl_poll_timeout(amd_manager->mmio + ACP_SW_EN_STATUS, val, !val, ACP_DELAY_US,
				  AMD_SDW_TIMEOUT);
}

static void amd_enable_sdw_interrupts(struct amd_sdw_manager *amd_manager)
{
	u32 val;

	mutex_lock(amd_manager->acp_sdw_lock);
	val = sdw_manager_reg_mask_array[amd_manager->instance];
	amd_updatel(amd_manager->acp_mmio, ACP_EXTERNAL_INTR_CNTL(amd_manager->instance), val, val);
	mutex_unlock(amd_manager->acp_sdw_lock);

	writel(AMD_SDW_IRQ_MASK_0TO7, amd_manager->mmio +
		       ACP_SW_STATE_CHANGE_STATUS_MASK_0TO7);
	writel(AMD_SDW_IRQ_MASK_8TO11, amd_manager->mmio +
		       ACP_SW_STATE_CHANGE_STATUS_MASK_8TO11);
	writel(AMD_SDW_IRQ_ERROR_MASK, amd_manager->mmio + ACP_SW_ERROR_INTR_MASK);
}

static void amd_disable_sdw_interrupts(struct amd_sdw_manager *amd_manager)
{
	u32 irq_mask;

	mutex_lock(amd_manager->acp_sdw_lock);
	irq_mask = sdw_manager_reg_mask_array[amd_manager->instance];
	amd_updatel(amd_manager->acp_mmio, ACP_EXTERNAL_INTR_CNTL(amd_manager->instance),
		    irq_mask, 0);
	mutex_unlock(amd_manager->acp_sdw_lock);

	writel(0x00, amd_manager->mmio + ACP_SW_STATE_CHANGE_STATUS_MASK_0TO7);
	writel(0x00, amd_manager->mmio + ACP_SW_STATE_CHANGE_STATUS_MASK_8TO11);
	writel(0x00, amd_manager->mmio + ACP_SW_ERROR_INTR_MASK);
}

static int amd_deinit_sdw_manager(struct amd_sdw_manager *amd_manager)
{
	amd_disable_sdw_interrupts(amd_manager);
	return amd_disable_sdw_manager(amd_manager);
}

static void amd_sdw_set_frameshape(struct amd_sdw_manager *amd_manager)
{
	u32 frame_size;

	frame_size = (amd_manager->rows_index << 3) | amd_manager->cols_index;
	writel(frame_size, amd_manager->mmio + ACP_SW_FRAMESIZE);
}

static void amd_sdw_wake_enable(struct amd_sdw_manager *amd_manager, bool enable)
{
	u32 wake_ctrl;

	wake_ctrl = readl(amd_manager->mmio + ACP_SW_STATE_CHANGE_STATUS_MASK_8TO11);
	if (enable)
		wake_ctrl |= AMD_SDW_WAKE_INTR_MASK;
	else
		wake_ctrl &= ~AMD_SDW_WAKE_INTR_MASK;

	writel(wake_ctrl, amd_manager->mmio + ACP_SW_STATE_CHANGE_STATUS_MASK_8TO11);
}

static int amd_sdw_set_device_state(struct amd_sdw_manager *amd_manager, u32 target_device_state)
{
	u32 sdw_dev_state;

	sdw_dev_state = readl(amd_manager->acp_mmio + AMD_SDW_DEVICE_STATE);
	switch (amd_manager->instance) {
	case ACP_SDW0:
		u32p_replace_bits(&sdw_dev_state, target_device_state,
				  AMD_SDW0_DEVICE_STATE_MASK);
		break;
	case ACP_SDW1:
		u32p_replace_bits(&sdw_dev_state, target_device_state,
				  AMD_SDW1_DEVICE_STATE_MASK);
		break;
	default:
		return -EINVAL;
	}
	writel(sdw_dev_state, amd_manager->acp_mmio + AMD_SDW_DEVICE_STATE);
	sdw_dev_state = readl(amd_manager->acp_mmio + AMD_SDW_DEVICE_STATE);
	dev_dbg(amd_manager->dev, "AMD_SDW_DEVICE_STATE:0x%x\n", sdw_dev_state);
	return 0;
}

static int amd_sdw_host_wake_enable(struct amd_sdw_manager *amd_manager, bool enable)
{
	u32 intr_cntl1;
	u32 sdw_host_wake_irq_mask;

	if (!amd_manager->wake_en_mask)
		return 0;

	switch (amd_manager->instance) {
	case ACP_SDW0:
		sdw_host_wake_irq_mask = AMD_SDW0_HOST_WAKE_INTR_MASK;
		break;
	case ACP_SDW1:
		sdw_host_wake_irq_mask = AMD_SDW1_HOST_WAKE_INTR_MASK;
		break;
	default:
		return -EINVAL;
	}

	intr_cntl1 = readl(amd_manager->acp_mmio + ACP_EXTERNAL_INTR_CNTL(ACP_SDW1));
	if (enable)
		intr_cntl1 |= sdw_host_wake_irq_mask;
	else
		intr_cntl1 &= ~sdw_host_wake_irq_mask;
	writel(intr_cntl1, amd_manager->acp_mmio + ACP_EXTERNAL_INTR_CNTL(ACP_SDW1));
	return 0;
}

static void amd_sdw_ctl_word_prep(u32 *lower_word, u32 *upper_word, struct sdw_msg *msg,
				  int cmd_offset)
{
	u32 upper_data;
	u32 lower_data = 0;
	u16 addr;
	u8 upper_addr, lower_addr;
	u8 data = 0;

	addr = msg->addr + cmd_offset;
	upper_addr = (addr & 0xFF00) >> 8;
	lower_addr = addr & 0xFF;

	if (msg->flags == SDW_MSG_FLAG_WRITE)
		data = msg->buf[cmd_offset];

	upper_data = FIELD_PREP(AMD_SDW_MCP_CMD_DEV_ADDR, msg->dev_num);
	upper_data |= FIELD_PREP(AMD_SDW_MCP_CMD_COMMAND, msg->flags + 2);
	upper_data |= FIELD_PREP(AMD_SDW_MCP_CMD_REG_ADDR_HIGH, upper_addr);
	lower_data |= FIELD_PREP(AMD_SDW_MCP_CMD_REG_ADDR_LOW, lower_addr);
	lower_data |= FIELD_PREP(AMD_SDW_MCP_CMD_REG_DATA, data);

	*upper_word = upper_data;
	*lower_word = lower_data;
}

static u64 amd_sdw_send_cmd_get_resp(struct amd_sdw_manager *amd_manager, u32 lower_data,
				     u32 upper_data)
{
	u64 resp;
	u32 lower_resp, upper_resp;
	u32 sts;
	int ret;

	ret = readl_poll_timeout(amd_manager->mmio + ACP_SW_IMM_CMD_STS, sts,
				 !(sts & AMD_SDW_IMM_CMD_BUSY), ACP_DELAY_US, AMD_SDW_TIMEOUT);
	if (ret) {
		dev_err(amd_manager->dev, "SDW%x previous cmd status clear failed\n",
			amd_manager->instance);
		return ret;
	}

	if (sts & AMD_SDW_IMM_RES_VALID) {
		dev_warn(amd_manager->dev, "SDW%x stale IMM response cleared\n",
			 amd_manager->instance);
		writel(AMD_SDW_IMM_RES_VALID, amd_manager->mmio + ACP_SW_IMM_CMD_STS);
	}
	writel(upper_data, amd_manager->mmio + ACP_SW_IMM_CMD_UPPER_WORD);
	writel(lower_data, amd_manager->mmio + ACP_SW_IMM_CMD_LOWER_QWORD);

	ret = readl_poll_timeout(amd_manager->mmio + ACP_SW_IMM_CMD_STS, sts,
				 (sts & AMD_SDW_IMM_RES_VALID), ACP_DELAY_US, AMD_SDW_TIMEOUT);
	if (ret) {
		dev_err(amd_manager->dev, "SDW%x cmd response timeout occurred\n",
			amd_manager->instance);
		return ret;
	}
	upper_resp = readl(amd_manager->mmio + ACP_SW_IMM_RESP_UPPER_WORD);
	lower_resp = readl(amd_manager->mmio + ACP_SW_IMM_RESP_LOWER_QWORD);

	writel(AMD_SDW_IMM_RES_VALID, amd_manager->mmio + ACP_SW_IMM_CMD_STS);
	ret = readl_poll_timeout(amd_manager->mmio + ACP_SW_IMM_CMD_STS, sts,
				 !(sts & AMD_SDW_IMM_RES_VALID), ACP_DELAY_US, AMD_SDW_TIMEOUT);
	if (ret) {
		dev_err(amd_manager->dev, "SDW%x cmd status retry failed\n",
			amd_manager->instance);
		return ret;
	}
	resp = upper_resp;
	resp = (resp << 32) | lower_resp;
	return resp;
}

static enum sdw_command_response
amd_program_scp_addr(struct amd_sdw_manager *amd_manager, struct sdw_msg *msg)
{
	struct sdw_msg scp_msg = {0};
	u64 response_buf[2] = {0};
	u32 upper_data = 0, lower_data = 0;
	int index;

	scp_msg.dev_num = msg->dev_num;
	scp_msg.addr = SDW_SCP_ADDRPAGE1;
	scp_msg.buf = &msg->addr_page1;
	scp_msg.flags = SDW_MSG_FLAG_WRITE;
	amd_sdw_ctl_word_prep(&lower_data, &upper_data, &scp_msg, 0);
	response_buf[0] = amd_sdw_send_cmd_get_resp(amd_manager, lower_data, upper_data);
	scp_msg.addr = SDW_SCP_ADDRPAGE2;
	scp_msg.buf = &msg->addr_page2;
	amd_sdw_ctl_word_prep(&lower_data, &upper_data, &scp_msg, 0);
	response_buf[1] = amd_sdw_send_cmd_get_resp(amd_manager, lower_data, upper_data);

	for (index = 0; index < 2; index++) {
		if (response_buf[index] == -ETIMEDOUT) {
			dev_err_ratelimited(amd_manager->dev,
					    "SCP_addrpage command timeout for Slave %d\n",
					    msg->dev_num);
			return SDW_CMD_TIMEOUT;
		} else if (!(response_buf[index] & AMD_SDW_MCP_RESP_ACK)) {
			if (response_buf[index] & AMD_SDW_MCP_RESP_NACK) {
				dev_err_ratelimited(amd_manager->dev,
						    "SCP_addrpage NACKed for Slave %d\n",
						    msg->dev_num);
				return SDW_CMD_FAIL;
			}
			dev_dbg_ratelimited(amd_manager->dev, "SCP_addrpage ignored for Slave %d\n",
					    msg->dev_num);
			return SDW_CMD_IGNORED;
		}
	}
	return SDW_CMD_OK;
}

static int amd_prep_msg(struct amd_sdw_manager *amd_manager, struct sdw_msg *msg)
{
	int ret;

	if (msg->page) {
		ret = amd_program_scp_addr(amd_manager, msg);
		if (ret) {
			msg->len = 0;
			return ret;
		}
	}
	switch (msg->flags) {
	case SDW_MSG_FLAG_READ:
	case SDW_MSG_FLAG_WRITE:
		break;
	default:
		dev_err(amd_manager->dev, "Invalid msg cmd: %d\n", msg->flags);
		return -EINVAL;
	}
	return 0;
}

static enum sdw_command_response amd_sdw_fill_msg_resp(struct amd_sdw_manager *amd_manager,
						       struct sdw_msg *msg, u64 response,
						       int offset)
{
	if (response & AMD_SDW_MCP_RESP_ACK) {
		if (msg->flags == SDW_MSG_FLAG_READ)
			msg->buf[offset] = FIELD_GET(AMD_SDW_MCP_RESP_RDATA, response);
	} else {
		if (response == -ETIMEDOUT) {
			dev_err_ratelimited(amd_manager->dev, "command timeout for Slave %d\n",
					    msg->dev_num);
			return SDW_CMD_TIMEOUT;
		} else if (response & AMD_SDW_MCP_RESP_NACK) {
			dev_err_ratelimited(amd_manager->dev,
					    "command response NACK received for Slave %d\n",
					    msg->dev_num);
			return SDW_CMD_FAIL;
		}
		dev_dbg_ratelimited(amd_manager->dev, "command is ignored for Slave %d\n",
				    msg->dev_num);
		return SDW_CMD_IGNORED;
	}
	return SDW_CMD_OK;
}

static unsigned int _amd_sdw_xfer_msg(struct amd_sdw_manager *amd_manager, struct sdw_msg *msg,
				      int cmd_offset)
{
	u64 response;
	u32 upper_data = 0, lower_data = 0;

	amd_sdw_ctl_word_prep(&lower_data, &upper_data, msg, cmd_offset);
	response = amd_sdw_send_cmd_get_resp(amd_manager, lower_data, upper_data);
	return amd_sdw_fill_msg_resp(amd_manager, msg, response, cmd_offset);
}

static enum sdw_command_response amd_sdw_xfer_msg(struct sdw_bus *bus, struct sdw_msg *msg)
{
	struct amd_sdw_manager *amd_manager = to_amd_sdw(bus);
	int ret, i;

	ret = amd_prep_msg(amd_manager, msg);
	if (ret)
		return SDW_CMD_FAIL_OTHER;
	for (i = 0; i < msg->len; i++) {
		ret = _amd_sdw_xfer_msg(amd_manager, msg, i);
		if (ret)
			return ret;
	}
	return SDW_CMD_OK;
}

static void amd_sdw_fill_slave_status(struct amd_sdw_manager *amd_manager, u16 index, u32 status)
{
	switch (status) {
	case SDW_SLAVE_ATTACHED:
	case SDW_SLAVE_UNATTACHED:
	case SDW_SLAVE_ALERT:
		amd_manager->status[index] = status;
		break;
	default:
		amd_manager->status[index] = SDW_SLAVE_RESERVED;
		break;
	}
}

static void amd_sdw_process_ping_status(u64 response, struct amd_sdw_manager *amd_manager)
{
	u64 slave_stat;
	u32 val;
	u16 dev_index;

	/* slave status response */
	slave_stat = FIELD_GET(AMD_SDW_MCP_SLAVE_STAT_0_3, response);
	slave_stat |= FIELD_GET(AMD_SDW_MCP_SLAVE_STAT_4_11, response) << 8;
	dev_dbg(amd_manager->dev, "slave_stat:0x%llx\n", slave_stat);
	for (dev_index = 0; dev_index <= SDW_MAX_DEVICES; ++dev_index) {
		val = (slave_stat >> (dev_index * 2)) & AMD_SDW_MCP_SLAVE_STATUS_MASK;
		dev_dbg(amd_manager->dev, "val:0x%x\n", val);
		amd_sdw_fill_slave_status(amd_manager, dev_index, val);
	}
}

static void amd_sdw_read_and_process_ping_status(struct amd_sdw_manager *amd_manager)
{
	u64 response;

	mutex_lock(&amd_manager->bus.msg_lock);
	response = amd_sdw_send_cmd_get_resp(amd_manager, 0, 0);
	mutex_unlock(&amd_manager->bus.msg_lock);
	amd_sdw_process_ping_status(response, amd_manager);
}

static u32 amd_sdw_read_ping_status(struct sdw_bus *bus)
{
	struct amd_sdw_manager *amd_manager = to_amd_sdw(bus);
	u64 response;
	u32 slave_stat;

	response = amd_sdw_send_cmd_get_resp(amd_manager, 0, 0);
	/* slave status from ping response */
	slave_stat = FIELD_GET(AMD_SDW_MCP_SLAVE_STAT_0_3, response);
	slave_stat |= FIELD_GET(AMD_SDW_MCP_SLAVE_STAT_4_11, response) << 8;
	dev_dbg(amd_manager->dev, "slave_stat:0x%x\n", slave_stat);
	return slave_stat;
}

/*
 * amd_sdw_bra_sample_interval() - Derive the BRA SampleInterval
 *
 * The ACP BRA hardware descriptor and the peripheral DP0 must be
 * programmed with an identical SampleInterval, otherwise the frame
 * layout seen by the two ends diverges and the transfer misaligns.
 * Both amd_sdw_compute_params() (peripheral DP0) and
 * amd_sdw_calculate_bra_params() (ACP BRA descriptor) use this helper
 * so the value can never diverge.
 *
 * BlockCount algorithm (col_width = hstop - hstart + 1):
 *  - col_width >= 8: BlockCount = 1, SI = nc
 *  - col_width == 1: SI = nc * 8
 *  - otherwise:      smallest BlockCount in [2..8] such that
 *                    (BlockCount * col_width >= 8) && (nr % BlockCount == 0),
 *                    SI = nc * BlockCount
 *
 * Returns 0 if no valid BlockCount can be found.
 */
static u32 amd_sdw_bra_sample_interval(u32 nr, u32 nc, u8 hstart, u8 hstop)
{
	u8 col_width = hstop - hstart + 1;
	u32 block_count;

	if (col_width >= 8)
		return nc;		/* BlockCount = 1 */
	if (col_width == 1)
		return nc * 8;

	for (block_count = 2; block_count <= 8; block_count++) {
		if ((block_count * col_width >= 8) &&
		    (nr % block_count == 0))
			return nc * block_count;
	}

	return 0;
}

static int amd_sdw_compute_params(struct sdw_bus *bus, struct sdw_stream_runtime *stream)
{
	struct amd_sdw_manager *amd_manager = to_amd_sdw(bus);
	struct sdw_transport_data t_data = {0};
	struct sdw_master_runtime *m_rt;
	struct sdw_port_runtime *p_rt;
	struct sdw_slave_runtime *s_rt;
	struct sdw_bus_params *b_params = &bus->params;
	int port_bo, hstart, hstop, sample_int;
	unsigned int rate, bps, channels;
	unsigned int stream_slot_size, max_slots;
	static unsigned int next_offset[AMD_SDW_MAX_MANAGER_COUNT] = {1};
	unsigned int inst_id = amd_manager->instance;

	/*
	 * BPT stream: compute DP0 transport/port params so the SoundWire stream
	 * framework can program peripheral DP0 registers in sdw_prepare_stream().
	 */
	if (stream->type == SDW_STREAM_BPT) {
		u32 nc = bus->params.col;
		u8 bpt_hstart = amd_manager->bra_hstart;
		u8 bpt_hstop = amd_manager->bra_hstop;
		u32 bpt_si = amd_sdw_bra_sample_interval(bus->params.row, nc,
							 bpt_hstart, bpt_hstop);

		if (!bpt_si || bpt_si > bus->params.row * nc) {
			dev_err(bus->dev,
				"BPT: cannot derive SI: NR=%u NC=%u hstart=%u hstop=%u\n",
				bus->params.row, nc, bpt_hstart, bpt_hstop);
			return -EINVAL;
		}

		dev_dbg(bus->dev, "BPT compute: NC=%u hstart=%u hstop=%u SI=%u\n",
			nc, bpt_hstart, bpt_hstop, bpt_si);

		list_for_each_entry(m_rt, &bus->m_rt_list, bus_node) {
			if (m_rt->stream != stream)
				continue;

			list_for_each_entry(p_rt, &m_rt->port_list, port_node) {
				sdw_fill_xport_params(&p_rt->transport_params,
						      p_rt->num, false,
						      SDW_BLK_GRP_CNT_1, bpt_si,
						      0, 0, bpt_hstart, bpt_hstop,
						      SDW_BLK_PKG_PER_PORT, 0);
				sdw_fill_port_params(&p_rt->port_params,
						     p_rt->num, 8,
						     SDW_PORT_FLOW_MODE_ISOCH,
						     SDW_PORT_DATA_MODE_NORMAL);
			}

			list_for_each_entry(s_rt, &m_rt->slave_rt_list, m_rt_node) {
				list_for_each_entry(p_rt, &s_rt->port_list, port_node) {
					sdw_fill_xport_params(&p_rt->transport_params,
							      p_rt->num, false,
							      SDW_BLK_GRP_CNT_1, bpt_si,
							      0, 0, bpt_hstart, bpt_hstop,
							      SDW_BLK_PKG_PER_PORT, 0);
					sdw_fill_port_params(&p_rt->port_params,
							     p_rt->num, 8,
							     SDW_PORT_FLOW_MODE_ISOCH,
							     SDW_PORT_DATA_MODE_NORMAL);
				}
			}
		}
		return 0;
	}

	port_bo = 0;
	hstart = 1;
	hstop = bus->params.col - 1;
	t_data.hstop = hstop;
	t_data.hstart = hstart;

	list_for_each_entry(m_rt, &bus->m_rt_list, bus_node) {
		/*
		 * Skip BPT stream entries when computing audio params.
		 * BPT may have stream params (rate/bps) that don't match the
		 * assumptions below and can lead to division by zero.
		 */
		if (m_rt->stream->type == SDW_STREAM_BPT)
			continue;

		rate = m_rt->stream->params.rate;
		bps = m_rt->stream->params.bps;
		channels = m_rt->stream->params.ch_count;
		sample_int = (bus->params.curr_dr_freq / rate);

		/* Compute slots required for this stream dynamically */
		stream_slot_size = bps * channels;

		list_for_each_entry(p_rt, &m_rt->port_list, port_node) {
			if (p_rt->num >= amd_manager->max_ports) {
				dev_err(bus->dev, "Port %d exceeds max ports %d\n",
					p_rt->num, amd_manager->max_ports);
				return -EINVAL;
			}

			if (!amd_manager->port_offset_map[p_rt->num]) {
				/*
				 * port block offset calculation for 6MHz bus clock frequency with
				 * different frame sizes 50 x 10 and 125 x 2
				 */
				if (bus->params.curr_dr_freq == 12000000) {
					max_slots = bus->params.row * (bus->params.col - 1);
					if (next_offset[inst_id] + stream_slot_size <=
					    (max_slots - 1)) {
						amd_manager->port_offset_map[p_rt->num] =
									next_offset[inst_id];
						next_offset[inst_id] += stream_slot_size;
					} else {
						dev_err(bus->dev,
							"No space for port %d\n", p_rt->num);
						return -ENOMEM;
					}
				} else {
					 /*
					  * port block offset calculation for 12MHz bus clock
					  * frequency
					  */
					amd_manager->port_offset_map[p_rt->num] =
									(p_rt->num * 64) + 1;
				}
			}
			port_bo = amd_manager->port_offset_map[p_rt->num];
			dev_dbg(bus->dev,
				"Port=%d hstart=%d hstop=%d port_bo=%d slots=%d max_ports=%d\n",
				p_rt->num, hstart, hstop, port_bo, stream_slot_size,
				amd_manager->max_ports);

			sdw_fill_xport_params(&p_rt->transport_params, p_rt->num,
					      false, SDW_BLK_GRP_CNT_1, sample_int,
					      port_bo, port_bo >> 8, hstart, hstop,
					      SDW_BLK_PKG_PER_PORT, p_rt->lane);

			sdw_fill_port_params(&p_rt->port_params,
					     p_rt->num, bps,
					     SDW_PORT_FLOW_MODE_ISOCH,
					     b_params->m_data_mode);
			t_data.hstart = hstart;
			t_data.hstop = hstop;
			t_data.block_offset = port_bo;
			t_data.sub_block_offset = 0;
		}
		sdw_compute_slave_ports(m_rt, &t_data);
	}
	return 0;
}

static int amd_sdw_port_params(struct sdw_bus *bus, struct sdw_port_params *p_params,
			       unsigned int bank)
{
	struct amd_sdw_manager *amd_manager = to_amd_sdw(bus);
	u32 frame_fmt_reg, dpn_frame_fmt;

	/*
	 * BPT uses dedicated ACP BRA descriptor registers; ignore DP0 ops only.
	 * Allow DPn audio port ops to proceed even while BPT is active.
	 */
	if (READ_ONCE(bus->bpt_stream) && p_params->num == 0)
		return 0;

	dev_dbg(amd_manager->dev, "p_params->num:0x%x\n", p_params->num);
	switch (amd_manager->acp_rev) {
	case ACP63_PCI_REV_ID:
		switch (amd_manager->instance) {
		case ACP_SDW0:
			frame_fmt_reg = acp63_sdw0_dp_reg[p_params->num].frame_fmt_reg;
			break;
		case ACP_SDW1:
			frame_fmt_reg = acp63_sdw1_dp_reg[p_params->num].frame_fmt_reg;
			break;
		default:
			return -EINVAL;
		}
		break;
	case ACP70_PCI_REV_ID:
	case ACP71_PCI_REV_ID:
	case ACP72_PCI_REV_ID:
		frame_fmt_reg = acp70_sdw_dp_reg[p_params->num].frame_fmt_reg;
		break;
	default:
		return -EINVAL;
	}

	dpn_frame_fmt = readl(amd_manager->mmio + frame_fmt_reg);
	u32p_replace_bits(&dpn_frame_fmt, p_params->flow_mode, AMD_DPN_FRAME_FMT_PFM);
	u32p_replace_bits(&dpn_frame_fmt, p_params->data_mode, AMD_DPN_FRAME_FMT_PDM);
	u32p_replace_bits(&dpn_frame_fmt, p_params->bps - 1, AMD_DPN_FRAME_FMT_WORD_LEN);
	writel(dpn_frame_fmt, amd_manager->mmio + frame_fmt_reg);
	return 0;
}

static int amd_sdw_transport_params(struct sdw_bus *bus,
				    struct sdw_transport_params *params,
				    enum sdw_reg_bank bank)
{
	struct amd_sdw_manager *amd_manager = to_amd_sdw(bus);
	u32 dpn_frame_fmt;
	u32 dpn_sampleinterval;
	u32 dpn_hctrl;
	u32 dpn_offsetctrl;
	u32 dpn_lanectrl;
	u32 frame_fmt_reg, sample_int_reg, hctrl_dp0_reg;
	u32 offset_reg, lane_ctrl_ch_en_reg;

	/*
	 * BPT uses dedicated ACP BRA descriptor registers; ignore DP0 ops only.
	 * Allow DPn audio port ops to proceed even while BPT is active.
	 */
	if (READ_ONCE(bus->bpt_stream) && params->port_num == 0)
		return 0;

	switch (amd_manager->acp_rev) {
	case ACP63_PCI_REV_ID:
		switch (amd_manager->instance) {
		case ACP_SDW0:
			frame_fmt_reg = acp63_sdw0_dp_reg[params->port_num].frame_fmt_reg;
			sample_int_reg = acp63_sdw0_dp_reg[params->port_num].sample_int_reg;
			hctrl_dp0_reg = acp63_sdw0_dp_reg[params->port_num].hctrl_dp0_reg;
			offset_reg = acp63_sdw0_dp_reg[params->port_num].offset_reg;
			lane_ctrl_ch_en_reg =
					acp63_sdw0_dp_reg[params->port_num].lane_ctrl_ch_en_reg;
			break;
		case ACP_SDW1:
			frame_fmt_reg = acp63_sdw1_dp_reg[params->port_num].frame_fmt_reg;
			sample_int_reg = acp63_sdw1_dp_reg[params->port_num].sample_int_reg;
			hctrl_dp0_reg = acp63_sdw1_dp_reg[params->port_num].hctrl_dp0_reg;
			offset_reg = acp63_sdw1_dp_reg[params->port_num].offset_reg;
			lane_ctrl_ch_en_reg =
					acp63_sdw1_dp_reg[params->port_num].lane_ctrl_ch_en_reg;
			break;
		default:
			return -EINVAL;
		}
		break;
	case ACP70_PCI_REV_ID:
	case ACP71_PCI_REV_ID:
	case ACP72_PCI_REV_ID:
		frame_fmt_reg = acp70_sdw_dp_reg[params->port_num].frame_fmt_reg;
		sample_int_reg = acp70_sdw_dp_reg[params->port_num].sample_int_reg;
		hctrl_dp0_reg = acp70_sdw_dp_reg[params->port_num].hctrl_dp0_reg;
		offset_reg = acp70_sdw_dp_reg[params->port_num].offset_reg;
		lane_ctrl_ch_en_reg = acp70_sdw_dp_reg[params->port_num].lane_ctrl_ch_en_reg;
		break;
	default:
		return -EINVAL;
	}
	writel(AMD_SDW_SSP_COUNTER_VAL, amd_manager->mmio + ACP_SW_SSP_COUNTER);

	dpn_frame_fmt = readl(amd_manager->mmio + frame_fmt_reg);
	u32p_replace_bits(&dpn_frame_fmt, params->blk_pkg_mode, AMD_DPN_FRAME_FMT_BLK_PKG_MODE);
	u32p_replace_bits(&dpn_frame_fmt, params->blk_grp_ctrl, AMD_DPN_FRAME_FMT_BLK_GRP_CTRL);
	u32p_replace_bits(&dpn_frame_fmt, SDW_STREAM_PCM, AMD_DPN_FRAME_FMT_PCM_OR_PDM);
	writel(dpn_frame_fmt, amd_manager->mmio + frame_fmt_reg);

	dpn_sampleinterval = params->sample_interval - 1;
	writel(dpn_sampleinterval, amd_manager->mmio + sample_int_reg);

	dpn_hctrl = FIELD_PREP(AMD_DPN_HCTRL_HSTOP, params->hstop);
	dpn_hctrl |= FIELD_PREP(AMD_DPN_HCTRL_HSTART, params->hstart);
	writel(dpn_hctrl, amd_manager->mmio + hctrl_dp0_reg);

	dpn_offsetctrl = FIELD_PREP(AMD_DPN_OFFSET_CTRL_1, params->offset1);
	dpn_offsetctrl |= FIELD_PREP(AMD_DPN_OFFSET_CTRL_2, params->offset2);
	writel(dpn_offsetctrl, amd_manager->mmio + offset_reg);

	/*
	 * lane_ctrl_ch_en_reg will be used to program lane_ctrl and ch_mask
	 * parameters.
	 */
	dpn_lanectrl = readl(amd_manager->mmio + lane_ctrl_ch_en_reg);
	u32p_replace_bits(&dpn_lanectrl, params->lane_ctrl, AMD_DPN_CH_EN_LCTRL);
	writel(dpn_lanectrl, amd_manager->mmio + lane_ctrl_ch_en_reg);
	return 0;
}

static int amd_sdw_port_enable(struct sdw_bus *bus,
			       struct sdw_enable_ch *enable_ch,
			       unsigned int bank)
{
	struct amd_sdw_manager *amd_manager = to_amd_sdw(bus);
	u32 dpn_ch_enable;
	u32 lane_ctrl_ch_en_reg;

	/*
	 * BPT port enable/disable is handled in execute_bra_transfer; ignore
	 * DP0 ops only. Allow DPn audio port ops even while BPT is active.
	 */
	if (READ_ONCE(bus->bpt_stream) && enable_ch->port_num == 0)
		return 0;

	switch (amd_manager->acp_rev) {
	case ACP63_PCI_REV_ID:
		switch (amd_manager->instance) {
		case ACP_SDW0:
			lane_ctrl_ch_en_reg =
					acp63_sdw0_dp_reg[enable_ch->port_num].lane_ctrl_ch_en_reg;
			break;
		case ACP_SDW1:
			lane_ctrl_ch_en_reg =
					acp63_sdw1_dp_reg[enable_ch->port_num].lane_ctrl_ch_en_reg;
			break;
		default:
			return -EINVAL;
		}
		break;
	case ACP70_PCI_REV_ID:
	case ACP71_PCI_REV_ID:
	case ACP72_PCI_REV_ID:
		lane_ctrl_ch_en_reg = acp70_sdw_dp_reg[enable_ch->port_num].lane_ctrl_ch_en_reg;
		break;
	default:
		return -EINVAL;
	}

	/*
	 * lane_ctrl_ch_en_reg will be used to program lane_ctrl and ch_mask
	 * parameters.
	 */
	dpn_ch_enable = readl(amd_manager->mmio + lane_ctrl_ch_en_reg);
	u32p_replace_bits(&dpn_ch_enable, enable_ch->ch_mask, AMD_DPN_CH_EN_CHMASK);
	if (enable_ch->enable)
		writel(dpn_ch_enable, amd_manager->mmio + lane_ctrl_ch_en_reg);
	else
		writel(0, amd_manager->mmio + lane_ctrl_ch_en_reg);
	return 0;
}

static int amd_sdw_calculate_bra_params(struct amd_sdw_manager *amd_manager,
					struct amd_bra_params *params,
					u8 peripheral_addr)
{
	struct sdw_bus *bus = &amd_manager->bus;
	u32 nr = bus->params.row;  /* current rows - NOT enlarged */
	u32 nc = bus->params.col;  /* current cols */
	u8 hstart = amd_manager->bra_hstart;
	u8 hstop  = amd_manager->bra_hstop;
	u8 col_width = hstop - hstart + 1;
	u32 sample_interval;
	u32 bits_per_frame;
	u32 bpf;

	params->hstart      = hstart;
	params->hstop       = hstop;
	params->word_length = 8;  /* BRA is always byte-oriented: WL=8 */

	/*
	 * Derive SampleInterval via the shared helper so the ACP BRA
	 * descriptor and the peripheral DP0 (programmed in
	 * amd_sdw_compute_params()) always agree.
	 */
	sample_interval = amd_sdw_bra_sample_interval(nr, nc, hstart, hstop);

	if (!sample_interval || sample_interval > nr * nc) {
		dev_err(amd_manager->dev,
			"BPT: cannot derive SI: col_width=%u NR=%u NC=%u\n",
			col_width, nr, nc);
		return -EINVAL;
	}

	bits_per_frame = ((nr * nc) / sample_interval) * params->word_length;
	bpf = bits_per_frame / 8;
	if (bpf <= 10) {
		dev_err(amd_manager->dev,
			"BPT: frame too small: %u bytes (NR=%u NC=%u SI=%u)\n",
			bpf, nr, nc, sample_interval);
		return -EINVAL;
	}
	bpf -= 10;  /* subtract BRA protocol overhead */
	if (bpf > 511)
		bpf = 511;

	params->sample_interval = sample_interval;
	params->bytes_per_frame = bpf;
	params->dev_addr        = peripheral_addr;

	dev_dbg(amd_manager->dev,
		"BPT calc_params: NR=%u NC=%u col_width=%u WL=%u SI=%u BPF=%u hstart=%u hstop=%u dev=%u\n",
		nr, nc, col_width, params->word_length, sample_interval,
		bpf, hstart, hstop, peripheral_addr);
	return 0;
}

static u32 amd_sdw_bra_configure_pte(struct amd_sdw_manager *amd_manager,
				     dma_addr_t dma_addr, size_t size)
{
	u32 num_pages = (u32)(PAGE_ALIGN(size) >> PAGE_SHIFT);
	u32 low, high, val;
	u16 page_idx;
	dma_addr_t addr = dma_addr;

	if (num_pages > ACP_BRA_MAX_PTE_ENTRIES) {
		dev_err(amd_manager->dev,
			"BRA buffer too large: %u pages (max %u)\n",
			num_pages, ACP_BRA_MAX_PTE_ENTRIES);
		return 0;
	}

	/*
	 * Program ATU GRP_1 with 4KB pages. PTE entries start at scratch offset
	 * ACP_BRA_PTE_OFFSET (0x0); DMA AXI base = ACP_BRA_MEM_WINDOW_START.
	 *
	 * The ATU and scratch registers are ACP-global, so they must be
	 * accessed via acp_mmio (the shared ACP base) rather than mmio
	 * (which carries the per-instance SDW_MANAGER_REG_OFFSET). Otherwise
	 * SoundWire instance 1 would program the wrong physical addresses.
	 */
	writel(ACP_BRA_SRAM_GRP1_BASE | BIT(31),
	       amd_manager->acp_mmio + ACPAXI2AXI_ATU_BASE_ADDR_GRP_1);
	writel(ACP_BRA_PAGE_SIZE_4K_ENABLE,
	       amd_manager->acp_mmio + ACPAXI2AXI_ATU_PAGE_SIZE_GRP_1);

	val = ACP_BRA_PTE_OFFSET;
	for (page_idx = 0; page_idx < num_pages; page_idx++) {
		low  = lower_32_bits(addr);
		high = upper_32_bits(addr) | BIT(31);
		writel(low,  amd_manager->acp_mmio + ACP_SCRATCH_REG_0 + val);
		writel(high, amd_manager->acp_mmio + ACP_SCRATCH_REG_0 + val + 4);
		val += ACP_BRA_ATU_PTE_ENTRY_SIZE;
		addr += PAGE_SIZE;
	}

	/* Flush ATU cache to ensure PTE update takes effect */
	writel(0x1, amd_manager->acp_mmio + ACPAXI2AXI_ATU_CTRL);

	dev_dbg(amd_manager->dev,
		"BRA PTE: phys=0x%llx pages=%u ACP=0x%08x\n",
		(u64)dma_addr, num_pages, ACP_BRA_MEM_WINDOW_START);

	return ACP_BRA_MEM_WINDOW_START;
}

static void amd_sdw_bra_deconfigure_pte(struct amd_sdw_manager *amd_manager,
					size_t size)
{
	u32 num_pages = (u32)(PAGE_ALIGN(size) >> PAGE_SHIFT);
	u32 val;
	u16 page_idx;

	if (num_pages > ACP_BRA_MAX_PTE_ENTRIES)
		num_pages = ACP_BRA_MAX_PTE_ENTRIES;

	/* Clear all BRA PTE entries at scratch[ACP_BRA_PTE_OFFSET..] */
	val = ACP_BRA_PTE_OFFSET;
	for (page_idx = 0; page_idx < num_pages; page_idx++) {
		writel(0, amd_manager->acp_mmio + ACP_SCRATCH_REG_0 + val);
		writel(0, amd_manager->acp_mmio + ACP_SCRATCH_REG_0 + val + 4);
		val += ACP_BRA_ATU_PTE_ENTRY_SIZE;
	}

	/* Flush ATU cache */
	writel(0x1, amd_manager->acp_mmio + ACPAXI2AXI_ATU_CTRL);
}

static int amd_sdw_config_bra_descriptor(struct amd_sdw_manager *amd_manager,
					 struct amd_bra_params *params)
{
	u32 frame_format, hctrl;
	u32 int_mask;

	if (params->dev_addr > 15 ||
	    params->sample_interval == 0 || params->sample_interval > 65536 ||
	    params->hstart > 15 || params->hstop > 15 ||
	    params->word_length == 0 || params->word_length > 64 ||
	    params->bytes_per_frame > 511)
		return -EINVAL;

	/*
	 * ACP BPT_PORT_HCTRL: encode as (hstart << 4) | hstop.
	 * The ACP BRA engine starts reading from column hstart (including
	 * the BRA frame header).  No offset adjustment is needed.
	 */
	hctrl = (u32)((params->hstart << 4) | params->hstop);

	frame_format =
		((u32)(params->word_length - 1) << 2)  |  /* [7:2]  WordLength-1       */
		(params->write_mode ? BIT(10) : 0U)    |  /* [10]   Write/Read         */
		((u32)params->bytes_per_frame << 11)   |  /* [19:11] BytesPerFrame     */
		((u32)params->dev_addr << 20);		   /* [23:20] DeviceAddr        */

	/* Mask BRA error interrupts while programming descriptor */
	int_mask = readl(amd_manager->mmio + ACP_SW_ERROR_INTR_MASK);
	int_mask &= ~(u32)AMD_SDW_BPT_ERR_INTR_MASK;
	writel(int_mask, amd_manager->mmio + ACP_SW_ERROR_INTR_MASK);

	writel(frame_format,                  amd_manager->mmio + ACP_SW_BPT_PORT_FRAME_FORMAT);
	writel(params->sample_interval - 1,   amd_manager->mmio + ACP_SW_BPT_PORT_SAMPLEINTERVAL);
	writel(hctrl,                         amd_manager->mmio + ACP_SW_BPT_PORT_HCTRL);
	writel(0,                             amd_manager->mmio + ACP_SW_BPT_PORT_OFFSET);
	writel(BIT(3),                        amd_manager->mmio + ACP_SW_BPT_PORT_CHANNEL_ENABLE);
	writel(params->peripheral_first_byte_addr,
	       amd_manager->mmio + ACP_SW_BPT_PORT_FIRST_BYTE_ADDR);
	writel(params->dma_base_addr,         amd_manager->mmio + ACP_SW_BRA_BASE_ADDRESS);
	writel(params->transfer_length,       amd_manager->mmio + ACP_SW_BRA_TRANSFER_SIZE);

	int_mask |= AMD_SDW_BPT_ERR_INTR_MASK;
	writel(int_mask, amd_manager->mmio + ACP_SW_ERROR_INTR_MASK);

	dev_dbg(amd_manager->dev,
		"BPT config_desc: FF=0x%08x SI=0x%x HC=0x%02x CE=0x%02x periph=0x%08x dma=0x%08x xfer=%u\n",
		frame_format, params->sample_interval - 1, hctrl, (u32)BIT(3),
		params->peripheral_first_byte_addr, params->dma_base_addr,
		params->transfer_length);
	return 0;
}

static void amd_sdw_deconfig_bra_descriptor(struct amd_sdw_manager *amd_manager)
{
	writel(0, amd_manager->mmio + ACP_SW_BPT_PORT_FRAME_FORMAT);
	writel(0, amd_manager->mmio + ACP_SW_BPT_PORT_SAMPLEINTERVAL);
	writel(0, amd_manager->mmio + ACP_SW_BPT_PORT_HCTRL);
	writel(0, amd_manager->mmio + ACP_SW_BPT_PORT_OFFSET);
	writel(0, amd_manager->mmio + ACP_SW_BPT_PORT_CHANNEL_ENABLE);
	writel(0, amd_manager->mmio + ACP_SW_BPT_PORT_FIRST_BYTE_ADDR);
	writel(0, amd_manager->mmio + ACP_SW_BRA_BASE_ADDRESS);
	writel(0, amd_manager->mmio + ACP_SW_BRA_TRANSFER_SIZE);
}

static int amd_sdw_execute_bra_transfer(struct amd_sdw_manager *amd_manager,
					 struct sdw_slave *slave,
					 bool *dma_unsafe)
{
	struct sdw_bus *bus = &amd_manager->bus;
	u32 i2s_err_offset;
	u32 saved_intr_mask;
	u32 reg_addr, len;
	u32 val;
	int ret, ret_disable;

	/* Read descriptor regs before enabling the DMA engine. */
	reg_addr = readl(amd_manager->mmio + ACP_SW_BPT_PORT_FIRST_BYTE_ADDR);
	len = readl(amd_manager->mmio + ACP_SW_BRA_TRANSFER_SIZE);

	i2s_err_offset = (amd_manager->instance == 0) ?
			 ACP_SW_I2S_ERROR_REASON : ACP_P1_SW_I2S_ERROR_REASON;

	/*
	 * Save and disable the error interrupt mask for manual error
	 * checking.  acp_bra_lock is held across the whole BPT sequence by
	 * amd_sdw_bpt_wait(), which serialises this shared-register
	 * read-modify-write against the other manager instance.
	 */
	saved_intr_mask = readl(amd_manager->mmio + ACP_SW_ERROR_INTR_MASK);
	writel(0, amd_manager->mmio + ACP_SW_ERROR_INTR_MASK);
	writel(0, amd_manager->acp_mmio + i2s_err_offset);
	writel(0, amd_manager->mmio + ACP_SW_ERROR_REASON1);

	/* Arm the ACP BPT DMA engine */
	writel(1, amd_manager->mmio + ACP_SW_BPT_PORT_EN);

	/*
	 * Use the framework's sdw_enable_stream() to write CHANNELEN and
	 * perform a bank switch.  The ACP BPT hardware uses the bank switch
	 * as the trigger to start the DMA transfer.  The framework manages
	 * bank state consistently, eliminating the need for a manual bank
	 * switch or DP0 bank mirror.
	 */
	dev_dbg(amd_manager->dev,
		"BPT: pre-enable: curr_bank=%u next_bank=%u BPT_EN_STATUS=0x%x stream_state=%d\n",
		bus->params.curr_bank, bus->params.next_bank,
		readl(amd_manager->mmio + ACP_SW_BPT_PORT_EN_STATUS),
		bus->bpt_stream->state);

	ret = sdw_enable_stream(bus->bpt_stream);
	if (ret < 0) {
		dev_err(amd_manager->dev,
			"BPT: sdw_enable_stream failed: %d\n", ret);
		/*
		 * Disarm the engine and wait for the port to quiesce before
		 * returning: the caller (amd_sdw_bra_transfer()) then deconfigures
		 * the BRA descriptor unconditionally, zeroing BASE_ADDRESS and
		 * TRANSFER_SIZE, so the port must be idle first -- mirrors the
		 * disable path below.
		 */
		writel(0, amd_manager->mmio + ACP_SW_BPT_PORT_EN);
		ret_disable = readl_poll_timeout(amd_manager->mmio + ACP_SW_BPT_PORT_EN_STATUS,
						 val, !val, ACP_DELAY_US, AMD_SDW_TIMEOUT);
		if (ret_disable < 0) {
			dev_err(amd_manager->dev, "BPT: PORT_EN disable timeout\n");
			*dma_unsafe = true;
		}
		goto restore_intr;
	}

	dev_dbg(amd_manager->dev,
		"BPT: DMA started: curr_bank=%u next_bank=%u BPT_EN_STATUS=0x%x stream_state=%d\n",
		bus->params.curr_bank, bus->params.next_bank,
		readl(amd_manager->mmio + ACP_SW_BPT_PORT_EN_STATUS),
		bus->bpt_stream->state);

	/* Poll DMA_BUSY until transfer completes */
	{
		unsigned long timeout;

		timeout = jiffies + msecs_to_jiffies(BRA_DMA_TIMEOUT_MS);
		do {
			val = readl(amd_manager->mmio + ACP_SW_BRA_DMA_BUSY);
			/*
			 * Side-effectful read: reading ACP_SW_BRA_CURRENT_TRANSFER_SIZE
			 * advances the BRA DMA engine to the next frame.  The value is
			 * intentionally discarded (hence the (void) cast); removing this
			 * read stalls multi-frame transfers, which then time out.
			 */
			(void)readl(amd_manager->mmio + ACP_SW_BRA_CURRENT_TRANSFER_SIZE);
			if (!(val & 0x01))
				break;
			if (time_after(jiffies, timeout)) {
				dev_err(amd_manager->dev,
					"BPT: DMA timeout: periph=0x%08x len=%u EN_STATUS=0x%x RESP=0x%x I2S_ERR=0x%08x\n",
					reg_addr, len,
					readl(amd_manager->mmio + ACP_SW_BPT_PORT_EN_STATUS),
					readl(amd_manager->mmio + ACP_SW_BRA_RESP),
					readl(amd_manager->acp_mmio + i2s_err_offset));
				ret = -ETIMEDOUT;
				break;
			}
			/*
			 * Runs in process context. Yield the CPU so a
			 * multi-frame transfer cannot busy-spin for up to
			 * BRA_DMA_TIMEOUT_MS and trip the soft lockup
			 * watchdog on non-preemptible kernels. cond_resched()
			 * keeps the poll cadence tight -- reading
			 * ACP_SW_BRA_CURRENT_TRANSFER_SIZE every iteration is
			 * required to advance the DMA engine between frames,
			 * so usleep_range() (which would space out the reads)
			 * is deliberately not used here.
			 */
			cond_resched();
		} while (1);
	}

	/* Check for I2S/BRA errors */
	val = readl(amd_manager->acp_mmio + i2s_err_offset);
	if (val & AMD_SDW_BRA_I2S_ERROR_MASK) {
		dev_err(amd_manager->dev,
			"BPT: BRA failed I2S_ERROR_REASON=0x%08x (NAK=%u Clash=%u HdrResp=%u FtrResp=%u CRC=%u DMA=%u Cmd=%u)\n",
			val,
			!!(val & BIT(18)), !!(val & BIT(19)),
			!!(val & BIT(26)), !!(val & BIT(27)),
			!!(val & BIT(28)), !!(val & BIT(29)), !!(val & BIT(30)));
		writel(0, amd_manager->acp_mmio + i2s_err_offset);
		if (!ret)
			ret = -EIO;
	} else if (!ret) {
		dev_dbg(amd_manager->dev,
			"BPT: DMA done: periph=0x%08x len=%u\n", reg_addr, len);
	}

	/*
	 * On an aborted or timed-out transfer the ACP BPT DMA engine can still
	 * be armed (PORT_EN=1, DMA_BUSY=1). sdw_disable_stream() below performs
	 * a bank switch, which is the BPT DMA start trigger, so disarm the engine
	 * first -- otherwise the bank switch could (re)start a DMA write into
	 * dma_buf, which amd_sdw_bpt_wait() frees as soon as this transfer
	 * returns. On the success path the DMA has already completed, so this
	 * early clear is a no-op.
	 */
	if (ret < 0)
		writel(0, amd_manager->mmio + ACP_SW_BPT_PORT_EN);

	/*
	 * Disable the stream via framework (writes CHANNELEN=0 + bank switch).
	 * This cleanly stops the port and keeps bank state consistent.
	 * Propagate a disable failure: it leaves the stream in
	 * SDW_STREAM_ENABLED, and sdw_enable_stream() returns 0 early for an
	 * already-ENABLED stream without the bank switch that triggers the BRA
	 * DMA, so the next section would be silently skipped rather than
	 * transferred.  Report the failure to the caller so it stops instead of
	 * chaining a section whose DMA never starts.
	 */
	ret_disable = sdw_disable_stream(bus->bpt_stream);
	if (ret_disable < 0) {
		dev_err(amd_manager->dev, "BPT: sdw_disable_stream failed: %d\n",
			ret_disable);
		if (!ret)
			ret = ret_disable;
	}

	dev_dbg(amd_manager->dev,
		"BPT: post-disable: curr_bank=%u next_bank=%u stream_state=%d\n",
		bus->params.curr_bank, bus->params.next_bank,
		bus->bpt_stream->state);

	/*
	 * Disarm the ACP BPT DMA engine and wait for the port to quiesce.
	 * Like the manager enable/disable sequence (ACP_SW_EN paired with
	 * ACP_SW_EN_STATUS), ACP_SW_BPT_PORT_EN_STATUS reflects the real port
	 * state, so polling it here guarantees a non-contiguous transfer's next
	 * section cannot re-arm PORT_EN before the current one has torn down.
	 */
	writel(0, amd_manager->mmio + ACP_SW_BPT_PORT_EN);
	ret_disable = readl_poll_timeout(amd_manager->mmio + ACP_SW_BPT_PORT_EN_STATUS,
					 val, !val, ACP_DELAY_US, AMD_SDW_TIMEOUT);
	if (ret_disable < 0) {
		dev_err(amd_manager->dev, "BPT: PORT_EN disable timeout\n");
		if (!ret)
			ret = ret_disable;
		*dma_unsafe = true;
	}
	writel(0, amd_manager->acp_mmio + i2s_err_offset);
	writel(0, amd_manager->mmio + ACP_SW_ERROR_REASON1);
	/*
	 * Clearing the immediate-command response-valid status races with the
	 * immediate-command path (amd_sdw_send_cmd_get_resp()), which polls and
	 * clears the same ACP_SW_IMM_CMD_STS bit under bus->msg_lock. The BPT DMA
	 * poll loop above drops all locks, so a concurrent slave enumeration or
	 * register access (amd_sdw_work / IRQ thread / codec regmap) can be
	 * mid-command here. Take msg_lock so this teardown clear cannot erase a
	 * response the command path has not yet consumed, which would make that
	 * command time out. This msg_lock nests inside acp_bra_lock and bpt_lock,
	 * both already held by the enclosing amd_sdw_bpt_wait(), preserving the
	 * bpt_lock -> acp_bra_lock -> msg_lock order. do_bank_switch() inside
	 * sdw_enable_stream()/sdw_disable_stream() only takes msg_lock for
	 * multi-link managers, so this single-link manager establishes that order
	 * via the explicit msg_lock here and in the non-contiguous
	 * sdw_nwrite_no_pm()/sdw_nread_no_pm() fallback.
	 */
	mutex_lock(&bus->msg_lock);
	writel(AMD_SDW_IMM_RES_VALID, amd_manager->mmio + ACP_SW_IMM_CMD_STS);
	mutex_unlock(&bus->msg_lock);

restore_intr:
	writel(saved_intr_mask, amd_manager->mmio + ACP_SW_ERROR_INTR_MASK);
	return ret;
}

/**
 * amd_sdw_bra_transfer() - Execute a single-shot BRA DMA transfer
 * @amd_manager: AMD SoundWire manager
 * @slave: SoundWire slave device
 * @reg_addr: Peripheral register start address
 * @acp_base_addr: Pre-configured ACP system address for the DMA buffer
 * @len: Total number of bytes to transfer
 * @write: true for write, false for read
 *
 * Programs the BRA descriptor once with the full transfer length and triggers
 * a single DMA operation.  The ACP BRA hardware autonomously slices the
 * buffer into BPF-sized BRA frames, auto-incrementing PERIPHERAL_FIRST_BYTE_ADDR
 * and DMA_BASE_ADDRESS between frames.  The last frame may be shorter than BPF;
 * the hardware handles partial final frames correctly.
 *
 * The poll loop in amd_sdw_execute_bra_transfer() reads
 * ACP_SW_BRA_CURRENT_TRANSFER_SIZE in every iteration, which is required
 * to advance the DMA engine between frames.
 */
static int amd_sdw_bra_transfer(struct amd_sdw_manager *amd_manager,
			   struct sdw_slave *slave, u32 reg_addr,
			   u32 acp_base_addr, size_t len, bool write,
			   bool *dma_unsafe)
{
	struct amd_bra_params params = {0};
	int ret;

	ret = amd_sdw_calculate_bra_params(amd_manager, &params,
					   (u8)slave->dev_num);
	if (ret < 0)
		return ret;

	params.peripheral_first_byte_addr = reg_addr;
	params.dma_base_addr = acp_base_addr;
	params.transfer_length = (u32)len;
	params.write_mode = write;

	ret = amd_sdw_config_bra_descriptor(amd_manager, &params);
	if (ret < 0)
		return ret;

	ret = amd_sdw_execute_bra_transfer(amd_manager, slave, dma_unsafe);
	amd_sdw_deconfig_bra_descriptor(amd_manager);

	return ret;
}

/**
 * amd_sdw_bpt_open_stream() - Allocate and prepare BPT stream
 * @amd_manager: AMD SoundWire manager
 * @slave: SoundWire slave device
 * @msg: BPT message with transfer direction
 *
 * Allocates a SoundWire BPT stream and adds slave and master runtime
 * entries so that transport column params are visible to the port ops
 * callbacks. DP0 is programmed through the SoundWire stream framework in
 * amd_sdw_bpt_wait() immediately before the DMA transfer.
 */
static int amd_sdw_bpt_open_stream(struct amd_sdw_manager *amd_manager,
				   struct sdw_slave *slave,
				   struct sdw_bpt_msg *msg)
{
	struct sdw_bus *bus = &amd_manager->bus;
	struct sdw_stream_config sconfig = {0};
	struct sdw_port_config pconfig = {0};
	struct sdw_stream_runtime *stream;
	int ret;

	stream = sdw_alloc_stream("BPT", SDW_STREAM_BPT);
	if (!stream)
		return -ENOMEM;

	/*
	 * BPT has no PCM sample rate, but sdw_prepare_stream() still validates
	 * the stream rate against the bus clock: _sdw_prepare_stream() rejects a
	 * rate that does not evenly divide max_clk_freq ("Async mode not
	 * supported"). Use the bus frame rate, which the driver derives as
	 * curr_dr_freq / (rows * cols) and therefore divides the SoundWire clock
	 * by construction; this passes on any valid clock instead of only when
	 * max_clk_freq happens to be a multiple of 48 kHz. The DP0 sample interval
	 * used for the transfer is computed from the BRA frame geometry in
	 * amd_sdw_compute_params(), not from this rate.
	 */
	sconfig.frame_rate = bus->prop.default_frame_rate;
	sconfig.ch_count = 1;
	sconfig.bps = 8;
	sconfig.direction = (msg->flags & SDW_MSG_FLAG_WRITE) ?
			    SDW_DATA_DIR_TX : SDW_DATA_DIR_RX;
	sconfig.type = SDW_STREAM_BPT;

	pconfig.num = 0;
	pconfig.ch_mask = BIT(0);

	/*
	 * Flag this BPT transfer as a firmware download that may run while
	 * audio streams on this bus are allocated but idle. sdw_stream_add_slave()
	 * allocates the master runtime, and sdw_master_rt_alloc() consults
	 * bus->bpt_fw_download to permit that allocation even when idle audio
	 * streams are still allocated. The peripheral does not start its own
	 * audio stream until the download has completed, so no stream is made
	 * active on the bus for the duration.
	 */
	WRITE_ONCE(bus->bpt_fw_download, true);

	ret = sdw_stream_add_slave(slave, &sconfig, &pconfig, 1, stream);
	if (ret < 0) {
		dev_err(amd_manager->dev,
			"add slave to BPT stream failed: %d\n", ret);
		goto remove_rt;
	}

	ret = sdw_stream_add_master(bus, &sconfig, &pconfig, 1, stream);
	if (ret < 0) {
		dev_err(amd_manager->dev,
			"add master to BPT stream failed: %d\n", ret);
		goto remove_rt;
	}

	/*
	 * Publish bus->bpt_stream only after the runtime is added and
	 * bus->bpt_stream_refcount has been raised under bus_lock. The refcount
	 * is raised by sdw_stream_add_slave() above (via sdw_master_rt_alloc());
	 * sdw_stream_add_master() then reuses that runtime through
	 * sdw_master_rt_find() without incrementing it again.
	 * sdw_program_params() keys off bpt_stream; publishing it last keeps
	 * it consistent with the refcount, so an audio path that observes
	 * refcount == 0 under bus_lock also sees bpt_stream == NULL and
	 * programs its own parameters instead of being skipped.
	 */
	WRITE_ONCE(bus->bpt_stream, stream);

	return 0;

remove_rt:
	/*
	 * sdw_stream_add_slave() allocates the master runtime and raises
	 * bus->bpt_stream_refcount. If it or the following sdw_stream_add_master()
	 * fails, sdw_stream_remove_slave() frees only the slave runtime and
	 * sdw_release_stream() only frees the stream, leaving the master runtime on
	 * bus->m_rt_list dangling at the freed stream with bpt_stream_refcount stuck
	 * non-zero -- which rejects every future BPT transfer with -EBUSY. Mirror
	 * the amd_sdw_bpt_close_stream() teardown and remove the slave then the
	 * master runtime (dropping the refcount) before releasing the stream; both
	 * removes are no-ops when nothing was allocated.
	 */
	WRITE_ONCE(bus->bpt_fw_download, false);
	sdw_stream_remove_slave(slave, stream);
	sdw_stream_remove_master(bus, stream);
	sdw_release_stream(stream);
	return ret;
}

/**
 * amd_sdw_bpt_close_stream() - Deprepare and release BPT stream
 * @amd_manager: AMD SoundWire manager
 * @slave: SoundWire slave device
 *
 * Deprepares slave DP0 and removes the stream/master/slave entries.
 * Hardware teardown (CHANNELEN=0, bank-switch, PORT_EN=0) was handled by
 * execute_bra_transfer()'s cleanup path.
 */
static void amd_sdw_bpt_close_stream(struct amd_sdw_manager *amd_manager,
				     struct sdw_slave *slave)
{
	struct sdw_bus *bus = &amd_manager->bus;
	struct sdw_stream_runtime *stream = READ_ONCE(bus->bpt_stream);
	int ret;

	if (!stream)
		return;

	/*
	 * Deprepare DP0 via SoundWire framework to leave the peripheral in a
	 * clean state for subsequent audio streams. Only deprepare when the
	 * stream actually reached a depreparable state: on early-failure paths
	 * (e.g. DMA buffer alloc failed before sdw_prepare_stream()) the stream
	 * is still SDW_STREAM_CONFIGURED and sdw_deprepare_stream() would reject
	 * it with -EINVAL. In that case DP0 was never programmed, so there is
	 * nothing to clean up.
	 */
	dev_dbg(amd_manager->dev,
		"BPT: pre-deprepare: curr_bank=%u next_bank=%u stream_state=%d\n",
		bus->params.curr_bank, bus->params.next_bank,
		stream->state);

	/*
	 * A failed sdw_disable_stream() during the transfer can leave the
	 * stream in SDW_STREAM_ENABLED. Disable it first so it reaches a
	 * depreparable state and the peripheral DP0 is not left active.
	 */
	if (stream->state == SDW_STREAM_ENABLED) {
		ret = sdw_disable_stream(stream);
		if (ret < 0)
			dev_err(amd_manager->dev,
				"BPT: sdw_disable_stream (cleanup) failed: %d\n", ret);
	}

	if (stream->state == SDW_STREAM_PREPARED ||
	    stream->state == SDW_STREAM_DISABLED) {
		ret = sdw_deprepare_stream(stream);
		if (ret < 0)
			dev_err(amd_manager->dev,
				"BPT: sdw_deprepare_stream failed: %d\n", ret);
		else
			dev_dbg(amd_manager->dev,
				"BPT: deprepared: curr_bank=%u next_bank=%u stream_state=%d\n",
				bus->params.curr_bank, bus->params.next_bank,
				stream->state);
	}

	if (stream->state == SDW_STREAM_ENABLED)
		dev_warn(amd_manager->dev,
			 "BPT: stream still ENABLED after cleanup; DP0 may remain active, peripheral may need re-enumeration\n");

	/*
	 * Clear bus->bpt_stream while bpt_stream_refcount is still raised so
	 * the two remain consistent for lockless observers.
	 * sdw_stream_remove_master() drops the refcount under bus_lock, so
	 * clearing bpt_stream first means any audio path that observes
	 * refcount == 0 also sees bpt_stream == NULL and programs its own
	 * parameters rather than skipping them in sdw_program_params().
	 */
	WRITE_ONCE(bus->bpt_stream, NULL);
	WRITE_ONCE(bus->bpt_fw_download, false);

	ret = sdw_stream_remove_slave(slave, stream);
	if (ret < 0)
		dev_err(amd_manager->dev,
			"remove slave from BPT stream failed: %d\n", ret);

	ret = sdw_stream_remove_master(bus, stream);
	if (ret < 0)
		dev_err(amd_manager->dev,
			"remove master from BPT stream failed: %d\n", ret);

	sdw_release_stream(stream);
}

/**
 * amd_sdw_bpt_send_async() - Validate a BPT message before transfer
 * @bus: SoundWire bus
 * @slave: SoundWire slave device
 * @msg: BPT message with transfer sections
 *
 * For AMD the BRA engine transfer is synchronous, so the entire transfer
 * (stream open/prepare, DMA, poll and teardown) is performed in
 * amd_sdw_bpt_wait().  This callback only validates the message so a bad
 * request fails fast.  Deliberately, no lock, runtime-PM reference or
 * stream is taken here: nothing is held across the send_async()/wait()
 * boundary, so a caller that never reaches bpt_wait() cannot leak the BPT
 * lock or a runtime-PM reference.
 */
static int amd_sdw_bpt_send_async(struct sdw_bus *bus,
				   struct sdw_slave *slave,
				   struct sdw_bpt_msg *msg)
{
	struct amd_sdw_manager *amd_manager = to_amd_sdw(bus);
	size_t total_len = 0;
	int i;

	for (i = 0; i < msg->sections; i++)
		total_len += msg->sec[i].len;

	if (total_len < AMD_BPT_MSG_BYTE_MIN) {
		dev_err(amd_manager->dev,
			"BPT msg length %zu < minimum %d bytes\n",
			total_len, AMD_BPT_MSG_BYTE_MIN);
		return -EINVAL;
	}

	if (total_len > SDW_BPT_MSG_MAX_BYTES) {
		dev_err(amd_manager->dev,
			"BPT msg length %zu > maximum %d bytes\n",
			total_len, SDW_BPT_MSG_MAX_BYTES);
		return -EINVAL;
	}

	return 0;
}

static bool amd_sdw_sections_are_contiguous(struct sdw_bpt_msg *msg)
{
	int i;

	for (i = 1; i < msg->sections; i++) {
		if (msg->sec[i].addr != msg->sec[i - 1].addr + msg->sec[i - 1].len)
			return false;
	}
	return true;
}

/**
 * amd_sdw_bpt_wait() - Execute a BPT transfer and release all resources
 * @bus: SoundWire bus
 * @slave: SoundWire slave device
 * @msg: BPT message with transfer sections
 *
 * Performs the whole BPT transfer for AMD: it serialises against other BPT
 * transfers, holds a runtime-PM reference, opens/prepares the BPT stream,
 * allocates the DMA buffer, configures the PTE mapping, runs the BRA DMA
 * transfer and finally tears everything down.  Acquiring and releasing the
 * BPT lock and the runtime-PM reference within this single function
 * guarantees they cannot leak across the send_async()/wait() boundary.
 */
static int amd_sdw_bpt_wait(struct sdw_bus *bus,
			     struct sdw_slave *slave,
			     struct sdw_bpt_msg *msg)
{
	struct amd_sdw_manager *amd_manager = to_amd_sdw(bus);
	bool is_write = (msg->flags & SDW_MSG_FLAG_WRITE);
	struct amd_bra_params prep_params = {0};
	u32 acp_sys_addr;
	dma_addr_t dma_addr = 0;
	u8 *dma_buf = NULL;
	size_t offset = 0;
	size_t total_len = 0;
	int ret = 0;
	int i;
	bool dma_unsafe = false;

	for (i = 0; i < msg->sections; i++)
		total_len += msg->sec[i].len;

	if (total_len < AMD_BPT_MSG_BYTE_MIN) {
		dev_err(amd_manager->dev,
			"BPT msg length %zu < minimum %d bytes\n",
			total_len, AMD_BPT_MSG_BYTE_MIN);
		return -EINVAL;
	}

	if (total_len > SDW_BPT_MSG_MAX_BYTES) {
		dev_err(amd_manager->dev,
			"BPT msg length %zu > maximum %d bytes\n",
			total_len, SDW_BPT_MSG_MAX_BYTES);
		return -EINVAL;
	}

	/*
	 * Take the runtime-PM reference that keeps the bus clock alive before
	 * acquiring bpt_lock, not after.  When the manager is runtime
	 * suspended, pm_runtime_get_sync() runs amd_resume_runtime()
	 * synchronously in this task, and that callback acquires bpt_lock to
	 * clear bpt_disabled.  Taking the reference while already holding
	 * bpt_lock would therefore deadlock against ourselves.
	 */
	ret = pm_runtime_get_sync(amd_manager->dev);
	/*
	 * -EACCES only occurs when runtime PM is disabled, which for this
	 * device happens across the system suspend/resume window.  amd_suspend()
	 * sets bpt_disabled before the clock is stopped and amd_resume_runtime()
	 * clears it only once the clock is back, so the bpt_disabled check below
	 * rejects the transfer with -ESHUTDOWN before any BRA access whenever the
	 * clock could be down.  A merely runtime-suspended manager (clock stopped,
	 * PM still enabled) is resumed by the get above and returns success, not
	 * -EACCES; tolerating -EACCES therefore never drives the BRA engine on a
	 * stopped clock.
	 */
	if (ret < 0 && ret != -EACCES) {
		pm_runtime_put_noidle(amd_manager->dev);
		dev_err(amd_manager->dev, "BPT: pm_runtime_get failed: %d\n", ret);
		return ret;
	}

	/*
	 * The ACP BRA engine is a single shared resource.  Serialise all BPT
	 * transfers so concurrent slave probes do not race over the hardware.
	 */
	mutex_lock(&amd_manager->bpt_lock);

	/*
	 * Refuse the transfer if the bus is being (or has been) clock-stopped
	 * for system suspend.  Driving the BRA/command channel while the clock
	 * is stopping wedges the channel and leaves the peripheral unable to
	 * re-enumerate on resume; the codec retries once it re-attaches.
	 */
	if (amd_manager->bpt_disabled) {
		mutex_unlock(&amd_manager->bpt_lock);
		pm_runtime_mark_last_busy(amd_manager->dev);
		pm_runtime_put_autosuspend(amd_manager->dev);
		return -ESHUTDOWN;
	}

	/* BRA always uses all available data columns (1..NC-1). */
	amd_manager->bra_hstart = 1;
	amd_manager->bra_hstop  = bus->params.col - 1;

	/*
	 * Open the BPT stream so the framework stream state machine tracks the
	 * transfer.  Slave DP0 is prepared below before bra_transfer().
	 */
	ret = amd_sdw_bpt_open_stream(amd_manager, slave, msg);
	if (ret < 0)
		goto close_stream;

	/* Allocate single DMA buffer for entire BPT message */
	dma_buf = dma_alloc_coherent(amd_manager->dev->parent, total_len,
				     &dma_addr, GFP_KERNEL);
	if (!dma_buf) {
		ret = -ENOMEM;
		goto close_stream;
	}

	/*
	 * The ATU GRP_1 and scratch PTE registers programmed here are
	 * ACP-global and shared by both SoundWire manager instances, as is the
	 * BRA DMA engine that reads through them.  bpt_lock is per-manager and
	 * does not serialise across instances, so hold the ACP-wide
	 * acp_bra_lock across the whole configure -> transfer -> deconfigure
	 * sequence to stop the other instance reprogramming the shared PTEs
	 * mid-transfer.
	 *
	 * A dedicated lock (not acp_sdw_lock) is used so that a transfer, which
	 * can last up to BRA_DMA_TIMEOUT_MS, does not block the brief
	 * ACP_EXTERNAL_INTR_CNTL updates done under acp_sdw_lock by
	 * amd_enable_sdw_interrupts()/amd_disable_sdw_interrupts() and the PDM
	 * interrupt helpers, which touch a different ACP-global register.
	 */
	mutex_lock(amd_manager->acp_bra_lock);
	acp_sys_addr = amd_sdw_bra_configure_pte(amd_manager, dma_addr, total_len);
	if (!acp_sys_addr) {
		ret = -ENOMEM;
		mutex_unlock(amd_manager->acp_bra_lock);
		goto free_dma;
	}

	/* For writes, copy all section data into the DMA buffer */
	if (is_write) {
		for (i = 0; i < msg->sections; i++) {
			memcpy(dma_buf + offset, msg->sec[i].buf, msg->sec[i].len);
			offset += msg->sec[i].len;
		}
	}

	dev_dbg(amd_manager->dev,
		"BPT %s start: dev_num=%d sections=%d total_len=%zu dma_addr=0x%llx\n",
		is_write ? "write" : "read",
		msg->dev_num, msg->sections, total_len, (unsigned long long)dma_addr);

	/*
	 * Calculate BRA parameters for this device. We use bytes_per_frame to
	 * decide whether a section is too small for BRA DMA and must fallback
	 * to register read/write commands.
	 */
	ret = amd_sdw_calculate_bra_params(amd_manager, &prep_params,
					   (u8)slave->dev_num);
	if (ret < 0) {
		dev_err(amd_manager->dev,
			"BPT: failed to calc params: %d\n", ret);
		goto deconfigure_pte;
	}

	/*
	 * Prepare DP0 via SoundWire framework so the core programs the
	 * peripheral DP0 transport/port registers and issues PREPARECTRL.
	 * This is invoked from the BPT transfer context (firmware callback)
	 * and not from update_status(), so it is safe w.r.t. sdw_dev_lock.
	 */
	ret = sdw_prepare_stream(bus->bpt_stream);
	if (ret < 0) {
		dev_err(amd_manager->dev,
			"BPT: sdw_prepare_stream failed: %d\n", ret);
		goto deconfigure_pte;
	}
	dev_dbg(amd_manager->dev,
		"BPT: stream prepared, curr_bank=%u next_bank=%u state=%d\n",
		bus->params.curr_bank, bus->params.next_bank,
		bus->bpt_stream->state);

	if (amd_sdw_sections_are_contiguous(msg)) {
		/*
		 * All sections are contiguous in peripheral address space.
		 * A single BRA call covers the entire firmware image.
		 */
		ret = amd_sdw_bra_transfer(amd_manager, slave,
				   msg->sec[0].addr,
				   acp_sys_addr,
				   total_len, is_write,
				   &dma_unsafe);
		if (ret < 0) {
			dev_err(amd_manager->dev,
				"BPT contiguous transfer failed: addr=0x%x len=%zu ret=%d\n",
				msg->sec[0].addr, total_len, ret);
			/*
			 * Skip the read-back copy below so a failed read
			 * cannot return stale DMA buffer contents to the
			 * caller as if the transfer had succeeded.
			 */
			goto deconfigure_pte;
		}
	} else {
		/*
		 * Non-contiguous sections: each section targets a different
		 * peripheral address range.  The ACP BRA DMA engine is
		 * triggered by sdw_enable_stream() (bank switch + CHANNELEN), so
		 * each section needs its own full config -> activate ->
		 * run_dma -> deactivate -> deconfig cycle.
		 *
		 * Sections smaller than one BRA frame (bytes_per_frame)
		 * cannot be transferred via DMA because the engine never
		 * starts for sub-frame payloads.  Use regular SDW register
		 * read/write commands for those tiny sections instead.
		 */
		offset = 0;
		for (i = 0; i < msg->sections; i++) {
			if (i < 3 || i == msg->sections - 1)
				dev_dbg(amd_manager->dev,
					"BPT nc sec[%d/%d]: periph=0x%08x len=%u acp=0x%08x\n",
					i, msg->sections, msg->sec[i].addr,
					msg->sec[i].len,
					acp_sys_addr + (u32)offset);
			if (msg->sec[i].len < prep_params.bytes_per_frame) {
				/*
				 * Section too small for BRA DMA -- use
				 * regular SDW byte-level commands instead.
				 */
				if (is_write)
					ret = sdw_nwrite_no_pm(slave,
							       msg->sec[i].addr,
							       msg->sec[i].len,
							       dma_buf + offset);
				else
					ret = sdw_nread_no_pm(slave,
							      msg->sec[i].addr,
							      msg->sec[i].len,
							      dma_buf + offset);
				if (ret < 0)
					dev_err(amd_manager->dev,
						"BPT reg %s failed: sec=%d/%d addr=0x%x len=%u ret=%d\n",
						is_write ? "write" : "read",
						i, msg->sections,
						msg->sec[i].addr,
						msg->sec[i].len, ret);
				else
					dev_dbg(amd_manager->dev,
						"BPT sec[%d/%d]: used reg %s for %u bytes at 0x%08x\n",
						i, msg->sections,
						is_write ? "write" : "read",
						msg->sec[i].len,
						msg->sec[i].addr);
			} else {
				ret = amd_sdw_bra_transfer(amd_manager, slave,
							   msg->sec[i].addr,
							   acp_sys_addr + (u32)offset,
							   msg->sec[i].len, is_write,
							   &dma_unsafe);
				if (ret < 0)
					dev_err(amd_manager->dev,
						"BPT failed: sec=%d/%d addr=0x%x len=%u ret=%d\n",
						i, msg->sections, msg->sec[i].addr,
						msg->sec[i].len, ret);
			}
			if (ret < 0)
				break;
			offset += msg->sec[i].len;
		}
		if (ret < 0)
			goto deconfigure_pte;
	}

	offset = 0;
	/* For reads, copy all section data out of the DMA buffer */
	if (!is_write) {
		for (i = 0; i < msg->sections; i++) {
			memcpy(msg->sec[i].buf, dma_buf + offset, msg->sec[i].len);
			offset += msg->sec[i].len;
		}
	}

deconfigure_pte:
	if (dma_unsafe) {
		/*
		 * PORT_EN_STATUS never cleared: the ACP BPT DMA engine did not
		 * confirm it stopped and may still be bus-mastering through the
		 * ATU into dma_buf.  PORT_EN=0 is the only stop control the
		 * hardware exposes -- the audio DMA path relies on the same
		 * enable-clear-then-poll-status primitive -- so there is no
		 * stronger barrier to force it off here.  Leave the ATU PTEs
		 * pointing at dma_buf and do NOT free the buffer: returning
		 * these pages to the allocator while a wedged engine can still
		 * write would corrupt memory reused for another purpose.
		 * Deliberately leak the buffer instead -- the only memory-safe
		 * option, matching the guarantee the managed-buffer audio path
		 * gets from the PCM core (buffer never reused while the engine
		 * may be active).
		 */
		dev_err(amd_manager->dev,
			"BPT: engine did not quiesce; leaking %zu bytes to avoid corruption\n",
			total_len);
		mutex_unlock(amd_manager->acp_bra_lock);
		goto close_stream;
	}
	amd_sdw_bra_deconfigure_pte(amd_manager, total_len);
	mutex_unlock(amd_manager->acp_bra_lock);
free_dma:
	dma_free_coherent(amd_manager->dev->parent, total_len, dma_buf, dma_addr);
close_stream:
	amd_sdw_bpt_close_stream(amd_manager, slave);
	/*
	 * Release in the reverse of the acquire order documented at the top:
	 * drop bpt_lock before releasing the runtime-PM reference (matching the
	 * bpt_disabled early-exit path above), so the lock is never held across
	 * the PM put.
	 */
	mutex_unlock(&amd_manager->bpt_lock);
	pm_runtime_mark_last_busy(amd_manager->dev);
	pm_runtime_put_autosuspend(amd_manager->dev);
	return ret;
}

static int sdw_master_read_amd_prop(struct sdw_bus *bus)
{
	struct amd_sdw_manager *amd_manager = to_amd_sdw(bus);
	struct fwnode_handle *link;
	struct sdw_master_prop *prop;
	u32 quirk_mask = 0;
	u32 wake_en_mask = 0;
	u32 power_mode_mask = 0;
	char name[32];

	prop = &bus->prop;
	/* Find manager handle */
	snprintf(name, sizeof(name), "mipi-sdw-link-%d-subproperties", bus->link_id);
	link = device_get_named_child_node(bus->dev, name);
	if (!link) {
		dev_err(bus->dev, "Manager node %s not found\n", name);
		return -EIO;
	}
	fwnode_property_read_u32(link, "amd-sdw-enable", &quirk_mask);
	if (!(quirk_mask & AMD_SDW_QUIRK_MASK_BUS_ENABLE))
		prop->hw_disabled = true;
	prop->quirks = SDW_MASTER_QUIRKS_CLEAR_INITIAL_CLASH |
		       SDW_MASTER_QUIRKS_CLEAR_INITIAL_PARITY;

	fwnode_property_read_u32(link, "amd-sdw-wakeup-enable", &wake_en_mask);
	amd_manager->wake_en_mask = wake_en_mask;
	fwnode_property_read_u32(link, "amd-sdw-power-mode", &power_mode_mask);
	amd_manager->power_mode_mask = power_mode_mask;

	fwnode_handle_put(link);

	return 0;
}

static int amd_prop_read(struct sdw_bus *bus)
{
	sdw_master_read_prop(bus);
	sdw_master_read_amd_prop(bus);
	return 0;
}

static const struct sdw_master_port_ops amd_sdw_port_ops = {
	.dpn_set_port_params = amd_sdw_port_params,
	.dpn_set_port_transport_params = amd_sdw_transport_params,
	.dpn_port_enable_ch = amd_sdw_port_enable,
};

static const struct sdw_master_ops amd_sdw_ops = {
	.read_prop = amd_prop_read,
	.xfer_msg = amd_sdw_xfer_msg,
	.read_ping_status = amd_sdw_read_ping_status,
	.bpt_send_async = amd_sdw_bpt_send_async,
	.bpt_wait = amd_sdw_bpt_wait,
};

/*
 * BRA/BPT was validated only on ACP70 and later.  ACP63 uses a different
 * BPT register layout (see the per-revision port/transport-params tables),
 * so it must not advertise the BPT callbacks -- doing so would let a BPT
 * transfer program the wrong registers and corrupt audio DMA state.
 */
static const struct sdw_master_ops amd_sdw_ops_no_bpt = {
	.read_prop = amd_prop_read,
	.xfer_msg = amd_sdw_xfer_msg,
	.read_ping_status = amd_sdw_read_ping_status,
};

static int amd_sdw_hw_params(struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *params,
			     struct snd_soc_dai *dai)
{
	struct amd_sdw_manager *amd_manager = snd_soc_dai_get_drvdata(dai);
	struct sdw_amd_dai_runtime *dai_runtime;
	struct sdw_stream_config sconfig;
	int ch, dir;
	int ret;

	dai_runtime = amd_manager->dai_runtime_array[dai->id];
	if (!dai_runtime)
		return -EIO;

	ch = params_channels(params);
	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		dir = SDW_DATA_DIR_RX;
	else
		dir = SDW_DATA_DIR_TX;
	dev_dbg(amd_manager->dev, "dir:%d dai->id:0x%x\n", dir, dai->id);

	sconfig.direction = dir;
	sconfig.ch_count = ch;
	sconfig.frame_rate = params_rate(params);
	sconfig.type = dai_runtime->stream_type;

	sconfig.bps = snd_pcm_format_width(params_format(params));

	/* Port configuration */
	struct sdw_port_config *pconfig __free(kfree) = kzalloc_obj(*pconfig);
	if (!pconfig)
		return -ENOMEM;

	pconfig->num = dai->id;
	pconfig->ch_mask = (1 << ch) - 1;
	ret = sdw_stream_add_master(&amd_manager->bus, &sconfig,
				    pconfig, 1, dai_runtime->stream);
	if (ret)
		dev_err(amd_manager->dev, "add manager to stream failed:%d\n", ret);

	return ret;
}

static int amd_sdw_hw_free(struct snd_pcm_substream *substream, struct snd_soc_dai *dai)
{
	struct amd_sdw_manager *amd_manager = snd_soc_dai_get_drvdata(dai);
	struct sdw_amd_dai_runtime *dai_runtime;
	int ret;

	dai_runtime = amd_manager->dai_runtime_array[dai->id];
	if (!dai_runtime)
		return -EIO;

	ret = sdw_stream_remove_master(&amd_manager->bus, dai_runtime->stream);
	if (ret < 0)
		dev_err(dai->dev, "remove manager from stream %s failed: %d\n",
			dai_runtime->stream->name, ret);
	return ret;
}

static int amd_set_sdw_stream(struct snd_soc_dai *dai, void *stream, int direction)
{
	struct amd_sdw_manager *amd_manager = snd_soc_dai_get_drvdata(dai);
	struct sdw_amd_dai_runtime *dai_runtime;

	dai_runtime = amd_manager->dai_runtime_array[dai->id];
	if (stream) {
		/* first paranoia check */
		if (dai_runtime) {
			dev_err(dai->dev, "dai_runtime already allocated for dai %s\n",	dai->name);
			return -EINVAL;
		}

		/* allocate and set dai_runtime info */
		dai_runtime = kzalloc_obj(*dai_runtime);
		if (!dai_runtime)
			return -ENOMEM;

		dai_runtime->stream_type = SDW_STREAM_PCM;
		dai_runtime->bus = &amd_manager->bus;
		dai_runtime->stream = stream;
		amd_manager->dai_runtime_array[dai->id] = dai_runtime;
	} else {
		/* second paranoia check */
		if (!dai_runtime) {
			dev_err(dai->dev, "dai_runtime not allocated for dai %s\n", dai->name);
			return -EINVAL;
		}

		/* for NULL stream we release allocated dai_runtime */
		kfree(dai_runtime);
		amd_manager->dai_runtime_array[dai->id] = NULL;
	}
	return 0;
}

static int amd_pcm_set_sdw_stream(struct snd_soc_dai *dai, void *stream, int direction)
{
	return amd_set_sdw_stream(dai, stream, direction);
}

static void *amd_get_sdw_stream(struct snd_soc_dai *dai, int direction)
{
	struct amd_sdw_manager *amd_manager = snd_soc_dai_get_drvdata(dai);
	struct sdw_amd_dai_runtime *dai_runtime;

	dai_runtime = amd_manager->dai_runtime_array[dai->id];
	if (!dai_runtime)
		return ERR_PTR(-EINVAL);

	return dai_runtime->stream;
}

static const struct snd_soc_dai_ops amd_sdw_dai_ops = {
	.hw_params = amd_sdw_hw_params,
	.hw_free = amd_sdw_hw_free,
	.set_stream = amd_pcm_set_sdw_stream,
	.get_stream = amd_get_sdw_stream,
};

static const struct snd_soc_component_driver amd_sdw_dai_component = {
	.name = "soundwire",
};

static int amd_sdw_register_dais(struct amd_sdw_manager *amd_manager)
{
	struct sdw_amd_dai_runtime **dai_runtime_array;
	struct snd_soc_dai_driver *dais;
	struct snd_soc_pcm_stream *stream;
	struct device *dev;
	int i, num_dais;

	dev = amd_manager->dev;
	num_dais = amd_manager->num_dout_ports + amd_manager->num_din_ports;
	dais = devm_kcalloc(dev, num_dais, sizeof(*dais), GFP_KERNEL);
	if (!dais)
		return -ENOMEM;

	dai_runtime_array = devm_kcalloc(dev, num_dais,
					 sizeof(struct sdw_amd_dai_runtime *),
					 GFP_KERNEL);
	if (!dai_runtime_array)
		return -ENOMEM;
	amd_manager->dai_runtime_array = dai_runtime_array;
	for (i = 0; i < num_dais; i++) {
		dais[i].name = devm_kasprintf(dev, GFP_KERNEL, "SDW%d Pin%d", amd_manager->instance,
					      i);
		if (!dais[i].name)
			return -ENOMEM;
		if (i < amd_manager->num_dout_ports)
			stream = &dais[i].playback;
		else
			stream = &dais[i].capture;

		stream->channels_min = 2;
		stream->channels_max = 2;
		stream->rates = SNDRV_PCM_RATE_48000;
		stream->formats = SNDRV_PCM_FMTBIT_S16_LE;

		dais[i].ops = &amd_sdw_dai_ops;
		dais[i].id = i;
	}

	return devm_snd_soc_register_component(dev, &amd_sdw_dai_component,
					       dais, num_dais);
}

static void amd_sdw_update_slave_status_work(struct work_struct *work)
{
	struct amd_sdw_manager *amd_manager =
		container_of(work, struct amd_sdw_manager, amd_sdw_work);
	int retry_count = 0;

	if (amd_manager->status[0] == SDW_SLAVE_ATTACHED) {
		writel(0, amd_manager->mmio + ACP_SW_STATE_CHANGE_STATUS_MASK_0TO7);
		writel(0, amd_manager->mmio + ACP_SW_STATE_CHANGE_STATUS_MASK_8TO11);
	}

update_status:
	sdw_handle_slave_status(&amd_manager->bus, amd_manager->status);
	/*
	 * During the peripheral enumeration sequence, the SoundWire manager interrupts
	 * are masked. Once the device number programming is done for all peripherals,
	 * interrupts will be unmasked. Read the peripheral device status from ping command
	 * and process the response. This sequence will ensure all peripheral devices enumerated
	 * and initialized properly.
	 */
	if (amd_manager->status[0] == SDW_SLAVE_ATTACHED) {
		if (retry_count++ < SDW_MAX_DEVICES) {
			writel(AMD_SDW_IRQ_MASK_0TO7, amd_manager->mmio +
			       ACP_SW_STATE_CHANGE_STATUS_MASK_0TO7);
			writel(AMD_SDW_IRQ_MASK_8TO11, amd_manager->mmio +
			       ACP_SW_STATE_CHANGE_STATUS_MASK_8TO11);
			amd_sdw_read_and_process_ping_status(amd_manager);
			goto update_status;
		} else {
			dev_err_ratelimited(amd_manager->dev,
					    "Device0 detected after %d iterations\n",
					    retry_count);
		}
	}
}

static void amd_sdw_update_slave_status(u32 status_change_0to7, u32 status_change_8to11,
					struct amd_sdw_manager *amd_manager)
{
	u64 slave_stat;
	u32 val;
	int dev_index;

	if (status_change_0to7 == AMD_SDW_SLAVE_0_ATTACHED)
		memset(amd_manager->status, 0, sizeof(amd_manager->status));
	slave_stat = status_change_0to7;
	slave_stat |= FIELD_GET(AMD_SDW_MCP_SLAVE_STATUS_8TO_11, status_change_8to11) << 32;
	dev_dbg(amd_manager->dev, "status_change_0to7:0x%x status_change_8to11:0x%x\n",
		status_change_0to7, status_change_8to11);
	if (slave_stat) {
		for (dev_index = 0; dev_index <= SDW_MAX_DEVICES; ++dev_index) {
			if (slave_stat & AMD_SDW_MCP_SLAVE_STATUS_VALID_MASK(dev_index)) {
				val = (slave_stat >> AMD_SDW_MCP_SLAVE_STAT_SHIFT_MASK(dev_index)) &
				      AMD_SDW_MCP_SLAVE_STATUS_MASK;
				amd_sdw_fill_slave_status(amd_manager, dev_index, val);
			}
		}
	}
}

static void amd_sdw_process_wake_event(struct amd_sdw_manager *amd_manager)
{
	dev_dbg(amd_manager->dev, "SoundWire Wake event reported\n");
	pm_request_resume(amd_manager->dev);
	writel(0x00, amd_manager->acp_mmio + ACP_SW_WAKE_EN(amd_manager->instance));
	writel(0x00, amd_manager->mmio + ACP_SW_STATE_CHANGE_STATUS_8TO11);
}

static void amd_sdw_irq_thread(struct work_struct *work)
{
	struct amd_sdw_manager *amd_manager =
			container_of(work, struct amd_sdw_manager, amd_sdw_irq_thread);
	u32 status_change_8to11;
	u32 status_change_0to7;

	status_change_8to11 = readl(amd_manager->mmio + ACP_SW_STATE_CHANGE_STATUS_8TO11);
	status_change_0to7 = readl(amd_manager->mmio + ACP_SW_STATE_CHANGE_STATUS_0TO7);
	if (!status_change_0to7 && !status_change_8to11)
		return;

	dev_dbg(amd_manager->dev, "[SDW%d] SDW INT: 0to7=0x%x, 8to11=0x%x\n",
		amd_manager->instance, status_change_0to7, status_change_8to11);

	/*
	 * Clear non-slave-status bits before processing.
	 * Bit 18 (BRA DMA completion) and bit 17 (command response)
	 * are informational -- not tied to slave state changes.
	 * Leaving them set can confuse the slave status update path.
	 */
	status_change_8to11 &= ~(AMD_SDW_BRA_DMA_COMPLETION_STAT |
				   AMD_SDW_CMD_RESP_INTR_STAT);

	if (status_change_8to11 & AMD_SDW_WAKE_STAT_MASK)
		return amd_sdw_process_wake_event(amd_manager);

	if (status_change_8to11 & AMD_SDW_PREQ_INTR_STAT) {
		amd_sdw_read_and_process_ping_status(amd_manager);
	} else {
		/* Check for the updated status on peripheral device */
		amd_sdw_update_slave_status(status_change_0to7, status_change_8to11, amd_manager);
	}
	if (status_change_8to11 || status_change_0to7)
		schedule_work(&amd_manager->amd_sdw_work);
	writel(0x00, amd_manager->mmio + ACP_SW_STATE_CHANGE_STATUS_8TO11);
	writel(0x00, amd_manager->mmio + ACP_SW_STATE_CHANGE_STATUS_0TO7);
}

int amd_sdw_manager_start(struct amd_sdw_manager *amd_manager)
{
	struct sdw_master_prop *prop;
	int ret;

	prop = &amd_manager->bus.prop;
	if (!prop->hw_disabled) {
		ret = amd_sdw_clk_init_ctrl(amd_manager);
		if (ret)
			return ret;
		ret = amd_init_sdw_manager(amd_manager);
		if (ret)
			return ret;
		amd_enable_sdw_interrupts(amd_manager);
		ret = amd_enable_sdw_manager(amd_manager);
		if (ret)
			return ret;
		amd_sdw_set_frameshape(amd_manager);
	}
	/* Enable runtime PM */
	pm_runtime_set_autosuspend_delay(amd_manager->dev, AMD_SDW_MASTER_SUSPEND_DELAY_MS);
	pm_runtime_use_autosuspend(amd_manager->dev);
	pm_runtime_mark_last_busy(amd_manager->dev);
	pm_runtime_set_active(amd_manager->dev);
	pm_runtime_enable(amd_manager->dev);
	return 0;
}

static int amd_sdw_manager_probe(struct platform_device *pdev)
{
	const struct acp_sdw_pdata *pdata = pdev->dev.platform_data;
	struct resource *res;
	struct device *dev = &pdev->dev;
	struct sdw_master_prop *prop;
	struct amd_sdw_manager *amd_manager;
	int ret;

	amd_manager = devm_kzalloc(dev, sizeof(struct amd_sdw_manager), GFP_KERNEL);
	if (!amd_manager)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENOMEM;

	amd_manager->acp_mmio = devm_ioremap(dev, res->start, resource_size(res));
	if (!amd_manager->acp_mmio) {
		dev_err(dev, "mmio not found\n");
		return -ENOMEM;
	}
	amd_manager->instance = pdata->instance;
	amd_manager->mmio = amd_manager->acp_mmio +
			    (amd_manager->instance * SDW_MANAGER_REG_OFFSET);
	amd_manager->acp_sdw_lock = pdata->acp_sdw_lock;
	amd_manager->acp_bra_lock = pdata->acp_bra_lock;
	amd_manager->acp_rev = pdata->acp_rev;
	amd_manager->cols_index = sdw_find_col_index(AMD_SDW_DEFAULT_COLUMNS);
	amd_manager->rows_index = sdw_find_row_index(AMD_SDW_DEFAULT_ROWS);
	amd_manager->dev = dev;
	if (amd_manager->acp_rev >= ACP70_PCI_REV_ID)
		amd_manager->bus.ops = &amd_sdw_ops;
	else
		amd_manager->bus.ops = &amd_sdw_ops_no_bpt;
	amd_manager->bus.port_ops = &amd_sdw_port_ops;
	amd_manager->bus.compute_params = &amd_sdw_compute_params;
	amd_manager->bus.clk_stop_timeout = 200;
	amd_manager->bus.link_id = amd_manager->instance;

	/*
	 * Due to BIOS compatibility, the two links are exposed within
	 * the scope of a single controller. If this changes, the
	 * controller_id will have to be updated with drv_data
	 * information.
	 */
	amd_manager->bus.controller_id = 0;
	dev_dbg(dev, "acp_rev:0x%x\n", amd_manager->acp_rev);
	switch (amd_manager->acp_rev) {
	case ACP63_PCI_REV_ID:
		switch (amd_manager->instance) {
		case ACP_SDW0:
			amd_manager->num_dout_ports = AMD_ACP63_SDW0_MAX_TX_PORTS;
			amd_manager->num_din_ports = AMD_ACP63_SDW0_MAX_RX_PORTS;
			break;
		case ACP_SDW1:
			amd_manager->num_dout_ports = AMD_ACP63_SDW1_MAX_TX_PORTS;
			amd_manager->num_din_ports = AMD_ACP63_SDW1_MAX_RX_PORTS;
			break;
		default:
			return -EINVAL;
		}
		break;
	case ACP70_PCI_REV_ID:
	case ACP71_PCI_REV_ID:
	case ACP72_PCI_REV_ID:
		amd_manager->num_dout_ports = AMD_ACP70_SDW_MAX_TX_PORTS;
		amd_manager->num_din_ports = AMD_ACP70_SDW_MAX_RX_PORTS;
		break;
	default:
		return -EINVAL;
	}
	amd_manager->max_ports = amd_manager->num_dout_ports + amd_manager->num_din_ports;
	amd_manager->port_offset_map = devm_kcalloc(dev, amd_manager->max_ports,
						    sizeof(int), GFP_KERNEL);
	if (!amd_manager->port_offset_map)
		return -ENOMEM;

	prop = &amd_manager->bus.prop;
	prop->mclk_freq = AMD_SDW_BUS_BASE_FREQ;

	ret = devm_mutex_init(dev, &amd_manager->bpt_lock);
	if (ret)
		return ret;

	ret = sdw_bus_master_add(&amd_manager->bus, dev, dev->fwnode);
	if (ret) {
		dev_err(dev, "Failed to register SoundWire manager(%d)\n", ret);
		return ret;
	}
	ret = amd_sdw_register_dais(amd_manager);
	if (ret) {
		dev_err(dev, "CPU DAI registration failed\n");
		sdw_bus_master_delete(&amd_manager->bus);
		return ret;
	}
	dev_set_drvdata(dev, amd_manager);
	INIT_WORK(&amd_manager->amd_sdw_irq_thread, amd_sdw_irq_thread);
	INIT_WORK(&amd_manager->amd_sdw_work, amd_sdw_update_slave_status_work);
	return 0;
}

static void amd_sdw_manager_remove(struct platform_device *pdev)
{
	struct amd_sdw_manager *amd_manager = dev_get_drvdata(&pdev->dev);
	int ret;

	/*
	 * A BPT firmware transfer runs in the codec's firmware-download
	 * context (amd_sdw_bpt_wait()) and holds bpt_lock for its entire
	 * duration.  Latch bpt_disabled under the lock (mirroring amd_suspend())
	 * before tearing anything down: draining alone is not enough, because as
	 * soon as bpt_lock is released a task already blocked on it could start a
	 * new transfer that then races sdw_bus_master_delete() into a
	 * use-after-free.  Setting bpt_disabled makes any such transfer bail out
	 * with -ESHUTDOWN instead, and the drain below waits for a transfer that
	 * is already in flight to finish, so sdw_bus_master_delete() cannot free
	 * bus structures still in use by amd_sdw_bpt_wait().
	 */
	mutex_lock(&amd_manager->bpt_lock);
	amd_manager->bpt_disabled = true;
	mutex_unlock(&amd_manager->bpt_lock);

	pm_runtime_disable(&pdev->dev);
	/*
	 * Disable interrupts first so the ACP ISR can no longer schedule
	 * amd_sdw_irq_thread, then drain the IRQ bottom-half before the
	 * slave-status work: amd_sdw_irq_thread may schedule amd_sdw_work, so
	 * it must be cancelled first (mirrors the suspend path ordering).
	 */
	amd_disable_sdw_interrupts(amd_manager);
	cancel_work_sync(&amd_manager->amd_sdw_irq_thread);
	cancel_work_sync(&amd_manager->amd_sdw_work);
	sdw_bus_master_delete(&amd_manager->bus);
	ret = amd_disable_sdw_manager(amd_manager);
	if (ret)
		dev_err(&pdev->dev, "Failed to disable device (%pe)\n", ERR_PTR(ret));
}

static int amd_sdw_clock_stop(struct amd_sdw_manager *amd_manager)
{
	u32 val;
	int ret;

	ret = sdw_bus_prep_clk_stop(&amd_manager->bus);
	if (ret < 0 && ret != -ENODATA) {
		dev_err(amd_manager->dev, "prepare clock stop failed %d", ret);
		return 0;
	}
	ret = sdw_bus_clk_stop(&amd_manager->bus);
	if (ret < 0 && ret != -ENODATA) {
		dev_err(amd_manager->dev, "bus clock stop failed %d", ret);
		return 0;
	}

	ret = readl_poll_timeout(amd_manager->mmio + ACP_SW_CLK_RESUME_CTRL, val,
				 (val & AMD_SDW_CLK_STOP_DONE), ACP_DELAY_US, AMD_SDW_TIMEOUT);
	if (ret) {
		dev_err(amd_manager->dev, "SDW%x clock stop failed\n", amd_manager->instance);
		return 0;
	}

	amd_manager->clk_stopped = true;
	if (amd_manager->wake_en_mask)
		writel(0x01, amd_manager->acp_mmio + ACP_SW_WAKE_EN(amd_manager->instance));

	dev_dbg(amd_manager->dev, "SDW%x clock stop successful\n", amd_manager->instance);
	return 0;
}

static int amd_sdw_clock_stop_exit(struct amd_sdw_manager *amd_manager)
{
	int ret;
	u32 val;

	if (amd_manager->clk_stopped) {
		val = readl(amd_manager->mmio + ACP_SW_CLK_RESUME_CTRL);
		val |= AMD_SDW_CLK_RESUME_REQ;
		writel(val, amd_manager->mmio + ACP_SW_CLK_RESUME_CTRL);
		ret = readl_poll_timeout(amd_manager->mmio + ACP_SW_CLK_RESUME_CTRL, val,
					 (val & AMD_SDW_CLK_RESUME_DONE), ACP_DELAY_US,
					 AMD_SDW_TIMEOUT);
		if (val & AMD_SDW_CLK_RESUME_DONE) {
			writel(0, amd_manager->mmio + ACP_SW_CLK_RESUME_CTRL);
			ret = sdw_bus_exit_clk_stop(&amd_manager->bus);
			if (ret < 0)
				dev_err(amd_manager->dev, "bus failed to exit clock stop %d\n",
					ret);
			amd_manager->clk_stopped = false;
		}
	}
	if (amd_manager->clk_stopped) {
		dev_err(amd_manager->dev, "SDW%x clock stop exit failed\n", amd_manager->instance);
		return 0;
	}
	dev_dbg(amd_manager->dev, "SDW%x clock stop exit successful\n", amd_manager->instance);
	return 0;
}

static int amd_resume_child_device(struct device *dev, void *data)
{
	struct sdw_slave *slave = dev_to_sdw_dev(dev);
	int ret;

	if (!slave->probed) {
		dev_dbg(dev, "skipping device, no probed driver\n");
		return 0;
	}
	if (!slave->dev_num_sticky) {
		dev_dbg(dev, "skipping device, never detected on bus\n");
		return 0;
	}
	ret = pm_request_resume(dev);
	if (ret < 0) {
		dev_err(dev, "pm_request_resume failed: %d\n", ret);
		return ret;
	}
	return 0;
}

static int __maybe_unused amd_pm_prepare(struct device *dev)
{
	struct amd_sdw_manager *amd_manager = dev_get_drvdata(dev);
	struct sdw_bus *bus = &amd_manager->bus;
	int ret;

	if (bus->prop.hw_disabled) {
		dev_dbg(bus->dev, "SoundWire manager %d is disabled, ignoring\n",
			bus->link_id);
		return 0;
	}
	/*
	 * When multiple peripheral devices connected over the same link, if SoundWire manager
	 * device is not in runtime suspend state, observed that device alerts are missing
	 * without pm_prepare on AMD platforms in clockstop mode0.
	 */
	if (amd_manager->power_mode_mask) {
		ret = pm_runtime_resume(dev);
		if (ret < 0) {
			dev_err(bus->dev, "pm_runtime_resume failed: %d\n", ret);
			return 0;
		}
	}
	/* To force peripheral devices to system level suspend state, resume the devices
	 * from runtime suspend state first. Without that unable to dispatch the alert
	 * status to peripheral driver during system level resume as they are in runtime
	 * suspend state.
	 */
	ret = device_for_each_child(bus->dev, NULL, amd_resume_child_device);
	if (ret < 0)
		dev_err(dev, "amd_resume_child_device failed: %d\n", ret);
	return 0;
}

static int __maybe_unused amd_suspend(struct device *dev)
{
	struct amd_sdw_manager *amd_manager = dev_get_drvdata(dev);
	struct sdw_bus *bus = &amd_manager->bus;
	int ret;

	if (bus->prop.hw_disabled) {
		dev_dbg(bus->dev, "SoundWire manager %d is disabled, ignoring\n",
			bus->link_id);
		return 0;
	}

	/*
	 * Close the window between a BPT transfer and the clock stop below.
	 * A codec's async firmware download runs in a workqueue
	 * (amd_sdw_bpt_wait()) and its runtime-PM reference does not block
	 * system-suspend clock stop.  Taking bpt_lock drains any transfer
	 * already in progress (it completes on the still-live bus), and
	 * setting bpt_disabled under the lock makes every subsequent transfer
	 * bail with -ESHUTDOWN instead of racing amd_sdw_clock_stop().  A
	 * plain drain is not enough: a re-enumeration-triggered download can
	 * start a new BPT after the drain but before the clock stop.
	 * amd_resume_runtime() clears the flag when the bus comes back.
	 */
	mutex_lock(&amd_manager->bpt_lock);
	amd_manager->bpt_disabled = true;
	mutex_unlock(&amd_manager->bpt_lock);

	if (amd_manager->power_mode_mask & AMD_SDW_CLK_STOP_MODE) {
		cancel_work_sync(&amd_manager->amd_sdw_work);
		amd_sdw_wake_enable(amd_manager, false);
		if (amd_manager->acp_rev >= ACP70_PCI_REV_ID) {
			ret = amd_sdw_host_wake_enable(amd_manager, false);
			if (ret)
				return ret;
		}
		ret = amd_sdw_clock_stop(amd_manager);
		if (ret)
			return ret;
	} else if (amd_manager->power_mode_mask & AMD_SDW_POWER_OFF_MODE) {
		cancel_work_sync(&amd_manager->amd_sdw_work);
		amd_sdw_wake_enable(amd_manager, false);
		if (amd_manager->acp_rev >= ACP70_PCI_REV_ID) {
			ret = amd_sdw_host_wake_enable(amd_manager, false);
			if (ret)
				return ret;
		}
		/*
		 * As per hardware programming sequence on AMD platforms,
		 * clock stop should be invoked first before powering-off
		 */
		ret = amd_sdw_clock_stop(amd_manager);
		if (ret)
			return ret;
		ret = amd_deinit_sdw_manager(amd_manager);
		if (ret)
			return ret;
	}
	if (amd_manager->acp_rev >= ACP70_PCI_REV_ID) {
		ret = amd_sdw_set_device_state(amd_manager, AMD_SDW_DEVICE_STATE_D3);
		if (ret)
			return ret;
	}
	return 0;
}

static int __maybe_unused amd_suspend_runtime(struct device *dev)
{
	struct amd_sdw_manager *amd_manager = dev_get_drvdata(dev);
	struct sdw_bus *bus = &amd_manager->bus;
	int ret;
	u32 val;

	if (bus->prop.hw_disabled) {
		dev_dbg(bus->dev, "SoundWire manager %d is disabled,\n",
			bus->link_id);
		return 0;
	}
	/*
	 * A BPT (firmware) transfer holds bpt_lock for its whole duration and
	 * keeps a runtime-PM reference across it (pm_runtime_get_sync() in
	 * amd_sdw_bpt_wait() until the matching put), so usage_count stays > 0
	 * and this callback cannot be entered while a transfer is in flight; the
	 * trylock below is a belt-and-braces guard for that invariant.
	 *
	 * Unlike the system-suspend path, runtime PM is still enabled here, so
	 * there is no need to latch bpt_disabled to close the window between this
	 * unlock and amd_sdw_clock_stop(): a BPT that starts in that window calls
	 * pm_runtime_get_sync() first, and because the device is RPM_SUSPENDING
	 * the PM core makes that get wait for this suspend to complete and then
	 * resume the manager - restoring the clock - before it returns, so the
	 * transfer never drives the BRA engine on a stopped clock. (The
	 * system-suspend path must latch bpt_disabled because there runtime PM is
	 * disabled and pm_runtime_get_sync() returns -EACCES instead of resuming.)
	 */
	if (!mutex_trylock(&amd_manager->bpt_lock))
		return -EBUSY;
	mutex_unlock(&amd_manager->bpt_lock);
	if (amd_manager->power_mode_mask & AMD_SDW_CLK_STOP_MODE) {
		amd_sdw_wake_enable(amd_manager, true);
		if (amd_manager->acp_rev >= ACP70_PCI_REV_ID) {
			ret = amd_sdw_host_wake_enable(amd_manager, true);
			if (ret)
				return ret;
		}
		ret = amd_sdw_clock_stop(amd_manager);
		if (ret)
			return ret;
	} else if (amd_manager->power_mode_mask & AMD_SDW_POWER_OFF_MODE) {
		amd_sdw_wake_enable(amd_manager, true);
		if (amd_manager->acp_rev >= ACP70_PCI_REV_ID) {
			ret = amd_sdw_host_wake_enable(amd_manager, true);
			if (ret)
				return ret;
		}
		ret = amd_sdw_clock_stop(amd_manager);
		if (ret)
			return ret;
		ret = amd_deinit_sdw_manager(amd_manager);
		if (ret)
			return ret;
	}
	if (amd_manager->acp_rev >= ACP70_PCI_REV_ID) {
		ret = amd_sdw_set_device_state(amd_manager, AMD_SDW_DEVICE_STATE_D3);
		if (ret)
			return ret;
		if (amd_manager->wake_en_mask) {
			val = readl(amd_manager->acp_mmio + ACP_PME_EN);
			if (!val) {
				writel(1, amd_manager->acp_mmio + ACP_PME_EN);
				val = readl(amd_manager->acp_mmio + ACP_PME_EN);
				dev_dbg(amd_manager->dev, "ACP_PME_EN:0x%x\n", val);
			}
		}
	}
	return 0;
}

static int __maybe_unused amd_resume_runtime(struct device *dev)
{
	struct amd_sdw_manager *amd_manager = dev_get_drvdata(dev);
	struct sdw_bus *bus = &amd_manager->bus;
	int ret;
	u32 val;

	if (bus->prop.hw_disabled) {
		dev_dbg(bus->dev, "SoundWire manager %d is disabled, ignoring\n",
			bus->link_id);
		return 0;
	}

	if (amd_manager->power_mode_mask & AMD_SDW_CLK_STOP_MODE) {
		ret = amd_sdw_clock_stop_exit(amd_manager);
		if (ret)
			return ret;
		/*
		 * The bus clock is live again as soon as clock_stop_exit()
		 * succeeds, so re-allow BPT here.  Doing it before the host-wake
		 * step below means a spurious host_wake_enable() error cannot
		 * leave BPT wedged at -ESHUTDOWN on an otherwise running clock.
		 */
		mutex_lock(&amd_manager->bpt_lock);
		amd_manager->bpt_disabled = false;
		mutex_unlock(&amd_manager->bpt_lock);
		if (amd_manager->acp_rev >= ACP70_PCI_REV_ID) {
			ret = amd_sdw_host_wake_enable(amd_manager, false);
			if (ret)
				return ret;
		}
	} else if (amd_manager->power_mode_mask & AMD_SDW_POWER_OFF_MODE) {
		writel(0x00, amd_manager->acp_mmio + ACP_SW_WAKE_EN(amd_manager->instance));
		if (amd_manager->acp_rev >= ACP70_PCI_REV_ID) {
			ret = amd_sdw_host_wake_enable(amd_manager, false);
			if (ret)
				return ret;
		}
		val = readl(amd_manager->mmio + ACP_SW_CLK_RESUME_CTRL);
		if (val) {
			val |= AMD_SDW_CLK_RESUME_REQ;
			writel(val, amd_manager->mmio + ACP_SW_CLK_RESUME_CTRL);
			ret = readl_poll_timeout(amd_manager->mmio + ACP_SW_CLK_RESUME_CTRL, val,
						 (val & AMD_SDW_CLK_RESUME_DONE), ACP_DELAY_US,
						 AMD_SDW_TIMEOUT);
			if (val & AMD_SDW_CLK_RESUME_DONE) {
				writel(0, amd_manager->mmio + ACP_SW_CLK_RESUME_CTRL);
				amd_manager->clk_stopped = false;
			}
		}
		sdw_clear_slave_status(bus, SDW_UNATTACH_REQUEST_MASTER_RESET);
		ret = amd_sdw_clk_init_ctrl(amd_manager);
		if (ret)
			return ret;
		amd_init_sdw_manager(amd_manager);
		amd_enable_sdw_interrupts(amd_manager);
		ret = amd_enable_sdw_manager(amd_manager);
		if (ret)
			return ret;
		amd_sdw_set_frameshape(amd_manager);
	}

	/*
	 * Re-allow BPT transfers now that the bus clock is restored.  amd_suspend()
	 * latches bpt_disabled under bpt_lock before stopping the clock; this
	 * callback (which serves both runtime and system resume) clears it once the
	 * clock-restore steps above have succeeded, before the BPT-independent
	 * set_device_state() call below.  A BPT that tolerated a
	 * pm_runtime_get_sync() -EACCES during the system-resume window still sees
	 * bpt_disabled and bails out with -ESHUTDOWN instead of driving the BRA
	 * engine on a not-yet-running clock.
	 *
	 * These paths differ in when the flag is cleared.  POWER_OFF_MODE clears it
	 * only here, so a clock-restore step that fails returns above with
	 * bpt_disabled still set and a failed resume correctly leaves BPT blocked.
	 * CLK_STOP_MODE instead clears it right after clock_stop_exit() succeeds --
	 * the clock is already live there -- so a subsequent host_wake_enable()
	 * failure returns with BPT already re-allowed, which is intentional: blocking
	 * BPT on a running clock would be needlessly conservative.  For the same
	 * reason a set_device_state() failure below also leaves BPT enabled.
	 */
	mutex_lock(&amd_manager->bpt_lock);
	amd_manager->bpt_disabled = false;
	mutex_unlock(&amd_manager->bpt_lock);

	if (amd_manager->acp_rev >= ACP70_PCI_REV_ID) {
		ret = amd_sdw_set_device_state(amd_manager, AMD_SDW_DEVICE_STATE_D0);
		if (ret)
			return ret;
	}

	return 0;
}

static const struct dev_pm_ops amd_pm = {
	.prepare = amd_pm_prepare,
	SET_SYSTEM_SLEEP_PM_OPS(amd_suspend, amd_resume_runtime)
	SET_RUNTIME_PM_OPS(amd_suspend_runtime, amd_resume_runtime, NULL)
};

static struct platform_driver amd_sdw_driver = {
	.probe	= &amd_sdw_manager_probe,
	.remove = &amd_sdw_manager_remove,
	.driver = {
		.name	= "amd_sdw_manager",
		.pm = &amd_pm,
	}
};
module_platform_driver(amd_sdw_driver);

MODULE_AUTHOR("Vijendar.Mukunda@amd.com");
MODULE_DESCRIPTION("AMD SoundWire driver");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_ALIAS("platform:" DRV_NAME);
