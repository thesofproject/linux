/* SPDX-License-Identifier: GPL-2.0 */
/*
 * rt1316.h -- SoundWire RT1316 header
 *
 * Copyright(c) 2020 Intel Corporation
 */

#ifndef __SDW_RT1316_H__
#define __SDW_RT1316_H__

struct rt1316_priv {
	struct sdw_slave *sdw_slave;
	enum sdw_slave_status status;
	bool hw_init;
	bool first_hw_init;
};

struct sdw_stream_data {
	struct sdw_stream_runtime *sdw_stream;
};

#endif /* __SDW_RT1316_H__ */
