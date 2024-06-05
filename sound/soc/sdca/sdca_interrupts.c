// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
// Copyright(c) 2024 Intel Corporation

/*
 * The MIPI SDCA specification is available for public downloads at
 * https://www.mipi.org/mipi-sdca-v1-0-download
 */

#include <linux/device.h>
#include <linux/soundwire/sdw.h>
#include <linux/soundwire/sdw_registers.h>
#include <sound/sdca.h>

/**
 * sdca_interrupt_info_alloc() - Helper to allocate the 'struct sdca_interrupt_info'
 * @slave: the SoundWire peripheral
 *
 * Note: this helper is intended to be called in the SoundWire driver probe() callback.
 */

int sdca_interrupt_info_alloc(struct sdw_slave *slave)
{
	struct sdca_interrupt_info *interrupt_info;
	struct device *dev = &slave->dev;

	interrupt_info = devm_kzalloc(dev, sizeof(*interrupt_info), GFP_KERNEL);
	if (!interrupt_info)
		return -ENOMEM;

	mutex_init(&interrupt_info->irqs_lock);

	slave->sdca_data.interrupt_info = interrupt_info;

	return 0;
}
EXPORT_SYMBOL_NS(sdca_interrupt_info_alloc, SND_SOC_SDCA_IRQ_HANDLER);

/**
 * sdca_interrupt_info_release() - Helper to release the 'struct sdca_interrupt_info'
 * @slave: the SoundWire peripheral
 *
 * Note: this helper is intended to be called in the SoundWire driver remove() callback.
 */
void sdca_interrupt_info_release(struct sdw_slave *slave)
{
	struct sdca_interrupt_info *interrupt_info;

	if (!slave->sdca_data.interrupt_info)
		return;

	interrupt_info = slave->sdca_data.interrupt_info;
	mutex_destroy(&interrupt_info->irqs_lock);

	/* no memory to free since we used devm_ in the alloc */
}
EXPORT_SYMBOL_NS(sdca_interrupt_info_release, SND_SOC_SDCA_IRQ_HANDLER);

/**
 * sdca_interrupt_info_reset() - Helper to reset internal SDCA interrupt status
 * @slave: the SoundWire peripheral
 *
 * Note: SoundWire peripherals could be reset and/or re-attach on the bus.
 * This helper is intended to be called during the 'update_status' callback to
 * reconcile the internal state of the 'struct sdca_interrupt_info'
 */
int sdca_interrupt_info_reset(struct sdw_slave *slave)
{
	struct sdca_interrupt_info *interrupt_info;

	if (!slave->sdca_data.interrupt_info)
		return -ENODEV;

	interrupt_info = slave->sdca_data.interrupt_info;

	/* interrupts need to be re-enabled after a reset */
	interrupt_info->enabled_interrupt_mask = 0;

	return 0;
}
EXPORT_SYMBOL_NS(sdca_interrupt_info_reset, SND_SOC_SDCA_IRQ_HANDLER);

/**
 * sdca_interrupt_initialize() - device-level initialization of SDCA
 * interrupts
 * @slave: the SoundWire peripheral
 * @supported_hw_register_mask: One bit per supported SDCA interrupt register.
 * Valid values as in GENMASK(3, 0) since there are up to 3 registers in
 * hardware.
 */
int sdca_interrupt_initialize(struct sdw_slave *slave,
			      int supported_hw_register_mask)
{
	struct sdca_interrupt_info *interrupt_info;
	struct device *dev = &slave->dev;

	if (!slave->sdca_data.interrupt_info)
		return -ENODEV;

	if (supported_hw_register_mask & ~GENMASK(3, 0)) {
		dev_err(dev, "%s: invalid supported_hw_register_mask: %#x\n",
			__func__, supported_hw_register_mask);
		return -EINVAL;
	}

	interrupt_info = slave->sdca_data.interrupt_info;
	interrupt_info->supported_hw_register_mask = supported_hw_register_mask;

	return 0;
}
EXPORT_SYMBOL_NS(sdca_interrupt_initialize, SND_SOC_SDCA_IRQ_HANDLER);

