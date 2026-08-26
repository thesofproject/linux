// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Cirrus Logic, Inc. and
//                    Cirrus Logic International Semiconductor Ltd.

/*
 * The MIPI SDCA specification is available for public downloads at
 * https://www.mipi.org/mipi-sdca-v1-0-download
 */

#include <linux/array_size.h>
#include <linux/bitmap.h>
#include <linux/cleanup.h>
#include <linux/container_of.h>
#include <linux/device.h>
#include <linux/dev_printk.h>
#include <linux/iopoll.h>
#include <linux/minmax.h>
#include <linux/mutex.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/soundwire/sdw.h>
#include <linux/soundwire/sdw_registers.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <sound/compress_driver.h>
#include <sound/sdca.h>
#include <sound/sdca_function.h>
#include <sound/sdca_interrupts.h>
#include <sound/sdca_wov.h>
#include <sound/soc.h>

/* TODO: This is a temporary definition for the dummy compressed stream data */
#define SDCA_WOV_FRAGMENT_SIZE		256

static const u32 sdca_wov_trigger_cbn_list[] = {
	SDCA_TRIGGER_CBN_VOICE,
	SDCA_TRIGGER_CBN_SPEECH,
};

static const struct sdca_cbns sdca_wov_trigger_cbns = {
	.cbns = sdca_wov_trigger_cbn_list,
	.num_cbns = ARRAY_SIZE(sdca_wov_trigger_cbn_list),
};

struct sdca_wov_state {
	struct mutex lock;
	struct sdca_interrupt *interrupt;
	struct sdca_entity *mfpu;
	struct sdca_control *algorithm_enable;
	struct sdca_control *algorithm_ready;
	struct sdca_control *enable;
	struct sdca_control *status;
	struct sdca_control *ready;
	struct snd_compr_stream *stream;

	/* Trigger_Enable BMT bits mapped to voice or speech behaviors. */
	struct sdca_bmt_bits voice_trigger_bmt;
	/* Algorithm_Ready BMT bits marked MFPU-Required. */
	struct sdca_bmt_bits algorithm_bmt;

	/* True while an arm request is eligible for its first trigger notification. */
	bool armed;

	/* TODO: This is for the dummy compressed stream data */
	size_t produced_bytes;
};

struct sdca_wov_cn_status {
	u8 status[SDCA_MAX_CONTROL_NUMBERS];
};

struct sdca_wov_compr {
	struct snd_compress_ops ops;
	struct sdca_interrupt_info *irq_info;
};

static void sdca_wov_compr_voice_event(struct sdca_wov_state *state);

static int sdca_wov_ready(struct sdca_interrupt *interrupt)
{
	const struct sdca_wov_state *state = interrupt->priv;
	unsigned int number, reg, mask, ready;
	int ret;

	dev_dbg(interrupt->dev, "wov: ready mode %#x\n", state->ready->mode);

	/* SDCA 1.0 sec. 6.3.7.10: each selected trigger must be ready before enabling. */
	sdca_for_each_bmt_control(number, &state->voice_trigger_bmt) {
		mask = sdca_bmt_control_mask(&state->voice_trigger_bmt, number);
		reg = SDW_SDCA_CTL(interrupt->function->desc->adr, interrupt->entity->id,
				   state->ready->sel, number);
		ret = regmap_read(interrupt->function_regmap, reg, &ready);
		if (ret)
			return ret;

		dev_dbg(interrupt->dev, "wov: Trigger_Ready CN %u ready %#x mask %#x\n",
			number, ready, mask);

		if ((ready & mask) != mask)
			return -EAGAIN;
	}

	return 0;
}

static int sdca_wov_wait_ready(struct sdca_interrupt *interrupt)
{
	const struct sdca_wov_state *state = interrupt->priv;
	unsigned int timeout = interrupt->entity->smpu.trigger_ready_max_delay;
	unsigned int poll_us;
	int poll_ret;
	int ready_ret;

	dev_dbg(interrupt->dev, "wov: Wait for ready: mode %#x timeout %u us\n",
		state->ready->mode, timeout);

	/* SDCA 1.0 sec. 7.9.17 permits DC Trigger_Ready; a constant needs only one check. */
	if (state->ready->mode == SDCA_ACCESS_MODE_DC)
		return sdca_wov_ready(interrupt);

	if (!timeout)
		return -EINVAL;

	poll_us = min_t(unsigned int, timeout, SDCA_WOV_READY_POLL_US);
	poll_ret = read_poll_timeout(sdca_wov_ready, ready_ret,
				     ready_ret != -EAGAIN,
				     poll_us, timeout, false,
				     interrupt);
	if (poll_ret || ready_ret) {
		dev_warn(interrupt->dev,
			 "wov: Trigger_Ready not ready (poll_ret:%d ready_ret:%d)\n",
			 poll_ret, ready_ret);
	}

	if (poll_ret)
		return poll_ret;

	return ready_ret;
}

