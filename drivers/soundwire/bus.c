// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
// Copyright(c) 2015-17 Intel Corporation.

#include <linux/acpi.h>
#include <linux/delay.h>
#include <linux/mod_devicetable.h>
#include <linux/pm_runtime.h>
#include <linux/soundwire/sdw_registers.h>
#include <linux/soundwire/sdw.h>
#include "bus.h"
#include "sysfs_local.h"

static DEFINE_IDA(sdw_ida);

static int sdw_get_id(struct sdw_bus *bus)
{
	int rc = ida_alloc(&sdw_ida, GFP_KERNEL);

	if (rc < 0)
		return rc;

	bus->id = rc;
	return 0;
}

/**
 * sdw_bus_manager_add() - add a bus Manager instance
 * @bus: bus instance
 * @parent: parent device
 * @fwnode: firmware node handle
 *
 * Initializes the bus instance, read properties and create child
 * devices.
 */
int sdw_bus_manager_add(struct sdw_bus *bus, struct device *parent,
			struct fwnode_handle *fwnode)
{
	struct sdw_manager_prop *prop = NULL;
	int ret;

	if (!parent) {
		pr_err("SoundWire parent device is not set\n");
		return -ENODEV;
	}

	ret = sdw_get_id(bus);
	if (ret < 0) {
		dev_err(parent, "Failed to get bus id\n");
		return ret;
	}

	ret = sdw_manager_device_add(bus, parent, fwnode);
	if (ret < 0) {
		dev_err(parent, "Failed to add manager device at link %d\n",
			bus->link_id);
		return ret;
	}

	if (!bus->ops) {
		dev_err(bus->dev, "SoundWire Bus ops are not set\n");
		return -EINVAL;
	}

	if (!bus->compute_params) {
		dev_err(bus->dev,
			"Bandwidth allocation not configured, compute_params no set\n");
		return -EINVAL;
	}

	mutex_init(&bus->msg_lock);
	mutex_init(&bus->bus_lock);
	INIT_LIST_HEAD(&bus->peripherals);
	INIT_LIST_HEAD(&bus->m_rt_list);

	/*
	 * Initialize multi_link flag
	 * TODO: populate this flag by reading property from FW node
	 */
	bus->multi_link = false;
	if (bus->ops->read_prop) {
		ret = bus->ops->read_prop(bus);
		if (ret < 0) {
			dev_err(bus->dev,
				"Bus read properties failed:%d\n", ret);
			return ret;
		}
	}

	sdw_bus_debugfs_init(bus);

	/*
	 * Device numbers in SoundWire are 0 through 15. Enumeration device
	 * number (0), Broadcast device number (15), Group numbers (12 and
	 * 13) and Manager device number (14) are not used for assignment so
	 * mask these and other higher bits.
	 */

	/* Set higher order bits */
	*bus->assigned = ~GENMASK(SDW_BROADCAST_DEV_NUM, SDW_ENUM_DEV_NUM);

	/* Set enumuration device number and broadcast device number */
	set_bit(SDW_ENUM_DEV_NUM, bus->assigned);
	set_bit(SDW_BROADCAST_DEV_NUM, bus->assigned);

	/* Set group device numbers and manager device number */
	set_bit(SDW_GROUP12_DEV_NUM, bus->assigned);
	set_bit(SDW_GROUP13_DEV_NUM, bus->assigned);
	set_bit(SDW_MANAGER_DEV_NUM, bus->assigned);

	/*
	 * SDW is an enumerable bus, but devices can be powered off. So,
	 * they won't be able to report as present.
	 *
	 * Create Peripheral devices based on Peripherals described in
	 * the respective firmware (ACPI/DT)
	 */
	if (IS_ENABLED(CONFIG_ACPI) && ACPI_HANDLE(bus->dev))
		ret = sdw_acpi_find_peripherals(bus);
	else if (IS_ENABLED(CONFIG_OF) && bus->dev->of_node)
		ret = sdw_of_find_peripherals(bus);
	else
		ret = -ENOTSUPP; /* No ACPI/DT so error out */

	if (ret < 0) {
		dev_err(bus->dev, "Finding peripherals failed:%d\n", ret);
		return ret;
	}

	/*
	 * Initialize clock values based on Manager properties. The max
	 * frequency is read from max_clk_freq property. Current assumption
	 * is that the bus will start at highest clock frequency when
	 * powered on.
	 *
	 * Default active bank will be 0 as out of reset the Peripherals have
	 * to start with bank 0 (Table 40 of Spec)
	 */
	prop = &bus->prop;
	bus->params.max_dr_freq = prop->max_clk_freq * SDW_DOUBLE_RATE_FACTOR;
	bus->params.curr_dr_freq = bus->params.max_dr_freq;
	bus->params.curr_bank = SDW_BANK0;
	bus->params.next_bank = SDW_BANK1;

	return 0;
}
EXPORT_SYMBOL(sdw_bus_manager_add);

static int sdw_delete_peripheral(struct device *dev, void *data)
{
	struct sdw_peripheral *peripheral = dev_to_sdw_dev(dev);
	struct sdw_bus *bus = peripheral->bus;

	pm_runtime_disable(dev);

	sdw_peripheral_debugfs_exit(peripheral);

	mutex_lock(&bus->bus_lock);

	if (peripheral->dev_num) /* clear dev_num if assigned */
		clear_bit(peripheral->dev_num, bus->assigned);

	list_del_init(&peripheral->node);
	mutex_unlock(&bus->bus_lock);

	device_unregister(dev);
	return 0;
}

/**
 * sdw_bus_manager_delete() - delete the bus manager instance
 * @bus: bus to be deleted
 *
 * Remove the instance, delete the child devices.
 */
void sdw_bus_manager_delete(struct sdw_bus *bus)
{
	device_for_each_child(bus->dev, NULL, sdw_delete_peripheral);
	sdw_manager_device_del(bus);

	sdw_bus_debugfs_exit(bus);
	ida_free(&sdw_ida, bus->id);
}
EXPORT_SYMBOL(sdw_bus_manager_delete);

/*
 * SDW IO Calls
 */

static inline int find_response_code(enum sdw_command_response resp)
{
	switch (resp) {
	case SDW_CMD_OK:
		return 0;

	case SDW_CMD_IGNORED:
		return -ENODATA;

	case SDW_CMD_TIMEOUT:
		return -ETIMEDOUT;

	default:
		return -EIO;
	}
}

static inline int do_transfer(struct sdw_bus *bus, struct sdw_msg *msg)
{
	int retry = bus->prop.err_threshold;
	enum sdw_command_response resp;
	int ret = 0, i;

	for (i = 0; i <= retry; i++) {
		resp = bus->ops->xfer_msg(bus, msg);
		ret = find_response_code(resp);

		/* if cmd is ok or ignored return */
		if (ret == 0 || ret == -ENODATA)
			return ret;
	}

	return ret;
}

static inline int do_transfer_defer(struct sdw_bus *bus,
				    struct sdw_msg *msg,
				    struct sdw_defer *defer)
{
	int retry = bus->prop.err_threshold;
	enum sdw_command_response resp;
	int ret = 0, i;

	defer->msg = msg;
	defer->length = msg->len;
	init_completion(&defer->complete);

	for (i = 0; i <= retry; i++) {
		resp = bus->ops->xfer_msg_defer(bus, msg, defer);
		ret = find_response_code(resp);
		/* if cmd is ok or ignored return */
		if (ret == 0 || ret == -ENODATA)
			return ret;
	}

	return ret;
}

static int sdw_reset_page(struct sdw_bus *bus, u16 dev_num)
{
	int retry = bus->prop.err_threshold;
	enum sdw_command_response resp;
	int ret = 0, i;

	for (i = 0; i <= retry; i++) {
		resp = bus->ops->reset_page_addr(bus, dev_num);
		ret = find_response_code(resp);
		/* if cmd is ok or ignored return */
		if (ret == 0 || ret == -ENODATA)
			return ret;
	}

	return ret;
}

