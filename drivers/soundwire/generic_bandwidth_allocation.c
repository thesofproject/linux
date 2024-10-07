// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
// Copyright(c) 2015-2020 Intel Corporation.

/*
 * Bandwidth management algorithm based on 2^n gears
 *
 */

#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/slab.h>
#include <linux/soundwire/sdw.h>
#include "bus.h"

#define SDW_STRM_RATE_GROUPING		1

struct sdw_group_params {
	unsigned int rate;
	unsigned int lane;
	int full_bw;
	int payload_bw;
	int hwidth;
};

struct sdw_group {
	unsigned int count;
	unsigned int max_size;
	unsigned int *rates;
	unsigned int *lanes;
};

void sdw_compute_slave_ports(struct sdw_master_runtime *m_rt,
			     struct sdw_transport_data *t_data)
{
	struct sdw_slave_runtime *s_rt = NULL;
	struct sdw_port_runtime *p_rt;
	int port_bo, sample_int;
	unsigned int rate, bps, ch = 0;
	unsigned int slave_total_ch;
	struct sdw_bus_params *b_params = &m_rt->bus->params;

	port_bo = t_data->block_offset;

	list_for_each_entry(s_rt, &m_rt->slave_rt_list, m_rt_node) {
		rate = m_rt->stream->params.rate;
		bps = m_rt->stream->params.bps;
		sample_int = (m_rt->bus->params.curr_dr_freq / rate);
		slave_total_ch = 0;

		list_for_each_entry(p_rt, &s_rt->port_list, port_node) {
			if (p_rt->lane != t_data->lane)
				continue;

			ch = hweight32(p_rt->ch_mask);

			dev_dbg(&s_rt->slave->dev, "%s p_rt->lane %d\n", __func__, p_rt->lane);
			sdw_fill_xport_params(&p_rt->transport_params,
					      p_rt->num, false,
					      SDW_BLK_GRP_CNT_1,
					      sample_int, port_bo, port_bo >> 8,
					      t_data->hstart,
					      t_data->hstop,
					      SDW_BLK_PKG_PER_PORT, p_rt->lane);

			sdw_fill_port_params(&p_rt->port_params,
					     p_rt->num, bps,
					     SDW_PORT_FLOW_MODE_ISOCH,
					     b_params->s_data_mode);

			port_bo += bps * ch;
			slave_total_ch += ch;
		}

		if (m_rt->direction == SDW_DATA_DIR_TX &&
		    m_rt->ch_count == slave_total_ch) {
			/*
			 * Slave devices were configured to access all channels
			 * of the stream, which indicates that they operate in
			 * 'mirror mode'. Make sure we reset the port offset for
			 * the next device in the list
			 */
			port_bo = t_data->block_offset;
		}
	}
}
EXPORT_SYMBOL(sdw_compute_slave_ports);

static void sdw_compute_master_ports(struct sdw_master_runtime *m_rt,
				     struct sdw_group_params *params,
				     int *port_bo, int hstop)
{
	struct sdw_transport_data t_data = {0};
	struct sdw_port_runtime *p_rt;
	struct sdw_bus *bus = m_rt->bus;
	struct sdw_bus_params *b_params = &bus->params;
	int sample_int, hstart = 0;
	unsigned int rate, bps, ch;

	rate = m_rt->stream->params.rate;
	bps = m_rt->stream->params.bps;
	ch = m_rt->ch_count;
	sample_int = (bus->params.curr_dr_freq / rate);

	if (rate != params->rate)
		return;

	t_data.hstop = hstop;
	hstart = hstop - params->hwidth + 1;
	t_data.hstart = hstart;

	list_for_each_entry(p_rt, &m_rt->port_list, port_node) {
		if (p_rt->lane != params->lane)
			continue;

		dev_dbg(bus->dev, "%s p_rt->lane %d\n", __func__, p_rt->lane);
		sdw_fill_xport_params(&p_rt->transport_params, p_rt->num,
				      false, SDW_BLK_GRP_CNT_1, sample_int,
				      *port_bo, (*port_bo) >> 8, hstart, hstop,
				      SDW_BLK_PKG_PER_PORT, p_rt->lane);

		sdw_fill_port_params(&p_rt->port_params,
				     p_rt->num, bps,
				     SDW_PORT_FLOW_MODE_ISOCH,
				     b_params->m_data_mode);

		/* Check for first entry */
		if (!(p_rt == list_first_entry(&m_rt->port_list,
					       struct sdw_port_runtime,
					       port_node))) {
			(*port_bo) += bps * ch;
			continue;
		}

		t_data.hstart = hstart;
		t_data.hstop = hstop;
		t_data.block_offset = *port_bo;
		t_data.sub_block_offset = 0;
		(*port_bo) += bps * ch;
	}

	t_data.lane = params->lane;
	sdw_compute_slave_ports(m_rt, &t_data);
}

