// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
// Copyright(c) 2015-18 Intel Corporation.

/*
 *  stream.c - SoundWire Bus stream operations.
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/slab.h>
#include <linux/soundwire/sdw_registers.h>
#include <linux/soundwire/sdw.h>
#include <sound/soc.h>
#include "bus.h"

/*
 * Array of supported rows and columns as per MIPI SoundWire Specification 1.1
 *
 * The rows are arranged as per the array index value programmed
 * in register. The index 15 has dummy value 0 in order to fill hole.
 */
int sdw_rows[SDW_FRAME_ROWS] = {48, 50, 60, 64, 75, 80, 125, 147,
			96, 100, 120, 128, 150, 160, 250, 0,
			192, 200, 240, 256, 72, 144, 90, 180};
EXPORT_SYMBOL(sdw_rows);

int sdw_cols[SDW_FRAME_COLS] = {2, 4, 6, 8, 10, 12, 14, 16};
EXPORT_SYMBOL(sdw_cols);

int sdw_find_col_index(int col)
{
	int i;

	for (i = 0; i < SDW_FRAME_COLS; i++) {
		if (sdw_cols[i] == col)
			return i;
	}

	pr_warn("Requested column not found, selecting lowest column no: 2\n");
	return 0;
}
EXPORT_SYMBOL(sdw_find_col_index);

int sdw_find_row_index(int row)
{
	int i;

	for (i = 0; i < SDW_FRAME_ROWS; i++) {
		if (sdw_rows[i] == row)
			return i;
	}

	pr_warn("Requested row not found, selecting lowest row no: 48\n");
	return 0;
}
EXPORT_SYMBOL(sdw_find_row_index);

static int _sdw_program_peripheral_port_params(struct sdw_bus *bus,
					       struct sdw_peripheral *peripheral,
					       struct sdw_transport_params *t_params,
					       enum sdw_dpn_type type)
{
	u32 addr1, addr2, addr3, addr4;
	int ret;
	u16 wbuf;

	if (bus->params.next_bank) {
		addr1 = SDW_DPN_OFFSETCTRL2_B1(t_params->port_num);
		addr2 = SDW_DPN_BLOCKCTRL3_B1(t_params->port_num);
		addr3 = SDW_DPN_SAMPLECTRL2_B1(t_params->port_num);
		addr4 = SDW_DPN_HCTRL_B1(t_params->port_num);
	} else {
		addr1 = SDW_DPN_OFFSETCTRL2_B0(t_params->port_num);
		addr2 = SDW_DPN_BLOCKCTRL3_B0(t_params->port_num);
		addr3 = SDW_DPN_SAMPLECTRL2_B0(t_params->port_num);
		addr4 = SDW_DPN_HCTRL_B0(t_params->port_num);
	}

	/* Program DPN_OffsetCtrl2 registers */
	ret = sdw_write(peripheral, addr1, t_params->offset2);
	if (ret < 0) {
		dev_err(bus->dev, "DPN_OffsetCtrl2 register write failed\n");
		return ret;
	}

	/* Program DPN_BlockCtrl3 register */
	ret = sdw_write(peripheral, addr2, t_params->blk_pkg_mode);
	if (ret < 0) {
		dev_err(bus->dev, "DPN_BlockCtrl3 register write failed\n");
		return ret;
	}

	/*
	 * Data ports are FULL, SIMPLE and REDUCED. This function handles
	 * FULL and REDUCED only and beyond this point only FULL is
	 * handled, so bail out if we are not FULL data port type
	 */
	if (type != SDW_DPN_FULL)
		return ret;

	/* Program DPN_SampleCtrl2 register */
	wbuf = FIELD_GET(SDW_DPN_SAMPLECTRL_HIGH, t_params->sample_interval - 1);

	ret = sdw_write(peripheral, addr3, wbuf);
	if (ret < 0) {
		dev_err(bus->dev, "DPN_SampleCtrl2 register write failed\n");
		return ret;
	}

	/* Program DPN_HCtrl register */
	wbuf = FIELD_PREP(SDW_DPN_HCTRL_HSTART, t_params->hstart);
	wbuf |= FIELD_PREP(SDW_DPN_HCTRL_HSTOP, t_params->hstop);

	ret = sdw_write(peripheral, addr4, wbuf);
	if (ret < 0)
		dev_err(bus->dev, "DPN_HCtrl register write failed\n");

	return ret;
}

static int sdw_program_peripheral_port_params(struct sdw_bus *bus,
					      struct sdw_peripheral_runtime *peri_rt,
					      struct sdw_port_runtime *p_rt)
{
	struct sdw_transport_params *t_params = &p_rt->transport_params;
	struct sdw_port_params *p_params = &p_rt->port_params;
	struct sdw_peripheral_prop *peripheral_prop = &peri_rt->peripheral->prop;
	u32 addr1, addr2, addr3, addr4, addr5, addr6;
	struct sdw_dpn_prop *dpn_prop;
	int ret;
	u8 wbuf;

	dpn_prop = sdw_get_peripheral_dpn_prop(peri_rt->peripheral,
					       peri_rt->direction,
					       t_params->port_num);
	if (!dpn_prop)
		return -EINVAL;

	addr1 = SDW_DPN_PORTCTRL(t_params->port_num);
	addr2 = SDW_DPN_BLOCKCTRL1(t_params->port_num);

	if (bus->params.next_bank) {
		addr3 = SDW_DPN_SAMPLECTRL1_B1(t_params->port_num);
		addr4 = SDW_DPN_OFFSETCTRL1_B1(t_params->port_num);
		addr5 = SDW_DPN_BLOCKCTRL2_B1(t_params->port_num);
		addr6 = SDW_DPN_LANECTRL_B1(t_params->port_num);

	} else {
		addr3 = SDW_DPN_SAMPLECTRL1_B0(t_params->port_num);
		addr4 = SDW_DPN_OFFSETCTRL1_B0(t_params->port_num);
		addr5 = SDW_DPN_BLOCKCTRL2_B0(t_params->port_num);
		addr6 = SDW_DPN_LANECTRL_B0(t_params->port_num);
	}

	/* Program DPN_PortCtrl register */
	wbuf = FIELD_PREP(SDW_DPN_PORTCTRL_DATAMODE, p_params->data_mode);
	wbuf |= FIELD_PREP(SDW_DPN_PORTCTRL_FLOWMODE, p_params->flow_mode);

	ret = sdw_update(peri_rt->peripheral, addr1, 0xF, wbuf);
	if (ret < 0) {
		dev_err(&peri_rt->peripheral->dev,
			"DPN_PortCtrl register write failed for port %d\n",
			t_params->port_num);
		return ret;
	}

	if (!dpn_prop->read_only_wordlength) {
		/* Program DPN_BlockCtrl1 register */
		ret = sdw_write(peri_rt->peripheral, addr2, (p_params->bps - 1));
		if (ret < 0) {
			dev_err(&peri_rt->peripheral->dev,
				"DPN_BlockCtrl1 register write failed for port %d\n",
				t_params->port_num);
			return ret;
		}
	}

	/* Program DPN_SampleCtrl1 register */
	wbuf = (t_params->sample_interval - 1) & SDW_DPN_SAMPLECTRL_LOW;
	ret = sdw_write(peri_rt->peripheral, addr3, wbuf);
	if (ret < 0) {
		dev_err(&peri_rt->peripheral->dev,
			"DPN_SampleCtrl1 register write failed for port %d\n",
			t_params->port_num);
		return ret;
	}

	/* Program DPN_OffsetCtrl1 registers */
	ret = sdw_write(peri_rt->peripheral, addr4, t_params->offset1);
	if (ret < 0) {
		dev_err(&peri_rt->peripheral->dev,
			"DPN_OffsetCtrl1 register write failed for port %d\n",
			t_params->port_num);
		return ret;
	}