static int sdw_transfer_unlocked(struct sdw_bus *bus, struct sdw_msg *msg)
{
	int ret;

	ret = do_transfer(bus, msg);
	if (ret != 0 && ret != -ENODATA)
		dev_err(bus->dev, "trf on Peripheral %d failed:%d %s addr %x count %d\n",
			msg->dev_num, ret,
			(msg->flags & SDW_MSG_FLAG_WRITE) ? "write" : "read",
			msg->addr, msg->len);

	if (msg->page)
		sdw_reset_page(bus, msg->dev_num);

	return ret;
}

/**
 * sdw_transfer() - Synchronous transfer message to a SDW Peripheral device
 * @bus: SDW bus
 * @msg: SDW message to be xfered
 */
int sdw_transfer(struct sdw_bus *bus, struct sdw_msg *msg)
{
	int ret;

	mutex_lock(&bus->msg_lock);

	ret = sdw_transfer_unlocked(bus, msg);

	mutex_unlock(&bus->msg_lock);

	return ret;
}

/**
 * sdw_transfer_defer() - Asynchronously transfer message to a SDW Peripheral device
 * @bus: SDW bus
 * @msg: SDW message to be xfered
 * @defer: Defer block for signal completion
 *
 * Caller needs to hold the msg_lock lock while calling this
 */
int sdw_transfer_defer(struct sdw_bus *bus, struct sdw_msg *msg,
		       struct sdw_defer *defer)
{
	int ret;

	if (!bus->ops->xfer_msg_defer)
		return -ENOTSUPP;

	ret = do_transfer_defer(bus, msg, defer);
	if (ret != 0 && ret != -ENODATA)
		dev_err(bus->dev, "Defer trf on Peripheral %d failed:%d\n",
			msg->dev_num, ret);

	if (msg->page)
		sdw_reset_page(bus, msg->dev_num);

	return ret;
}

int sdw_fill_msg(struct sdw_msg *msg, struct sdw_peripheral *peripheral,
		 u32 addr, size_t count, u16 dev_num, u8 flags, u8 *buf)
{
	memset(msg, 0, sizeof(*msg));
	msg->addr = addr; /* addr is 16 bit and truncated here */
	msg->len = count;
	msg->dev_num = dev_num;
	msg->flags = flags;
	msg->buf = buf;

	if (addr < SDW_REG_NO_PAGE) /* no paging area */
		return 0;

	if (addr >= SDW_REG_MAX) { /* illegal addr */
		pr_err("SDW: Invalid address %x passed\n", addr);
		return -EINVAL;
	}

	if (addr < SDW_REG_OPTIONAL_PAGE) { /* 32k but no page */
		if (peripheral && !peripheral->prop.paging_support)
			return 0;
		/* no need for else as that will fall-through to paging */
	}

	/* paging mandatory */
	if (dev_num == SDW_ENUM_DEV_NUM || dev_num == SDW_BROADCAST_DEV_NUM) {
		pr_err("SDW: Invalid device for paging :%d\n", dev_num);
		return -EINVAL;
	}

	if (!peripheral) {
		pr_err("SDW: No peripheral for paging addr\n");
		return -EINVAL;
	}

	if (!peripheral->prop.paging_support) {
		dev_err(&peripheral->dev,
			"address %x needs paging but no support\n", addr);
		return -EINVAL;
	}

	msg->addr_page1 = FIELD_GET(SDW_SCP_ADDRPAGE1_MASK, addr);
	msg->addr_page2 = FIELD_GET(SDW_SCP_ADDRPAGE2_MASK, addr);
	msg->addr |= BIT(15);
	msg->page = true;

	return 0;
}

/*
 * Read/Write IO functions.
 * no_pm versions can only be called by the bus, e.g. while enumerating or
 * handling suspend-resume sequences.
 * all clients need to use the pm versions
 */

static int
sdw_nread_no_pm(struct sdw_peripheral *peripheral, u32 addr, size_t count, u8 *val)
{
	struct sdw_msg msg;
	int ret;

	ret = sdw_fill_msg(&msg, peripheral, addr, count,
			   peripheral->dev_num, SDW_MSG_FLAG_READ, val);
	if (ret < 0)
		return ret;

	return sdw_transfer(peripheral->bus, &msg);
}

static int
sdw_nwrite_no_pm(struct sdw_peripheral *peripheral, u32 addr, size_t count, u8 *val)
{
	struct sdw_msg msg;
	int ret;

	ret = sdw_fill_msg(&msg, peripheral, addr, count,
			   peripheral->dev_num, SDW_MSG_FLAG_WRITE, val);
	if (ret < 0)
		return ret;

	return sdw_transfer(peripheral->bus, &msg);
}

int sdw_write_no_pm(struct sdw_peripheral *peripheral, u32 addr, u8 value)
{
	return sdw_nwrite_no_pm(peripheral, addr, 1, &value);
}
EXPORT_SYMBOL(sdw_write_no_pm);

static int
sdw_bread_no_pm(struct sdw_bus *bus, u16 dev_num, u32 addr)
{
	struct sdw_msg msg;
	u8 buf;
	int ret;

	ret = sdw_fill_msg(&msg, NULL, addr, 1, dev_num,
			   SDW_MSG_FLAG_READ, &buf);
	if (ret < 0)
		return ret;

	ret = sdw_transfer(bus, &msg);
	if (ret < 0)
		return ret;

	return buf;
}

static int
sdw_bwrite_no_pm(struct sdw_bus *bus, u16 dev_num, u32 addr, u8 value)
{
	struct sdw_msg msg;
	int ret;

	ret = sdw_fill_msg(&msg, NULL, addr, 1, dev_num,
			   SDW_MSG_FLAG_WRITE, &value);
	if (ret < 0)
		return ret;

	return sdw_transfer(bus, &msg);
}

int sdw_bread_no_pm_unlocked(struct sdw_bus *bus, u16 dev_num, u32 addr)
{
	struct sdw_msg msg;
	u8 buf;
	int ret;

	ret = sdw_fill_msg(&msg, NULL, addr, 1, dev_num,
			   SDW_MSG_FLAG_READ, &buf);
	if (ret < 0)
		return ret;

	ret = sdw_transfer_unlocked(bus, &msg);
	if (ret < 0)
		return ret;

	return buf;
}
EXPORT_SYMBOL(sdw_bread_no_pm_unlocked);

int sdw_bwrite_no_pm_unlocked(struct sdw_bus *bus, u16 dev_num, u32 addr, u8 value)
{
	struct sdw_msg msg;
	int ret;

	ret = sdw_fill_msg(&msg, NULL, addr, 1, dev_num,
			   SDW_MSG_FLAG_WRITE, &value);
	if (ret < 0)
		return ret;

	return sdw_transfer_unlocked(bus, &msg);
}
EXPORT_SYMBOL(sdw_bwrite_no_pm_unlocked);

int sdw_read_no_pm(struct sdw_peripheral *peripheral, u32 addr)
{
	u8 buf;
	int ret;

	ret = sdw_nread_no_pm(peripheral, addr, 1, &buf);
	if (ret < 0)
		return ret;
	else
		return buf;
}
EXPORT_SYMBOL(sdw_read_no_pm);

static int sdw_update_no_pm(struct sdw_peripheral *peripheral, u32 addr, u8 mask, u8 val)
{
	int tmp;

	tmp = sdw_read_no_pm(peripheral, addr);
	if (tmp < 0)
		return tmp;

	tmp = (tmp & ~mask) | val;
	return sdw_write_no_pm(peripheral, addr, tmp);
}

/**
 * sdw_nread() - Read "n" contiguous SDW Peripheral registers
 * @peripheral: SDW Peripheral
 * @addr: Register address
 * @count: length
 * @val: Buffer for values to be read
 */