/**
 * sdca_interrupt_register_source() - Helper to register a SDCA interrupt source interrupts
 * @slave: the SoundWire peripheral
 * @source:  source-specific information and callback, providing an opaque interface
 * with the bus interrupt handling core
 *
 * Note: to prevent race conditions, the code relies on the 'irq_lock' mutex
 */
int sdca_interrupt_register_source(struct sdw_slave *slave,
				   struct sdca_interrupt_source *source)
{
	struct sdca_interrupt_info *interrupt_info;
	struct device *dev = &slave->dev;
	int index;

	if (!slave->sdca_data.interrupt_info)
		return -ENODEV;

	if (!source)
		return -EINVAL;

	index = source->index;
	if (index < 0 && index >= SDCA_MAX_INTERRUPTS) {
		dev_err(dev,
			"%s: invalid source index %d\n",
			__func__, source->index);
		return -EINVAL;
	}

	if (!source->callback) {
		dev_err(dev,
			"%s: no callback provided for source index %d\n",
			__func__, source->index);
		return -EINVAL;
	}

	if (!source->context) {
		dev_err(dev,
			"%s: no context provided for source index %d\n",
			__func__, source->index);
		return -EINVAL;
	}

	interrupt_info = slave->sdca_data.interrupt_info;

	mutex_lock(&interrupt_info->irqs_lock);

	interrupt_info->sources[index] = source;
	interrupt_info->registered_source_mask |= BIT(source->index);

	mutex_unlock(&interrupt_info->irqs_lock);

	return 0;
}
EXPORT_SYMBOL_NS(sdca_interrupt_register_source, SND_SOC_SDCA_IRQ_HANDLER);

static int sdca_interrupt_register_mask_rmw(struct sdw_slave *slave,
					    int reg_index,
					    u8 byte_mask,
					    int enable)
{
	struct device *dev = &slave->dev;
	u8 mask;
	int ret;

	ret = sdw_read_no_pm(slave, SDW_SCP_SDCA_INTMASK1 + reg_index);
	if (ret < 0) {
		dev_err(dev,
			"%s: read from SDW_SCP_SDCA_INT_%d failed: %d\n",
			__func__, reg_index, ret);
		goto io_error;
	}
	mask = ret;

	if (enable)
		mask |= byte_mask;
	else
		mask &= byte_mask;

	ret = sdw_write_no_pm(slave, SDW_SCP_SDCA_INTMASK1 + reg_index, mask);
	if (ret < 0)
		dev_err(dev,
			"%s: write to SDW_SCP_SDCA_INT_%d failed: %d\n",
			__func__, reg_index, ret);

io_error:
	return ret;
}

/**
 * sdca_interrupt_enable() - Helper to enable/disable SDCA interrupt sources
 * @slave: the SoundWire peripheral
 * @source_mask: a bitmask of all interrupt sources to enable/disable
 * @enable: boolean to enable/disable
 *
 * Note: to prevent race conditions, the code relies on the 'irq_lock' mutex
 */