static void _sdw_compute_port_params(struct sdw_bus *bus,
				     struct sdw_group_params *params, int count)
{
	struct sdw_master_runtime *m_rt;
	int port_bo, i, l;
	int hstop;

	/* Run loop for all groups to compute transport parameters */
	for (l = 0; l < SDW_MAX_LANES; l++) {
		if (l > 0 && !bus->lane_used_bandwidth[l])
			continue;
		/* reset hstop for each lane */
		hstop = bus->params.col - 1;
		for (i = 0; i < count; i++) {
			if (params[i].lane != l)
				continue;
			port_bo = 1;

			list_for_each_entry(m_rt, &bus->m_rt_list, bus_node) {
				sdw_compute_master_ports(m_rt, &params[i], &port_bo, hstop);
			}

			hstop = hstop - params[i].hwidth;
		}
	}
}

static int sdw_compute_group_params(struct sdw_bus *bus,
				    struct sdw_group_params *params,
				    int *rates, int *lanes, int count)
{
	struct sdw_master_runtime *m_rt;
	struct sdw_port_runtime *p_rt;
	int sel_col = bus->params.col;
	unsigned int rate, bps, ch;
	int i, l, column_needed;

	/* Calculate bandwidth per group */
	for (i = 0; i < count; i++) {
		params[i].rate = rates[i];
		params[i].lane = lanes[i];
		params[i].full_bw = bus->params.curr_dr_freq / params[i].rate;
	}

	list_for_each_entry(m_rt, &bus->m_rt_list, bus_node) {
		list_for_each_entry(p_rt, &m_rt->port_list, port_node) {
			rate = m_rt->stream->params.rate;
			bps = m_rt->stream->params.bps;
			ch = hweight32(p_rt->ch_mask);

			for (i = 0; i < count; i++) {
				if (rate == params[i].rate && p_rt->lane == params[i].lane)
					params[i].payload_bw += bps * ch;
			}
		}
	}

	for (l = 0; l < SDW_MAX_LANES; l++) {
		if (l > 0 && !bus->lane_used_bandwidth[l])
			continue;
		/* reset column_needed for each lane */
		column_needed = 0;
		for (i = 0; i < count; i++) {
			if (params[i].lane != l)
				continue;

			params[i].hwidth = (sel_col * params[i].payload_bw +
					    params[i].full_bw - 1) / params[i].full_bw;

			column_needed += params[i].hwidth;
			/* There is no control column for lane 1 and above */
			if (column_needed > sel_col)
				return -EINVAL;
			/* Column 0 is control column on lane 0 */
			if (params[i].lane == 0 && column_needed > sel_col - 1)
				return -EINVAL;
		}
	}


	return 0;
}

static int sdw_add_element_group_count(struct sdw_group *group,
				       unsigned int rate, unsigned int lane)
{
	int num = group->count;
	int i;

	for (i = 0; i <= num; i++) {
		if (rate == group->rates[i] && lane == group->lanes[i])
			break;

		if (i != num)
			continue;

		if (group->count >= group->max_size) {
			unsigned int *rates;
			unsigned int *lanes;

			group->max_size += 1;
			rates = krealloc(group->rates,
					 (sizeof(int) * group->max_size),
					 GFP_KERNEL);
			if (!rates)
				return -ENOMEM;

			group->rates = rates;

			lanes = krealloc(group->lanes,
					 (sizeof(int) * group->max_size),
					 GFP_KERNEL);
			if (!lanes)
				return -ENOMEM;

			group->lanes = lanes;
		}

		group->rates[group->count] = rate;
		group->lanes[group->count++] = lane;
	}

	return 0;
}

static int sdw_get_group_count(struct sdw_bus *bus,
			       struct sdw_group *group)
{
	struct sdw_master_runtime *m_rt;
	struct sdw_port_runtime *p_rt;
	unsigned int rate;
	int ret = 0;

	group->count = 0;
	group->max_size = SDW_STRM_RATE_GROUPING;
	group->rates = kcalloc(group->max_size, sizeof(int), GFP_KERNEL);
	if (!group->rates)
		return -ENOMEM;

	group->lanes = kcalloc(group->max_size, sizeof(int), GFP_KERNEL);
	if (!group->lanes) {
		kfree(group->rates);
		group->rates = NULL;
		return -ENOMEM;
	}

	list_for_each_entry(m_rt, &bus->m_rt_list, bus_node) {
		if (m_rt->stream->state == SDW_STREAM_DEPREPARED)
			continue;

		rate = m_rt->stream->params.rate;
		if (m_rt == list_first_entry(&bus->m_rt_list,
					     struct sdw_master_runtime,
					     bus_node)) {
			group->rates[group->count++] = rate;
		}
		/*
		 * Different ports could use different lane, add group element
		 * even if m_rt is the first entry
		 */
		list_for_each_entry(p_rt, &m_rt->port_list, port_node) {
			ret = sdw_add_element_group_count(group, rate, p_rt->lane);
			if (ret < 0) {
				kfree(group->rates);
				kfree(group->lanes);
				return ret;
			}
		}
	}