int sdw_nread(struct sdw_peripheral *peripheral, u32 addr, size_t count, u8 *val)
{
	int ret;

	ret = pm_runtime_get_sync(&peripheral->dev);
	if (ret < 0 && ret != -EACCES) {
		pm_runtime_put_noidle(&peripheral->dev);
		return ret;
	}

	ret = sdw_nread_no_pm(peripheral, addr, count, val);

	pm_runtime_mark_last_busy(&peripheral->dev);
	pm_runtime_put(&peripheral->dev);

	return ret;
}
EXPORT_SYMBOL(sdw_nread);

/**
 * sdw_nwrite() - Write "n" contiguous SDW Peripheral registers
 * @peripheral: SDW Peripheral
 * @addr: Register address
 * @count: length
 * @val: Buffer for values to be read
 */
int sdw_nwrite(struct sdw_peripheral *peripheral, u32 addr, size_t count, u8 *val)
{
	int ret;

	ret = pm_runtime_get_sync(&peripheral->dev);
	if (ret < 0 && ret != -EACCES) {
		pm_runtime_put_noidle(&peripheral->dev);
		return ret;
	}

	ret = sdw_nwrite_no_pm(peripheral, addr, count, val);

	pm_runtime_mark_last_busy(&peripheral->dev);
	pm_runtime_put(&peripheral->dev);

	return ret;
}
EXPORT_SYMBOL(sdw_nwrite);

/**
 * sdw_read() - Read a SDW Peripheral register
 * @peripheral: SDW Peripheral
 * @addr: Register address
 */
int sdw_read(struct sdw_peripheral *peripheral, u32 addr)
{
	u8 buf;
	int ret;

	ret = sdw_nread(peripheral, addr, 1, &buf);
	if (ret < 0)
		return ret;

	return buf;
}
EXPORT_SYMBOL(sdw_read);

/**
 * sdw_write() - Write a SDW Peripheral register
 * @peripheral: SDW Peripheral
 * @addr: Register address
 * @value: Register value
 */
int sdw_write(struct sdw_peripheral *peripheral, u32 addr, u8 value)
{
	return sdw_nwrite(peripheral, addr, 1, &value);
}
EXPORT_SYMBOL(sdw_write);

/*
 * SDW alert handling
 */

/* called with bus_lock held */
static struct sdw_peripheral *sdw_get_peripheral(struct sdw_bus *bus, int i)
{
	struct sdw_peripheral *peripheral;

	list_for_each_entry(peripheral, &bus->peripherals, node) {
		if (peripheral->dev_num == i)
			return peripheral;
	}

	return NULL;
}

int sdw_compare_devid(struct sdw_peripheral *peripheral, struct sdw_peripheral_id id)
{
	if (peripheral->id.mfg_id != id.mfg_id ||
	    peripheral->id.part_id != id.part_id ||
	    peripheral->id.class_id != id.class_id ||
	    (peripheral->id.unique_id != SDW_IGNORED_UNIQUE_ID &&
	     peripheral->id.unique_id != id.unique_id))
		return -ENODEV;

	return 0;
}
EXPORT_SYMBOL(sdw_compare_devid);

/* called with bus_lock held */
static int sdw_get_device_num(struct sdw_peripheral *peripheral)
{
	int bit;

	bit = find_first_zero_bit(peripheral->bus->assigned, SDW_MAX_DEVICES);
	if (bit == SDW_MAX_DEVICES) {
		bit = -ENODEV;
		goto err;
	}

	/*
	 * Do not update dev_num in Peripheral data structure here,
	 * Update once program dev_num is successful
	 */
	set_bit(bit, peripheral->bus->assigned);

err:
	return bit;
}

static int sdw_assign_device_num(struct sdw_peripheral *peripheral)
{
	struct sdw_bus *bus = peripheral->bus;
	int ret, dev_num;
	bool new_device = false;

	/* check first if device number is assigned, if so reuse that */
	if (!peripheral->dev_num) {
		if (!peripheral->dev_num_sticky) {
			mutex_lock(&peripheral->bus->bus_lock);
			dev_num = sdw_get_device_num(peripheral);
			mutex_unlock(&peripheral->bus->bus_lock);
			if (dev_num < 0) {
				dev_err(bus->dev, "Get dev_num failed: %d\n",
					dev_num);
				return dev_num;
			}
			peripheral->dev_num = dev_num;
			peripheral->dev_num_sticky = dev_num;
			new_device = true;
		} else {
			peripheral->dev_num = peripheral->dev_num_sticky;
		}
	}

	if (!new_device)
		dev_dbg(bus->dev,
			"Peripheral already registered, reusing dev_num:%d\n",
			peripheral->dev_num);

	/* Clear the peripheral->dev_num to transfer message on device 0 */
	dev_num = peripheral->dev_num;
	peripheral->dev_num = 0;

	ret = sdw_write_no_pm(peripheral, SDW_SCP_DEVNUMBER, dev_num);
	if (ret < 0) {
		dev_err(bus->dev, "Program device_num %d failed: %d\n",
			dev_num, ret);
		return ret;
	}

	/* After xfer of msg, restore dev_num */
	peripheral->dev_num = peripheral->dev_num_sticky;

	return 0;
}

void sdw_extract_peripheral_id(struct sdw_bus *bus,
			       u64 addr, struct sdw_peripheral_id *id)
{
	dev_dbg(bus->dev, "SDW Peripheral Addr: %llx\n", addr);

	id->sdw_version = SDW_VERSION(addr);
	id->unique_id = SDW_UNIQUE_ID(addr);
	id->mfg_id = SDW_MFG_ID(addr);
	id->part_id = SDW_PART_ID(addr);
	id->class_id = SDW_CLASS_ID(addr);

	dev_dbg(bus->dev,
		"SDW Peripheral class_id 0x%02x, mfg_id 0x%04x, part_id 0x%04x, unique_id 0x%x, version 0x%x\n",
		id->class_id, id->mfg_id, id->part_id, id->unique_id, id->sdw_version);
}
EXPORT_SYMBOL(sdw_extract_peripheral_id);

static int sdw_program_device_num(struct sdw_bus *bus)
{
	u8 buf[SDW_NUM_DEV_ID_REGISTERS] = {0};
	struct sdw_peripheral *peripheral, *_s;
	struct sdw_peripheral_id id;
	struct sdw_msg msg;
	bool found;
	int count = 0, ret;
	u64 addr;

	/* No Peripheral, so use raw xfer api */
	ret = sdw_fill_msg(&msg, NULL, SDW_SCP_DEVID_0,
			   SDW_NUM_DEV_ID_REGISTERS, 0, SDW_MSG_FLAG_READ, buf);
	if (ret < 0)
		return ret;

	do {
		ret = sdw_transfer(bus, &msg);
		if (ret == -ENODATA) { /* end of device id reads */
			dev_dbg(bus->dev, "No more devices to enumerate\n");
			ret = 0;
			break;
		}
		if (ret < 0) {
			dev_err(bus->dev, "DEVID read fail:%d\n", ret);
			break;
		}

		/*
		 * Construct the addr and extract. Cast the higher shift
		 * bits to avoid truncation due to size limit.
		 */
		addr = buf[5] | (buf[4] << 8) | (buf[3] << 16) |
			((u64)buf[2] << 24) | ((u64)buf[1] << 32) |
			((u64)buf[0] << 40);

		sdw_extract_peripheral_id(bus, addr, &id);

		found = false;
		/* Now compare with entries */
		list_for_each_entry_safe(peripheral, _s, &bus->peripherals, node) {
			if (sdw_compare_devid(peripheral, id) == 0) {
				found = true;

				/*
				 * Assign a new dev_num to this Peripheral and
				 * not mark it present. It will be marked
				 * present after it reports ATTACHED on new
				 * dev_num
				 */
				ret = sdw_assign_device_num(peripheral);
				if (ret < 0) {
					dev_err(bus->dev,
						"Assign dev_num failed:%d\n",
						ret);
					return ret;
				}

				break;
			}
		}

		if (!found) {
			/* TODO: Park this device in Group 13 */

			/*
			 * add Peripheral device even if there is no platform
			 * firmware description. There will be no driver probe
			 * but the user/integration will be able to see the
			 * device, enumeration status and device number in sysfs
			 */
			sdw_peripheral_add(bus, &id, NULL);

			dev_err(bus->dev, "Peripheral Entry not found\n");
		}

		count++;

		/*
		 * Check till error out or retry (count) exhausts.
		 * Device can drop off and rejoin during enumeration
		 * so count till twice the bound.
		 */

	} while (ret == 0 && count < (SDW_MAX_DEVICES * 2));

	return ret;
}