static int sdca_wov_read_status(struct sdca_interrupt *interrupt,
				struct sdca_wov_cn_status *status, bool *pending)
{
	const struct sdca_wov_state *state = interrupt->priv;
	unsigned int number;
	int ret;

	dev_vdbg(interrupt->dev, "wov: Read selector %#x\n", state->status->sel);

	memset(status, 0, sizeof(*status));
	*pending = false;

	/* SDCA 1.0 sec. 6.3.7.11.1, example 1, step 4: snapshot Trigger_Status. */
	sdca_for_each_bmt_control(number, &state->voice_trigger_bmt) {
		unsigned int mask = sdca_bmt_control_mask(&state->voice_trigger_bmt, number);
		unsigned int reg, value;

		reg = SDW_SDCA_CTL(interrupt->function->desc->adr, interrupt->entity->id,
				   state->status->sel, number);
		ret = regmap_read(interrupt->function_regmap, reg, &value);
		if (ret)
			return ret;

		dev_vdbg(interrupt->dev, "wov: CN %u status %#x mask %#x\n",
			 number, value, mask);

		status->status[number] = value & mask;
		if (status->status[number])
			*pending = true;
	}

	return 0;
}

static int sdca_wov_clear_status(struct sdca_interrupt *interrupt,
				 const struct sdca_wov_cn_status *status)
{
	const struct sdca_wov_state *state = interrupt->priv;
	unsigned int number;
	unsigned int reg;
	int ret;

	dev_vdbg(interrupt->dev, "wov: Write selector %#x\n", state->status->sel);

	/* SDCA 1.0 sec. 6.3.7.11.1, example 1, step 6: clear the observed status bits. */
	sdca_for_each_bmt_control(number, &state->voice_trigger_bmt) {
		if (!status->status[number])
			continue;

		dev_dbg(interrupt->dev, "wov: CN %u clear %#x\n", number, status->status[number]);

		reg = SDW_SDCA_CTL(interrupt->function->desc->adr, interrupt->entity->id,
				   state->status->sel, number);

		ret = regmap_write(interrupt->function_regmap, reg, status->status[number]);
		if (ret)
			return ret;
	}

	return 0;
}

/*
 * SDCA 1.0 sec. 6.3.7.11, Fig. 150: a repeated event between the status read and
 * RW1C write keeps its status bit set. Drain pending status so stale events
 * cannot raise an interrupt when triggers are enabled again.
 */
static int sdca_wov_drain_status(struct sdca_interrupt *interrupt)
{
	struct sdca_wov_cn_status status;
	unsigned int pass;
	bool pending;
	int ret;

	dev_dbg(interrupt->dev, "wov: Drain status (IRQ %d)\n", interrupt->irq);

	for (pass = 0; pass < SDCA_WOV_MAX_DRAIN_PASSES; pass++) {
		ret = sdca_wov_read_status(interrupt, &status, &pending);
		if (ret || !pending)
			return ret;

		ret = sdca_wov_clear_status(interrupt, &status);
		if (ret)
			return ret;
	}

	return -EAGAIN;
}

static void sdca_wov_clear_device_interrupt(struct sdca_interrupt *interrupt)
{
	unsigned int interrupt_position = interrupt->control->interrupt_position;
	unsigned int reg = SDW_SCP_SDCA_INT1 + (interrupt_position / BITS_PER_BYTE);
	unsigned int mask = BIT(interrupt_position % BITS_PER_BYTE);
	int ret;

	dev_vdbg(interrupt->dev, "wov: Clear IRQ position %d (reg %#x mask %#x)\n",
		 interrupt_position, reg, mask);

	ret = regmap_write(interrupt->device_regmap, reg, mask);
	if (ret) {
		dev_warn(interrupt->dev, "wov: Clear IRQ position %d failed: %d\n",
			 interrupt_position, ret);
	}
}