	/* Program DPN_BlockCtrl2 register*/
	if (t_params->blk_grp_ctrl_valid) {
		ret = sdw_write(peri_rt->peripheral, addr5, t_params->blk_grp_ctrl);
		if (ret < 0) {
			dev_err(&peri_rt->peripheral->dev,
				"DPN_BlockCtrl2 reg write failed for port %d\n",
				t_params->port_num);
			return ret;
		}
	}

	/* program DPN_LaneCtrl register */
	if (peripheral_prop->lane_control_support) {
		ret = sdw_write(peri_rt->peripheral, addr6, t_params->lane_ctrl);
		if (ret < 0) {
			dev_err(&peri_rt->peripheral->dev,
				"DPN_LaneCtrl register write failed for port %d\n",
				t_params->port_num);
			return ret;
		}
	}

	if (dpn_prop->type != SDW_DPN_SIMPLE) {
		ret = _sdw_program_peripheral_port_params(bus, peri_rt->peripheral,
							  t_params, dpn_prop->type);
		if (ret < 0)
			dev_err(&peri_rt->peripheral->dev,
				"Transport reg write failed for port: %d\n",
				t_params->port_num);
	}

	return ret;
}

static int sdw_program_manager_port_params(struct sdw_bus *bus,
					   struct sdw_port_runtime *p_rt)
{
	int ret;

	/*
	 * we need to set transport and port parameters for the port.
	 * Transport parameters refers to the sample interval, offsets and
	 * hstart/stop etc of the data. Port parameters refers to word
	 * length, flow mode etc of the port
	 */
	ret = bus->port_ops->dpn_set_port_transport_params(bus,
					&p_rt->transport_params,
					bus->params.next_bank);
	if (ret < 0)
		return ret;

	return bus->port_ops->dpn_set_port_params(bus,
						  &p_rt->port_params,
						  bus->params.next_bank);
}

/**
 * sdw_program_port_params() - Programs transport parameters of Manager(s)
 * and Peripheral(s)
 *
 * @m_rt: Manager stream runtime
 */
static int sdw_program_port_params(struct sdw_manager_runtime *m_rt)
{
	struct sdw_peripheral_runtime *peri_rt;
	struct sdw_bus *bus = m_rt->bus;
	struct sdw_port_runtime *p_rt;
	int ret = 0;

	/* Program transport & port parameters for Peripheral(s) */
	list_for_each_entry(peri_rt, &m_rt->peripheral_rt_list, m_rt_node) {
		list_for_each_entry(p_rt, &peri_rt->port_list, port_node) {
			ret = sdw_program_peripheral_port_params(bus, peri_rt, p_rt);
			if (ret < 0)
				return ret;
		}
	}

	/* Program transport & port parameters for Manager(s) */
	list_for_each_entry(p_rt, &m_rt->port_list, port_node) {
		ret = sdw_program_manager_port_params(bus, p_rt);
		if (ret < 0)
			return ret;
	}

	return 0;
}

/**
 * sdw_enable_disable_peripheral_ports: Enable/disable peripheral data port
 *
 * @bus: bus instance
 * @peri_rt: peripheral runtime
 * @p_rt: port runtime
 * @en: enable or disable operation
 *
 * This function only sets the enable/disable bits in the relevant bank, the
 * actual enable/disable is done with a bank switch
 */
static int sdw_enable_disable_peripheral_ports(struct sdw_bus *bus,
					       struct sdw_peripheral_runtime *peri_rt,
					       struct sdw_port_runtime *p_rt,
					       bool en)
{
	struct sdw_transport_params *t_params = &p_rt->transport_params;
	u32 addr;
	int ret;

	if (bus->params.next_bank)
		addr = SDW_DPN_CHANNELEN_B1(p_rt->num);
	else
		addr = SDW_DPN_CHANNELEN_B0(p_rt->num);

	/*
	 * Since bus doesn't support sharing a port across two streams,
	 * it is safe to reset this register
	 */
	if (en)
		ret = sdw_write(peri_rt->peripheral, addr, p_rt->ch_mask);
	else
		ret = sdw_write(peri_rt->peripheral, addr, 0x0);

	if (ret < 0)
		dev_err(&peri_rt->peripheral->dev,
			"Peripheral chn_en reg write failed:%d port:%d\n",
			ret, t_params->port_num);

	return ret;
}

static int sdw_enable_disable_manager_ports(struct sdw_manager_runtime *m_rt,
					    struct sdw_port_runtime *p_rt,
					    bool en)
{
	struct sdw_transport_params *t_params = &p_rt->transport_params;
	struct sdw_bus *bus = m_rt->bus;
	struct sdw_enable_ch enable_ch;
	int ret;

	enable_ch.port_num = p_rt->num;
	enable_ch.ch_mask = p_rt->ch_mask;
	enable_ch.enable = en;

	/* Perform Manager port channel(s) enable/disable */
	if (bus->port_ops->dpn_port_enable_ch) {
		ret = bus->port_ops->dpn_port_enable_ch(bus,
							&enable_ch,
							bus->params.next_bank);
		if (ret < 0) {
			dev_err(bus->dev,
				"Manager chn_en write failed:%d port:%d\n",
				ret, t_params->port_num);
			return ret;
		}
	} else {
		dev_err(bus->dev,
			"dpn_port_enable_ch not supported, %s failed\n",
			en ? "enable" : "disable");
		return -EINVAL;
	}

	return 0;
}

/**
 * sdw_enable_disable_ports() - Enable/disable port(s) for Manager and
 * Peripheral(s)
 *
 * @m_rt: Manager stream runtime
 * @en: mode (enable/disable)
 */
