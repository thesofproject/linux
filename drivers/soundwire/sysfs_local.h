/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright(c) 2015-2020 Intel Corporation. */

#ifndef __SDW_SYSFS_LOCAL_H
#define __SDW_SYSFS_LOCAL_H

/*
 * SDW sysfs APIs -
 */

/* basic attributes to report status of Peripheral (attachment, dev_num) */
extern const struct attribute_group *sdw_peripheral_status_attr_groups[];

/* additional device-managed properties reported after driver probe */
int sdw_peripheral_sysfs_init(struct sdw_peripheral *peripheral);
int sdw_peripheral_sysfs_dpn_init(struct sdw_peripheral *peripheral);

#endif /* __SDW_SYSFS_LOCAL_H */
