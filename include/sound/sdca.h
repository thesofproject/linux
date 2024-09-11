/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause) */
/*
 * The MIPI SDCA specification is available for public downloads at
 * https://www.mipi.org/mipi-sdca-v1-0-download
 *
 * Copyright(c) 2024 Intel Corporation
 */

#ifndef __SDCA_H__
#define __SDCA_H__

struct regmap;
struct regmap_config;
struct sdw_slave;
struct sdca_dev;
struct sdca_function_data;

#define SDCA_MAX_INTERRUPTS 31 /* the last bit is reserved for future extensions */

/**
 * struct sdca_interrupt_source - interface between interrupt source and
 * SoundWire SDCA interrupt handler
 *
 * @index: SDCA interrupt number in [0, SDCA_MAX_INTERRUPTS - 1]
 * @context: source-specific information, used by @callback
 * @callback: provided by interrupt source for source-specific handling.
 */
struct sdca_interrupt_source {
	int index;
	void *context;
	void (*callback)(void *context);
};

/**
 * struct sdca_interrupt_info - Peripheral device-level information
 * used for interrupt handler
 *
 * @sources: array of pointers, addressed with an interrupt index
 * matching @registered_source_mask bits.
 * @irqs_lock: mutex protecting concurrent access to @sources,
 * @registered_source_mask and reventing SDCA interrupts from being disabled
 * on suspend while being handled.
 * @enabled_interrupt_mask: mask indicating which interrupts from @registered_source_mask
 * are currently enabled.
 * @detected_interrupt_mask: bitfields set in interrupt handler, and accessible
 * in deferred processing.
 * @supported_hw_register_mask: Up to 4 registers may be implemented
 */
struct sdca_interrupt_info {
	struct sdca_interrupt_source *sources[SDCA_MAX_INTERRUPTS];
	struct mutex irqs_lock; /* protects SDCA interrupts */
	u32 registered_source_mask;
	u32 enabled_interrupt_mask;
	u32 detected_interrupt_mask;
	int supported_hw_register_mask;
};

#define SDCA_MAX_FUNCTION_COUNT 8

/**
 * sdca_device_desc - short descriptor for an SDCA Function
 * @function_node: firmware node for the Function
 * @func_dev: pointer to SDCA function device.
 * @adr: ACPI address (used for SDCA register access)
 * @type: Function topology type
 * @name: human-readable string
 */
struct sdca_function_desc {
	struct fwnode_handle *function_node;
	struct sdca_function_data *function;
	struct sdca_dev *func_dev;
	u64 adr;
	u32 type;
	const char *name;
};

/**
 * sdca_device_data - structure containing all SDCA related information
 * @sdca_interface_revision: value read from _DSD property, mainly to check
 * for changes between silicon versions
 * @num_functions: total number of supported SDCA functions. Invalid/unsupported
 * functions will be skipped.
 * @sdca_func: array of function descriptors
 * @interrupt_info: device-level interrupt configuration/handling
 */
struct sdca_device_data {
	u32 interface_revision;
	int num_functions;
	struct sdca_function_desc sdca_func[SDCA_MAX_FUNCTION_COUNT];
	struct sdca_interrupt_info *interrupt_info;
	struct regmap *regmap;
};

enum sdca_quirk {
	SDCA_QUIRKS_RT712_VB,
};

#if IS_ENABLED(CONFIG_ACPI) && IS_ENABLED(CONFIG_SND_SOC_SDCA)

void sdca_lookup_functions(struct sdw_slave *slave);
void sdca_lookup_interface_revision(struct sdw_slave *slave);
bool sdca_device_quirk_match(struct sdw_slave *slave, enum sdca_quirk quirk);
int sdca_dev_register_functions(struct sdw_slave *slave, struct regmap *regmap);
int sdca_dev_parse_functions(struct sdw_slave *slave);
void sdca_dev_unregister_functions(struct sdw_slave *slave);
int sdca_dev_populate_constants(struct sdw_slave *slave, struct regmap_config *config);

bool sdca_disco_regmap_readable(struct device *dev, unsigned int reg);
bool sdca_disco_regmap_writeable(struct device *dev, unsigned int reg);
bool sdca_disco_regmap_volatile(struct device *dev, unsigned int reg);
bool sdca_disco_regmap_deferrable(struct device *dev, unsigned int reg);
int sdca_disco_regmap_mbq_size(struct device *dev, unsigned int reg);

#else

static inline void sdca_lookup_functions(struct sdw_slave *slave) {}
static inline void sdca_lookup_interface_revision(struct sdw_slave *slave) {}
static inline bool sdca_device_quirk_match(struct sdw_slave *slave, enum sdca_quirk quirk)
{
	return false;
}

static inline int sdca_dev_register_functions(struct sdw_slave *slave, struct regmap *regmap)
{
	return 0;
}

static inline int sdca_dev_parse_functions(struct sdw_slave *slave)
{
	return 0;
}

static inline void sdca_dev_unregister_functions(struct sdw_slave *slave) {}

static inline int sdca_dev_populate_constants(struct sdw_slave *slave,
					      struct regmap_config *config)
{
	return 0;
}

static inline bool sdca_disco_regmap_readable(struct device *dev, unsigned int reg)
{
	return false;
}

static inline bool sdca_disco_regmap_writeable(struct device *dev, unsigned int reg)
{
	return false;
}

static inline bool sdca_disco_regmap_volatile(struct device *dev, unsigned int reg)
{
	return false;
}

static inline bool sdca_disco_regmap_deferrable(struct device *dev, unsigned int reg)
{
	return false;
}

static inline int sdca_disco_regmap_mbq_size(struct device *dev, unsigned int reg)
{
	return 0;
}

#endif

#if IS_ENABLED(CONFIG_SND_SOC_SDCA_IRQ_HANDLER)

int sdca_interrupt_info_alloc(struct sdw_slave *slave);
void sdca_interrupt_info_release(struct sdw_slave *slave);
int sdca_interrupt_info_reset(struct sdw_slave *slave);
int sdca_interrupt_initialize(struct sdw_slave *slave,
			      int supported_hw_register_mask);
int sdca_interrupt_register_source(struct sdw_slave *slave,
				   struct sdca_interrupt_source *source);
int sdca_interrupt_enable(struct sdw_slave *slave,
			  u32 source_mask,
			  bool enable);
void sdca_interrupt_clear_history(struct sdw_slave *slave, u32 preserve_mask);
int sdca_interrupt_handler(struct sdw_slave *slave);

#else

static inline int sdca_interrupt_info_alloc(struct sdw_slave *slave)
{
	return 0;
}

static inline void sdca_interrupt_info_release(struct sdw_slave *slave) {}

static inline int sdca_interrupt_info_reset(struct sdw_slave *slave)
{
	return 0;
}

static inline int sdca_interrupt_initialize(struct sdw_slave *slave,
					    int supported_hw_register_mask)
{
	return 0;
}

static inline int sdca_interrupt_register_source(struct sdw_slave *slave,
						 struct sdca_interrupt_source *source)
{
	return 0;
}

static inline int sdca_interrupt_enable(struct sdw_slave *slave,
					u32 source_mask,
					bool enable)
{
	return 0;
}

static inline void sdca_interrupt_clear_history(struct sdw_slave *slave, u32 preserve_mask) {}

static inline int sdca_interrupt_handler(struct sdw_slave *slave)
{
	return 0;
}

#endif /* IS_ENABLED(CONFIG_SND_SOC_SDCA_IRQ_HANDLER) */

#endif
