/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright(c) 2022 Intel Corporation
 *
 * Author: Noah Klayman <noah.klayman@intel.com>
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM sof

#if !defined(_TRACE_SOF_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_SOF_H
#include <linux/tracepoint.h>
#include <linux/types.h>
#include "../../../sound/soc/sof/sof-priv.h"

TRACE_EVENT(sof_stream_position_ipc_rx,
	TP_PROTO(struct device *dev),
	TP_ARGS(dev),
	TP_STRUCT__entry(
		__string(device_name, dev_name(dev))
	),
	TP_fast_assign(
		__assign_str(device_name);
	),
	TP_printk("device_name=%s", __get_str(device_name))
);

TRACE_EVENT(sof_ipc4_fw_config,
	TP_PROTO(struct snd_sof_dev *sdev, char *key, u32 value),
	TP_ARGS(sdev, key, value),
	TP_STRUCT__entry(
		__string(device_name, dev_name(sdev->dev))
		__string(key, key)
		__field(u32, value)
	),
	TP_fast_assign(
		__assign_str(device_name);
		__assign_str(key);
		__entry->value = value;
	),
	TP_printk("device_name=%s key=%s value=%d",
		  __get_str(device_name), __get_str(key), __entry->value)
);

#endif /* _TRACE_SOF_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
