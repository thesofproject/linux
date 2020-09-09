/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright(c) 2015-2020 Intel Corporation. */

#ifndef __SDW_SYSFS_LOCAL_H
#define __SDW_SYSFS_LOCAL_H

/*
 * SDW sysfs APIs -
 */

/* basic routine to report status of Slave (attachment, dev_num) */
int sdw_slave_status_sysfs_init(struct sdw_slave *slave);

/* additional device-managed properties reported after driver probe */
int sdw_slave_sysfs_init(struct sdw_slave *slave);
int sdw_slave_sysfs_dpn_init(struct sdw_slave *slave);

#endif /* __SDW_SYSFS_LOCAL_H */