static void sdw_modify_peripheral_status(struct sdw_peripheral *peripheral,
					 enum sdw_peripheral_status status)
{
	struct sdw_bus *bus = peripheral->bus;

	mutex_lock(&bus->bus_lock);

	dev_vdbg(bus->dev,
		 "%s: changing status peripheral %d status %d new status %d\n",
		 __func__, peripheral->dev_num, peripheral->status, status);

	if (status == SDW_PERIPHERAL_UNATTACHED) {
		dev_dbg(&peripheral->dev,
			"%s: initializing  enumeration and init completion for Peripheral %d\n",
			__func__, peripheral->dev_num);

		init_completion(&peripheral->enumeration_complete);
		init_completion(&peripheral->initialization_complete);
	} else if ((status == SDW_PERIPHERAL_ATTACHED) &&
		   (peripheral->status == SDW_PERIPHERAL_UNATTACHED)) {
		dev_dbg(&peripheral->dev,
			"%s: signaling enumeration completion for Peripheral %d\n",
			__func__, peripheral->dev_num);

		complete(&peripheral->enumeration_complete);
	}
	peripheral->status = status;
	mutex_unlock(&bus->bus_lock);
}

static enum sdw_clk_stop_mode sdw_get_clk_stop_mode(struct sdw_peripheral *peripheral)
{
	enum sdw_clk_stop_mode mode;

	/*
	 * Query for clock stop mode if Peripheral implements
	 * ops->get_clk_stop_mode, else read from property.
	 */
	if (peripheral->ops && peripheral->ops->get_clk_stop_mode) {
		mode = peripheral->ops->get_clk_stop_mode(peripheral);
	} else {
		if (peripheral->prop.clk_stop_mode1)
			mode = SDW_CLK_STOP_MODE1;
		else
			mode = SDW_CLK_STOP_MODE0;
	}

	return mode;
}

static int sdw_peripheral_clk_stop_callback(struct sdw_peripheral *peripheral,
					    enum sdw_clk_stop_mode mode,
					    enum sdw_clk_stop_type type)
{
	int ret;

	if (peripheral->ops && peripheral->ops->clk_stop) {
		ret = peripheral->ops->clk_stop(peripheral, mode, type);
		if (ret < 0) {
			sdw_dev_dbg_or_err(&peripheral->dev, ret != -ENODATA,
					   "Clk Stop mode %d type =%d failed: %d\n",
					   mode, type, ret);
			return ret;
		}
	}

	return 0;
}

static int sdw_peripheral_clk_stop_prepare(struct sdw_peripheral *peripheral,
					   enum sdw_clk_stop_mode mode,
					   bool prepare)
{
	bool wake_en;
	u32 val = 0;
	int ret;

	wake_en = peripheral->prop.wake_capable;

	if (prepare) {
		val = SDW_SCP_SYSTEMCTRL_CLK_STP_PREP;

		if (mode == SDW_CLK_STOP_MODE1)
			val |= SDW_SCP_SYSTEMCTRL_CLK_STP_MODE1;

		if (wake_en)
			val |= SDW_SCP_SYSTEMCTRL_WAKE_UP_EN;
	} else {
		ret = sdw_read_no_pm(peripheral, SDW_SCP_SYSTEMCTRL);
		if (ret < 0) {
			sdw_dev_dbg_or_err(&peripheral->dev, ret != -ENODATA,
					   "SDW_SCP_SYSTEMCTRL read failed:%d\n", ret);
			return ret;
		}
		val = ret;
		val &= ~(SDW_SCP_SYSTEMCTRL_CLK_STP_PREP);
	}

	ret = sdw_write_no_pm(peripheral, SDW_SCP_SYSTEMCTRL, val);
	if (ret < 0)
		sdw_dev_dbg_or_err(&peripheral->dev, ret != -ENODATA,
				   "SDW_SCP_SYSTEMCTRL write ignored:%d\n", ret);

	return ret;
}

static int sdw_bus_wait_for_clk_prep_deprep(struct sdw_bus *bus, u16 dev_num)
{
	int retry = bus->clk_stop_timeout;
	int val;

	do {
		val = sdw_bread_no_pm(bus, dev_num, SDW_SCP_STAT);
		if (val < 0) {
			dev_err(bus->dev, "SDW_SCP_STAT bread failed:%d\n", val);
			return val;
		}
		val &= SDW_SCP_STAT_CLK_STP_NF;
		if (!val) {
			dev_dbg(bus->dev, "clock stop prep/de-prep done peripheral:%d\n",
				dev_num);
			return 0;
		}

		usleep_range(1000, 1500);
		retry--;
	} while (retry);

	dev_err(bus->dev, "clock stop prep/de-prep failed peripheral:%d\n", dev_num);

	return -ETIMEDOUT;
}

/**
 * sdw_bus_prep_clk_stop: prepare Peripheral(s) for clock stop
 *
 * @bus: SDW bus instance
 *
 * Query Peripheral for clock stop mode and prepare for that mode.
 */
int sdw_bus_prep_clk_stop(struct sdw_bus *bus)
{
	enum sdw_clk_stop_mode peripheral_mode;
	bool simple_clk_stop = true;
	struct sdw_peripheral *peripheral;
	bool is_peripheral = false;
	int ret = 0;

	/*
	 * In order to save on transition time, prepare
	 * each Peripheral and then wait for all Peripheral(s) to be
	 * prepared for clock stop.
	 */
	list_for_each_entry(peripheral, &bus->peripherals, node) {
		if (!peripheral->dev_num)
			continue;

		if (peripheral->status != SDW_PERIPHERAL_ATTACHED &&
		    peripheral->status != SDW_PERIPHERAL_ALERT)
			continue;

		/* Identify if Peripheral(s) are available on Bus */
		is_peripheral = true;

		peripheral_mode = sdw_get_clk_stop_mode(peripheral);
		peripheral->curr_clk_stop_mode = peripheral_mode;

		ret = sdw_peripheral_clk_stop_callback(peripheral, peripheral_mode, SDW_CLK_PRE_PREPARE);
		if (ret < 0) {
			sdw_dev_dbg_or_err(&peripheral->dev, ret != -ENODATA,
					   "clock stop pre prepare cb failed:%d\n", ret);
			return ret;
		}

		ret = sdw_peripheral_clk_stop_prepare(peripheral,
						      peripheral_mode, true);
		if (ret < 0) {
			sdw_dev_dbg_or_err(&peripheral->dev, ret != -ENODATA,
					   "clock stop prepare failed:%d\n", ret);
			return ret;
		}

		if (peripheral_mode == SDW_CLK_STOP_MODE1)
			simple_clk_stop = false;
	}

	/* Skip remaining clock stop preparation if no Peripheral is attached */
	if (!is_peripheral)
		return ret;

	if (!simple_clk_stop) {
		ret = sdw_bus_wait_for_clk_prep_deprep(bus,
						       SDW_BROADCAST_DEV_NUM);
		if (ret < 0)
			return ret;
	}

	/* Inform peripherals that prep is done */
	list_for_each_entry(peripheral, &bus->peripherals, node) {
		if (!peripheral->dev_num)
			continue;

		if (peripheral->status != SDW_PERIPHERAL_ATTACHED &&
		    peripheral->status != SDW_PERIPHERAL_ALERT)
			continue;

		peripheral_mode = peripheral->curr_clk_stop_mode;

		if (peripheral_mode == SDW_CLK_STOP_MODE1) {
			ret = sdw_peripheral_clk_stop_callback(peripheral, peripheral_mode, SDW_CLK_POST_PREPARE);
			if (ret < 0) {
				sdw_dev_dbg_or_err(&peripheral->dev, ret != -ENODATA,
						   "clock stop post-prepare cb failed:%d\n", ret);
				return ret;
			}
		}
	}

	return ret;
}
EXPORT_SYMBOL(sdw_bus_prep_clk_stop);

