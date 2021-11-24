/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause) */
/*
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * Copyright(c) 2018 Intel Corporation. All rights reserved.
 */

#ifndef __INCLUDE_UAPI_SOUND_SOF_ABI_H__
#define __INCLUDE_UAPI_SOUND_SOF_ABI_H__

/* SOF ABI version number. Format within 32bit word is MMmmmppp */
#define SOF_ABI_MAJOR_SHIFT	24
#define SOF_ABI_MAJOR_MASK	0xff
#define SOF_ABI_MINOR_SHIFT	12
#define SOF_ABI_MINOR_MASK	0xfff
#define SOF_ABI_PATCH_SHIFT	0
#define SOF_ABI_PATCH_MASK	0xfff

#define SOF_ABI_VER(major, minor, patch) \
	(((major) << SOF_ABI_MAJOR_SHIFT) | \
	((minor) << SOF_ABI_MINOR_SHIFT) | \
	((patch) << SOF_ABI_PATCH_SHIFT))

#define SOF_ABI_VERSION_MAJOR(version) \
	(((version) >> SOF_ABI_MAJOR_SHIFT) & SOF_ABI_MAJOR_MASK)
#define SOF_ABI_VERSION_MINOR(version)	\
	(((version) >> SOF_ABI_MINOR_SHIFT) & SOF_ABI_MINOR_MASK)
#define SOF_ABI_VERSION_PATCH(version)	\
	(((version) >> SOF_ABI_PATCH_SHIFT) & SOF_ABI_PATCH_MASK)

#define SOF_ABI_VERSION_INCOMPATIBLE(sof_ver, client_ver)		\
	(SOF_ABI_VERSION_MAJOR((sof_ver)) !=				\
		SOF_ABI_VERSION_MAJOR((client_ver))			\
	)

/* SOF ABI magic number "SOF\0". */
#define SOF_ABI_MAGIC		0x00464F53

#endif
