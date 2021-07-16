/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause) */
/*
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * Copyright(c) 2021 Intel Corporation. All rights reserved.
 */

#ifndef __INCLUDE_SOUND_SOF_IPC4_HEADER_H__
#define __INCLUDE_SOUND_SOF_IPC4_HEADER_H__

#include <linux/types.h>
#include <uapi/sound/sof/abi.h>

/** \addtogroup sof_uapi uAPI
 *  SOF uAPI specification.
 *  @{
 */

/*
 * IPC4 messages have two 32 bit identifier made up as follows :-
 *
 * header - msg type, msg id, msg direction ...
 * extension - extra params such as msg data size in mailbox
 *
 * These are sent at the start of the IPC message in the mailbox. Messages
 * should not be sent in the doorbell (special exceptions for firmware).
 */

enum sof_ipc4_msg_target {
	/* Global FW message */
	SOF_IPC4_FW_GEN_MSG = 0,

	/* Module message */
	SOF_IPC4_MODULE_MSG = 1
};

enum sof_ipc4_global_msg {
	SOF_IPC4_GLB_BOOT_CONFIG = 0,
	SOF_IPC4_GLB_ROM_CONTROL = 1,
	SOF_IPC4_GLB_IPCGATEWAY_CMD = 2,

	SOF_IPC4_GLB_PERF_MEASUREMENTS_CMD = 13,
	SOF_IPC4_GLB_CHAIN_DMA = 14,

	SOF_IPC4_GLB_LOAD_MULTIPLE_MODULES = 15,
	SOF_IPC4_GLB_UNLOAD_MULTIPLE_MODULES = 16,

	/* pipeline settings */
	SOF_IPC4_GLB_CREATE_PIPELINE = 17,
	SOF_IPC4_GLB_DELETE_PIPELINE = 18,
	SOF_IPC4_GLB_SET_PIPELINE_STATE = 19,
	SOF_IPC4_GLB_GET_PIPELINE_STATE = 20,
	SOF_IPC4_GLB_GET_PIPELINE_CONTEXT_SIZE = 21,
	SOF_IPC4_GLB_SAVE_PIPELINE = 22,
	SOF_IPC4_GLB_RESTORE_PIPELINE = 23,

	/* Loads library (using Code Load or HD/A Host Output DMA) */
	SOF_IPC4_GLB_LOAD_LIBRARY = 24,
	SOF_IPC4_GLB_INTERNAL_MESSAGE = 26,

	/* Notification (FW to SW driver) */
	SOF_IPC4_GLB_NOTIFICATION = 27,
	SOF_IPC4_GLB_MAX_IXC_MESSAGE_TYPE = 31
};

/* Message direction */
enum sof_ipc4_msg_dir {
	SOF_IPC4_MSG_REQUEST = 0,
	SOF_IPC4_MSG_REPLY = 1,
};

enum sof_ipc4_pipe_line_state {
	SOF_IPC4_PIPE_INVALID_STATE = 0,
	SOF_IPC4_PIPE_UNINITIALIZED = 1,
	SOF_IPC4_PIPE_RESET = 2,
	SOF_IPC4_PIPE_PAUSED = 3,
	SOF_IPC4_PIPE_RUNNING = 4,
	SOF_IPC4_PIPE_EOS = 5
};

/* global common ipc msg */
#define SOF_IPC4_GLB_MSG_TARGET_SHIFT		30
#define SOF_IPC4_GLB_MSG_TARGET_MASK		BIT(30)
#define SOF_IPC4_GLB_MSG_TARGET(x)		((x) << SOF_IPC4_GLB_MSG_TARGET_SHIFT)

#define SOF_IPC4_GLB_MSG_DIR_SHIFT		29
#define SOF_IPC4_GLB_MSG_DIR_MASK		BIT(29)
#define SOF_IPC4_GLB_MSG_DIR(x)			((x) << SOF_IPC4_GLB_MSG_DIR_SHIFT)

