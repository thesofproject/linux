/* SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause) */
/* Copyright(c) 2015-17 Intel Corporation. */

#ifndef __SDW_INTEL_LOCAL_H
#define __SDW_INTEL_LOCAL_H

/**
 * struct sdw_intel_link_res - Soundwire Intel link resource structure,
 * typically populated by the controller driver.
 * @mmio_base: mmio base of SoundWire registers
 * @registers: Link IO registers base
 * @shim: Audio shim pointer
 * @alh: ALH (Audio Link Hub) pointer
 * @irq: Interrupt line
 * @ops: Shim callback ops
 * @dev: device implementing hw_params and free callbacks
 * @shim_lock: mutex to handle access to shared SHIM registers
 * @shim_mask: global pointer to check SHIM register initialization
 * @clock_stop_quirks: mask defining requested behavior on pm_suspend
 * @link_mask: global mask needed for power-up/down sequences
 * @cdns: Cadence master descriptor
 * @list: used to walk-through all masters exposed by the same controller
 */
struct sdw_intel_link_res {
	void __iomem *mmio_base; /* not strictly needed, useful for debug */
	void __iomem *registers;
	void __iomem *shim;
	void __iomem *alh;
	int irq;
	const struct sdw_intel_ops *ops;
	struct device *dev;
	struct mutex *shim_lock; /* protect shared registers */
	u32 *shim_mask;
	u32 clock_stop_quirks;
	u32 link_mask;
	struct sdw_cdns *cdns;
	struct list_head list;
};

struct sdw_intel {
	struct sdw_cdns cdns;
	int instance;
	struct sdw_intel_link_res *link_res;
	bool startup_done;
#ifdef CONFIG_DEBUG_FS
	struct dentry *debugfs;
#endif
};

#define cdns_to_intel(_cdns) container_of(_cdns, struct sdw_intel, cdns)

#define INTEL_MASTER_RESET_ITERATIONS	10

/* functions needed for ops */
int intel_pre_bank_switch(struct sdw_bus *bus);
int intel_post_bank_switch(struct sdw_bus *bus);

/* interface with intel-device */
void intel_debugfs_init(struct sdw_intel *sdw);
void intel_debugfs_exit(struct sdw_intel *sdw);

int intel_register_dai(struct sdw_intel *sdw);

void intel_check_clock_stop(struct sdw_intel *sdw);
int intel_start_bus(struct sdw_intel *sdw);
int intel_start_bus_after_reset(struct sdw_intel *sdw);
int intel_start_bus_after_clock_stop(struct sdw_intel *sdw);
int intel_stop_bus(struct sdw_intel *sdw, bool clock_stop);

int intel_link_power_up(struct sdw_intel *sdw);
int intel_link_power_down(struct sdw_intel *sdw);

int  intel_shim_check_wake(struct sdw_intel *sdw);
void intel_shim_wake(struct sdw_intel *sdw, bool wake_enable);

#endif /* __SDW_INTEL_LOCAL_H */