static int sdca_wov_update_enable(struct sdca_interrupt *interrupt, bool enable)
{
	const struct sdca_wov_state *state = interrupt->priv;
	unsigned int number, reg;
	int first_ret = 0;
	int ret;

	dev_dbg(interrupt->dev, "wov: Update enable=%d\n", enable);

	/*
	 * SDCA 1.0 sec. 7.9.18: Trigger_Enable uses the trigger BMT's BitIndex mapping.
	 * Bits may span multiple Control Number bytes (Table 178, note 6), so update
	 * every byte containing voice/speech bits while preserving unrelated bits.
	 */
	sdca_for_each_bmt_control(number, &state->voice_trigger_bmt) {
		unsigned int mask = sdca_bmt_control_mask(&state->voice_trigger_bmt, number);

		reg = SDW_SDCA_CTL(interrupt->function->desc->adr, interrupt->entity->id,
				   state->enable->sel, number);
		ret = regmap_assign_bits(interrupt->function_regmap, reg, mask, enable);

		/* Keep going on error if disabling. */
		if (ret && !first_ret)
			first_ret = ret;
		if (ret && enable)
			break;
	}

	return first_ret;
}

static int sdca_wov_update_algorithm_enable(struct sdca_interrupt *interrupt, bool enable)
{
	const struct sdca_function_data *function = interrupt->function;
	const struct sdca_wov_state *state = interrupt->priv;
	const struct sdca_entity *mfpu = state->mfpu;
	unsigned int number, reg, mask;
	int first_ret = 0;
	int ret;

	/* SDCA 1.0 sec. 7.9.7.1: Algorithm_Enable shares Algorithm_Ready's BMT. */
	sdca_for_each_bmt_control(number, &state->algorithm_bmt) {
		mask = sdca_bmt_control_mask(&state->algorithm_bmt, number);
		reg = SDW_SDCA_CTL(function->desc->adr, mfpu->id,
				   state->algorithm_enable->sel, number);
		ret = regmap_assign_bits(interrupt->function_regmap, reg, mask, enable);
		if (ret) {
			dev_err(interrupt->dev,
				"wov: %s: failed to write Algorithm_Enable CN %u: %d\n",
				mfpu->label, number, ret);
			if (!first_ret)
				first_ret = ret;
			/* Attempt all control bytes when disabling. */
			if (enable)
				break;
		}
	}

	return first_ret;
}

static int sdca_wov_set_algorithm_enable(struct sdca_interrupt *interrupt, bool enable)
{
	const struct sdca_function_data *function = interrupt->function;
	const struct sdca_wov_state *state = interrupt->priv;
	const struct sdca_entity *mfpu = state->mfpu;
	unsigned int number, reg, mask, value;
	int ret, restore_ret;

	if (!enable)
		return sdca_wov_update_algorithm_enable(interrupt, false);

	/* SDCA 1.0 sec. 6.3.3.7, rule 4: verify all selected algorithms before enabling. */
	sdca_for_each_bmt_control(number, &state->algorithm_bmt) {
		mask = sdca_bmt_control_mask(&state->algorithm_bmt, number);
		reg = SDW_SDCA_CTL(function->desc->adr, mfpu->id,
				   state->algorithm_ready->sel, number);
		ret = regmap_read(interrupt->function_regmap, reg, &value);
		if (ret) {
			dev_err(interrupt->dev,
				"wov: %s: failed to read Algorithm_Ready CN %u: %d\n",
				mfpu->label, number, ret);
			return ret;
		}

		dev_dbg(interrupt->dev, "wov: %s Algorithm_Ready CN %u value %#x mask %#x\n",
			mfpu->label, number, value, mask);

		if ((value & mask) != mask) {
			dev_err(interrupt->dev,
				"wov: %s: required algorithms not ready (CN %u value %#x mask %#x)\n",
				mfpu->label, number, value, mask);
			/* Algorithm preparation must complete before arming the stream. */
			return -EIO;
		}
	}

	ret = sdca_wov_update_algorithm_enable(interrupt, true);
	if (ret) {
		/* A later control byte may have failed after earlier algorithms were enabled. */
		restore_ret = sdca_wov_update_algorithm_enable(interrupt, false);
		if (restore_ret)
			dev_err(interrupt->dev, "wov: failed to disable MFPU algorithms: %d\n",
				restore_ret);
		return ret;
	}

	dev_dbg(interrupt->dev, "wov: %s required algorithms enabled\n", mfpu->label);

	return 0;
}