#define SOF_IPC4_GLB_MSG_TYPE_SHIFT		24
#define SOF_IPC4_GLB_MSG_TYPE_MASK		GENMASK(28, 24)
#define SOF_IPC4_GLB_MSG_TYPE(x)		((x) << SOF_IPC4_GLB_MSG_TYPE_SHIFT)

/* pipeline creation ipc msg */
#define SOF_IPC4_GLB_PIPE_INSTANCE_SHIFT	16
#define SOF_IPC4_GLB_PIPE_INSTANCE_MASK		GENMASK(23, 16)
#define SOF_IPC4_GLB_PIPE_INSTANCE_ID(x)	((x) << SOF_IPC4_GLB_PIPE_INSTANCE_SHIFT)

#define SOF_IPC4_GLB_PIPE_PRIORITY_SHIFT	11
#define SOF_IPC4_GLB_PIPE_PRIORITY_MASK		GENMASK(15, 11)
#define SOF_IPC4_GLB_PIPE_PRIORITY(x)		((x) << SOF_IPC4_GLB_PIPE_PRIORITY_SHIFT)

#define SOF_IPC4_GLB_PIPE_MEM_SIZE_SHIFT	0
#define SOF_IPC4_GLB_PIPE_MEM_SIZE_MASK		GENMASK(10, 0)
#define SOF_IPC4_GLB_PIPE_MEM_SIZE(x)		((x) << SOF_IPC4_GLB_PIPE_MEM_SIZE_SHIFT)

#define SOF_IPC4_GL_PIPE_EXT_LP_SHIFT		0
#define SOF_IPC4_GL_PIPE_EXT_LP_MASK		BIT(0)
#define SOF_IPC4_GL_PIPE_EXT_LP(x)		((x) << SOF_IPC4_GL_PIPE_EXT_LP_SHIFT)

/* pipeline set state ipc msg */
#define SOF_IPC4_GL_PIPE_STATE_TYPE_SHIFT	24
#define SOF_IPC4_GL_PIPE_STATE_TYPE_MASK	GENMASK(28, 24)
#define SOF_IPC4_GL_PIPE_STATE_TYPE(x)		((x) << SOF_IPC4_GL_PIPE_STATE_TYPE_SHIFT)

#define SOF_IPC4_GL_PIPE_STATE_ID_SHIFT		16
#define SOF_IPC4_GL_PIPE_STATE_ID_MASK		GENMASK(23, 16)
#define SOF_IPC4_GL_PIPE_STATE_ID(x)		((x) << SOF_IPC4_GL_PIPE_STATE_ID_SHIFT)

#define SOF_IPC4_GL_PIPE_STATE_SHIFT		0
#define SOF_IPC4_GL_PIPE_STATE_MASK		GENMASK(15, 0)
#define SOF_IPC4_GL_PIPE_STATE(x)		((x) << SOF_IPC4_GL_PIPE_STATE_SHIFT)

enum sof_ipc4_sampling_frequency {
	SOF_IPC4_FS_8000HZ	= 8000,
	SOF_IPC4_FS_11025HZ	= 11025,
	SOF_IPC4_FS_16000HZ	= 16000,
	SOF_IPC4_FS_22050HZ	= 22050,
	SOF_IPC4_FS_32000HZ	= 32000,
	SOF_IPC4_FS_44100HZ	= 44100,
	SOF_IPC4_FS_48000HZ	= 48000, /* Default. */
};

enum sof_ipc4_bit_depth {
	SOF_IPC4_DEPTH_8BIT	= 8, /* 8 bits depth */
	SOF_IPC4_DEPTH_16BIT	= 16, /* 16 bits depth */
	SOF_IPC4_DEPTH_24BIT	= 24, /* 24 bits depth - Default */
	SOF_IPC4_DEPTH_32BIT	= 32, /* 32 bits depth */
	SOF_IPC4_DEPTH_64BIT	= 64, /* 64 bits depth */
};