/**
 * sdw_bus_clk_stop: stop bus clock
 *
 * @bus: SDW bus instance
 *
 * After preparing the Peripherals for clock stop, stop the clock by broadcasting
 * write to SCP_CTRL register.
 */
int sdw_bus_clk_stop(struct sdw_bus *bus)
{
	int ret;

	/*
	 * broadcast clock stop now, attached Peripherals will ACK this,
	 * unattached will ignore
	 */
	ret = sdw_bwrite_no_pm(bus, SDW_BROADCAST_DEV_NUM,
			       SDW_SCP_CTRL, SDW_SCP_CTRL_CLK_STP_NOW);
	if (ret < 0) {
		sdw_dev_dbg_or_err(bus->dev, ret != -ENODATA,
				   "ClockStopNow Broadcast msg failed %d\n", ret);
		return ret;
	}

	return 0;
}
EXPORT_SYMBOL(sdw_bus_clk_stop);

/**
 * sdw_bus_exit_clk_stop: Exit clock stop mode
 *
 * @bus: SDW bus instance
 *
 * This De-prepares the Peripherals by exiting Clock Stop Mode 0. For the Peripherals
 * exiting Clock Stop Mode 1, they will be de-prepared after they enumerate
 * back.
 */
int sdw_bus_exit_clk_stop(struct sdw_bus *bus)
{
	enum sdw_clk_stop_mode mode;
	bool simple_clk_stop = true;
	struct sdw_peripheral *peripheral;
	bool is_peripheral = false;
	int ret;

	/*
	 * In order to save on transition time, de-prepare
	 * each Peripheral and then wait for all Peripheral(s) to be
	 * de-prepared after clock resume.
	 */
	list_for_each_entry(peripheral, &bus->peripherals, node) {
		if (!peripheral->dev_num)
			continue;

		if (peripheral->status != SDW_PERIPHERAL_ATTACHED &&
		    peripheral->status != SDW_PERIPHERAL_ALERT)
			continue;

		/* Identify if Peripheral(s) are available on Bus */
		is_peripheral = true;

		mode = peripheral->curr_clk_stop_mode;

		if (mode == SDW_CLK_STOP_MODE1) {
			simple_clk_stop = false;
			continue;
		}

		ret = sdw_peripheral_clk_stop_callback(peripheral, mode, SDW_CLK_PRE_DEPREPARE);
		if (ret < 0)
			dev_warn(&peripheral->dev, "clock stop pre deprepare cb failed:%d\n", ret);

		ret = sdw_peripheral_clk_stop_prepare(peripheral, mode, false);
		if (ret < 0)
			dev_warn(&peripheral->dev, "clock stop deprepare failed:%d\n", ret);
	}

	/* Skip remaining clock stop de-preparation if no Peripheral is attached */
	if (!is_peripheral)
		return 0;

	if (!simple_clk_stop) {
		ret = sdw_bus_wait_for_clk_prep_deprep(bus, SDW_BROADCAST_DEV_NUM);
		if (ret < 0)
			dev_warn(&peripheral->dev, "clock stop deprepare wait failed:%d\n", ret);
	}

	list_for_each_entry(peripheral, &bus->peripherals, node) {
		if (!peripheral->dev_num)
			continue;

		if (peripheral->status != SDW_PERIPHERAL_ATTACHED &&
		    peripheral->status != SDW_PERIPHERAL_ALERT)
			continue;

		mode = peripheral->curr_clk_stop_mode;
		ret = sdw_peripheral_clk_stop_callback(peripheral, mode, SDW_CLK_POST_DEPREPARE);
		if (ret < 0)
			dev_warn(&peripheral->dev, "clock stop post deprepare cb failed:%d\n", ret);
	}

	return 0;
}
EXPORT_SYMBOL(sdw_bus_exit_clk_stop);

int sdw_configure_dpn_intr(struct sdw_peripheral *peripheral,
			   int port, bool enable, int mask)
{
	u32 addr;
	int ret;
	u8 val = 0;

	if (peripheral->bus->params.s_data_mode != SDW_PORT_DATA_MODE_NORMAL) {
		dev_dbg(&peripheral->dev, "TEST FAIL interrupt %s\n",
			enable ? "on" : "off");
		mask |= SDW_DPN_INT_TEST_FAIL;
	}

	addr = SDW_DPN_INTMASK(port);

	/* Set/Clear port ready interrupt mask */
	if (enable) {
		val |= mask;
		val |= SDW_DPN_INT_PORT_READY;
	} else {
		val &= ~(mask);
		val &= ~SDW_DPN_INT_PORT_READY;
	}

	ret = sdw_update(peripheral, addr, (mask | SDW_DPN_INT_PORT_READY), val);
	if (ret < 0)
		dev_err(&peripheral->dev,
			"SDW_DPN_INTMASK write failed:%d\n", val);

	return ret;
}

static int sdw_peripheral_set_frequency(struct sdw_peripheral *peripheral)
{
	u32 mclk_freq = peripheral->bus->prop.mclk_freq;
	u32 curr_freq = peripheral->bus->params.curr_dr_freq >> 1;
	unsigned int scale;
	u8 scale_index;
	u8 base;
	int ret;

	/*
	 * frequency base and scale registers are required for SDCA
	 * devices. They may also be used for 1.2+/non-SDCA devices,
	 * but we will need a DisCo property to cover this case
	 */
	if (!peripheral->id.class_id)
		return 0;

	if (!mclk_freq) {
		dev_err(&peripheral->dev,
			"no bus MCLK, cannot set SDW_SCP_BUS_CLOCK_BASE\n");
		return -EINVAL;
	}

	/*
	 * map base frequency using Table 89 of SoundWire 1.2 spec.
	 * The order of the tests just follows the specification, this
	 * is not a selection between possible values or a search for
	 * the best value but just a mapping.  Only one case per platform
	 * is relevant.
	 * Some BIOS have inconsistent values for mclk_freq but a
	 * correct root so we force the mclk_freq to avoid variations.
	 */
	if (!(19200000 % mclk_freq)) {
		mclk_freq = 19200000;
		base = SDW_SCP_BASE_CLOCK_19200000_HZ;
	} else if (!(24000000 % mclk_freq)) {
		mclk_freq = 24000000;
		base = SDW_SCP_BASE_CLOCK_24000000_HZ;
	} else if (!(24576000 % mclk_freq)) {
		mclk_freq = 24576000;
		base = SDW_SCP_BASE_CLOCK_24576000_HZ;
	} else if (!(22579200 % mclk_freq)) {
		mclk_freq = 22579200;
		base = SDW_SCP_BASE_CLOCK_22579200_HZ;
	} else if (!(32000000 % mclk_freq)) {
		mclk_freq = 32000000;
		base = SDW_SCP_BASE_CLOCK_32000000_HZ;
	} else {
		dev_err(&peripheral->dev,
			"Unsupported clock base, mclk %d\n",
			mclk_freq);
		return -EINVAL;
	}

	if (mclk_freq % curr_freq) {
		dev_err(&peripheral->dev,
			"mclk %d is not multiple of bus curr_freq %d\n",
			mclk_freq, curr_freq);
		return -EINVAL;
	}

	scale = mclk_freq / curr_freq;

	/*
	 * map scale to Table 90 of SoundWire 1.2 spec - and check
	 * that the scale is a power of two and maximum 64
	 */
	scale_index = ilog2(scale);

	if (BIT(scale_index) != scale || scale_index > 6) {
		dev_err(&peripheral->dev,
			"No match found for scale %d, bus mclk %d curr_freq %d\n",
			scale, mclk_freq, curr_freq);
		return -EINVAL;
	}
	scale_index++;

	ret = sdw_write_no_pm(peripheral, SDW_SCP_BUS_CLOCK_BASE, base);
	if (ret < 0) {
		dev_err(&peripheral->dev,
			"SDW_SCP_BUS_CLOCK_BASE write failed:%d\n", ret);
		return ret;
	}

	/* initialize scale for both banks */
	ret = sdw_write_no_pm(peripheral, SDW_SCP_BUSCLOCK_SCALE_B0, scale_index);
	if (ret < 0) {
		dev_err(&peripheral->dev,
			"SDW_SCP_BUSCLOCK_SCALE_B0 write failed:%d\n", ret);
		return ret;
	}
	ret = sdw_write_no_pm(peripheral, SDW_SCP_BUSCLOCK_SCALE_B1, scale_index);
	if (ret < 0)
		dev_err(&peripheral->dev,
			"SDW_SCP_BUSCLOCK_SCALE_B1 write failed:%d\n", ret);

	dev_dbg(&peripheral->dev,
		"Configured bus base %d, scale %d, mclk %d, curr_freq %d\n",
		base, scale_index, mclk_freq, curr_freq);

	return ret;
}