static int sdca_wov_enable_voice_trigger(struct sdca_interrupt *interrupt)
{
	struct sdca_wov_state *state = interrupt->priv;
	int restore_ret;
	int ret;

	dev_dbg(interrupt->dev, "wov: armed=%d\n", state->armed);

	guard(mutex)(&state->lock);

	/* Already armed; leave trigger status intact in case an IRQ is pending. */
	if (state->armed)
		return 0;

	/* Keep triggers disabled while enabling the algorithm and clearing stale status. */
	ret = sdca_wov_update_enable(interrupt, false);
	if (ret) {
		dev_err(interrupt->dev,
			"wov: %s: failed to disable triggers before arming: %d\n",
			interrupt->entity->label, ret);
		return ret;
	}

	/* Keep the upstream MFPU processing active while the SMPU listens for a trigger. */
	ret = sdca_wov_set_algorithm_enable(interrupt, true);
	if (ret)
		return ret;

	/* Discard stale events from before this arm request. */
	ret = sdca_wov_drain_status(interrupt);
	if (ret) {
		dev_err(interrupt->dev,
			"wov: %s: failed to drain trigger status before arming: %d\n",
			interrupt->entity->label, ret);
		return ret;
	}

	sdca_wov_clear_device_interrupt(interrupt);

	/* SDCA 1.0 sec. 6.3.7.10: wait for Trigger_Ready before setting Trigger_Enable. */
	ret = sdca_wov_wait_ready(interrupt);
	if (ret) {
		dev_err(interrupt->dev, "wov: %s: Trigger_Ready wait failed: %d\n",
			interrupt->entity->label, ret);
		return ret;
	}

	/* SDCA 1.0 sec. 7.9.18: enable the triggers. */
	ret = sdca_wov_update_enable(interrupt, true);
	if (ret) {
		dev_err(interrupt->dev, "wov: %s: failed to enable voice triggers: %d\n",
			interrupt->entity->label, ret);

		/* Roll back enables. */
		restore_ret = sdca_wov_update_enable(interrupt, false);
		if (restore_ret)
			dev_err(interrupt->dev,
				"wov: failed to disable WoV triggers: %d\n",
				restore_ret);

		return ret;
	}

	state->armed = true;
	dev_dbg(interrupt->dev, "wov: %s: wake-on-voice trigger started\n",
		interrupt->entity->label);

	return 0;
}

static int sdca_wov_cancel_voice_trigger(struct sdca_interrupt *interrupt)
{
	struct sdca_wov_state *state = interrupt->priv;
	int ret;

	dev_dbg(interrupt->dev, "wov: Cancel voice trigger (armed=%d)\n", state->armed);

	guard(mutex)(&state->lock);

	state->armed = false;

	ret = sdca_wov_update_enable(interrupt, false);
	if (ret)
		return ret;

	dev_dbg(interrupt->dev, "wov: %s: trigger stopped\n", interrupt->entity->label);

	/* Discard events from the canceled request. */
	ret = sdca_wov_drain_status(interrupt);
	if (ret)
		return ret;

	sdca_wov_clear_device_interrupt(interrupt);

	return sdca_wov_set_algorithm_enable(interrupt, false);
}

int sdca_wov_process_voice_trigger(struct sdca_interrupt *interrupt)
{
	struct sdca_wov_state *state = interrupt->priv;
	struct sdca_wov_cn_status status;
	bool notify = false;
	bool pending;
	int ret;

	if (!state)
		return -EINVAL;

	WARN_ON_ONCE(!pm_runtime_active(interrupt->dev));

	guard(mutex)(&state->lock);

	dev_dbg(interrupt->dev, "wov: IRQ %d Process trigger (armed=%d)\n",
		interrupt->irq, state->armed);

	/* SDCA 1.0 sec. 6.3.7.11.1, example 1, step 4: read the detected trigger bits. */
	ret = sdca_wov_read_status(interrupt, &status, &pending);
	if (ret)
		goto err;

	if (pending) {
		/* Deliver at most one notification per arm, even if the interrupt repeats. */
		notify = state->armed;
		state->armed = false;

		/* One event completes the request; disable triggers until the next explicit arm. */
		ret = sdca_wov_update_enable(interrupt, false);
		if (ret)
			goto err;

		dev_dbg(interrupt->dev, "wov: %s: trigger stopped\n",
			interrupt->entity->label);

		ret = sdca_wov_clear_status(interrupt, &status);
		if (ret)
			goto err;
	}

err:
	/* Attempt IRQ acknowledgment even on errors to avoid an interrupt storm. */
	sdca_wov_clear_device_interrupt(interrupt);

	/* Report the observed event even if subsequent hardware cleanup failed. */
	if (notify)
		sdca_wov_compr_voice_event(state);

	return ret;
}

