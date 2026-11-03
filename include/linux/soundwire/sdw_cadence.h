/* SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause) */
/* Copyright(c) 2024-2026 Intel Corporation */

#ifndef ___SDW_CADENCE_H
#define ___SDW_CADENCE_H

#include <linux/soundwire/sdw.h>

/**
 * struct sdw_cdns_pdi: PDI (Physical Data Interface) instance
 *
 * @num: pdi number
 * @intel_alh_id: link identifier
 * @l_ch_num: low channel for PDI
 * @h_ch_num: high channel for PDI
 * @ch_count: total channel count for PDI
 * @dir: data direction
 * @type: stream type, (only PCM supported)
 * @assigned: whether PDI is in use
 */
struct sdw_cdns_pdi {
	int num;
	int intel_alh_id;
	int l_ch_num;
	int h_ch_num;
	int ch_count;
	enum sdw_data_direction dir;
	enum sdw_stream_type type;
	bool assigned;
};

struct sdw_cdns_pdi *sdw_cdns_assign_pdi(struct sdw_bus *bus, u32 dir);
void sdw_cdns_release_pdi(struct sdw_bus *bus, struct sdw_cdns_pdi *pdi);

#endif
