/* SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause) */
/* Copyright(c) 2015-17 Intel Corporation. */

#ifndef __SDW_INTEL_H
#define __SDW_INTEL_H

/**
 * struct sdw_intel_stream_params_data: configuration passed during
 * the @params_stream callback, e.g. for interaction with DSP
 * firmware.
 */
struct sdw_intel_stream_params_data {
	struct snd_pcm_substream *substream;
	struct snd_soc_dai *dai;
	struct snd_pcm_hw_params *hw_params;
	int link_id;
	int alh_stream_id;
};

/**
 * struct sdw_intel_stream_free_data: configuration passed during
 * the @free_stream callback, e.g. for interaction with DSP
 * firmware.
 */
struct sdw_intel_stream_free_data {
	struct snd_pcm_substream *substream;
	struct snd_soc_dai *dai;
	int link_id;
};

/**
 * struct sdw_intel_ops: Intel audio driver callback ops
 *
 */
struct sdw_intel_ops {
	int (*params_stream)(struct device *dev,
			     struct sdw_intel_stream_params_data *params_data);
	int (*free_stream)(struct device *dev,
			   struct sdw_intel_stream_free_data *free_data);
};

/**
 * struct sdw_intel_acpi_info - Soundwire Intel information found in ACPI tables
 * @handle: ACPI controller handle
 * @count: link count found with "sdw-master-count" property
 * @link_mask: bit-wise mask listing links enabled by BIOS menu
 *
 * this structure could be expanded to e.g. provide all the _ADR
 * information in case the link_mask is not sufficient to identify
 * platform capabilities.
 */
struct sdw_intel_acpi_info {
	acpi_handle handle;
	int count;
	u32 link_mask;
};

/**
 * struct sdw_intel_res - Soundwire Intel global resource structure,
 * typically populated by the DSP driver
 *
 * @count: link count (may be filtered by DSP driver)
 * @mmio_base: mmio base of SoundWire registers
 * @irq: interrupt number
 * @handle: ACPI parent handle
 * @parent: parent device
 * @ops: callback ops
 * @dev: device implementing hwparams and free callbacks
 * @link_mask: bit-wise mask listing links selected by the DSP driver
 */
struct sdw_intel_res {
	int count;
	void __iomem *mmio_base;
	int irq;
	acpi_handle handle;
	struct device *parent;
	const struct sdw_intel_ops *ops;
	struct device *dev;
	u32 link_mask;
};

/**
 * struct sdw_intel_link_res - Soundwire Intel link resource structure,
 * typically populated by the controller driver.
 * @md: Master device
 * @mmio_base: mmio base of SoundWire registers
 * @registers: Link IO registers base
 * @shim: Audio shim pointer
 * @alh: ALH (Audio Link Hub) pointer
 * @irq: Interrupt line
 * @ops: Shim callback ops
 * @dev: Device implementing the callbacks for params/free
 */
struct sdw_intel_link_res {
	struct sdw_master_device *md;
	void __iomem *mmio_base; /* not strictly needed, useful for debug */
	void __iomem *registers;
	void __iomem *shim;
	void __iomem *alh;
	int irq;
	const struct sdw_intel_ops *ops;
	struct device *dev;
};

/**
 * struct sdw_intel_ctx - context allocated by the controller
 * driver probe
 * @count: link count
 * @mmio_base: mmio base of SoundWire registers, only used to check
 * hardware capabilities after all power dependencies are settled.
 * @arg: Shim callback ops argument
 */
struct sdw_intel_ctx {
	int count;
	void __iomem *mmio_base;
	u32 link_mask;
	acpi_handle handle;
	struct sdw_intel_link_res *links;
};

/*
 * On Intel platforms, the SoundWire IP has dependencies on power
 * rails shared with the DSP, and the initialization steps are split
 * in three. First an ACPI scan to check what the firmware describes
 * in DSDT tables, then an allocation step (with no hardware
 * configuration but with all the relevant devices created) and last
 * the actual hardware configuration. The final stage is a global
 * interrupt enable which is controlled by the DSP driver. Splitting
 * these phases helps simplify the boot flow and make early decisions
 * on e.g. which machine driver to select (I2S mode, HDaudio or
 * SoundWire).
 */
int sdw_intel_acpi_scan(acpi_handle *parent_handle,
			struct sdw_intel_acpi_info *info);

struct sdw_intel_ctx *
sdw_intel_probe(struct sdw_intel_res *res);

int sdw_intel_startup(struct sdw_intel_ctx *ctx);

void sdw_intel_exit(struct sdw_intel_ctx *ctx);

#endif