static bool sdca_wov_is_supported_interrupt(struct sdca_interrupt *interrupt)
{
	const struct sdca_function_data *function;
	u32 type;

	function = interrupt->function;
	type = function->desc->type;

	dev_vdbg(interrupt->dev, "wov: function %#x type %#x entity %s\n",
		function->desc->adr, type, interrupt->entity->label);

	switch (type) {
	case SDCA_FUNCTION_TYPE_SMART_MIC:
		if (sdca_find_entity_by_label(function, SDCA_WOV_SMPU_LABEL) == interrupt->entity)
			return true;

		return false;
	default:
		return false;
	}
}

static bool sdca_behavior_set_is_wov(struct device *dev,
				     const struct sdca_behavior_set *set)
{
	if (sdca_behavior_set_contains_cbn(set, &sdca_wov_trigger_cbns)) {
		dev_dbg(dev, "wov: behavior set %#x is WoV\n", set->id);
		return true;
	}

	dev_vdbg(dev, "wov: behavior set %#x (CBN count %d) not WoV\n", set->id, set->num_cbns);

	return false;
}

static int sdca_wov_check_bmt_controls(const struct sdca_interrupt *interrupt,
				       const struct sdca_entity *entity,
				       const struct sdca_bmt_bits *bmt, u64 cn_list)
{
	unsigned int number;

	sdca_for_each_bmt_control(number, bmt) {
		if (!(cn_list & BIT_ULL(number))) {
			dev_err(interrupt->dev,
				"wov: %s: BMT CN %u has incomplete controls\n",
				entity->label, number);
			return -EINVAL;
		}
	}

	return 0;
}

static int sdca_wov_lookup_algorithm_bmt_bits(struct sdca_interrupt *interrupt,
					      struct sdca_wov_state *state)
{
	static const u32 cbn_list[] = { SDCA_ALGORITHM_CBN_REQUIRED };
	static const struct sdca_cbns cbns = {
		.cbns = cbn_list,
		.num_cbns = ARRAY_SIZE(cbn_list),
	};
	int ret;

	/* SDCA 1.0 sec. 7.9.7.1: the algorithm BMT is in Algorithm_Ready's range. */
	/* Select MFPU-Required algorithms (SDCA 1.0 Table 103) for WoV. */
	ret = sdca_lookup_bmt_bits(interrupt->dev, interrupt->function,
				   state->mfpu, state->algorithm_ready, &cbns,
				   &state->algorithm_bmt);
	if (ret)
		return ret;

	return sdca_wov_check_bmt_controls(interrupt, state->mfpu, &state->algorithm_bmt,
					  state->algorithm_ready->cn_list &
					  state->algorithm_enable->cn_list);
}

static int sdca_wov_lookup_trigger_bmt_bits(struct sdca_interrupt *interrupt,
					    struct sdca_wov_state *state)
{
	int ret;

	/* SDCA 1.0 sec. 6.3.7.7: Trigger_Ready, Trigger_Enable and Trigger_Status share a BMT. */
	/* Select Voice/Speech triggers (SDCA 1.0 sec. 6.3.7.7.1, Table 124). */
	ret = sdca_lookup_bmt_bits(interrupt->dev, interrupt->function,
				   interrupt->entity, state->enable,
				   &sdca_wov_trigger_cbns, &state->voice_trigger_bmt);
	if (ret)
		return ret;

	if (bitmap_empty(state->voice_trigger_bmt.bits, SDCA_MAX_BMT_BITS)) {
		dev_err(interrupt->dev, "wov: No Voice/Speech triggers defined\n");
		return -ENODEV;
	}

	return sdca_wov_check_bmt_controls(interrupt, interrupt->entity,
					  &state->voice_trigger_bmt,
					  state->enable->cn_list & state->ready->cn_list &
					  state->status->cn_list);
}

