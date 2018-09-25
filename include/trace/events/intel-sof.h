/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM intel-sof

/*
 * The TRACE_SYSTEM_VAR defaults to TRACE_SYSTEM, but must be a
 * legitimate C variable. It is not exported to user space.
 */
#undef TRACE_SYSTEM_VAR
#define TRACE_SYSTEM_VAR intel_sof

#if !defined(_TRACE_INTEL_SOF_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_INTEL_SOF_H

#include <linux/types.h>
#include <linux/ktime.h>
#include <linux/tracepoint.h>

/* should be the same as rmbox */
#define TRACE_CLASS_IRQ		(1 << 24)
#define TRACE_CLASS_IPC		(2 << 24)
#define TRACE_CLASS_PIPE	(3 << 24)
#define TRACE_CLASS_HOST	(4 << 24)
#define TRACE_CLASS_DAI		(5 << 24)
#define TRACE_CLASS_DMA		(6 << 24)
#define TRACE_CLASS_SSP		(7 << 24)
#define TRACE_CLASS_COMP	(8 << 24)
#define TRACE_CLASS_WAIT	(9 << 24)
#define TRACE_CLASS_LOCK	(10 << 24)
#define TRACE_CLASS_MEM		(11 << 24)
#define TRACE_CLASS_MIXER	(12 << 24)
#define TRACE_CLASS_BUFFER	(13 << 24)
#define TRACE_CLASS_VOLUME	(14 << 24)
#define TRACE_CLASS_SWITCH	(15 << 24)
#define TRACE_CLASS_MUX		(16 << 24)
#define TRACE_CLASS_SRC         (17 << 24)
#define TRACE_CLASS_TONE        (18 << 24)
#define TRACE_CLASS_EQ_FIR      (19 << 24)
#define TRACE_CLASS_EQ_IIR      (20 << 24)
#define TRACE_CLASS_SA          (21 << 24)
#define TRACE_CLASS_DMIC        (22 << 24)
#define TRACE_CLASS_POWER       (23 << 24)

#define show_trace_class(class)					\
	__print_symbolic(class,					\
		{ TRACE_CLASS_IRQ,	"irq" },		\
		{ TRACE_CLASS_IPC,	"ipc" },		\
		{ TRACE_CLASS_PIPE,	"pipe" },		\
		{ TRACE_CLASS_HOST,	"host" },		\
		{ TRACE_CLASS_DAI,	"dai" },		\
		{ TRACE_CLASS_DMA,	"dma" },		\
		{ TRACE_CLASS_SSP,	"ssp" },		\
		{ TRACE_CLASS_COMP,	"comp" },		\
		{ TRACE_CLASS_WAIT,	"wait" },		\
		{ TRACE_CLASS_LOCK,	"lock" },		\
		{ TRACE_CLASS_MEM,	"mem" },		\
		{ TRACE_CLASS_MIXER,	"mixer" },		\
		{ TRACE_CLASS_BUFFER,	"buffer" },		\
		{ TRACE_CLASS_VOLUME,	"volume" },		\
		{ TRACE_CLASS_SWITCH,	"switch" },		\
		{ TRACE_CLASS_MUX,	"mux" },		\
		{ TRACE_CLASS_SRC,	"src" },		\
		{ TRACE_CLASS_TONE,	"tone" },		\
		{ TRACE_CLASS_EQ_FIR,	"eq-fir" },		\
		{ TRACE_CLASS_EQ_IIR,	"eq-iir" },		\
		{ TRACE_CLASS_SA,	"sa" },			\
		{ TRACE_CLASS_DMIC,	"dmic" },		\
		{ TRACE_CLASS_POWER,	"pm" })

DECLARE_EVENT_CLASS(sof_dma_buf_cmd,

	TP_PROTO(uint32_t offset, uint64_t time, uint64_t val),

	TP_ARGS(offset, time, val),

	TP_STRUCT__entry(
		__field(uint32_t, offset)
		__field(uint64_t, time)
		__field(uint64_t, val)
	),

	TP_fast_assign(
		__entry->offset = offset;
		__entry->time = time;
		__entry->val = val;
	),

	TP_printk("0x%x %llx %s %c%c%c",
		__entry->offset,
		__entry->time,
		show_trace_class((uint32_t)(__entry->val & 0xff000000)),
		(char)(__entry->val >> 16),
		(char)(__entry->val >> 8),
		(char)(__entry->val))
);

DEFINE_EVENT(sof_dma_buf_cmd, sof_dma_read_cmd,

	TP_PROTO(uint32_t offset, uint64_t time, uint64_t val),

	TP_ARGS(offset, time, val)

);

DECLARE_EVENT_CLASS(sof_dma_buf_val,

	TP_PROTO(uint32_t offset, uint64_t time, uint64_t val),

	TP_ARGS(offset, time, val),

	TP_STRUCT__entry(
		__field(uint32_t, offset)
		__field(uint64_t, time)
		__field(uint64_t, val)
	),

	TP_fast_assign(
		__entry->offset = offset;
		__entry->time = time;
		__entry->val = val;
	),

	TP_printk("0x%x %llx value 0x%16.16llx",
		__entry->offset, __entry->time, __entry->val)
);

DEFINE_EVENT(sof_dma_buf_val, sof_dma_read_val,

	TP_PROTO(uint32_t offset, uint64_t time, uint64_t val),

	TP_ARGS(offset, time, val)

);

#endif /* _TRACE_SOF_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