static int sdw_initialize_peripheral(struct sdw_peripheral *peripheral)
{
	struct sdw_peripheral_prop *prop = &peripheral->prop;
	int status;
	int ret;
	u8 val;

	ret = sdw_peripheral_set_frequency(peripheral);
	if (ret < 0)
		return ret;

	if (peripheral->bus->prop.quirks & SDW_MANAGER_QUIRKS_CLEAR_INITIAL_CLASH) {
		/* Clear bus clash interrupt before enabling interrupt mask */
		status = sdw_read_no_pm(peripheral, SDW_SCP_INT1);
		if (status < 0) {
			dev_err(&peripheral->dev,
				"SDW_SCP_INT1 (BUS_CLASH) read failed:%d\n", status);
			return status;
		}
		if (status & SDW_SCP_INT1_BUS_CLASH) {
			dev_warn(&peripheral->dev, "Bus clash detected before INT mask is enabled\n");
			ret = sdw_write_no_pm(peripheral, SDW_SCP_INT1, SDW_SCP_INT1_BUS_CLASH);
			if (ret < 0) {
				dev_err(&peripheral->dev,
					"SDW_SCP_INT1 (BUS_CLASH) write failed:%d\n", ret);
				return ret;
			}
		}
	}
	if ((peripheral->bus->prop.quirks & SDW_MANAGER_QUIRKS_CLEAR_INITIAL_PARITY) &&
	    !(peripheral->prop.quirks & SDW_PERIPHERAL_QUIRKS_INVALID_INITIAL_PARITY)) {
		/* Clear parity interrupt before enabling interrupt mask */
		status = sdw_read_no_pm(peripheral, SDW_SCP_INT1);
		if (status < 0) {
			dev_err(&peripheral->dev,
				"SDW_SCP_INT1 (PARITY) read failed:%d\n", status);
			return status;
		}
		if (status & SDW_SCP_INT1_PARITY) {
			dev_warn(&peripheral->dev, "PARITY error detected before INT mask is enabled\n");
			ret = sdw_write_no_pm(peripheral, SDW_SCP_INT1, SDW_SCP_INT1_PARITY);
			if (ret < 0) {
				dev_err(&peripheral->dev,
					"SDW_SCP_INT1 (PARITY) write failed:%d\n", ret);
				return ret;
			}
		}
	}

	/*
	 * Set SCP_INT1_MASK register, typically bus clash and
	 * implementation-defined interrupt mask. The Parity detection
	 * may not always be correct on startup so its use is
	 * device-dependent, it might e.g. only be enabled in
	 * steady-state after a couple of frames.
	 */
	val = peripheral->prop.scp_int1_mask;

	/* Enable SCP interrupts */
	ret = sdw_update_no_pm(peripheral, SDW_SCP_INTMASK1, val, val);
	if (ret < 0) {
		dev_err(&peripheral->dev,
			"SDW_SCP_INTMASK1 write failed:%d\n", ret);
		return ret;
	}

	/* No need to continue if DP0 is not present */
	if (!peripheral->prop.dp0_prop)
		return 0;

	/* Enable DP0 interrupts */
	val = prop->dp0_prop->imp_def_interrupts;
	val |= SDW_DP0_INT_PORT_READY | SDW_DP0_INT_BRA_FAILURE;

	ret = sdw_update_no_pm(peripheral, SDW_DP0_INTMASK, val, val);
	if (ret < 0)
		dev_err(&peripheral->dev,
			"SDW_DP0_INTMASK read failed:%d\n", ret);
	return ret;
}

static int sdw_handle_dp0_interrupt(struct sdw_peripheral *peripheral, u8 *peripheral_status)
{
	u8 clear, impl_int_mask;
	int status, status2, ret, count = 0;

	status = sdw_read_no_pm(peripheral, SDW_DP0_INT);
	if (status < 0) {
		dev_err(&peripheral->dev,
			"SDW_DP0_INT read failed:%d\n", status);
		return status;
	}

	do {
		clear = status & ~SDW_DP0_INTERRUPTS;

		if (status & SDW_DP0_INT_TEST_FAIL) {
			dev_err(&peripheral->dev, "Test fail for port 0\n");
			clear |= SDW_DP0_INT_TEST_FAIL;
		}

		/*
		 * Assumption: PORT_READY interrupt will be received only for
		 * ports implementing Channel Prepare state machine (CP_SM)
		 */

		if (status & SDW_DP0_INT_PORT_READY) {
			complete(&peripheral->port_ready[0]);
			clear |= SDW_DP0_INT_PORT_READY;
		}

		if (status & SDW_DP0_INT_BRA_FAILURE) {
			dev_err(&peripheral->dev, "BRA failed\n");
			clear |= SDW_DP0_INT_BRA_FAILURE;
		}

		impl_int_mask = SDW_DP0_INT_IMPDEF1 |
			SDW_DP0_INT_IMPDEF2 | SDW_DP0_INT_IMPDEF3;

		if (status & impl_int_mask) {
			clear |= impl_int_mask;
			*peripheral_status = clear;
		}

		/* clear the interrupts but don't touch reserved and SDCA_CASCADE fields */
		ret = sdw_write_no_pm(peripheral, SDW_DP0_INT, clear);
		if (ret < 0) {
			dev_err(&peripheral->dev,
				"SDW_DP0_INT write failed:%d\n", ret);
			return ret;
		}

		/* Read DP0 interrupt again */
		status2 = sdw_read_no_pm(peripheral, SDW_DP0_INT);
		if (status2 < 0) {
			dev_err(&peripheral->dev,
				"SDW_DP0_INT read failed:%d\n", status2);
			return status2;
		}
		/* filter to limit loop to interrupts identified in the first status read */
		status &= status2;

		count++;

		/* we can get alerts while processing so keep retrying */
	} while ((status & SDW_DP0_INTERRUPTS) && (count < SDW_READ_INTR_CLEAR_RETRY));

	if (count == SDW_READ_INTR_CLEAR_RETRY)
		dev_warn(&peripheral->dev, "Reached MAX_RETRY on DP0 read\n");

	return ret;
}