/**
 * sdca_wov_alloc_state - allocate wake-on-voice state for an SMPU interrupt
 * @interrupt: Trigger_Status interrupt and its SDCA context.
 *
 * The standard SMPU 16 role is selected by its DisCo Entity Label. Voice and
 * Speech trigger bits are then derived from the Trigger_Enable BitIndex
 * Mapping Table and the Function's DisCo Behavior Set library. Required MFPU 16
 * algorithm bits are derived from its Algorithm_Ready BitIndex Mapping Table.
 *
 * Return: Zero on success, -ENODEV if this is not the standard WoV trigger
 * interrupt, or another negative error code.
 */
int sdca_wov_alloc_state(struct sdca_interrupt *interrupt)
{
	const struct sdca_entity *smpu;
	int ret;

	dev_dbg(interrupt->dev, "wov: %s\n", interrupt->entity->label);

	if (!sdca_wov_is_supported_interrupt(interrupt))
		return -ENODEV;

	/* SDCA 1.0 sec. 7.9.19: Trigger_Status must support RW1C acknowledgment. */
	smpu = interrupt->entity;
	if (interrupt->control->mode != SDCA_ACCESS_MODE_RW1C) {
		dev_err(interrupt->dev,
			"wov: %s: Trigger_Status access mode %#x is not RW1C\n",
			smpu->label, interrupt->control->mode);
		return -EINVAL;
	}

	if (interrupt->control->interrupt_position < 0 ||
	    interrupt->control->interrupt_position >= SDCA_MAX_INTERRUPTS)
		return -EINVAL;

	struct sdca_wov_state *state __free(kfree) = kzalloc_obj(*state);
	if (!state)
		return -ENOMEM;

	state->interrupt = interrupt;
	state->status = interrupt->control;
	state->enable = sdca_selector_find_control(interrupt->dev, smpu,
						   SDCA_CTL_SMPU_TRIGGER_ENABLE);
	state->ready = sdca_selector_find_control(interrupt->dev, smpu,
						  SDCA_CTL_SMPU_TRIGGER_READY);

	if (!state->enable || !state->ready) {
		dev_err(interrupt->dev, "wov: %s: missing trigger controls\n", smpu->label);
		return -EINVAL;
	}

	if (state->enable->mode != SDCA_ACCESS_MODE_RW) {
		dev_err(interrupt->dev, "wov: %s: Trigger_Enable access mode %#x is not RW\n",
			smpu->label, state->enable->mode);
		return -EINVAL;
	}

	switch (state->ready->mode) {
	case SDCA_ACCESS_MODE_DC:
		break;
	default:
		if (!smpu->smpu.trigger_ready_max_delay) {
			dev_err(interrupt->dev, "wov: %s: trigger ready delay missing\n",
				smpu->label);
			return -EINVAL;
		}
		break;
	}

	state->mfpu = sdca_find_entity_by_label(interrupt->function, "MFPU 16");
	if (!state->mfpu || state->mfpu->type != SDCA_ENTITY_TYPE_MFPU) {
		dev_err(interrupt->dev, "%s: MFPU 16 not found\n", smpu->label);
		return -ENODEV;
	}

	state->algorithm_enable = sdca_selector_find_control(interrupt->dev, state->mfpu,
							     SDCA_CTL_MFPU_ALGORITHM_ENABLE);
	state->algorithm_ready = sdca_selector_find_control(interrupt->dev, state->mfpu,
							    SDCA_CTL_MFPU_ALGORITHM_READY);
	if (!state->algorithm_enable || !state->algorithm_ready)
		return -EINVAL;

	ret = sdca_wov_lookup_algorithm_bmt_bits(interrupt, state);
	if (ret)
		return ret;

	ret = sdca_wov_lookup_trigger_bmt_bits(interrupt, state);
	if (ret)
		return ret;

	mutex_init(&state->lock);
	interrupt->priv = no_free_ptr(state);
	interrupt->free_priv = sdca_wov_free_state;

	return 0;
}

static struct sdca_wov_state *sdca_wov_get_state(const struct sdca_interrupt *interrupt)
{
	struct sdca_wov_state *state;

	if (interrupt->free_priv != sdca_wov_free_state)
		return NULL;

	state = interrupt->priv;
	if (!state || state->interrupt != interrupt)
		return NULL;

	return state;
}

static int sdca_wov_compr_open(struct snd_soc_component *component,
			       struct snd_compr_stream *stream)
{
	const struct sdca_wov_compr *compr =
		container_of_const(component->driver->compress_ops, struct sdca_wov_compr, ops);
	struct sdca_interrupt_info *info = compr->irq_info;
	int i;