static int sdw_enable_disable_ports(struct sdw_manager_runtime *m_rt, bool en)
{
	struct sdw_port_runtime *s_port, *m_port;
	struct sdw_peripheral_runtime *peri_rt;
	int ret = 0;

	/* Enable/Disable Peripheral port(s) */
	list_for_each_entry(peri_rt, &m_rt->peripheral_rt_list, m_rt_node) {
		list_for_each_entry(s_port, &peri_rt->port_list, port_node) {
			ret = sdw_enable_disable_peripheral_ports(m_rt->bus, peri_rt,
								  s_port, en);
			if (ret < 0)
				return ret;
		}
	}

	/* Enable/Disable Manager port(s) */
	list_for_each_entry(m_port, &m_rt->port_list, port_node) {
		ret = sdw_enable_disable_manager_ports(m_rt, m_port, en);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int sdw_do_port_prep(struct sdw_peripheral_runtime *peri_rt,
			    struct sdw_prepare_ch prep_ch,
			    enum sdw_port_prep_ops cmd)
{
	const struct sdw_peripheral_ops *ops = peri_rt->peripheral->ops;
	int ret;

	if (ops->port_prep) {
		ret = ops->port_prep(peri_rt->peripheral, &prep_ch, cmd);
		if (ret < 0) {
			dev_err(&peri_rt->peripheral->dev,
				"Peripheral Port Prep cmd %d failed: %d\n",
				cmd, ret);
			return ret;
		}
	}

	return 0;
}

static int sdw_prep_deprep_peripheral_ports(struct sdw_bus *bus,
					    struct sdw_peripheral_runtime *peri_rt,
					    struct sdw_port_runtime *p_rt,
					    bool prep)
{
	struct completion *port_ready;
	struct sdw_dpn_prop *dpn_prop;
	struct sdw_prepare_ch prep_ch;
	unsigned int time_left;
	bool intr = false;
	int ret = 0, val;
	u32 addr;

	prep_ch.num = p_rt->num;
	prep_ch.ch_mask = p_rt->ch_mask;

	dpn_prop = sdw_get_peripheral_dpn_prop(peri_rt->peripheral,
					       peri_rt->direction,
					       prep_ch.num);
	if (!dpn_prop) {
		dev_err(bus->dev,
			"Peripheral Port:%d properties not found\n", prep_ch.num);
		return -EINVAL;
	}

	prep_ch.prepare = prep;

	prep_ch.bank = bus->params.next_bank;

	if (dpn_prop->imp_def_interrupts || !dpn_prop->simple_ch_prep_sm ||
	    bus->params.s_data_mode != SDW_PORT_DATA_MODE_NORMAL)
		intr = true;

	/*
	 * Enable interrupt before Port prepare.
	 * For Port de-prepare, it is assumed that port
	 * was prepared earlier
	 */
	if (prep && intr) {
		ret = sdw_configure_dpn_intr(peri_rt->peripheral, p_rt->num, prep,
					     dpn_prop->imp_def_interrupts);
		if (ret < 0)
			return ret;
	}

	/* Inform peripheral about the impending port prepare */
	sdw_do_port_prep(peri_rt, prep_ch, SDW_OPS_PORT_PRE_PREP);

	/* Prepare Peripheral port implementing CP_SM */
	if (!dpn_prop->simple_ch_prep_sm) {
		addr = SDW_DPN_PREPARECTRL(p_rt->num);

		if (prep)
			ret = sdw_write(peri_rt->peripheral, addr, p_rt->ch_mask);
		else
			ret = sdw_write(peri_rt->peripheral, addr, 0x0);

		if (ret < 0) {
			dev_err(&peri_rt->peripheral->dev,
				"Peripheral prep_ctrl reg write failed\n");
			return ret;
		}

		/* Wait for completion on port ready */
		port_ready = &peri_rt->peripheral->port_ready[prep_ch.num];
		time_left = wait_for_completion_timeout(port_ready,
						msecs_to_jiffies(dpn_prop->ch_prep_timeout));

		val = sdw_read(peri_rt->peripheral, SDW_DPN_PREPARESTATUS(p_rt->num));
		val &= p_rt->ch_mask;
		if (!time_left || val) {
			dev_err(&peri_rt->peripheral->dev,
				"Chn prep failed for port:%d\n", prep_ch.num);
			return -ETIMEDOUT;
		}
	}

	/* Inform peripherals about ports prepared */
	sdw_do_port_prep(peri_rt, prep_ch, SDW_OPS_PORT_POST_PREP);

	/* Disable interrupt after Port de-prepare */
	if (!prep && intr)
		ret = sdw_configure_dpn_intr(peri_rt->peripheral, p_rt->num, prep,
					     dpn_prop->imp_def_interrupts);

	return ret;
}

static int sdw_prep_deprep_manager_ports(struct sdw_manager_runtime *m_rt,
					 struct sdw_port_runtime *p_rt,
					 bool prep)
{
	struct sdw_transport_params *t_params = &p_rt->transport_params;
	struct sdw_bus *bus = m_rt->bus;
	const struct sdw_manager_port_ops *ops = bus->port_ops;
	struct sdw_prepare_ch prep_ch;
	int ret = 0;

	prep_ch.num = p_rt->num;
	prep_ch.ch_mask = p_rt->ch_mask;
	prep_ch.prepare = prep; /* Prepare/De-prepare */
	prep_ch.bank = bus->params.next_bank;

	/* Pre-prepare/Pre-deprepare port(s) */
	if (ops->dpn_port_prep) {
		ret = ops->dpn_port_prep(bus, &prep_ch);
		if (ret < 0) {
			dev_err(bus->dev, "Port prepare failed for port:%d\n",
				t_params->port_num);
			return ret;
		}
	}

	return ret;
}

/**
 * sdw_prep_deprep_ports() - Prepare/De-prepare port(s) for Manager(s) and
 * Peripheral(s)
 *
 * @m_rt: Manager runtime handle
 * @prep: Prepare or De-prepare
 */
static int sdw_prep_deprep_ports(struct sdw_manager_runtime *m_rt, bool prep)
{
	struct sdw_peripheral_runtime *peri_rt;
	struct sdw_port_runtime *p_rt;
	int ret = 0;

	/* Prepare/De-prepare Peripheral port(s) */
	list_for_each_entry(peri_rt, &m_rt->peripheral_rt_list, m_rt_node) {
		list_for_each_entry(p_rt, &peri_rt->port_list, port_node) {
			ret = sdw_prep_deprep_peripheral_ports(m_rt->bus, peri_rt,
							       p_rt, prep);
			if (ret < 0)
				return ret;
		}
	}

	/* Prepare/De-prepare Manager port(s) */
	list_for_each_entry(p_rt, &m_rt->port_list, port_node) {
		ret = sdw_prep_deprep_manager_ports(m_rt, p_rt, prep);
		if (ret < 0)
			return ret;
	}

	return ret;
}

/**
 * sdw_notify_config() - Notify bus configuration
 *
 * @m_rt: Manager runtime handle
 *
 * This function notifies the Manager(s) and Peripheral(s) of the
 * new bus configuration.
 */
static int sdw_notify_config(struct sdw_manager_runtime *m_rt)
{
	struct sdw_peripheral_runtime *peri_rt;
	struct sdw_bus *bus = m_rt->bus;
	struct sdw_peripheral *peripheral;
	int ret = 0;

	if (bus->ops->set_bus_conf) {
		ret = bus->ops->set_bus_conf(bus, &bus->params);
		if (ret < 0)
			return ret;
	}

	list_for_each_entry(peri_rt, &m_rt->peripheral_rt_list, m_rt_node) {
		peripheral = peri_rt->peripheral;

		if (peripheral->ops->bus_config) {
			ret = peripheral->ops->bus_config(peripheral, &bus->params);
			if (ret < 0) {
				dev_err(bus->dev, "Notify Peripheral: %d failed\n",
					peripheral->dev_num);
				return ret;
			}
		}
	}

	return ret;
}

/**
 * sdw_program_params() - Program transport and port parameters for Manager(s)
 * and Peripheral(s)
 *
 * @bus: SDW bus instance
 * @prepare: true if sdw_program_params() is called by _prepare.
 */
static int sdw_program_params(struct sdw_bus *bus, bool prepare)
{
	struct sdw_manager_runtime *m_rt;
	int ret = 0;

	list_for_each_entry(m_rt, &bus->m_rt_list, bus_node) {

		/*
		 * this loop walks through all manager runtimes for a
		 * bus, but the ports can only be configured while
		 * explicitly preparing a stream or handling an
		 * already-prepared stream otherwise.
		 */
		if (!prepare &&
		    m_rt->stream->state == SDW_STREAM_CONFIGURED)
			continue;

		ret = sdw_program_port_params(m_rt);
		if (ret < 0) {
			dev_err(bus->dev,
				"Program transport params failed: %d\n", ret);
			return ret;
		}

		ret = sdw_notify_config(m_rt);
		if (ret < 0) {
			dev_err(bus->dev,
				"Notify bus config failed: %d\n", ret);
			return ret;
		}

		/* Enable port(s) on alternate bank for all active streams */
		if (m_rt->stream->state != SDW_STREAM_ENABLED)
			continue;

		ret = sdw_enable_disable_ports(m_rt, true);
		if (ret < 0) {
			dev_err(bus->dev, "Enable channel failed: %d\n", ret);
			return ret;
		}
	}

	return ret;
}

static int sdw_bank_switch(struct sdw_bus *bus, int m_rt_count)
{
	int col_index, row_index;
	bool multi_link;
	struct sdw_msg *wr_msg;
	u8 *wbuf;
	int ret;
	u16 addr;

	wr_msg = kzalloc(sizeof(*wr_msg), GFP_KERNEL);
	if (!wr_msg)
		return -ENOMEM;

	bus->defer_msg.msg = wr_msg;

	wbuf = kzalloc(sizeof(*wbuf), GFP_KERNEL);
	if (!wbuf) {
		ret = -ENOMEM;
		goto error_1;
	}

	/* Get row and column index to program register */
	col_index = sdw_find_col_index(bus->params.col);
	row_index = sdw_find_row_index(bus->params.row);
	wbuf[0] = col_index | (row_index << 3);

	if (bus->params.next_bank)
		addr = SDW_SCP_FRAMECTRL_B1;
	else
		addr = SDW_SCP_FRAMECTRL_B0;

	sdw_fill_msg(wr_msg, NULL, addr, 1, SDW_BROADCAST_DEV_NUM,
		     SDW_MSG_FLAG_WRITE, wbuf);
	wr_msg->ssp_sync = true;

	/*
	 * Set the multi_link flag only when both the hardware supports
	 * and hardware-based sync is required
	 */
	multi_link = bus->multi_link && (m_rt_count >= bus->hw_sync_min_links);

	if (multi_link)
		ret = sdw_transfer_defer(bus, wr_msg, &bus->defer_msg);
	else
		ret = sdw_transfer(bus, wr_msg);

	if (ret < 0) {
		dev_err(bus->dev, "Peripheral frame_ctrl reg write failed\n");
		goto error;
	}

	if (!multi_link) {
		kfree(wr_msg);
		kfree(wbuf);
		bus->defer_msg.msg = NULL;
		bus->params.curr_bank = !bus->params.curr_bank;
		bus->params.next_bank = !bus->params.next_bank;
	}

	return 0;

error:
	kfree(wbuf);
error_1:
	kfree(wr_msg);
	bus->defer_msg.msg = NULL;
	return ret;
}

/**
 * sdw_ml_sync_bank_switch: Multilink register bank switch
 *
 * @bus: SDW bus instance
 *
 * Caller function should free the buffers on error
 */
static int sdw_ml_sync_bank_switch(struct sdw_bus *bus)
{
	unsigned long time_left;

	if (!bus->multi_link)
		return 0;

	/* Wait for completion of transfer */
	time_left = wait_for_completion_timeout(&bus->defer_msg.complete,
						bus->bank_switch_timeout);

	if (!time_left) {
		dev_err(bus->dev, "Controller Timed out on bank switch\n");
		return -ETIMEDOUT;
	}

	bus->params.curr_bank = !bus->params.curr_bank;
	bus->params.next_bank = !bus->params.next_bank;

	if (bus->defer_msg.msg) {
		kfree(bus->defer_msg.msg->buf);
		kfree(bus->defer_msg.msg);
	}

	return 0;
}

static int do_bank_switch(struct sdw_stream_runtime *stream)
{
	struct sdw_manager_runtime *m_rt;
	const struct sdw_manager_ops *ops;
	struct sdw_bus *bus;
	bool multi_link = false;
	int m_rt_count;
	int ret = 0;

	m_rt_count = stream->m_rt_count;

	list_for_each_entry(m_rt, &stream->manager_list, stream_node) {
		bus = m_rt->bus;
		ops = bus->ops;

		if (bus->multi_link && m_rt_count >= bus->hw_sync_min_links) {
			multi_link = true;
			mutex_lock(&bus->msg_lock);
		}

		/* Pre-bank switch */
		if (ops->pre_bank_switch) {
			ret = ops->pre_bank_switch(bus);
			if (ret < 0) {
				dev_err(bus->dev,
					"Pre bank switch op failed: %d\n", ret);
				goto msg_unlock;
			}
		}

		/*
		 * Perform Bank switch operation.
		 * For multi link cases, the actual bank switch is
		 * synchronized across all Managers and happens later as a
		 * part of post_bank_switch ops.
		 */
		ret = sdw_bank_switch(bus, m_rt_count);
		if (ret < 0) {
			dev_err(bus->dev, "Bank switch failed: %d\n", ret);
			goto error;
		}
	}

	/*
	 * For multi link cases, it is expected that the bank switch is
	 * triggered by the post_bank_switch for the first Manager in the list
	 * and for the other Managers the post_bank_switch() should return doing
	 * nothing.
	 */
	list_for_each_entry(m_rt, &stream->manager_list, stream_node) {
		bus = m_rt->bus;
		ops = bus->ops;

		/* Post-bank switch */
		if (ops->post_bank_switch) {
			ret = ops->post_bank_switch(bus);
			if (ret < 0) {
				dev_err(bus->dev,
					"Post bank switch op failed: %d\n",
					ret);
				goto error;
			}
		} else if (multi_link) {
			dev_err(bus->dev,
				"Post bank switch ops not implemented\n");
			goto error;
		}

		/* Set the bank switch timeout to default, if not set */
		if (!bus->bank_switch_timeout)
			bus->bank_switch_timeout = DEFAULT_BANK_SWITCH_TIMEOUT;

		/* Check if bank switch was successful */
		ret = sdw_ml_sync_bank_switch(bus);
		if (ret < 0) {
			dev_err(bus->dev,
				"multi link bank switch failed: %d\n", ret);
			goto error;
		}

		if (multi_link)
			mutex_unlock(&bus->msg_lock);
	}

	return ret;

error:
	list_for_each_entry(m_rt, &stream->manager_list, stream_node) {
		bus = m_rt->bus;
		if (bus->defer_msg.msg) {
			kfree(bus->defer_msg.msg->buf);
			kfree(bus->defer_msg.msg);
		}
	}

msg_unlock:

	if (multi_link) {
		list_for_each_entry(m_rt, &stream->manager_list, stream_node) {
			bus = m_rt->bus;
			if (mutex_is_locked(&bus->msg_lock))
				mutex_unlock(&bus->msg_lock);
		}
	}

	return ret;
}

/**
 * sdw_release_stream() - Free the assigned stream runtime
 *
 * @stream: SoundWire stream runtime
 *
 * sdw_release_stream should be called only once per stream
 */
void sdw_release_stream(struct sdw_stream_runtime *stream)
{
	kfree(stream);
}
EXPORT_SYMBOL(sdw_release_stream);

/**
 * sdw_alloc_stream() - Allocate and return stream runtime
 *
 * @stream_name: SoundWire stream name
 *
 * Allocates a SoundWire stream runtime instance.
 * sdw_alloc_stream should be called only once per stream. Typically
 * invoked from ALSA/ASoC machine/platform driver.
 */
struct sdw_stream_runtime *sdw_alloc_stream(const char *stream_name)
{
	struct sdw_stream_runtime *stream;

	stream = kzalloc(sizeof(*stream), GFP_KERNEL);
	if (!stream)
		return NULL;

	stream->name = stream_name;
	INIT_LIST_HEAD(&stream->manager_list);
	stream->state = SDW_STREAM_ALLOCATED;
	stream->m_rt_count = 0;

	return stream;
}
EXPORT_SYMBOL(sdw_alloc_stream);

static struct sdw_manager_runtime
*sdw_find_manager_rt(struct sdw_bus *bus,
		     struct sdw_stream_runtime *stream)
{
	struct sdw_manager_runtime *m_rt;

	/* Retrieve Bus handle if already available */
	list_for_each_entry(m_rt, &stream->manager_list, stream_node) {
		if (m_rt->bus == bus)
			return m_rt;
	}

	return NULL;
}

/**
 * sdw_alloc_manager_rt() - Allocates and initialize Manager runtime handle
 *
 * @bus: SDW bus instance
 * @stream_config: Stream configuration
 * @stream: Stream runtime handle.
 *
 * This function is to be called with bus_lock held.
 */
static struct sdw_manager_runtime
*sdw_alloc_manager_rt(struct sdw_bus *bus,
		      struct sdw_stream_config *stream_config,
		      struct sdw_stream_runtime *stream)
{
	struct sdw_manager_runtime *m_rt;

	/*
	 * check if Manager is already allocated (as a result of Peripheral adding
	 * it first), if so skip allocation and go to configure
	 */
	m_rt = sdw_find_manager_rt(bus, stream);
	if (m_rt)
		goto stream_config;

	m_rt = kzalloc(sizeof(*m_rt), GFP_KERNEL);
	if (!m_rt)
		return NULL;

	/* Initialization of Manager runtime handle */
	INIT_LIST_HEAD(&m_rt->port_list);
	INIT_LIST_HEAD(&m_rt->peripheral_rt_list);
	list_add_tail(&m_rt->stream_node, &stream->manager_list);

	list_add_tail(&m_rt->bus_node, &bus->m_rt_list);

stream_config:
	m_rt->ch_count = stream_config->ch_count;
	m_rt->bus = bus;
	m_rt->stream = stream;
	m_rt->direction = stream_config->direction;

	return m_rt;
}

/**
 * sdw_alloc_peripheral_rt() - Allocate and initialize Peripheral runtime handle.
 *
 * @peripheral: Peripheral handle
 * @stream_config: Stream configuration
 * @stream: Stream runtime handle
 *
 * This function is to be called with bus_lock held.
 */
static struct sdw_peripheral_runtime
*sdw_alloc_peripheral_rt(struct sdw_peripheral *peripheral,
			 struct sdw_stream_config *stream_config,
			 struct sdw_stream_runtime *stream)
{
	struct sdw_peripheral_runtime *peri_rt;

	peri_rt = kzalloc(sizeof(*peri_rt), GFP_KERNEL);
	if (!peri_rt)
		return NULL;

	INIT_LIST_HEAD(&peri_rt->port_list);
	peri_rt->ch_count = stream_config->ch_count;
	peri_rt->direction = stream_config->direction;
	peri_rt->peripheral = peripheral;

	return peri_rt;
}

static void sdw_manager_port_release(struct sdw_bus *bus,
				     struct sdw_manager_runtime *m_rt)
{
	struct sdw_port_runtime *p_rt, *_p_rt;

	list_for_each_entry_safe(p_rt, _p_rt, &m_rt->port_list, port_node) {
		list_del(&p_rt->port_node);
		kfree(p_rt);
	}
}

static void sdw_peripheral_port_release(struct sdw_bus *bus,
					struct sdw_peripheral *peripheral,
					struct sdw_stream_runtime *stream)
{
	struct sdw_port_runtime *p_rt, *_p_rt;
	struct sdw_manager_runtime *m_rt;
	struct sdw_peripheral_runtime *peri_rt;

	list_for_each_entry(m_rt, &stream->manager_list, stream_node) {
		list_for_each_entry(peri_rt, &m_rt->peripheral_rt_list, m_rt_node) {
			if (peri_rt->peripheral != peripheral)
				continue;

			list_for_each_entry_safe(p_rt, _p_rt,
						 &peri_rt->port_list, port_node) {
				list_del(&p_rt->port_node);
				kfree(p_rt);
			}
		}
	}
}

/**
 * sdw_release_peripheral_stream() - Free Peripheral(s) runtime handle
 *
 * @peripheral: Peripheral handle.
 * @stream: Stream runtime handle.
 *
 * This function is to be called with bus_lock held.
 */
static void sdw_release_peripheral_stream(struct sdw_peripheral *peripheral,
					  struct sdw_stream_runtime *stream)
{
	struct sdw_peripheral_runtime *peri_rt, *_peri_rt;
	struct sdw_manager_runtime *m_rt;

	list_for_each_entry(m_rt, &stream->manager_list, stream_node) {
		/* Retrieve Peripheral runtime handle */
		list_for_each_entry_safe(peri_rt, _peri_rt,
					 &m_rt->peripheral_rt_list, m_rt_node) {
			if (peri_rt->peripheral == peripheral) {
				list_del(&peri_rt->m_rt_node);
				kfree(peri_rt);
				return;
			}
		}
	}
}

/**
 * sdw_release_manager_stream() - Free Manager runtime handle
 *
 * @m_rt: Manager runtime node
 * @stream: Stream runtime handle.
 *
 * This function is to be called with bus_lock held
 * It frees the Manager runtime handle and associated Peripheral(s) runtime
 * handle. If this is called first then sdw_release_peripheral_stream() will have
 * no effect as Peripheral(s) runtime handle would already be freed up.
 */
static void sdw_release_manager_stream(struct sdw_manager_runtime *m_rt,
				       struct sdw_stream_runtime *stream)
{
	struct sdw_peripheral_runtime *peri_rt, *_peri_rt;

	list_for_each_entry_safe(peri_rt, _peri_rt, &m_rt->peripheral_rt_list, m_rt_node) {
		sdw_peripheral_port_release(peri_rt->peripheral->bus, peri_rt->peripheral, stream);
		sdw_release_peripheral_stream(peri_rt->peripheral, stream);
	}

	list_del(&m_rt->stream_node);
	list_del(&m_rt->bus_node);
	kfree(m_rt);
}

/**
 * sdw_stream_remove_manager() - Remove manager from sdw_stream
 *
 * @bus: SDW Bus instance
 * @stream: SoundWire stream
 *
 * This removes and frees port_rt and manager_rt from a stream
 */
int sdw_stream_remove_manager(struct sdw_bus *bus,
			      struct sdw_stream_runtime *stream)
{
	struct sdw_manager_runtime *m_rt, *_m_rt;

	mutex_lock(&bus->bus_lock);

	list_for_each_entry_safe(m_rt, _m_rt,
				 &stream->manager_list, stream_node) {
		if (m_rt->bus != bus)
			continue;

		sdw_manager_port_release(bus, m_rt);
		sdw_release_manager_stream(m_rt, stream);
		stream->m_rt_count--;
	}

	if (list_empty(&stream->manager_list))
		stream->state = SDW_STREAM_RELEASED;

	mutex_unlock(&bus->bus_lock);

	return 0;
}
EXPORT_SYMBOL(sdw_stream_remove_manager);

/**
 * sdw_stream_remove_peripheral() - Remove peripheral from sdw_stream
 *
 * @peripheral: SDW Peripheral instance
 * @stream: SoundWire stream
 *
 * This removes and frees port_rt and peripheral_rt from a stream
 */
int sdw_stream_remove_peripheral(struct sdw_peripheral *peripheral,
				 struct sdw_stream_runtime *stream)
{
	mutex_lock(&peripheral->bus->bus_lock);

	sdw_peripheral_port_release(peripheral->bus, peripheral, stream);
	sdw_release_peripheral_stream(peripheral, stream);

	mutex_unlock(&peripheral->bus->bus_lock);

	return 0;
}
EXPORT_SYMBOL(sdw_stream_remove_peripheral);

/**
 * sdw_config_stream() - Configure the allocated stream
 *
 * @dev: SDW device
 * @stream: SoundWire stream
 * @stream_config: Stream configuration for audio stream
 * @is_peripheral: is API called from Peripheral or Manager
 *
 * This function is to be called with bus_lock held.
 */
static int sdw_config_stream(struct device *dev,
			     struct sdw_stream_runtime *stream,
			     struct sdw_stream_config *stream_config,
			     bool is_peripheral)
{
	/*
	 * Update the stream rate, channel and bps based on data
	 * source. For more than one data source (multilink),
	 * match the rate, bps, stream type and increment number of channels.
	 *
	 * If rate/bps is zero, it means the values are not set, so skip
	 * comparison and allow the value to be set and stored in stream
	 */
	if (stream->params.rate &&
	    stream->params.rate != stream_config->frame_rate) {
		dev_err(dev, "rate not matching, stream:%s\n", stream->name);
		return -EINVAL;
	}

	if (stream->params.bps &&
	    stream->params.bps != stream_config->bps) {
		dev_err(dev, "bps not matching, stream:%s\n", stream->name);
		return -EINVAL;
	}

	stream->type = stream_config->type;
	stream->params.rate = stream_config->frame_rate;
	stream->params.bps = stream_config->bps;

	/* TODO: Update this check during Device-device support */
	if (is_peripheral)
		stream->params.ch_count += stream_config->ch_count;

	return 0;
}

static int sdw_is_valid_port_range(struct device *dev,
				   struct sdw_port_runtime *p_rt)
{
	if (!SDW_VALID_PORT_RANGE(p_rt->num)) {
		dev_err(dev,
			"SoundWire: Invalid port number :%d\n", p_rt->num);
		return -EINVAL;
	}

	return 0;
}

static struct sdw_port_runtime
*sdw_port_alloc(struct device *dev,
		struct sdw_port_config *port_config,
		int port_index)
{
	struct sdw_port_runtime *p_rt;

	p_rt = kzalloc(sizeof(*p_rt), GFP_KERNEL);
	if (!p_rt)
		return NULL;

	p_rt->ch_mask = port_config[port_index].ch_mask;
	p_rt->num = port_config[port_index].num;

	return p_rt;
}

static int sdw_manager_port_config(struct sdw_bus *bus,
				   struct sdw_manager_runtime *m_rt,
				   struct sdw_port_config *port_config,
				   unsigned int num_ports)
{
	struct sdw_port_runtime *p_rt;
	int i;

	/* Iterate for number of ports to perform initialization */
	for (i = 0; i < num_ports; i++) {
		p_rt = sdw_port_alloc(bus->dev, port_config, i);
		if (!p_rt)
			return -ENOMEM;

		/*
		 * TODO: Check port capabilities for requested
		 * configuration (audio mode support)
		 */

		list_add_tail(&p_rt->port_node, &m_rt->port_list);
	}

	return 0;
}

static int sdw_peripheral_port_config(struct sdw_peripheral *peripheral,
				      struct sdw_peripheral_runtime *peri_rt,
				      struct sdw_port_config *port_config,
				      unsigned int num_config)
{
	struct sdw_port_runtime *p_rt;
	int i, ret;

	/* Iterate for number of ports to perform initialization */
	for (i = 0; i < num_config; i++) {
		p_rt = sdw_port_alloc(&peripheral->dev, port_config, i);
		if (!p_rt)
			return -ENOMEM;

		/*
		 * TODO: Check valid port range as defined by DisCo/
		 * peripheral
		 */
		ret = sdw_is_valid_port_range(&peripheral->dev, p_rt);
		if (ret < 0) {
			kfree(p_rt);
			return ret;
		}

		/*
		 * TODO: Check port capabilities for requested
		 * configuration (audio mode support)
		 */

		list_add_tail(&p_rt->port_node, &peri_rt->port_list);
	}

	return 0;
}

/**
 * sdw_stream_add_manager() - Allocate and add manager runtime to a stream
 *
 * @bus: SDW Bus instance
 * @stream_config: Stream configuration for audio stream
 * @port_config: Port configuration for audio stream
 * @num_ports: Number of ports
 * @stream: SoundWire stream
 */
int sdw_stream_add_manager(struct sdw_bus *bus,
			   struct sdw_stream_config *stream_config,
			   struct sdw_port_config *port_config,
			   unsigned int num_ports,
			   struct sdw_stream_runtime *stream)
{
	struct sdw_manager_runtime *m_rt;
	int ret;

	mutex_lock(&bus->bus_lock);

	/*
	 * For multi link streams, add the second manager only if
	 * the bus supports it.
	 * Check if bus->multi_link is set
	 */
	if (!bus->multi_link && stream->m_rt_count > 0) {
		dev_err(bus->dev,
			"Multilink not supported, link %d\n", bus->link_id);
		ret = -EINVAL;
		goto unlock;
	}

	m_rt = sdw_alloc_manager_rt(bus, stream_config, stream);
	if (!m_rt) {
		dev_err(bus->dev,
			"Manager runtime config failed for stream:%s\n",
			stream->name);
		ret = -ENOMEM;
		goto unlock;
	}

	ret = sdw_config_stream(bus->dev, stream, stream_config, false);
	if (ret)
		goto stream_error;

	ret = sdw_manager_port_config(bus, m_rt, port_config, num_ports);
	if (ret)
		goto stream_error;

	stream->m_rt_count++;

	goto unlock;

stream_error:
	sdw_release_manager_stream(m_rt, stream);
unlock:
	mutex_unlock(&bus->bus_lock);
	return ret;
}
EXPORT_SYMBOL(sdw_stream_add_manager);

/**
 * sdw_stream_add_peripheral() - Allocate and add manager/peripheral runtime to a stream
 *
 * @peripheral: SDW Peripheral instance
 * @stream_config: Stream configuration for audio stream
 * @stream: SoundWire stream
 * @port_config: Port configuration for audio stream
 * @num_ports: Number of ports
 *
 * It is expected that Peripheral is added before adding Manager
 * to the Stream.
 *
 */
int sdw_stream_add_peripheral(struct sdw_peripheral *peripheral,
			      struct sdw_stream_config *stream_config,
			      struct sdw_port_config *port_config,
			      unsigned int num_ports,
			      struct sdw_stream_runtime *stream)
{
	struct sdw_peripheral_runtime *peri_rt;
	struct sdw_manager_runtime *m_rt;
	int ret;

	mutex_lock(&peripheral->bus->bus_lock);

	/*
	 * If this API is invoked by Peripheral first then m_rt is not valid.
	 * So, allocate m_rt and add Peripheral to it.
	 */
	m_rt = sdw_alloc_manager_rt(peripheral->bus, stream_config, stream);
	if (!m_rt) {
		dev_err(&peripheral->dev,
			"alloc manager runtime failed for stream:%s\n",
			stream->name);
		ret = -ENOMEM;
		goto error;
	}

	peri_rt = sdw_alloc_peripheral_rt(peripheral, stream_config, stream);
	if (!peri_rt) {
		dev_err(&peripheral->dev,
			"Peripheral runtime config failed for stream:%s\n",
			stream->name);
		ret = -ENOMEM;
		goto stream_error;
	}

	ret = sdw_config_stream(&peripheral->dev, stream, stream_config, true);
	if (ret) {
		/*
		 * sdw_release_manager_stream will release peri_rt in peripheral_rt_list in
		 * stream_error case, but peri_rt is only added to peripheral_rt_list
		 * when sdw_config_stream is successful, so free peri_rt explicitly
		 * when sdw_config_stream is failed.
		 */
		kfree(peri_rt);
		goto stream_error;
	}

	list_add_tail(&peri_rt->m_rt_node, &m_rt->peripheral_rt_list);

	ret = sdw_peripheral_port_config(peripheral, peri_rt, port_config, num_ports);
	if (ret)
		goto stream_error;

	/*
	 * Change stream state to CONFIGURED on first Peripheral add.
	 * Bus is not aware of number of Peripheral(s) in a stream at this
	 * point so cannot depend on all Peripheral(s) to be added in order to
	 * change stream state to CONFIGURED.
	 */
	stream->state = SDW_STREAM_CONFIGURED;
	goto error;

stream_error:
	/*
	 * we hit error so cleanup the stream, release all Peripheral(s) and
	 * Manager runtime
	 */
	sdw_release_manager_stream(m_rt, stream);
error:
	mutex_unlock(&peripheral->bus->bus_lock);
	return ret;
}
EXPORT_SYMBOL(sdw_stream_add_peripheral);

/**
 * sdw_get_peripheral_dpn_prop() - Get Peripheral port capabilities
 *
 * @peripheral: Peripheral handle
 * @direction: Data direction.
 * @port_num: Port number
 */
struct sdw_dpn_prop *sdw_get_peripheral_dpn_prop(struct sdw_peripheral *peripheral,
						 enum sdw_data_direction direction,
						 unsigned int port_num)
{
	struct sdw_dpn_prop *dpn_prop;
	u8 num_ports;
	int i;

	if (direction == SDW_DATA_DIR_TX) {
		num_ports = hweight32(peripheral->prop.source_ports);
		dpn_prop = peripheral->prop.src_dpn_prop;
	} else {
		num_ports = hweight32(peripheral->prop.sink_ports);
		dpn_prop = peripheral->prop.sink_dpn_prop;
	}

	for (i = 0; i < num_ports; i++) {
		if (dpn_prop[i].num == port_num)
			return &dpn_prop[i];
	}

	return NULL;
}

/**
 * sdw_acquire_bus_lock: Acquire bus lock for all Manager runtime(s)
 *
 * @stream: SoundWire stream
 *
 * Acquire bus_lock for each of the manager runtime(m_rt) part of this
 * stream to reconfigure the bus.
 * NOTE: This function is called from SoundWire stream ops and is
 * expected that a global lock is held before acquiring bus_lock.
 */
static void sdw_acquire_bus_lock(struct sdw_stream_runtime *stream)
{
	struct sdw_manager_runtime *m_rt;
	struct sdw_bus *bus;

	/* Iterate for all Manager(s) in Manager list */
	list_for_each_entry(m_rt, &stream->manager_list, stream_node) {
		bus = m_rt->bus;

		mutex_lock(&bus->bus_lock);
	}
}

/**
 * sdw_release_bus_lock: Release bus lock for all Manager runtime(s)
 *
 * @stream: SoundWire stream
 *
 * Release the previously held bus_lock after reconfiguring the bus.
 * NOTE: This function is called from SoundWire stream ops and is
 * expected that a global lock is held before releasing bus_lock.
 */
static void sdw_release_bus_lock(struct sdw_stream_runtime *stream)
{
	struct sdw_manager_runtime *m_rt;
	struct sdw_bus *bus;

	/* Iterate for all Manager(s) in Manager list */
	list_for_each_entry_reverse(m_rt, &stream->manager_list, stream_node) {
		bus = m_rt->bus;
		mutex_unlock(&bus->bus_lock);
	}
}

static int _sdw_prepare_stream(struct sdw_stream_runtime *stream,
			       bool update_params)
{
	struct sdw_manager_runtime *m_rt;
	struct sdw_bus *bus = NULL;
	struct sdw_manager_prop *prop;
	struct sdw_bus_params params;
	int ret;

	/* Prepare  Manager(s) and Peripheral(s) port(s) associated with stream */
	list_for_each_entry(m_rt, &stream->manager_list, stream_node) {
		bus = m_rt->bus;
		prop = &bus->prop;
		memcpy(&params, &bus->params, sizeof(params));

		/* TODO: Support Asynchronous mode */
		if ((prop->max_clk_freq % stream->params.rate) != 0) {
			dev_err(bus->dev, "Async mode not supported\n");
			return -EINVAL;
		}

		if (!update_params)
			goto program_params;

		/* Increment cumulative bus bandwidth */
		/* TODO: Update this during Device-Device support */
		bus->params.bandwidth += m_rt->stream->params.rate *
			m_rt->ch_count * m_rt->stream->params.bps;

		/* Compute params */
		if (bus->compute_params) {
			ret = bus->compute_params(bus);
			if (ret < 0) {
				dev_err(bus->dev, "Compute params failed: %d\n",
					ret);
				return ret;
			}
		}

program_params:
		/* Program params */
		ret = sdw_program_params(bus, true);
		if (ret < 0) {
			dev_err(bus->dev, "Program params failed: %d\n", ret);
			goto restore_params;
		}
	}

	if (!bus) {
		pr_err("Configuration error in %s\n", __func__);
		return -EINVAL;
	}

	ret = do_bank_switch(stream);
	if (ret < 0) {
		dev_err(bus->dev, "Bank switch failed: %d\n", ret);
		goto restore_params;
	}

	list_for_each_entry(m_rt, &stream->manager_list, stream_node) {
		bus = m_rt->bus;

		/* Prepare port(s) on the new clock configuration */
		ret = sdw_prep_deprep_ports(m_rt, true);
		if (ret < 0) {
			dev_err(bus->dev, "Prepare port(s) failed ret = %d\n",
				ret);
			return ret;
		}
	}

	stream->state = SDW_STREAM_PREPARED;

	return ret;

restore_params:
	memcpy(&bus->params, &params, sizeof(params));
	return ret;
}

/**
 * sdw_prepare_stream() - Prepare SoundWire stream
 *
 * @stream: Soundwire stream
 *
 * Documentation/driver-api/soundwire/stream.rst explains this API in detail
 */
int sdw_prepare_stream(struct sdw_stream_runtime *stream)
{
	bool update_params = true;
	int ret;

	if (!stream) {
		pr_err("SoundWire: Handle not found for stream\n");
		return -EINVAL;
	}

	sdw_acquire_bus_lock(stream);

	if (stream->state == SDW_STREAM_PREPARED) {
		ret = 0;
		goto state_err;
	}

	if (stream->state != SDW_STREAM_CONFIGURED &&
	    stream->state != SDW_STREAM_DEPREPARED &&
	    stream->state != SDW_STREAM_DISABLED) {
		pr_err("%s: %s: inconsistent state state %d\n",
		       __func__, stream->name, stream->state);
		ret = -EINVAL;
		goto state_err;
	}

	/*
	 * when the stream is DISABLED, this means sdw_prepare_stream()
	 * is called as a result of an underflow or a resume operation.
	 * In this case, the bus parameters shall not be recomputed, but
	 * still need to be re-applied
	 */
	if (stream->state == SDW_STREAM_DISABLED)
		update_params = false;

	ret = _sdw_prepare_stream(stream, update_params);

state_err:
	sdw_release_bus_lock(stream);
	return ret;
}
EXPORT_SYMBOL(sdw_prepare_stream);

static int _sdw_enable_stream(struct sdw_stream_runtime *stream)
{
	struct sdw_manager_runtime *m_rt;
	struct sdw_bus *bus = NULL;
	int ret;

	/* Enable Manager(s) and Peripheral(s) port(s) associated with stream */
	list_for_each_entry(m_rt, &stream->manager_list, stream_node) {
		bus = m_rt->bus;

		/* Program params */
		ret = sdw_program_params(bus, false);
		if (ret < 0) {
			dev_err(bus->dev, "Program params failed: %d\n", ret);
			return ret;
		}

		/* Enable port(s) */
		ret = sdw_enable_disable_ports(m_rt, true);
		if (ret < 0) {
			dev_err(bus->dev,
				"Enable port(s) failed ret: %d\n", ret);
			return ret;
		}
	}

	if (!bus) {
		pr_err("Configuration error in %s\n", __func__);
		return -EINVAL;
	}

	ret = do_bank_switch(stream);
	if (ret < 0) {
		dev_err(bus->dev, "Bank switch failed: %d\n", ret);
		return ret;
	}

	stream->state = SDW_STREAM_ENABLED;
	return 0;
}

/**
 * sdw_enable_stream() - Enable SoundWire stream
 *
 * @stream: Soundwire stream
 *
 * Documentation/driver-api/soundwire/stream.rst explains this API in detail
 */
int sdw_enable_stream(struct sdw_stream_runtime *stream)
{
	int ret;

	if (!stream) {
		pr_err("SoundWire: Handle not found for stream\n");
		return -EINVAL;
	}

	sdw_acquire_bus_lock(stream);

	if (stream->state != SDW_STREAM_PREPARED &&
	    stream->state != SDW_STREAM_DISABLED) {
		pr_err("%s: %s: inconsistent state state %d\n",
		       __func__, stream->name, stream->state);
		ret = -EINVAL;
		goto state_err;
	}

	ret = _sdw_enable_stream(stream);

state_err:
	sdw_release_bus_lock(stream);
	return ret;
}
EXPORT_SYMBOL(sdw_enable_stream);

static int _sdw_disable_stream(struct sdw_stream_runtime *stream)
{
	struct sdw_manager_runtime *m_rt;
	int ret;

	list_for_each_entry(m_rt, &stream->manager_list, stream_node) {
		struct sdw_bus *bus = m_rt->bus;

		/* Disable port(s) */
		ret = sdw_enable_disable_ports(m_rt, false);
		if (ret < 0) {
			dev_err(bus->dev, "Disable port(s) failed: %d\n", ret);
			return ret;
		}
	}
	stream->state = SDW_STREAM_DISABLED;

	list_for_each_entry(m_rt, &stream->manager_list, stream_node) {
		struct sdw_bus *bus = m_rt->bus;

		/* Program params */
		ret = sdw_program_params(bus, false);
		if (ret < 0) {
			dev_err(bus->dev, "Program params failed: %d\n", ret);
			return ret;
		}
	}

	ret = do_bank_switch(stream);
	if (ret < 0) {
		pr_err("Bank switch failed: %d\n", ret);
		return ret;
	}

	/* make sure alternate bank (previous current) is also disabled */
	list_for_each_entry(m_rt, &stream->manager_list, stream_node) {
		struct sdw_bus *bus = m_rt->bus;

		/* Disable port(s) */
		ret = sdw_enable_disable_ports(m_rt, false);
		if (ret < 0) {
			dev_err(bus->dev, "Disable port(s) failed: %d\n", ret);
			return ret;
		}
	}

	return 0;
}

/**
 * sdw_disable_stream() - Disable SoundWire stream
 *
 * @stream: Soundwire stream
 *
 * Documentation/driver-api/soundwire/stream.rst explains this API in detail
 */
int sdw_disable_stream(struct sdw_stream_runtime *stream)
{
	int ret;

	if (!stream) {
		pr_err("SoundWire: Handle not found for stream\n");
		return -EINVAL;
	}

	sdw_acquire_bus_lock(stream);

	if (stream->state != SDW_STREAM_ENABLED) {
		pr_err("%s: %s: inconsistent state state %d\n",
		       __func__, stream->name, stream->state);
		ret = -EINVAL;
		goto state_err;
	}

	ret = _sdw_disable_stream(stream);

state_err:
	sdw_release_bus_lock(stream);
	return ret;
}
EXPORT_SYMBOL(sdw_disable_stream);

static int _sdw_deprepare_stream(struct sdw_stream_runtime *stream)
{
	struct sdw_manager_runtime *m_rt;
	struct sdw_bus *bus;
	int ret = 0;

	list_for_each_entry(m_rt, &stream->manager_list, stream_node) {
		bus = m_rt->bus;
		/* De-prepare port(s) */
		ret = sdw_prep_deprep_ports(m_rt, false);
		if (ret < 0) {
			dev_err(bus->dev,
				"De-prepare port(s) failed: %d\n", ret);
			return ret;
		}

		/* TODO: Update this during Device-Device support */
		bus->params.bandwidth -= m_rt->stream->params.rate *
			m_rt->ch_count * m_rt->stream->params.bps;

		/* Compute params */
		if (bus->compute_params) {
			ret = bus->compute_params(bus);
			if (ret < 0) {
				dev_err(bus->dev, "Compute params failed: %d\n",
					ret);
				return ret;
			}
		}

		/* Program params */
		ret = sdw_program_params(bus, false);
		if (ret < 0) {
			dev_err(bus->dev, "Program params failed: %d\n", ret);
			return ret;
		}
	}

	stream->state = SDW_STREAM_DEPREPARED;
	return do_bank_switch(stream);
}

/**
 * sdw_deprepare_stream() - Deprepare SoundWire stream
 *
 * @stream: Soundwire stream
 *
 * Documentation/driver-api/soundwire/stream.rst explains this API in detail
 */
int sdw_deprepare_stream(struct sdw_stream_runtime *stream)
{
	int ret;

	if (!stream) {
		pr_err("SoundWire: Handle not found for stream\n");
		return -EINVAL;
	}

	sdw_acquire_bus_lock(stream);

	if (stream->state != SDW_STREAM_PREPARED &&
	    stream->state != SDW_STREAM_DISABLED) {
		pr_err("%s: %s: inconsistent state state %d\n",
		       __func__, stream->name, stream->state);
		ret = -EINVAL;
		goto state_err;
	}

	ret = _sdw_deprepare_stream(stream);

state_err:
	sdw_release_bus_lock(stream);
	return ret;
}
EXPORT_SYMBOL(sdw_deprepare_stream);

static int set_stream(struct snd_pcm_substream *substream,
		      struct sdw_stream_runtime *sdw_stream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *dai;
	int ret = 0;
	int i;

	/* Set stream pointer on all DAIs */
	for_each_rtd_dais(rtd, i, dai) {
		ret = snd_soc_dai_set_sdw_stream(dai, sdw_stream, substream->stream);
		if (ret < 0) {
			dev_err(rtd->dev, "failed to set stream pointer on dai %s\n", dai->name);
			break;
		}
	}

	return ret;
}

/**
 * sdw_startup_stream() - Startup SoundWire stream
 *
 * @sdw_substream: Soundwire stream
 *
 * Documentation/driver-api/soundwire/stream.rst explains this API in detail
 */
int sdw_startup_stream(void *sdw_substream)
{
	struct snd_pcm_substream *substream = sdw_substream;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct sdw_stream_runtime *sdw_stream;
	char *name;
	int ret;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		name = kasprintf(GFP_KERNEL, "%s-Playback", substream->name);
	else
		name = kasprintf(GFP_KERNEL, "%s-Capture", substream->name);

	if (!name)
		return -ENOMEM;

	sdw_stream = sdw_alloc_stream(name);
	if (!sdw_stream) {
		dev_err(rtd->dev, "alloc stream failed for substream DAI %s\n", substream->name);
		ret = -ENOMEM;
		goto error;
	}

	ret = set_stream(substream, sdw_stream);
	if (ret < 0)
		goto release_stream;
	return 0;

release_stream:
	sdw_release_stream(sdw_stream);
	set_stream(substream, NULL);
error:
	kfree(name);
	return ret;
}
EXPORT_SYMBOL(sdw_startup_stream);

/**
 * sdw_shutdown_stream() - Shutdown SoundWire stream
 *
 * @sdw_substream: Soundwire stream
 *
 * Documentation/driver-api/soundwire/stream.rst explains this API in detail
 */
void sdw_shutdown_stream(void *sdw_substream)
{
	struct snd_pcm_substream *substream = sdw_substream;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct sdw_stream_runtime *sdw_stream;
	struct snd_soc_dai *dai;

	/* Find stream from first CPU DAI */
	dai = asoc_rtd_to_cpu(rtd, 0);

	sdw_stream = snd_soc_dai_get_sdw_stream(dai, substream->stream);

	if (IS_ERR(sdw_stream)) {
		dev_err(rtd->dev, "no stream found for DAI %s\n", dai->name);
		return;
	}

	/* release memory */
	kfree(sdw_stream->name);
	sdw_release_stream(sdw_stream);

	/* clear DAI data */
	set_stream(substream, NULL);
}
EXPORT_SYMBOL(sdw_shutdown_stream);
