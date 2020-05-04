/* SPDX-License-Identifier: GPL-2.0 */
/*
 * sdca.h -- SoundWire Device Class header
 *
 * Copyright(c) 2020 Intel Corporation
 */

#ifndef __SDW_SDCA_H__
#define __SDW_SDCA_H__

struct sdca_priv {
	struct sdw_slave *sdw_slave;
	enum sdw_slave_status status;
	bool hw_init;
	bool first_hw_init;
};

struct sdw_stream_data {
	struct sdw_stream_runtime *sdw_stream;
};

#endif /* __SDW_SDCA_H__ */
