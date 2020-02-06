/* SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause) */
/* Copyright(c) 2015-17 Intel Corporation. */

#ifndef __SDW_INTEL_LOCAL_H
#define __SDW_INTEL_LOCAL_H

/**
 * struct sdw_intel_link_res - Soundwire Intel link resource structure,
 * typically populated by the controller driver.
 *
 * When used with platform_devices platdata, the data will be
 * copied. Any information provided back to the parent needs to use
 * the @parent_res indirection
 *
 * @parent_res: address of parent resources
 * @pdev: platform_device
 * @probe_complete: completion utility to signal successful probe completion
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
 * @cdns: Cadence master descriptor
 * @list: used to walk-through all masters exposed by the same controller
 * @pm_runtime_suspended: flag to avoid resuming before system suspend
 */
struct sdw_intel_link_res {
	void *parent_res;
	struct platform_device *pdev;
	struct completion probe_complete;
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
	struct sdw_cdns *cdns;
	struct list_head list;
	bool pm_runtime_suspended;
};

#define SDW_INTEL_QUIRK_MASK_BUS_DISABLE      BIT(1)

#define SDW_INTEL_MASTER_PROBE_TIMEOUT 2000

int intel_master_startup(struct platform_device *pdev);
int intel_master_process_wakeen_event(struct platform_device *pdev);

#endif /* __SDW_INTEL_LOCAL_H */