int sdca_interrupt_enable(struct sdw_slave *slave,
			  u32 source_mask,
			  bool enable)
{
	struct sdca_interrupt_info *interrupt_info;
	struct device *dev = &slave->dev;
	unsigned long hw_register_mask;
	int max_register;
	int max_source;
	int reg_index;
	int ret = 0;

	if (!slave->sdca_data.interrupt_info)
		return -ENODEV;

	interrupt_info = slave->sdca_data.interrupt_info;

	/* check first if the interrupt masks are consistent */
	hw_register_mask = interrupt_info->supported_hw_register_mask;
	if (!hw_register_mask) {
		dev_err(dev,
			"%s: supported_hw_register mask not initialized\n",
			__func__);
		return -EINVAL;
	}

	if (!source_mask) {
		dev_err(dev,
			"%s: source mask not set\n",
			__func__);
		return -EINVAL;
	}

	max_register = __fls(hw_register_mask); /* __fls is zero-based, values should be 0..3 */
	max_source = __fls(source_mask);     /* __fls is zero-based, values should be 0..31 */

	if (max_source >= (8 << max_register)) {
		dev_err(dev,
			"%s: source mask %#x incompatible with supported registers %#lx\n",
			__func__, source_mask, hw_register_mask);
		return -EINVAL;
	}

	/* now start the interrupt mask updates */
	mutex_lock(&interrupt_info->irqs_lock);

	for_each_set_bit(reg_index, &hw_register_mask, 32) {
		int shift = reg_index << 3;
		int source_mask_byte =  (source_mask >> shift) & GENMASK(7, 0);

		if (!source_mask_byte)
			continue;

		ret = sdca_interrupt_register_mask_rmw(slave, reg_index,
						       source_mask_byte,
						       enable);
		if (ret < 0)
			goto unlock;
	}

	/*
	 * Almost done, keep track of the combined interrupt mask used to
	 * filter interrupts in the handler.
	 */
	if (enable)
		interrupt_info->enabled_interrupt_mask |= source_mask;
	else
		interrupt_info->enabled_interrupt_mask &= ~source_mask;

unlock:
	mutex_unlock(&interrupt_info->irqs_lock);

	return ret;
}
EXPORT_SYMBOL_NS(sdca_interrupt_enable, SND_SOC_SDCA_IRQ_HANDLER);

void sdca_interrupt_clear_history(struct sdw_slave *slave, u32 preserve_mask)
{
	struct sdca_interrupt_info *interrupt_info;

	interrupt_info = slave->sdca_data.interrupt_info;

	/*
	 * Clear all history except for the interrupts set in preserve_mask.
	 * This is very useful for SDCA UMP processing, where the
	 * interrupt is only thrown once when the ownership changes to
	 * HOST. If the processing happens in a work queue, and a new interrupt
	 * cancels the work queue, the interrupt will not be signaled again
	 */
	interrupt_info->detected_interrupt_mask &= preserve_mask;
}
EXPORT_SYMBOL_NS(sdca_interrupt_clear_history, SND_SOC_SDCA_IRQ_HANDLER);

/* helper called with the 'irq_lock' mutex held */
static int sdca_interrupt_register_handler(struct sdw_slave *slave,
					   struct sdca_interrupt_info *interrupt_info,
					   int reg_index)
{
	struct device *dev = &slave->dev;
	unsigned long status2;
	unsigned long status;
	const int retry = 3;
	int count = 0;
	int bit;
	int ret;

	ret = sdw_read_no_pm(slave, SDW_SCP_SDCA_INT1 + reg_index);
	if (ret < 0) {
		dev_err_ratelimited(dev,
				    "%s: read of SDW_SCP_SDCA_INT%d failed: %d\n",
				    __func__, reg_index + 1, ret);
		goto io_error;
	}
	status = ret;

	if (!status)
		return 0;

	do {
		/*
		 * Record detected interrupt sources, source-specific actions
		 * will be taken after all interrupts have been cleared.
		 */

		for_each_set_bit(bit, &status, 8) {
			int index = (reg_index << 3) + bit;

			interrupt_info->detected_interrupt_mask |= BIT(index);
		}

		/* clear the interrupts for this register */
		ret = sdw_write_no_pm(slave,
				      SDW_SCP_SDCA_INT1 + reg_index,
				      status);
		if (ret < 0) {
			dev_err_ratelimited(dev,
					    "%s: write to SDW_SCP_SDCA_INT%d failed: %d\n",
					    __func__, reg_index + 1, ret);
			goto io_error;
		}

		/*
		 * The SoundWire specification requires an additional read to make sure
		 * no interrupts are lost
		 */
		ret = sdw_read_no_pm(slave, SDW_SCP_SDCA_INT1 + reg_index);
		if (ret < 0) {
			dev_err_ratelimited(dev,
					    "%s: re-read of SDW_SCP_SDCA_INT%d failed: %d\n",
					    __func__, reg_index + 1, ret);
			goto io_error;
		}
		status2 = ret;

		/* filter to limit loop to interrupts identified in the first status read */
		status &= status2;
	} while (status && (count < retry));

	if (count == retry)
		dev_warn_ratelimited(dev,
				     "%s: Reached max_retry %d on SDW_SCP_SDCA_INT%d\n",
				     __func__, retry, reg_index + 1);
io_error:
	return ret;
}

