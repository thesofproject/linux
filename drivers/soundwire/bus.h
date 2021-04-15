/* SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause) */
/* Copyright(c) 2015-17 Intel Corporation. */

#ifndef __SDW_BUS_H
#define __SDW_BUS_H

#define DEFAULT_BANK_SWITCH_TIMEOUT 3000
#define DEFAULT_PROBE_TIMEOUT       2000

u64 sdw_dmi_override_adr(struct sdw_bus *bus, u64 addr);

#if IS_ENABLED(CONFIG_ACPI)
int sdw_acpi_find_peripherals(struct sdw_bus *bus);
#else
static inline int sdw_acpi_find_peripherals(struct sdw_bus *bus)
{
	return -ENOTSUPP;
}
#endif

int sdw_of_find_peripherals(struct sdw_bus *bus);
void sdw_extract_peripheral_id(struct sdw_bus *bus,
			       u64 addr, struct sdw_peripheral_id *id);
int sdw_peripheral_add(struct sdw_bus *bus, struct sdw_peripheral_id *id,
		       struct fwnode_handle *fwnode);
int sdw_manager_device_add(struct sdw_bus *bus, struct device *parent,
			   struct fwnode_handle *fwnode);
int sdw_manager_device_del(struct sdw_bus *bus);

#ifdef CONFIG_DEBUG_FS
void sdw_bus_debugfs_init(struct sdw_bus *bus);
void sdw_bus_debugfs_exit(struct sdw_bus *bus);
void sdw_peripheral_debugfs_init(struct sdw_peripheral *peripheral);
void sdw_peripheral_debugfs_exit(struct sdw_peripheral *peripheral);
void sdw_debugfs_init(void);
void sdw_debugfs_exit(void);
#else
static inline void sdw_bus_debugfs_init(struct sdw_bus *bus) {}
static inline void sdw_bus_debugfs_exit(struct sdw_bus *bus) {}
static inline void sdw_peripheral_debugfs_init(struct sdw_peripheral *peripheral) {}
static inline void sdw_peripheral_debugfs_exit(struct sdw_peripheral *peripheral) {}
static inline void sdw_debugfs_init(void) {}
static inline void sdw_debugfs_exit(void) {}
#endif

enum {
	SDW_MSG_FLAG_READ = 0,
	SDW_MSG_FLAG_WRITE,
};

/**
 * struct sdw_msg - Message structure
 * @addr: Register address accessed in the Peripheral
 * @len: number of messages
 * @dev_num: Peripheral device number
 * @addr_page1: SCP address page 1 Peripheral register
 * @addr_page2: SCP address page 2 Peripheral register
 * @flags: transfer flags, indicate if xfer is read or write
 * @buf: message data buffer
 * @ssp_sync: Send message at SSP (Stream Synchronization Point)
 * @page: address requires paging
 */
struct sdw_msg {
	u16 addr;
	u16 len;
	u8 dev_num;
	u8 addr_page1;
	u8 addr_page2;
	u8 flags;
	u8 *buf;
	bool ssp_sync;
	bool page;
};

#define SDW_DOUBLE_RATE_FACTOR		2
#define SDW_STRM_RATE_GROUPING		1

extern int sdw_rows[SDW_FRAME_ROWS];
extern int sdw_cols[SDW_FRAME_COLS];

int sdw_find_row_index(int row);
int sdw_find_col_index(int col);

/**
 * sdw_port_runtime: Runtime port parameters for Manager or Peripheral
 *
 * @num: Port number. For audio streams, valid port number ranges from
 * [1,14]
 * @ch_mask: Channel mask
 * @transport_params: Transport parameters
 * @port_params: Port parameters
 * @port_node: List node for Manager or Peripheral port_list
 *
 * SoundWire spec has no mention of ports for Manager interface but the
 * concept is logically extended.
 */
struct sdw_port_runtime {
	int num;
	int ch_mask;
	struct sdw_transport_params transport_params;
	struct sdw_port_params port_params;
	struct list_head port_node;
};

/**
 * sdw_peripheral_runtime: Runtime Stream parameters for Peripheral
 *
 * @peripheral: Peripheral handle
 * @direction: Data direction for Peripheral
 * @ch_count: Number of channels handled by the Peripheral for
 * this stream
 * @m_rt_node: sdw_manager_runtime list node
 * @port_list: List of Peripheral Ports configured for this stream
 */