	if (stream->direction != SND_COMPRESS_CAPTURE)
		return -EINVAL;

	guard(mutex)(&info->irq_lock);

	for (i = 0; i < SDCA_MAX_INTERRUPTS; i++) {
		const struct sdca_interrupt *interrupt = &info->irqs[i];
		struct sdca_wov_state *state;

		if (interrupt->component != component || interrupt->irq <= 0)
			continue;

		state = sdca_wov_get_state(interrupt);
		if (!state)
			continue;

		guard(mutex)(&state->lock);
		if (state->stream)
			return -EBUSY;

		state->produced_bytes = 0;
		state->stream = stream;
		stream->runtime->private_data = state;
		return 0;
	}

	return -ENODEV;
}

static int sdca_wov_compr_free(struct snd_soc_component *component,
			       struct snd_compr_stream *stream)
{
	struct sdca_wov_state *state = stream->runtime->private_data;

	state->stream = NULL;
	stream->runtime->private_data = NULL;

	/* Cancel the outstanding arm before ASoC can power down the stream's DAPM path. */
	return sdca_wov_cancel_voice_trigger(state->interrupt);
}

static int sdca_wov_compr_trigger(struct snd_soc_component *component,
				  struct snd_compr_stream *stream, int cmd)
{
	struct sdca_wov_state *state = stream->runtime->private_data;
	int ret;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		return sdca_wov_enable_voice_trigger(state->interrupt);
	case SNDRV_PCM_TRIGGER_STOP:
		ret = sdca_wov_cancel_voice_trigger(state->interrupt);
		if (!ret)
			state->produced_bytes = 0;
		return ret;
	default:
		return -EINVAL;
	}
}

static int sdca_wov_compr_pointer(struct snd_soc_component *component,
				  struct snd_compr_stream *stream,
				  struct snd_compr_tstamp64 *tstamp)
{
	struct sdca_wov_state *state = stream->runtime->private_data;

	/* TODO: This is Dummy implementation for test only */

	guard(mutex)(&state->lock);

	memset(tstamp, 0, sizeof(*tstamp));
	tstamp->copied_total = state->produced_bytes;
	dev_dbg(state->interrupt->dev,
		"wov: compr pointer: produced %zu transferred %llu fragment %u state %d\n",
		state->produced_bytes, stream->runtime->total_bytes_transferred,
		stream->runtime->fragment_size, stream->runtime->state);

	return 0;
}

static int sdca_wov_compr_copy(struct snd_soc_component *component,
			       struct snd_compr_stream *stream, char __user *buf,
			       size_t count)
{
	const struct sdca_wov_state *state = stream->runtime->private_data;
	char data[SDCA_WOV_FRAGMENT_SIZE];

	/* TODO: This is Dummy implementation for test only, produces a buffer of '.' */
	count = min_t(size_t, count, SDCA_WOV_FRAGMENT_SIZE);
	memset(data, '.', count);
	if (copy_to_user(buf, data, count))
		return -EFAULT;

	dev_dbg(state->interrupt->dev, "wov: compr: copied %zu bytes\n", count);

	return count;
}

static int sdca_wov_compr_get_caps(struct snd_soc_component *component,
				   struct snd_compr_stream *stream,
				   struct snd_compr_caps *caps)
{
	memset(caps, 0, sizeof(*caps));
	caps->direction = SND_COMPRESS_CAPTURE;
	caps->num_codecs = 1;
	caps->codecs[0] = SND_AUDIOCODEC_BESPOKE;

	/* TODO: Provide real fragment size and max_fragments */
	caps->min_fragment_size = SDCA_WOV_FRAGMENT_SIZE;
	caps->max_fragment_size = SDCA_WOV_FRAGMENT_SIZE;
	caps->min_fragments = 1;
	caps->max_fragments = 1;

	return 0;
}

static void sdca_wov_compr_voice_event(struct sdca_wov_state *state)
{
	lockdep_assert_held(&state->lock);

	if (!state->stream) {
		dev_dbg(state->interrupt->dev, "wov: voice event without a compressed stream\n");
		return;
	}

	dev_dbg(state->interrupt->dev, "wov: compr: voice event\n");

	/* TODO: This is only a dummy fragment for testing */
	state->produced_bytes += SDCA_WOV_FRAGMENT_SIZE;

	/* Wake readers after the dummy fragment is visible through the pointer callback. */
	snd_compr_fragment_elapsed(state->stream);
}