static int sdw_handle_port_interrupt(struct sdw_peripheral *peripheral,
				     int port, u8 *peripheral_status)
{
	u8 clear, impl_int_mask;
	int status, status2, ret, count = 0;
	u32 addr;

	if (port == 0)
		return sdw_handle_dp0_interrupt(peripheral, peripheral_status);

	addr = SDW_DPN_INT(port);
	status = sdw_read_no_pm(peripheral, addr);
	if (status < 0) {
		dev_err(&peripheral->dev,
			"SDW_DPN_INT read failed:%d\n", status);

		return status;
	}

	do {
		clear = status & ~SDW_DPN_INTERRUPTS;

		if (status & SDW_DPN_INT_TEST_FAIL) {
			dev_err(&peripheral->dev, "Test fail for port:%d\n", port);
			clear |= SDW_DPN_INT_TEST_FAIL;
		}

		/*
		 * Assumption: PORT_READY interrupt will be received only
		 * for ports implementing CP_SM.
		 */
		if (status & SDW_DPN_INT_PORT_READY) {
			complete(&peripheral->port_ready[port]);
			clear |= SDW_DPN_INT_PORT_READY;
		}

		impl_int_mask = SDW_DPN_INT_IMPDEF1 |
			SDW_DPN_INT_IMPDEF2 | SDW_DPN_INT_IMPDEF3;

		if (status & impl_int_mask) {
			clear |= impl_int_mask;
			*peripheral_status = clear;
		}

		/* clear the interrupt but don't touch reserved fields */
		ret = sdw_write_no_pm(peripheral, addr, clear);
		if (ret < 0) {
			dev_err(&peripheral->dev,
				"SDW_DPN_INT write failed:%d\n", ret);
			return ret;
		}

		/* Read DPN interrupt again */
		status2 = sdw_read_no_pm(peripheral, addr);
		if (status2 < 0) {
			dev_err(&peripheral->dev,
				"SDW_DPN_INT read failed:%d\n", status2);
			return status2;
		}
		/* filter to limit loop to interrupts identified in the first status read */
		status &= status2;

		count++;

		/* we can get alerts while processing so keep retrying */
	} while ((status & SDW_DPN_INTERRUPTS) && (count < SDW_READ_INTR_CLEAR_RETRY));

	if (count == SDW_READ_INTR_CLEAR_RETRY)
		dev_warn(&peripheral->dev, "Reached MAX_RETRY on port read");

	return ret;
}

static int sdw_handle_peripheral_alerts(struct sdw_peripheral *peripheral)
{
	struct sdw_peripheral_intr_status peripheral_intr;
	u8 clear = 0, bit, port_status[15] = {0};
	int port_num, stat, ret, count = 0;
	unsigned long port;
	bool peripheral_notify;
	u8 sdca_cascade = 0;
	u8 buf, buf2[2], _buf, _buf2[2];
	bool parity_check;
	bool parity_quirk;

	sdw_modify_peripheral_status(peripheral, SDW_PERIPHERAL_ALERT);

	ret = pm_runtime_get_sync(&peripheral->dev);
	if (ret < 0 && ret != -EACCES) {
		dev_err(&peripheral->dev, "Failed to resume device: %d\n", ret);
		pm_runtime_put_noidle(&peripheral->dev);
		return ret;
	}

	/* Read Intstat 1, Intstat 2 and Intstat 3 registers */
	ret = sdw_read_no_pm(peripheral, SDW_SCP_INT1);
	if (ret < 0) {
		dev_err(&peripheral->dev,
			"SDW_SCP_INT1 read failed:%d\n", ret);
		goto io_err;
	}
	buf = ret;

	ret = sdw_nread_no_pm(peripheral, SDW_SCP_INTSTAT2, 2, buf2);
	if (ret < 0) {
		dev_err(&peripheral->dev,
			"SDW_SCP_INT2/3 read failed:%d\n", ret);
		goto io_err;
	}

	if (peripheral->prop.is_sdca) {
		ret = sdw_read_no_pm(peripheral, SDW_DP0_INT);
		if (ret < 0) {
			dev_err(&peripheral->dev,
				"SDW_DP0_INT read failed:%d\n", ret);
			goto io_err;
		}
		sdca_cascade = ret & SDW_DP0_SDCA_CASCADE;
	}

	do {
		peripheral_notify = false;

		/*
		 * Check parity, bus clash and Peripheral (impl defined)
		 * interrupt
		 */
		if (buf & SDW_SCP_INT1_PARITY) {
			parity_check = peripheral->prop.scp_int1_mask & SDW_SCP_INT1_PARITY;
			parity_quirk = !peripheral->first_interrupt_done &&
				(peripheral->prop.quirks &
				 SDW_PERIPHERAL_QUIRKS_INVALID_INITIAL_PARITY);

			if (parity_check && !parity_quirk)
				dev_err(&peripheral->dev, "Parity error detected\n");
			clear |= SDW_SCP_INT1_PARITY;
		}

		if (buf & SDW_SCP_INT1_BUS_CLASH) {
			if (peripheral->prop.scp_int1_mask & SDW_SCP_INT1_BUS_CLASH)
				dev_err(&peripheral->dev, "Bus clash detected\n");
			clear |= SDW_SCP_INT1_BUS_CLASH;
		}

		/*
		 * When bus clash or parity errors are detected, such errors
		 * are unlikely to be recoverable errors.
		 * TODO: In such scenario, reset bus. Make this configurable
		 * via sysfs property with bus reset being the default.
		 */

		if (buf & SDW_SCP_INT1_IMPL_DEF) {
			if (peripheral->prop.scp_int1_mask & SDW_SCP_INT1_IMPL_DEF) {
				dev_dbg(&peripheral->dev, "Peripheral impl defined interrupt\n");
				peripheral_notify = true;
			}
			clear |= SDW_SCP_INT1_IMPL_DEF;
		}

		/* the SDCA interrupts are cleared in the codec driver .interrupt_callback() */
		if (sdca_cascade)
			peripheral_notify = true;

		/* Check port 0 - 3 interrupts */
		port = buf & SDW_SCP_INT1_PORT0_3;

		/* To get port number corresponding to bits, shift it */
		port = FIELD_GET(SDW_SCP_INT1_PORT0_3, port);
		for_each_set_bit(bit, &port, 8) {
			sdw_handle_port_interrupt(peripheral, bit,
						  &port_status[bit]);
		}

		/* Check if cascade 2 interrupt is present */
		if (buf & SDW_SCP_INT1_SCP2_CASCADE) {
			port = buf2[0] & SDW_SCP_INTSTAT2_PORT4_10;
			for_each_set_bit(bit, &port, 8) {
				/* scp2 ports start from 4 */
				port_num = bit + 3;
				sdw_handle_port_interrupt(peripheral,
							  port_num,
							  &port_status[port_num]);
			}
		}

		/* now check last cascade */
		if (buf2[0] & SDW_SCP_INTSTAT2_SCP3_CASCADE) {
			port = buf2[1] & SDW_SCP_INTSTAT3_PORT11_14;
			for_each_set_bit(bit, &port, 8) {
				/* scp3 ports start from 11 */
				port_num = bit + 10;
				sdw_handle_port_interrupt(peripheral,
							  port_num,
							  &port_status[port_num]);
			}
		}

		/* Update the Peripheral driver */
		if (peripheral_notify && peripheral->ops &&
		    peripheral->ops->interrupt_callback) {
			peripheral_intr.sdca_cascade = sdca_cascade;
			peripheral_intr.control_port = clear;
			memcpy(peripheral_intr.port, &port_status,
			       sizeof(peripheral_intr.port));

			peripheral->ops->interrupt_callback(peripheral, &peripheral_intr);
		}

		/* Ack interrupt */
		ret = sdw_write_no_pm(peripheral, SDW_SCP_INT1, clear);
		if (ret < 0) {
			dev_err(&peripheral->dev,
				"SDW_SCP_INT1 write failed:%d\n", ret);
			goto io_err;
		}

		/* at this point all initial interrupt sources were handled */
		peripheral->first_interrupt_done = true;

		/*
		 * Read status again to ensure no new interrupts arrived
		 * while servicing interrupts.
		 */
		ret = sdw_read_no_pm(peripheral, SDW_SCP_INT1);
		if (ret < 0) {
			dev_err(&peripheral->dev,
				"SDW_SCP_INT1 recheck read failed:%d\n", ret);
			goto io_err;
		}
		_buf = ret;

		ret = sdw_nread_no_pm(peripheral, SDW_SCP_INTSTAT2, 2, _buf2);
		if (ret < 0) {
			dev_err(&peripheral->dev,
				"SDW_SCP_INT2/3 recheck read failed:%d\n", ret);
			goto io_err;
		}

		if (peripheral->prop.is_sdca) {
			ret = sdw_read_no_pm(peripheral, SDW_DP0_INT);
			if (ret < 0) {
				dev_err(&peripheral->dev,
					"SDW_DP0_INT recheck read failed:%d\n", ret);
				goto io_err;
			}
			sdca_cascade = ret & SDW_DP0_SDCA_CASCADE;
		}

		/*
		 * Make sure no interrupts are pending, but filter to limit loop
		 * to interrupts identified in the first status read
		 */
		buf &= _buf;
		buf2[0] &= _buf2[0];
		buf2[1] &= _buf2[1];
		stat = buf || buf2[0] || buf2[1] || sdca_cascade;

		/*
		 * Exit loop if Peripheral is continuously in ALERT state even
		 * after servicing the interrupt multiple times.
		 */
		count++;

		/* we can get alerts while processing so keep retrying */
	} while (stat != 0 && count < SDW_READ_INTR_CLEAR_RETRY);

	if (count == SDW_READ_INTR_CLEAR_RETRY)
		dev_warn(&peripheral->dev, "Reached MAX_RETRY on alert read\n");

io_err:
	pm_runtime_mark_last_busy(&peripheral->dev);
	pm_runtime_put_autosuspend(&peripheral->dev);

	return ret;
}