enum sof_ipc4_channel_config {
	/* one channel only. */
	SOF_IPC4_CHANNEL_CONFIG_MONO			= 0,
	/* L & R. */
	SOF_IPC4_CHANNEL_CONFIG_STEREO			= 1,
	/* L, R & LFE; PCM only. */
	SOF_IPC4_CHANNEL_CONFIG_2_POINT_1		= 2,
	/* L, C & R; MP3 & AAC only. */
	SOF_IPC4_CHANNEL_CONFIG_3_POINT_0		= 3,
	/* L, C, R & LFE; PCM only. */
	SOF_IPC4_CHANNEL_CONFIG_3_POINT_1		= 4,
	/* L, R, Ls & Rs; PCM only. */
	SOF_IPC4_CHANNEL_CONFIG_QUATRO			= 5,
	/* L, C, R & Cs; MP3 & AAC only. */
	SOF_IPC4_CHANNEL_CONFIG_4_POINT_0		= 6,
	/* L, C, R, Ls & Rs. */
	SOF_IPC4_CHANNEL_CONFIG_5_POINT_0		= 7,
	/* L, C, R, Ls, Rs & LFE. */
	SOF_IPC4_CHANNEL_CONFIG_5_POINT_1		= 8,
	/* one channel replicated in two. */
	SOF_IPC4_CHANNEL_CONFIG_DUAL_MONO		= 9,
	/* Stereo (L,R) in 4 slots, 1st stream: [ L, R, -, - ] */
	SOF_IPC4_CHANNEL_CONFIG_I2S_DUAL_STEREO_0	= 10,
	/* Stereo (L,R) in 4 slots, 2nd stream: [ -, -, L, R ] */
	SOF_IPC4_CHANNEL_CONFIG_I2S_DUAL_STEREO_1	= 11,
	/* L, C, R, Ls, Rs & LFE., LS, RS */
	SOF_IPC4_CHANNEL_CONFIG_7_POINT_1		= 12,
};

enum sof_ipc4_interleaved_style {
	SOF_IPC4_CHANNELS_INTERLEAVED = 0,
	SOF_IPC4_CHANNELS_NONINTERLEAVED = 1,
};

enum sof_ipc4_sample_type {
	SOF_IPC4_MSB_INTEGER = 0, /* integer with Most Significant Byte first */
	SOF_IPC4_LSB_INTEGER = 1, /* integer with Least Significant Byte first */
	SOF_IPC4_SIGNED_INTEGER = 2, /* signed integer */
	SOF_IPC4_UNSIGNED_INTEGER = 3, /* unsigned integer */
	SOF_IPC4_FLOAT = 4, /* unsigned integer */
};

struct sof_ipc4_audio_format {
	enum sof_ipc4_sampling_frequency sampling_frequency;
	enum sof_ipc4_bit_depth bit_depth;
	uint32_t ch_map;
	enum sof_ipc4_channel_config ch_cfg;
	uint32_t interleaving_style;
	uint8_t channels_count;
	uint8_t valid_bit_depth;
	uint8_t s_type; /* sof_ipc4_sample_type */
	uint8_t reserved;
};

struct sof_ipc4_basic_module_cfg {
	uint32_t cpc; /* the max count of Cycles Per Chunk processing */
	uint32_t ibs; /* input Buffer Size (in bytes)  */
	uint32_t obs; /* output Buffer Size (in bytes) */
	uint32_t is_pages; /* number of physical pages used */
	struct sof_ipc4_audio_format audio_fmt;
};

/* common module ipc msg */
#define SOF_IPC4_MOD_INSTANCE_SHIFT		16
#define SOF_IPC4_MOD_INSTANCE_MASK		GENMASK(23, 16)
#define SOF_IPC4_MOD_INSTANCE(x)		((x) << SOF_IPC4_MOD_INSTANCE_SHIFT)

