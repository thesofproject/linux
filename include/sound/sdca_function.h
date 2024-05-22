/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause) */
/*
 * The MIPI SDCA specification is available for public downloads at
 * https://www.mipi.org/mipi-sdca-v1-0-download
 *
 * Copyright(c) 2024 Intel Corporation
 */

#ifndef __SDCA_FUNCTION_H__
#define __SDCA_FUNCTION_H__

/*
 * SDCA Function Types from SDCA specification v1.0a Section 5.1.2
 * all Function types not described are reserved
 * Note that SIMPLE_AMP, SIMPLE_MIC and SIMPLE_JACK Function Types
 * are NOT defined in SDCA 1.0a, but they were defined in earlier
 * drafts and are planned for 1.1.
 */

enum sdca_function_type {
	SDCA_FUNCTION_TYPE_SMART_AMP	= 0x01,	/* Amplifier with protection features */
	SDCA_FUNCTION_TYPE_SIMPLE_AMP	= 0x02,	/* subset of SmartAmp */
	SDCA_FUNCTION_TYPE_SMART_MIC	= 0x03,	/* Smart microphone with acoustic triggers */
	SDCA_FUNCTION_TYPE_SIMPLE_MIC	= 0x04,	/* subset of SmartMic */
	SDCA_FUNCTION_TYPE_SPEAKER_MIC	= 0x05,	/* Combination of SmartMic and SmartAmp */
	SDCA_FUNCTION_TYPE_UAJ		= 0x06,	/* 3.5mm Universal Audio jack */
	SDCA_FUNCTION_TYPE_RJ		= 0x07,	/* Retaskable jack */
	SDCA_FUNCTION_TYPE_SIMPLE_JACK	= 0x08,	/* Subset of UAJ */
	SDCA_FUNCTION_TYPE_HID		= 0x0A,	/* Human Interface Device, for e.g. buttons */
	SDCA_FUNCTION_TYPE_IMP_DEF	= 0x1F,	/* Implementation-defined function */
};

/* Human-readable names used for kernel logs and Function device registration/bind */
#define	SDCA_FUNCTION_TYPE_SMART_AMP_NAME	"SmartAmp"
#define	SDCA_FUNCTION_TYPE_SIMPLE_AMP_NAME	"SimpleAmp"
#define	SDCA_FUNCTION_TYPE_SMART_MIC_NAME	"SmartMic"
#define	SDCA_FUNCTION_TYPE_SIMPLE_MIC_NAME	"SimpleMic"
#define	SDCA_FUNCTION_TYPE_SPEAKER_MIC_NAME	"SpeakerMic"
#define	SDCA_FUNCTION_TYPE_UAJ_NAME		"UAJ"
#define	SDCA_FUNCTION_TYPE_RJ_NAME		"RJ"
#define	SDCA_FUNCTION_TYPE_SIMPLE_NAME		"SimpleJack"
#define	SDCA_FUNCTION_TYPE_HID_NAME		"HID"

enum sdca_entity0_controls {
	SDCA_CONTROL_ENTITY_0_COMMIT_GROUP_MASK		= 0x01,
	SDCA_CONTROL_ENTITY_0_INTSTAT_CLEAR		= 0x02,
	SDCA_CONTROL_ENTITY_0_INT_ENABLE		= 0x03,
	SDCA_CONTROL_ENTITY_0_FUNCTION_SDCA_VERSION	= 0x04,
	SDCA_CONTROL_ENTITY_0_FUNCTION_TOPOLOGY		= 0x05,
	SDCA_CONTROL_ENTITY_0_FUNCTION_MANUFACTURER_ID	= 0x06,
	SDCA_CONTROL_ENTITY_0_FUNCTION_ID		= 0x07,
	SDCA_CONTROL_ENTITY_0_FUNCTION_VERSION		= 0x08
};

/*
 * The addressing space for SDCA relies on 7 bits for Entities, so a
 * maximum of 128 entities per function can be represented
 */
#define SDCA_MAX_ENTITY_COUNT 128

/*
 * The DisCo spec uses a 64-bit mask to represent input pins for
 * an entity
 */
#define SDCA_MAX_ENTITY_PIN_COUNT 64

/*
 * SDCA Entity Types from SDCA specification v1.a Section 6.1.2
 * all Entity Types not described are reserved
 */

enum sdca_entity_type {
	SDCA_ENTITY_TYPE_IT = 0x02,	/* Input Terminal */
	SDCA_ENTITY_TYPE_OT,		/* Output Terminal */
	SDCA_ENTITY_TYPE_MU = 0x05,	/* Mixer Unit */
	SDCA_ENTITY_TYPE_SU,		/* Selector Unit */
	SDCA_ENTITY_TYPE_FU,		/* Feature Unit */
	SDCA_ENTITY_TYPE_XU = 0x0A,	/* Extension Unit */
	SDCA_ENTITY_TYPE_CS,		/* Clock Source */
	SDCA_ENTITY_TYPE_CX,		/* Clock selector */
	SDCA_ENTITY_TYPE_PDE = 0x11,	/* Power-Domain Entity */
	SDCA_ENTITY_TYPE_GE,		/* Group Entity */
	SDCA_ENTITY_TYPE_PCE,		/* Privacy Control Entity */
	SDCA_ENTITY_TYPE_CRU = 0x20,	/* Channel Remapping Unit */
	SDCA_ENTITY_TYPE_UDMPU,		/* UpDownMixerUnit */
	SDCA_ENTITY_TYPE_MFPU,		/* Multi-Function Processing Unit */
	SDCA_ENTITY_TYPE_SMPU,		/* Smart Mic Processing Unit */
	SDCA_ENTITY_TYPE_SAPU,		/* Smart Amp Processing Unit */
	SDCA_ENTITY_TYPE_TG = 0x30,	/* Tone Generator */
	SDCA_ENTITY_TYPE_HIDE		/* HID Entity */
};

/**
 * struct sdca_entity - collection of information for one SDCA entity
 * @id: identifier used for addressing
 * @label: string such as "OT 12"
 * @entity_type: identifier for that entity
 * @sink_count: number of sinks for an Entity
 * @sinks: array containing the @id of each sink
 * @source_count: number of sources for an Entity
 * @sources: array containing the @id of each source
 */
struct sdca_entity {
	int id;
	char *label;
	enum sdca_entity_type entity_type;
	int sink_count;
	int sinks[SDCA_MAX_ENTITY_PIN_COUNT];
	int source_count;
	int sources[SDCA_MAX_ENTITY_PIN_COUNT];
};

/**
 * struct sdca_function_data - top-level information for one SDCA function
 * @function_desc: pointer to short descriptor used in initial parsing
 * @topology_features: mask of optional features in topology
 * @num_entities: number of entities reported in this function. This is a factor
 * of multiple options allowed in the SDCA specification
 * @entities: dynamically allocated array of entities.
 * @function_busy_max_delay_us: indicates if hardware can assert the Function_Busy
 * bit, which requires special-casing of the 'Command Ignored' response. If zero,
 * then the Host shall assume this bit is never asserted.
 * @initialization_table: set of 4-byte address/byte value to set-up the Function
 * during boot and resume if context is lost
 * @initialization_table_size: size of @initialization_table
 */
struct sdca_function_data {
	struct sdca_function_desc *function_desc;
	u64 topology_features;
	int num_entities;
	struct sdca_entity *entities;
	u32 function_busy_max_delay_us;
	u8 *initialization_table;
	int initialization_table_size;
};

int sdca_parse_function(struct device *dev,
			struct fwnode_handle *function_node,
			struct sdca_function_data *function);
#endif