struct sdw_peripheral_runtime {
	struct sdw_peripheral *peripheral;
	enum sdw_data_direction direction;
	unsigned int ch_count;
	struct list_head m_rt_node;
	struct list_head port_list;
};

/**
 * sdw_manager_runtime: Runtime stream parameters for Manager
 *
 * @bus: Bus handle
 * @stream: Stream runtime handle
 * @direction: Data direction for Manager
 * @ch_count: Number of channels handled by the Manager for
 * this stream, can be zero.
 * @peripheral_rt_list: Peripheral runtime list
 * @port_list: List of Manager Ports configured for this stream, can be zero.
 * @stream_node: sdw_stream_runtime manager_list node
 * @bus_node: sdw_bus m_rt_list node
 */
struct sdw_manager_runtime {
	struct sdw_bus *bus;
	struct sdw_stream_runtime *stream;
	enum sdw_data_direction direction;
	unsigned int ch_count;
	struct list_head peripheral_rt_list;
	struct list_head port_list;
	struct list_head stream_node;
	struct list_head bus_node;
};

struct sdw_dpn_prop *sdw_get_peripheral_dpn_prop(struct sdw_peripheral *peripheral,
						 enum sdw_data_direction direction,
						 unsigned int port_num);
int sdw_configure_dpn_intr(struct sdw_peripheral *peripheral, int port,
			   bool enable, int mask);

int sdw_transfer(struct sdw_bus *bus, struct sdw_msg *msg);
int sdw_transfer_defer(struct sdw_bus *bus, struct sdw_msg *msg,
		       struct sdw_defer *defer);

#define SDW_READ_INTR_CLEAR_RETRY	10

int sdw_fill_msg(struct sdw_msg *msg, struct sdw_peripheral *peripheral,
		 u32 addr, size_t count, u16 dev_num, u8 flags, u8 *buf);

/* Retrieve and return channel count from channel mask */
static inline int sdw_ch_mask_to_ch(int ch_mask)
{
	int c = 0;

	for (c = 0; ch_mask; ch_mask >>= 1)
		c += ch_mask & 1;

	return c;
}

/* Fill transport parameter data structure */
static inline void sdw_fill_xport_params(struct sdw_transport_params *params,
					 int port_num, bool grp_ctrl_valid,
					 int grp_ctrl, int sample_int,
					 int off1, int off2,
					 int hstart, int hstop,
					 int pack_mode, int lane_ctrl)
{
	params->port_num = port_num;
	params->blk_grp_ctrl_valid = grp_ctrl_valid;
	params->blk_grp_ctrl = grp_ctrl;
	params->sample_interval = sample_int;
	params->offset1 = off1;
	params->offset2 = off2;
	params->hstart = hstart;
	params->hstop = hstop;
	params->blk_pkg_mode = pack_mode;
	params->lane_ctrl = lane_ctrl;
}

/* Fill port parameter data structure */
static inline void sdw_fill_port_params(struct sdw_port_params *params,
					int port_num, int bps,
					int flow_mode, int data_mode)
{
	params->num = port_num;
	params->bps = bps;
	params->flow_mode = flow_mode;
	params->data_mode = data_mode;
}

/* Read-Modify-Write Peripheral register */
static inline int sdw_update(struct sdw_peripheral *peripheral, u32 addr, u8 mask, u8 val)
{
	int tmp;

	tmp = sdw_read(peripheral, addr);
	if (tmp < 0)
		return tmp;

	tmp = (tmp & ~mask) | val;
	return sdw_write(peripheral, addr, tmp);
}

/* broadcast read/write for tests */
int sdw_bread_no_pm_unlocked(struct sdw_bus *bus, u16 dev_num, u32 addr);
int sdw_bwrite_no_pm_unlocked(struct sdw_bus *bus, u16 dev_num, u32 addr, u8 value);

/*
 * At the moment we only track Manager-initiated hw_reset.
 * Additional fields can be added as needed
 */
#define SDW_UNATTACH_REQUEST_MANAGER_RESET	BIT(0)

void sdw_clear_peripheral_status(struct sdw_bus *bus, u32 request);
int sdw_peripheral_modalias(const struct sdw_peripheral *peripheral, char *buf, size_t size);

#define sdw_dev_dbg_or_err(dev, is_err, fmt, ...)			\
	do {								\
		if (is_err)						\
			dev_err(dev, fmt, __VA_ARGS__);			\
		else							\
			dev_dbg(dev, fmt, __VA_ARGS__);			\
	} while (0)

#endif /* __SDW_BUS_H */