#define SOF_IPC4_MOD_ID_SHIFT			0
#define SOF_IPC4_MOD_ID_MASK			GENMASK(15, 0)
#define SOF_IPC4_MOD_ID(x)			((x) << SOF_IPC4_MOD_ID_SHIFT)

/* init module ipc msg */
#define SOF_IPC4_MOD_EXT_PARAM_SIZE_SHIFT	0
#define SOF_IPC4_MOD_EXT_PARAM_SIZE_MASK	GENMASK(15, 0)
#define SOF_IPC4_MOD_EXT_PARAM_SIZE(x)		((x) << SOF_IPC4_MOD_EXT_PARAM_SIZE_SHIFT)

#define SOF_IPC4_MOD_EXT_PPL_ID_SHIFT		16
#define SOF_IPC4_MOD_EXT_PPL_ID_MASK		GENMASK(23, 16)
#define SOF_IPC4_MOD_EXT_PPL_ID(x)		((x) << SOF_IPC4_MOD_EXT_PPL_ID_SHIFT)

#define SOF_IPC4_MOD_EXT_CORE_ID_SHIFT		24
#define SOF_IPC4_MOD_EXT_CORE_ID_MASK		GENMASK(27, 24)
#define SOF_IPC4_MOD_EXT_CORE_ID(x)		((x) << SOF_IPC4_MOD_EXT_CORE_ID_SHIFT)

#define SOF_IPC4_MOD_EXT_DOMAIN_SHIFT		28
#define SOF_IPC4_MOD_EXT_DOMAIN_MASK		BIT(28)
#define SOF_IPC4_MOD_EXT_DOMAIN(x)		((x) << SOF_IPC4_MOD_EXT_DOMAIN_SHIFT)

/*  bind/unbind module ipc msg */
#define SOF_IPC4_MOD_EXT_DST_MOD_ID_SHIFT	0
#define SOF_IPC4_MOD_EXT_DST_MOD_ID_MASK	GENMASK(15, 0)
#define SOF_IPC4_MOD_EXT_DST_MOD_ID(x)		((x) << SOF_IPC4_MOD_EXT_DST_MOD_ID_SHIFT)

#define SOF_IPC4_MOD_EXT_DST_MOD_INSTANCE_SHIFT	16
#define SOF_IPC4_MOD_EXT_DST_MOD_INSTANCE_MASK	GENMASK(23, 16)
#define SOF_IPC4_MOD_EXT_DST_MOD_INSTANCE(x)	((x) << SOF_IPC4_MOD_EXT_DST_MOD_INSTANCE_SHIFT)

#define SOF_IPC4_MOD_EXT_DST_MOD_QUEUE_ID_SHIFT	24
#define SOF_IPC4_MOD_EXT_DST_MOD_QUEUE_ID_MASK	GENMASK(26, 24)
#define SOF_IPC4_MOD_EXT_DST_MOD_QUEUE_ID(x)	((x) << SOF_IPC4_MOD_EXT_DST_MOD_QUEUE_ID_SHIFT)

#define SOF_IPC4_MOD_EXT_SRC_MOD_QUEUE_ID_SHIFT	27
#define SOF_IPC4_MOD_EXT_SRC_MOD_QUEUE_ID_MASK	GENMASK(29, 27)
#define SOF_IPC4_MOD_EXT_SRC_MOD_QUEUE_ID(x)	((x) << SOF_IPC4_MOD_EXT_SRC_MOD_QUEUE_ID_SHIFT)

#define MOD_ENABLE_LOG	6
#define MOD_SYSTEM_TIME	20

/* set module large config */
#define SOF_IPC4_MOD_EXT_MSG_SIZE_SHIFT		0
#define SOF_IPC4_MOD_EXT_MSG_SIZE_MASK		GENMASK(19, 0)
#define SOF_IPC4_MOD_EXT_MSG_SIZE(x)		((x) << SOF_IPC4_MOD_EXT_MSG_SIZE_SHIFT)

