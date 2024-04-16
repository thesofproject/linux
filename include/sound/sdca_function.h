/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause) */
/*
 * The MIPI SDCA specification is available for public downloads at
 * https://www.mipi.org/mipi-sdca-v1-0-download
 *
 * Copyright(c) 2024 Intel Corporation
 */

#ifndef __SDCA_FUNCTION_H__
#define __SDCA_FUNCTION_H__

#define SDCA_MAX_FUNCTION_COUNT 8

/*
 * SDCA Function Types from SDCA specification v1.0a Section 5.1.2
 * all Function types not described are reserved
 */

enum sdca_function_type {
	SDCA_FUNCTION_TYPE_SMART_AMP	= 0x01,	/* Amplifier with protection features */
	SDCA_FUNCTION_TYPE_SMART_MIC	= 0x03,	/* Smart microphone with acoustic triggers */
	SDCA_FUNCTION_TYPE_SPEAKER_MIC	= 0x05,	/* Combination of SmartMic and SmartAmp */
	SDCA_FUNCTION_TYPE_UAJ		= 0x06,	/* 3.5mm Universal Audio jack */
	SDCA_FUNCTION_TYPE_RJ		= 0x07,	/* Retaskable jack */
	SDCA_FUNCTION_TYPE_HID		= 0x0A,	/* Human Interface Device, for e.g. buttons */
	SDCA_FUNCTION_TYPE_IMP_DEF	= 0x1F,	/* Implementation-defined function */
};

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

/**
 * struct sdca_function - top-level information for one SDCA function
 * @adr: ACPI _ADR field reported in platform firmware
 * @topology_type: identifier for that function
 */
struct sdca_function {
	u8 adr; /* limited range since only 8 functions can be supported */
	enum sdca_function_type topology_type;
};

/**
 * struct sdca_data - top-level information for all SDCA functions
 * enabled in a SoundWire peripheral.
 *
 * @parent: device for SoundWire peripheral. In the SDCA/DisCo specs,
 * the functions are represented as devices in the scope of the peripheral.
 * @slave: pointer to peripheral
 * @function_count: number of SDCA functions in peripheral
 * @functions: array of SDCA functions
 */
struct sdca_data {
	struct device *parent;
	struct sdw_slave *slave;
	int function_count;
	struct sdca_function functions[SDCA_MAX_FUNCTION_COUNT];
};

#endif