	return ret;
}

/**
 * sdw_compute_port_params: Compute transport and port parameters
 *
 * @bus: SDW Bus instance
 */
static int sdw_compute_port_params(struct sdw_bus *bus)
{
	struct sdw_group_params *params = NULL;
	struct sdw_group group;
	int ret;

	ret = sdw_get_group_count(bus, &group);
	if (ret < 0)
		return ret;

	if (group.count == 0)
		goto out;

	params = kcalloc(group.count, sizeof(*params), GFP_KERNEL);
	if (!params) {
		ret = -ENOMEM;
		goto out;
	}

	/* Compute transport parameters for grouped streams */
	ret = sdw_compute_group_params(bus, params,
				       &group.rates[0], &group.lanes[0], group.count);
	if (ret < 0)
		goto free_params;

	_sdw_compute_port_params(bus, params, group.count);

free_params:
	kfree(params);
out:
	kfree(group.rates);
	kfree(group.lanes);

	return ret;
}

static int sdw_select_row_col(struct sdw_bus *bus, int clk_freq)
{
	struct sdw_master_prop *prop = &bus->prop;
	int r, c;

	for (c = 0; c < SDW_FRAME_COLS; c++) {
		for (r = 0; r < SDW_FRAME_ROWS; r++) {
			if (sdw_rows[r] != prop->default_row ||
			    sdw_cols[c] != prop->default_col)
				continue;

			if (clk_freq * (sdw_cols[c] - 1) <
			    bus->params.bandwidth * sdw_cols[c])
				continue;

			bus->params.row = sdw_rows[r];
			bus->params.col = sdw_cols[c];
			return 0;
		}
	}

	return -EINVAL;
}

static bool is_clock_scaling_supported(struct sdw_bus *bus)
{
	struct sdw_master_runtime *m_rt;
	struct sdw_slave_runtime *s_rt;

	list_for_each_entry(m_rt, &bus->m_rt_list, bus_node) {
		list_for_each_entry(s_rt, &m_rt->slave_rt_list, m_rt_node) {
			if (!is_clock_scaling_supported_by_slave(s_rt->slave)) {
				return false;
			}
		}
	}
	return true;
}

/**
 * check_all_peripherals_connected: Check if all peripherals can use the lane
 *
 * @m_rt: Manager runtime
 * @lane: Lane number
 */
static bool check_all_peripherals_connected(struct sdw_master_runtime *m_rt, unsigned int lane)
{
	struct sdw_slave_prop *slave_prop;
	struct sdw_slave_runtime *s_rt;
	int i;

	list_for_each_entry(s_rt, &m_rt->slave_rt_list, m_rt_node) {
		slave_prop = &s_rt->slave->prop;
		for (i = 1; i < SDW_MAX_LANES; i++) {
			if (slave_prop->lane_maps[i] == lane) {
				dev_dbg(&s_rt->slave->dev,
					"lane_maps[%d] is connected to %d\n",
					i, slave_prop->lane_maps[i]);
				break;
			}
		}
		if (i == SDW_MAX_LANES) {
			dev_dbg(&s_rt->slave->dev, "%d is not connected\n", lane);
			return false;
		}
	}
	return true;
}

/**
 * sdw_compute_bus_params: Compute bus parameters
 *
 * @bus: SDW Bus instance
 */