#define SOF_IPC4_MOD_EXT_MSG_PARAM_ID_SHIFT	20
#define SOF_IPC4_MOD_EXT_MSG_PARAM_ID_MASK	GENMASK(27, 20)
#define SOF_IPC4_MOD_EXT_MSG_PARAM_ID(x)	((x) << SOF_IPC4_MOD_EXT_MSG_PARAM_ID_SHIFT)

#define SOF_IPC4_MOD_EXT_MSG_LAST_BLOCK_SHIFT	28
#define SOF_IPC4_MOD_EXT_MSG_LAST_BLOCK_MASK	BIT(28)
#define SOF_IPC4_MOD_EXT_MSG_LAST_BLOCK(x)	((x) << SOF_IPC4_MOD_EXT_MSG_LAST_BLOCK_SHIFT)

#define SOF_IPC4_MOD_EXT_MSG_FIRST_BLOCK_SHIFT	29
#define SOF_IPC4_MOD_EXT_MSG_FIRST_BLOCK_MASK	BIT(29)
#define SOF_IPC4_MOD_EXT_MSG_FIRST_BLOCK(x)	((x) << SOF_IPC4_MOD_EXT_MSG_FIRST_BLOCK_SHIFT)

/* ipc4 notification msg */
#define SOF_IPC4_GLB_NOTIFY_TYPE_SHIFT		16
#define SOF_IPC4_GLB_NOTIFY_TYPE_MASK		0xFF
#define SOF_IPC4_GLB_NOTIFY_TYPE(x)		(((x) >> SOF_IPC4_GLB_NOTIFY_TYPE_SHIFT) \
						& SOF_IPC4_GLB_NOTIFY_TYPE_MASK)

#define SOF_IPC4_GLB_NOTIFY_MSG_TYPE_SHIFT	24
#define SOF_IPC4_GLB_NOTIFY_MSG_TYPE_MASK	0x1F
#define SOF_IPC4_GLB_NOTIFY_MSG_TYPE(x)		(((x) >> SOF_IPC4_GLB_NOTIFY_MSG_TYPE_SHIFT) \
						& SOF_IPC4_GLB_NOTIFY_MSG_TYPE_MASK)

enum sof_ipc4_notification_type {
	/* Phrase detected (notification from WoV module) */
	SOF_IPC4_GLB_NOTIFY_PHRASE_DETECTED = 4,
	/*Event from a resource (pipeline or module instance) */
	SOF_IPC4_GLB_NOTIFY_RESOURCE_EVENT = 5,
	/* Debug log buffer status changed */
	SOF_IPC4_GLB_NOTIFY_LOG_BUFFER_STATUS = 6,
	/* Timestamp captured at the link */
	SOF_IPC4_GLB_NOTIFY_TIMESTAMP_CAPTURED = 7,
	/* FW complete initialization */
	SOF_IPC4_GLB_NOTIFY_FW_READY = 8,
	/* Audio classifier result (ACA) */
	SOF_IPC4_GLB_NOTIFY_FW_AUD_CLASS_RESULT = 9,
	/* Exception caught by DSP FW */
	SOF_IPC4_GLB_NOTIFY_EXCEPTION_CAUGHT = 10,
	/* 11 is skipped by the existing cavs firmware */
	/* Custom module notification */
	SOF_IPC4_GLB_NOTIFY_MODULE_NOTIFICATION = 12,
	/* Probe notify data available */
	SOF_IPC4_GLB_NOTIFY_PROBE_DATA_AVAILABLE = 14,
	/* AM module notifications */
	SOF_IPC4_GLB_NOTIFY_ASYNC_MSG_SRVC_MESSAGE = 15,
};

#define SOF_IPC4_GLB_NOTIFY_DIR_MASK		BIT(29)
#define SOF_IPC4_REPLY_STATUS_MASK		GENMASK(23, 0)

/** @}*/

#endif
