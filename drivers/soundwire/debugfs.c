// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
// Copyright(c) 2017-19 Intel Corporation.

#include <linux/device.h>
#include <linux/debugfs.h>
#include <linux/mod_devicetable.h>
#include <linux/slab.h>
#include <linux/soundwire/sdw.h>
#include <linux/soundwire/sdw_registers.h>
#include "bus.h"

#ifdef CONFIG_DEBUG_FS
struct dentry *sdw_debugfs_root;
#endif

struct dentry *sdw_bus_debugfs_init(struct sdw_bus *bus)
{
	struct dentry *d;
	char name[16];

	if (!sdw_debugfs_root)
		return NULL;

	/* create the debugfs master-N */
	snprintf(name, sizeof(name), "master-%d", bus->link_id);
	d = debugfs_create_dir(name, sdw_debugfs_root);

	return d;
}

void sdw_bus_debugfs_exit(struct dentry *d)
{
	debugfs_remove_recursive(d);
}

#define RD_BUF (3 * PAGE_SIZE)

static ssize_t sdw_sprintf(struct sdw_slave *slave,
			   char *buf, size_t pos, unsigned int reg)
{
	int value;

	value = sdw_read(slave, reg);

	if (value < 0)
		return scnprintf(buf + pos, RD_BUF - pos, "%3x\tXX\n", reg);
	else
		return scnprintf(buf + pos, RD_BUF - pos,
				"%3x\t%2x\n", reg, value);
}

static ssize_t sdw_slave_reg_read(struct file *file, char __user *user_buf,
				  size_t count, loff_t *ppos)
{
	struct sdw_slave *slave = file->private_data;
	unsigned int reg;
	char *buf;
	ssize_t ret;
	int i, j;

	buf = kzalloc(RD_BUF, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = scnprintf(buf, RD_BUF, "Register  Value\n");
	ret += scnprintf(buf + ret, RD_BUF - ret, "\nDP0\n");

	for (i = 0; i < 6; i++)
		ret += sdw_sprintf(slave, buf, ret, i);

	ret += scnprintf(buf + ret, RD_BUF - ret, "Bank0\n");
	ret += sdw_sprintf(slave, buf, ret, SDW_DP0_CHANNELEN);
	for (i = SDW_DP0_SAMPLECTRL1; i <= SDW_DP0_LANECTRL; i++)
		ret += sdw_sprintf(slave, buf, ret, i);

	ret += scnprintf(buf + ret, RD_BUF - ret, "Bank1\n");
	ret += sdw_sprintf(slave, buf, ret,
			SDW_DP0_CHANNELEN + SDW_BANK1_OFFSET);
	for (i = SDW_DP0_SAMPLECTRL1 + SDW_BANK1_OFFSET;
			i <= SDW_DP0_LANECTRL + SDW_BANK1_OFFSET; i++)
		ret += sdw_sprintf(slave, buf, ret, i);

	ret += scnprintf(buf + ret, RD_BUF - ret, "\nSCP\n");
	for (i = SDW_SCP_INT1; i <= SDW_SCP_BANKDELAY; i++)
		ret += sdw_sprintf(slave, buf, ret, i);
	for (i = SDW_SCP_DEVID_0; i <= SDW_SCP_DEVID_5; i++)
		ret += sdw_sprintf(slave, buf, ret, i);

	ret += scnprintf(buf + ret, RD_BUF - ret, "Bank0\n");
	ret += sdw_sprintf(slave, buf, ret, SDW_SCP_FRAMECTRL_B0);
	ret += sdw_sprintf(slave, buf, ret, SDW_SCP_NEXTFRAME_B0);

	ret += scnprintf(buf + ret, RD_BUF - ret, "Bank1\n");
	ret += sdw_sprintf(slave, buf, ret, SDW_SCP_FRAMECTRL_B1);
	ret += sdw_sprintf(slave, buf, ret, SDW_SCP_NEXTFRAME_B1);

	for (i = 1; i < 14; i++) {
		ret += scnprintf(buf + ret, RD_BUF - ret, "\nDP%d\n", i);
		reg = SDW_DPN_INT(i);
		for (j = 0; j < 6; j++)
			ret += sdw_sprintf(slave, buf, ret, reg + j);

		ret += scnprintf(buf + ret, RD_BUF - ret, "Bank0\n");
		reg = SDW_DPN_CHANNELEN_B0(i);
		for (j = 0; j < 9; j++)
			ret += sdw_sprintf(slave, buf, ret, reg + j);

		ret += scnprintf(buf + ret, RD_BUF - ret, "Bank1\n");
		reg = SDW_DPN_CHANNELEN_B1(i);
		for (j = 0; j < 9; j++)
			ret += sdw_sprintf(slave, buf, ret, reg + j);
	}

	ret = simple_read_from_buffer(user_buf, count, ppos, buf, ret);
	kfree(buf);

	return ret;
}

static const struct file_operations sdw_slave_reg_fops = {
	.open = simple_open,
	.read = sdw_slave_reg_read,
	.llseek = default_llseek,
};

struct dentry *sdw_slave_debugfs_init(struct sdw_slave *slave)
{
	struct dentry *master;
	struct dentry *d;
	char name[32];

	master = slave->bus->debugfs;

	/* create the debugfs slave-name */
	snprintf(name, sizeof(name), "%s", dev_name(&slave->dev));
	d = debugfs_create_dir(name, master);

	debugfs_create_file("registers", 0400, d, slave, &sdw_slave_reg_fops);

	return d;
}

void sdw_slave_debugfs_exit(struct dentry *d)
{
	debugfs_remove_recursive(d);
}

void sdw_debugfs_init(void)
{
	sdw_debugfs_root = debugfs_create_dir("soundwire", NULL);
}

void sdw_debugfs_exit(void)
{
	debugfs_remove_recursive(sdw_debugfs_root);
}