static int sdw_compute_bus_params(struct sdw_bus *bus)
{
	struct sdw_master_prop *mstr_prop = &bus->prop;
	struct sdw_slave_prop *slave_prop;
	struct sdw_port_runtime *m_p_rt;
	struct sdw_port_runtime *s_p_rt;
	struct sdw_master_runtime *m_rt;
	unsigned int required_bandwidth;
	struct sdw_slave_runtime *s_rt;
	unsigned int curr_dr_freq = 0;
	bool use_multi_lane = false;
	int i, l, clk_values, ret;
	bool is_gear = false;
	u32 *clk_buf;
	int m_lane;

	if (mstr_prop->num_clk_gears) {
		clk_values = mstr_prop->num_clk_gears;
		clk_buf = mstr_prop->clk_gears;
		is_gear = true;
	} else if (mstr_prop->num_clk_freq) {
		clk_values = mstr_prop->num_clk_freq;
		clk_buf = mstr_prop->clk_freq;
	} else {
		clk_values = 1;
		clk_buf = NULL;
	}

	/* If dynamic scaling is not supported, don't try higher freq */
	if (!is_clock_scaling_supported(bus))
		clk_values = 1;

	for (i = 0; i < clk_values; i++) {
		if (!clk_buf)
			curr_dr_freq = bus->params.max_dr_freq;
		else
			curr_dr_freq = (is_gear) ?
				(bus->params.max_dr_freq >>  clk_buf[i]) :
				clk_buf[i] * SDW_DOUBLE_RATE_FACTOR;

		if (curr_dr_freq * (mstr_prop->default_col - 1) >=
		    bus->params.bandwidth * mstr_prop->default_col)
			break;

		list_for_each_entry(m_rt, &bus->m_rt_list, bus_node) {
			/*
			 * Get the first s_rt that will be used to find the available lane that can be used.
			 */
			s_rt = list_first_entry(&m_rt->slave_rt_list, struct sdw_slave_runtime, m_rt_node);
			slave_prop = &s_rt->slave->prop;

			/*
			 * Find available manager lanes that connected to the first Peripheral.
			 * No need to check all Peripherals available lanes because we can't use
			 * multi-lane if we can't find any available lane for the first Peripheral.
			 */
			for (l = 1; l < SDW_MAX_LANES; l++) {
				if (!slave_prop->lane_maps[l])
					continue;

				dev_dbg(bus->dev, "%s: trying lane %d\n", __func__, l);
				required_bandwidth = 0;
				list_for_each_entry(m_p_rt, &m_rt->port_list, port_node) {
					required_bandwidth += m_rt->stream->params.rate *
						hweight32(m_p_rt->ch_mask) *
						m_rt->stream->params.bps;
				}
				if (required_bandwidth <= curr_dr_freq - bus->lane_used_bandwidth[l]) {
					/* Check if m_lane is connected to all Peripherals */
					if (!check_all_peripherals_connected(m_rt, slave_prop->lane_maps[l])) {
						dev_dbg(bus->dev,
							"some Peripherals are not connected to %d\n",
							slave_prop->lane_maps[l]);
						continue;
					}
					m_lane = slave_prop->lane_maps[l];
					dev_dbg(&s_rt->slave->dev,
						"M lane %d P lane %d can be used\n",
						m_lane, l);
					bus->lane_used_bandwidth[l] += required_bandwidth;
					/*
					 * Use non-zero manager lane, subtract the lane 0
					 * bandwidth that is already calculated
					 */
					bus->params.bandwidth -= required_bandwidth;
					use_multi_lane = true;
					goto out;
				}
			}
		}

		/*
		 * TODO: Check all the Slave(s) port(s) audio modes and find
		 * whether given clock rate is supported with glitchless
		 * transition.
		 */
	}

	if (i == clk_values) {
		dev_err(bus->dev, "%s: could not find clock value for bandwidth %d\n",
			__func__, bus->params.bandwidth);
		return -EINVAL;
	}
out:
	if (use_multi_lane) {
		/* Set Peripheral lanes */
		list_for_each_entry(s_rt, &m_rt->slave_rt_list, m_rt_node) {
			slave_prop = &s_rt->slave->prop;
			for (l = 1; l < SDW_MAX_LANES; l++) {
				if (slave_prop->lane_maps[l] == m_lane) {
					dev_dbg(&s_rt->slave->dev, "Set Peripheral lane = %d\n", l);
					list_for_each_entry(s_p_rt, &s_rt->port_list, port_node) {
						s_p_rt->lane = l;
					}
					break;
				}
			}
		}
		/*
		 * Set Manager lanes. Configure the last m_rt in bus->m_rt_list only since
		 * we don't want to touch other m_rts that are already working.
		 */
		list_for_each_entry(m_p_rt, &m_rt->port_list, port_node) {
			m_p_rt->lane = m_lane;
		}
	}

	mstr_prop->default_col = curr_dr_freq / mstr_prop->default_frame_rate / mstr_prop->default_row;

	ret = sdw_select_row_col(bus, curr_dr_freq);
	if (ret < 0) {
		dev_err(bus->dev, "%s: could not find frame configuration for bus dr_freq %d\n",
			__func__, curr_dr_freq);
		return -EINVAL;
	}

	bus->params.curr_dr_freq = curr_dr_freq;
	return 0;
}

/**
 * sdw_compute_params: Compute bus, transport and port parameters
 *
 * @bus: SDW Bus instance
 */
int sdw_compute_params(struct sdw_bus *bus)
{
	int ret;

	/* Computes clock frequency, frame shape and frame frequency */
	ret = sdw_compute_bus_params(bus);
	if (ret < 0)
		return ret;

	/* Compute transport and port params */
	ret = sdw_compute_port_params(bus);
	if (ret < 0) {
		dev_err(bus->dev, "Compute transport params failed: %d\n", ret);
		return ret;
	}

	return 0;
}
EXPORT_SYMBOL(sdw_compute_params);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("SoundWire Generic Bandwidth Allocation");