static const struct snd_compress_ops sdca_wov_compress_ops = {
	.open		= sdca_wov_compr_open,
	.free		= sdca_wov_compr_free,
	.trigger	= sdca_wov_compr_trigger,
	.pointer	= sdca_wov_compr_pointer,
	.copy		= sdca_wov_compr_copy,
	.get_caps	= sdca_wov_compr_get_caps,
};

int sdca_wov_populate_dapm(struct device *dev, struct sdca_entity *entity,
			   struct snd_soc_dapm_route **route)
{
	(*route)->sink = devm_kasprintf(dev, GFP_KERNEL, "%s WoV Capture", entity->label);
	if (!(*route)->sink)
		return -ENOMEM;

	(*route)->control = NULL;
	(*route)->source = entity->label;
	(*route)++;

	return 0;
}

int sdca_wov_populate_dai(struct device *dev,
			  struct sdca_function_data *function,
			  struct snd_soc_dai_driver *dai,
			  const struct snd_soc_pcm_stream *stream,
			  int entity_index)
{
	const struct sdca_entity *entity = &function->entities[entity_index];

	dai->name = devm_kasprintf(dev, GFP_KERNEL, "%s WoV", entity->label);
	if (!dai->name)
		return -ENOMEM;

	dai->id = function->num_entities + entity_index;
	dai->capture = *stream;
	dai->capture.channels_max = 1;

	/* sdca_wov_populate_dapm() routes this stream to the DisCo terminal. */
	dai->capture.stream_name = devm_kasprintf(dev, GFP_KERNEL, "%s Capture", dai->name);
	if (!dai->capture.stream_name)
		return -ENOMEM;

	return 0;
}

/* Match a data-port OT fed by the SMPU supported by the WoV callbacks. */
bool sdca_wov_is_terminal(struct device *dev, struct sdca_function_data *function,
			  struct sdca_entity *entity)
{
	const struct sdca_control_range *range;
	const struct sdca_entity *smpu;
	int i;

	if (function->desc->type != SDCA_FUNCTION_TYPE_SMART_MIC ||
	    entity->type != SDCA_ENTITY_TYPE_OT || !entity->iot.is_dataport)
		return false;

	smpu = sdca_find_entity_by_label(function, SDCA_WOV_SMPU_LABEL);
	if (!smpu || smpu->type != SDCA_ENTITY_TYPE_SMPU)
		return false;

	for (i = 0; i < entity->num_sources; i++) {
		if (entity->sources[i] == smpu)
			break;
	}
	if (i == entity->num_sources)
		return false;

	for (i = 0; i < smpu->num_controls; i++) {
		if (smpu->controls[i].sel == SDCA_CTL_SMPU_TRIGGER_ENABLE)
			break;
	}
	if (i == smpu->num_controls)
		return false;

	range = sdca_control_find_range(dev, smpu, &smpu->controls[i],
					SDCA_BMT_COLS, 0);
	if (!range)
		return false;

	for (i = 0; i < range->rows; i++) {
		const struct sdca_behavior_set *set;
		unsigned int set_id = sdca_range(range, SDCA_BMT_BEHAVIOR_SET, i);

		set = sdca_find_behavior_set(function, set_id);
		if (set && set->cbns && sdca_behavior_set_is_wov(dev, set))
			return true;
	}

	return false;
}

int sdca_wov_init_component(struct device *dev,
			    struct sdca_function_data *function,
			    struct sdca_interrupt_info *info,
			    struct snd_soc_component_driver *component_drv)
{
	struct sdca_wov_compr *compr;
	int i;

	for (i = 0; i < function->num_entities - 1; i++) {
		if (sdca_wov_is_terminal(dev, function, &function->entities[i]))
			break;
	}
	if (i == function->num_entities - 1)
		return 0;

	compr = devm_kzalloc(dev, sizeof(*compr), GFP_KERNEL);
	if (!compr)
		return -ENOMEM;

	compr->ops = sdca_wov_compress_ops;
	compr->irq_info = info;
	component_drv->compress_ops = &compr->ops;

	return 0;
}

void sdca_wov_free_state(struct sdca_interrupt *interrupt)
{
	struct sdca_wov_state *state;

	state = sdca_wov_get_state(interrupt);
	if (!state)
		return;

	interrupt->priv = NULL;
	interrupt->free_priv = NULL;
	mutex_destroy(&state->lock);
	kfree(state);
}