static int sdw_update_peripheral_status(struct sdw_peripheral *peripheral,
					enum sdw_peripheral_status status)
{
	unsigned long time;

	if (!peripheral->probed) {
		/*
		 * the peripheral status update is typically handled in an
		 * interrupt thread, which can race with the driver
		 * probe, e.g. when a module needs to be loaded.
		 *
		 * make sure the probe is complete before updating
		 * status.
		 */
		time = wait_for_completion_timeout(&peripheral->probe_complete,
						   msecs_to_jiffies(DEFAULT_PROBE_TIMEOUT));
		if (!time) {
			dev_err(&peripheral->dev, "Probe not complete, timed out\n");
			return -ETIMEDOUT;
		}
	}

	if (!peripheral->ops || !peripheral->ops->update_status)
		return 0;

	return peripheral->ops->update_status(peripheral, status);
}

/**
 * sdw_handle_peripheral_status() - Handle Peripheral status
 * @bus: SDW bus instance
 * @status: Status for all Peripheral(s)
 */
int sdw_handle_peripheral_status(struct sdw_bus *bus,
				 enum sdw_peripheral_status status[])
{
	enum sdw_peripheral_status prev_status;
	struct sdw_peripheral *peripheral;
	bool attached_initializing;
	int i, ret = 0;

	/* first check if any Peripherals fell off the bus */
	for (i = 1; i <= SDW_MAX_DEVICES; i++) {
		mutex_lock(&bus->bus_lock);
		if (test_bit(i, bus->assigned) == false) {
			mutex_unlock(&bus->bus_lock);
			continue;
		}
		mutex_unlock(&bus->bus_lock);

		peripheral = sdw_get_peripheral(bus, i);
		if (!peripheral)
			continue;

		if (status[i] == SDW_PERIPHERAL_UNATTACHED &&
		    peripheral->status != SDW_PERIPHERAL_UNATTACHED)
			sdw_modify_peripheral_status(peripheral, SDW_PERIPHERAL_UNATTACHED);
	}

	if (status[0] == SDW_PERIPHERAL_ATTACHED) {
		dev_dbg(bus->dev, "Peripheral attached, programming device number\n");
		ret = sdw_program_device_num(bus);
		if (ret < 0)
			dev_err(bus->dev, "Peripheral attach failed: %d\n", ret);
		/*
		 * programming a device number will have side effects,
		 * so we deal with other devices at a later time
		 */
		return ret;
	}

	/* Continue to check other peripheral statuses */
	for (i = 1; i <= SDW_MAX_DEVICES; i++) {
		mutex_lock(&bus->bus_lock);
		if (test_bit(i, bus->assigned) == false) {
			mutex_unlock(&bus->bus_lock);
			continue;
		}
		mutex_unlock(&bus->bus_lock);

		peripheral = sdw_get_peripheral(bus, i);
		if (!peripheral)
			continue;

		attached_initializing = false;

		switch (status[i]) {
		case SDW_PERIPHERAL_UNATTACHED:
			if (peripheral->status == SDW_PERIPHERAL_UNATTACHED)
				break;

			sdw_modify_peripheral_status(peripheral, SDW_PERIPHERAL_UNATTACHED);
			break;

		case SDW_PERIPHERAL_ALERT:
			ret = sdw_handle_peripheral_alerts(peripheral);
			if (ret < 0)
				dev_err(&peripheral->dev,
					"Peripheral %d alert handling failed: %d\n",
					i, ret);
			break;

		case SDW_PERIPHERAL_ATTACHED:
			if (peripheral->status == SDW_PERIPHERAL_ATTACHED)
				break;

			prev_status = peripheral->status;
			sdw_modify_peripheral_status(peripheral, SDW_PERIPHERAL_ATTACHED);

			if (prev_status == SDW_PERIPHERAL_ALERT)
				break;

			attached_initializing = true;

			ret = sdw_initialize_peripheral(peripheral);
			if (ret < 0)
				dev_err(&peripheral->dev,
					"Peripheral %d initialization failed: %d\n",
					i, ret);

			break;

		default:
			dev_err(&peripheral->dev, "Invalid peripheral %d status:%d\n",
				i, status[i]);
			break;
		}

		ret = sdw_update_peripheral_status(peripheral, status[i]);
		if (ret < 0)
			dev_err(&peripheral->dev,
				"Update Peripheral status failed:%d\n", ret);
		if (attached_initializing) {
			dev_dbg(&peripheral->dev,
				"%s: signaling initialization completion for Peripheral %d\n",
				__func__, peripheral->dev_num);

			complete(&peripheral->initialization_complete);
		}
	}

	return ret;
}
EXPORT_SYMBOL(sdw_handle_peripheral_status);

void sdw_clear_peripheral_status(struct sdw_bus *bus, u32 request)
{
	struct sdw_peripheral *peripheral;
	int i;

	/* Check all non-zero devices */
	for (i = 1; i <= SDW_MAX_DEVICES; i++) {
		mutex_lock(&bus->bus_lock);
		if (test_bit(i, bus->assigned) == false) {
			mutex_unlock(&bus->bus_lock);
			continue;
		}
		mutex_unlock(&bus->bus_lock);

		peripheral = sdw_get_peripheral(bus, i);
		if (!peripheral)
			continue;

		if (peripheral->status != SDW_PERIPHERAL_UNATTACHED) {
			sdw_modify_peripheral_status(peripheral, SDW_PERIPHERAL_UNATTACHED);
			peripheral->first_interrupt_done = false;
		}

		/* keep track of request, used in pm_runtime resume */
		peripheral->unattach_request = request;
	}
}
EXPORT_SYMBOL(sdw_clear_peripheral_status);