int sdca_interrupt_handler(struct sdw_slave *slave)
{
	struct device *dev = &slave->dev;
	struct sdca_interrupt_info *interrupt_info;
	unsigned long registered_source_mask;
	unsigned long enabled_register_mask;
	unsigned long hw_register_mask;
	unsigned long detected_mask;
	int index;
	int reg_index;
	int ret = 0;

	interrupt_info = slave->sdca_data.interrupt_info;
	if (!interrupt_info)
		return -ENODEV;

	/*
	 * The critical section below intentionally protects a rather large piece of code.
	 * We don't want to allow the system suspend to disable an interrupt while we are
	 * processing it, which could be problematic given the quirky SoundWire interrupt
	 * scheme. We do want however to prevent new workqueues from being scheduled if
	 * the disable_irq flag was set during system suspend.
	 */
	mutex_lock(&interrupt_info->irqs_lock);

	/* check first if the interrupt masks are consistent */
	registered_source_mask = interrupt_info->registered_source_mask;
	if (!registered_source_mask) {
		dev_err(dev, "%s: no interrupt sources registered\n",
			__func__);
		ret = -EINVAL;
		goto unlock;
	}

	hw_register_mask = interrupt_info->supported_hw_register_mask;
	if (!hw_register_mask) {
		dev_err(dev, "%s: supported register mask not initialized\n",
			__func__);
		ret = -EINVAL;
		goto unlock;
	}

	/*
	 * Optimization: filter which registers needs to be checked to
	 * avoid useless reads. There could be cases where the device
	 * supports M interrupts but only N sources have been registered
	 * by Function drivers.
	 */
	enabled_register_mask = 0;
	for_each_set_bit(reg_index, &hw_register_mask, 8) {
		int shift = reg_index << 3;
		int source_mask_byte =  (registered_source_mask >> shift) & GENMASK(7, 0);

		if (!source_mask_byte)
			continue;
		enabled_register_mask |= BIT(reg_index);
	}

	for_each_set_bit(reg_index, &enabled_register_mask, 8) {
		ret = sdca_interrupt_register_handler(slave,
						      interrupt_info,
						      reg_index);
		if (ret < 0)
			goto unlock;
	}

	/*
	 * Handle source-specific tasks.
	 */
	detected_mask =  interrupt_info->detected_interrupt_mask;
	for_each_set_bit(index, &detected_mask, 32) {
		void *context = interrupt_info->sources[index]->context;

		/*
		 * There could be a racy window where the interrupts are disabled
		 * between the time the peripheral signals its alert status and
		 * the time where this interrupt handler is scheduled.
		 * In this case we don't invoke the callbacks since presumably a
		 * higher-level transition such as system suspend is going on.
		 */
		if (BIT(index) & interrupt_info->enabled_interrupt_mask)
			interrupt_info->sources[index]->callback(context);
	}

unlock:
	mutex_unlock(&interrupt_info->irqs_lock);

	return ret;
}
EXPORT_SYMBOL_NS(sdca_interrupt_handler, SND_SOC_SDCA_IRQ_HANDLER);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("SDCA IRQ handler library");
