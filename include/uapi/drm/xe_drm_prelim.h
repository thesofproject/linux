/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 */

#ifndef _UAPI_XE_DRM_PRELIM_H_
#define _UAPI_XE_DRM_PRELIM_H_

#include "xe_drm.h"

/**
 * DOC: Xe PRELIM uAPI
 *
 * Reasoning:
 *
 * The DRM community imposes some strict requirements on the uAPI:
 *
 * - https://www.kernel.org/doc/html/latest/gpu/drm-uapi.html#open-source-userspace-requirements
 * - "The open-source userspace must not be a toy/test application, but the real thing."
 * - "The userspace patches must be against the canonical upstream, not some vendor fork."
 *
 * In some very specific cases, there will be a particular need to get a preliminary and
 * non-upstream uAPI merged in some of our internal branches. Either xe-internal, or
 * an RTL/PO branch, or a topic development branch. When this happens, the uAPI cannot
 * take a risk of conflicting IOCTL ranges with other preliminary uAPI or with a possible
 * real user space uAPI that could win the race towards upstream.
 *
 * 'PRELIM' uAPIs are APIs that are not yet merged on upstream. They were designed to be
 * in a different range in a way that the divergence with the upstream and other development
 * branches can be controlled and the conflicts minimized.
 *
 * It is also a mechanism that prevents user space regressions since the prelim modification
 * is always in a two-phase approach, where upstream and prelim can coexist for a period of
 * time while the UMDs adjust to the changes.
 *
 * Rules:
 *
 * - Communication will happen on a specific Teams channel: KMD uAPI Changes
 * - APIs to be considered Preliminary / WIP / Temporary to what will eventually be in upstream.
 *   It's designed to allow kernel and userspace to work together and make sure it works with all
 *   components before committing to support it forever in upstream (we don't want to break Linux's
 *   rule about not breaking userspace).
 * - IOCTL in separate file (this one).
 * - IOCTL, flags, enums, or any number that might conflict should be in separated range
 *   (i.e end of range like IOCTL numbers).
 * - PRELIM prefix mandatory in any define, struct, or non-static functions in PRELIM source files
 *   called by other source files (i.e xe_perf_ioctl -> prelim_xe_perf_ioctl)
 * - Code .c/.h using PRELIM should also be placed in prelim/ sub-directory for easier rebase.
 * - Two-Phase removal:
 *
 *   + When API needs to be modified in non-backwards compatible ways, the current PRELIM API
 *     cannot be removed (yet).
 *   + Either because it must change the behavior or because the final upstream one has landed.
 *   + If a new PRELIM is needed, it needs to be added as a new PRELIM_V<n+1> without removing the
 *     PRELIM_V<n>.
 *   + The previous one can only be removed after all user space components confirmed they are not
 *     using.
 *
 * Other prelim considerations:
 *
 * - Out of tree Sysfs and debugfs need to stay behind a 'prelim' directory.
 * - Out of tree module-parameters need to be identified by a PRELIM prefix.
 *   (xe.prelim_my_awesome_param=not_default)
 * - Internal/downstream declarations must be added here, not to xe_drm.h.
 * - The values in xe_drm_prelim.h must also be kept synchronized with values in xe_drm.h.
 * - PRELIM ioctl's: IOCTL numbers go down from 0x5f
 * - PRELIM reservation: xe-internal is the source of truth of the PRELIM stuff.
 *   Any developer working on a topic branch or RTL centric branch needs to first reserve the
 *   prelim range in this file at xe-internal branch.
 */

/*
 * IOCTL numbers listed below are reserved, they are taken up by other
 * components. Please add an unreserved ioctl number here to reserve that
 * number.
 */
#define PRELIM_DRM_XE_PERF			0x5f
#define PRELIM_DRM_XE_VM_EXPORT			0x5e
#define PRELIM_DRM_XE_EXT_MSIX			0x5d
#define PRELIM_DRM_XE_DEBUG_METADATA_DESTROY	0x5c
#define PRELIM_DRM_XE_DEBUG_METADATA_CREATE	0x5b
#define PRELIM_DRM_XE_EXEC_QUEUE_SET_PROPERTY	0x5a
#define PRELIM_DRM_XE_EUDEBUG_CONNECT		0x59

#define PRELIM_DRM_XE_MAMQ_HANDLE_TO_FD		0x55
#define PRELIM_DRM_XE_MAMQ_FD_TO_HANDLE		0x54

/* NOTE: PXP_OPS PRELIM ioctl code 0x52 maintains compatibility with older downstream products */
#define PRELIM_DRM_XE_PXP_OPS			0x52

#define PRELIM_DRM_IOCTL_XE_PERF	DRM_IOW(DRM_COMMAND_BASE + PRELIM_DRM_XE_PERF, struct prelim_drm_xe_perf_param)
#define PRELIM_DRM_IOCTL_XE_VM_EXPORT	DRM_IOWR(DRM_COMMAND_BASE + PRELIM_DRM_XE_VM_EXPORT, \
						 struct prelim_drm_xe_vm_export_param)
#define PRELIM_DRM_IOCTL_XE_EXT_MSIX \
		DRM_IOWR(DRM_COMMAND_BASE + PRELIM_DRM_XE_EXT_MSIX, struct prelim_drm_xe_ext_msix)

#define PRELIM_DRM_IOCTL_XE_EXEC_QUEUE_SET_PROPERTY					\
		DRM_IOW(DRM_COMMAND_BASE + PRELIM_DRM_XE_EXEC_QUEUE_SET_PROPERTY,	\
			struct prelim_drm_xe_exec_queue_set_property)

#define PRELIM_DRM_IOCTL_XE_EUDEBUG_CONNECT \
		DRM_IOWR(DRM_COMMAND_BASE + PRELIM_DRM_XE_EUDEBUG_CONNECT, struct prelim_drm_xe_eudebug_connect)
#define PRELIM_DRM_IOCTL_XE_DEBUG_METADATA_CREATE \
		DRM_IOWR(DRM_COMMAND_BASE + PRELIM_DRM_XE_DEBUG_METADATA_CREATE, struct prelim_drm_xe_debug_metadata_create)
#define PRELIM_DRM_IOCTL_XE_DEBUG_METADATA_DESTROY \
		DRM_IOW(DRM_COMMAND_BASE + PRELIM_DRM_XE_DEBUG_METADATA_DESTROY, struct prelim_drm_xe_debug_metadata_destroy)

#define PRELIM_DRM_IOCTL_XE_MAMQ_HANDLE_TO_FD \
		DRM_IOWR(DRM_COMMAND_BASE + PRELIM_DRM_XE_MAMQ_HANDLE_TO_FD, struct prelim_drm_xe_mamq_handle)
#define PRELIM_DRM_IOCTL_XE_MAMQ_FD_TO_HANDLE \
		DRM_IOWR(DRM_COMMAND_BASE + PRELIM_DRM_XE_MAMQ_FD_TO_HANDLE, struct prelim_drm_xe_mamq_handle)

#define PRELIM_DRM_IOCTL_XE_PXP_OPS	\
	DRM_IOWR(DRM_COMMAND_BASE + PRELIM_DRM_XE_PXP_OPS, struct prelim_drm_xe_pxp_ops)

/**
 * struct prelim_drm_xe_vm_export_param - Input/Output of &PRELIM_DRM_XE_VM_EXPORT
 */
struct prelim_drm_xe_vm_export_param {
	/** @vm_id: VM ID allocated during DRM_IOCTL_XE_VM_CREATE */
	__u32 vm_id;

	/** @pad: MBZ */
	__u32 pad;

	/** @fd: Returned FD */
	__s32 fd;

	/** @pad2: MBZ */
	__u32 pad2;

	/** @reserved: Reserved */
	__u64 reserved[2];
};

struct prelim_drm_xe_device_query {
#define PRELIM_DRM_XE_QUERY			(1 << 16)
#define PRELIM_DRM_XE_QUERY_MASK		0xffff

#define PRELIM_DRM_XE_DEVICE_QUERY_OA_UNITS		(PRELIM_DRM_XE_QUERY | 7)
#define PRELIM_DRM_XE_DEVICE_QUERY_HWTRACE_TIMESTAMP	(PRELIM_DRM_XE_QUERY | 8)
#define PRELIM_DRM_XE_DEVICE_QUERY_SCALE		(PRELIM_DRM_XE_QUERY | 9)
/* For the layout of the GPU capability buffer, please refer to the documentation
 * in the `xe_gpu_caps.c` file.
 */
#define PRELIM_DRM_XE_DEVICE_QUERY_GPU_CAP_LIST	(PRELIM_DRM_XE_QUERY | 10)
};

#define PRELIM_DRM_XE_ENGINE_CLASS_HL_DECODE		6
#define PRELIM_DRM_XE_ENGINE_CLASS_HL_ENCODE		7
#define PRELIM_DRM_XE_QUERY_GT_TYPE_HL_MEDIA		2

/* Internal DRM_XE_EXEC_QUEUE_EXTENSION_SET_PROPERTY properties */
#define   PRELIM_DRM_XE_EXEC_QUEUE_SET_PROPERTY_MULTI_GROUP		0x20
/* Create a new multi queue group with this queue as primary queue */
#define     PRELIM_DRM_XE_MULTI_GROUP_CREATE				(1ull << 63)
/* Keep the multi-queue group active after the primary queue is destroyed */
#define     PRELIM_DRM_XE_MULTI_GROUP_KEEP_ACTIVE			(1ull << 62)
/* The group is Multi Address Multi Queue (MAMQ) */
#define     PRELIM_DRM_XE_MULTI_GROUP_MAMQ				(1ull << 61)
#define   PRELIM_DRM_XE_EXEC_QUEUE_SET_PROPERTY_MULTI_QUEUE_PRIORITY	0x21
#define   PRELIM_DRM_XE_EXEC_QUEUE_SET_PROPERTY_SUPER_GROUP             0x22
/* Create a new super group with this queue as root queue */
#define     PRELIM_DRM_XE_SUPER_GROUP_CREATE				(1ull << 63)

/*
 * struct prelim_drm_xe_exec_queue_set_property - exec queue set property
 */
struct prelim_drm_xe_exec_queue_set_property {
	/** @extensions: Pointer to the first extension struct, if any */
	__u64 extensions;

	/** @exec_queue_id: Exec queue ID */
	__u32 exec_queue_id;

	/** @property: property to set */
	__u32 property;

	/** @value: property value */
	__u64 value;

	/** @reserved: Reserved */
	__u64 reserved[2];
};

/**
 * struct prelim_drm_xe_mamq_handle - Multi-queue group export/import handle
 */
struct prelim_drm_xe_mamq_handle {
	/** @exec_queue_id: Exec queue ID */
	__u32 exec_queue_id;

	/** @fd: Returned multi-queue group FD */
	__s32 fd;

	/** @reserved: Reserved */
	__u64 reserved[2];
};

/** enum prelim_drm_xe_perf_type - Perf stream types */
enum prelim_drm_xe_perf_type {
	PRELIM_DRM_XE_PERF_TYPE_OA = 128,
	PRELIM_DRM_XE_PERF_TYPE_HWTRACE,
	PRELIM_DRM_XE_PERF_TYPE_MAX,
};

/**
 * enum prelim_drm_xe_perf_op - Perf stream ops
 */
enum prelim_drm_xe_perf_op {
	/** @PRELIM_DRM_XE_PERF_OP_STREAM_OPEN: Open a perf counter stream */
	PRELIM_DRM_XE_PERF_OP_STREAM_OPEN = 128,

	/** @PRELIM_DRM_XE_PERF_OP_ADD_CONFIG: Add perf stream config */
	PRELIM_DRM_XE_PERF_OP_ADD_CONFIG,

	/** @PRELIM_DRM_XE_PERF_OP_REMOVE_CONFIG: Remove perf stream config */
	PRELIM_DRM_XE_PERF_OP_REMOVE_CONFIG,
};

/**
 * struct prelim_drm_xe_perf_param - Input of &PRELIM_DRM_XE_PERF
 *
 * The perf layer enables multiplexing perf counter streams of multiple
 * types. The actual params for a particular stream operation are supplied
 * via the @param pointer (use __copy_from_user to get these params).
 */
struct prelim_drm_xe_perf_param {
	/** @extensions: Pointer to the first extension struct, if any */
	__u64 extensions;
	/** @perf_type: Perf stream type, of enum @prelim_drm_xe_perf_type */
	__u64 perf_type;
	/** @perf_op: Perf op, of enum @prelim_drm_xe_perf_op */
	__u64 perf_op;
	/** @param: Pointer to actual stream params */
	__u64 param;
};

/**
 * enum prelim_drm_xe_perf_ioctls - Perf fd ioctl's
 *
 * Information exchanged between userspace and kernel for perf fd ioctl's
 * is stream type specific
 */
enum prelim_drm_xe_perf_ioctls {
	/** @PRELIM_DRM_XE_PERF_IOCTL_ENABLE: Enable data capture for a stream */
	PRELIM_DRM_XE_PERF_IOCTL_ENABLE = _IO('j', 0x0),

	/** @PRELIM_DRM_XE_PERF_IOCTL_DISABLE: Disable data capture for a stream */
	PRELIM_DRM_XE_PERF_IOCTL_DISABLE = _IO('j', 0x1),

	/** @PRELIM_DRM_XE_PERF_IOCTL_CONFIG: Change stream configuration */
	PRELIM_DRM_XE_PERF_IOCTL_CONFIG = _IO('j', 0x2),

	/** @PRELIM_DRM_XE_PERF_IOCTL_STATUS: Return stream status */
	PRELIM_DRM_XE_PERF_IOCTL_STATUS = _IO('j', 0x3),

	/** @PRELIM_DRM_XE_PERF_IOCTL_INFO: Return stream info */
	PRELIM_DRM_XE_PERF_IOCTL_INFO = _IO('j', 0x4),
};

/** enum prelim_drm_xe_oa_unit_type - OA unit types */
enum prelim_drm_xe_oa_unit_type {
	/**
	 * @PRELIM_DRM_XE_OA_UNIT_TYPE_OAG: OAG OA unit. OAR/OAC are considered
	 * sub-types of OAG. For OAR/OAC, use OAG.
	 */
	PRELIM_DRM_XE_OA_UNIT_TYPE_OAG = 128,

	/** @PRELIM_DRM_XE_OA_UNIT_TYPE_OAM: OAM OA unit */
	PRELIM_DRM_XE_OA_UNIT_TYPE_OAM,
};

/**
 * struct prelim_drm_xe_oa_unit - describe OA unit
 */
struct prelim_drm_xe_oa_unit {
	/** @extensions: Pointer to the first extension struct, if any */
	__u64 extensions;

	/** @oa_unit_id: OA unit ID */
	__u32 oa_unit_id;

	/** @oa_unit_type: OA unit type of @prelim_drm_xe_oa_unit_type */
	__u32 oa_unit_type;

	/** @capabilities: OA capabilities bit-mask */
	__u64 capabilities;
#define PRELIM_DRM_XE_OA_CAPS_BASE		(1 << 0)

	/** @oa_timestamp_freq: OA timestamp freq */
	__u64 oa_timestamp_freq;

	/** @reserved: MBZ */
	__u64 reserved[4];

	/** @num_engines: number of engines in @eci array */
	__u64 num_engines;

	/** @eci: engines attached to this OA unit */
	struct drm_xe_engine_class_instance eci[];
};

/**
 * struct prelim_drm_xe_query_oa_units - describe OA units
 *
 * If a query is made with a struct drm_xe_device_query where .query
 * is equal to PRELIM_DRM_XE_DEVICE_QUERY_OA_UNITS, then the reply uses struct
 * prelim_drm_xe_query_oa_units in .data.
 *
 * OA unit properties for all OA units can be accessed using a code block
 * such as the one below:
 *
 * .. code-block:: C
 *
 *	struct prelim_drm_xe_query_oa_units *qoa;
 *	struct prelim_drm_xe_oa_unit *oau;
 *	u8 *poau;
 *
 *	// malloc qoa and issue DRM_XE_DEVICE_QUERY_OA_UNITS. Then:
 *	poau = (u8 *)&qoa->oa_units[0];
 *	for (int i = 0; i < qoa->num_oa_units; i++) {
 *		oau = (struct prelim_drm_xe_oa_unit *)poau;
 *		// Access 'struct prelim_drm_xe_oa_unit' fields here
 *		poau += sizeof(*oau) + oau->num_engines * sizeof(oau->eci[0]);
 *	}
 */
struct prelim_drm_xe_query_oa_units {
	/** @extensions: Pointer to the first extension struct, if any */
	__u64 extensions;
	/** @num_oa_units: number of OA units returned in oau[] */
	__u32 num_oa_units;
	/** @pad: MBZ */
	__u32 pad;
	/**
	 * @oa_units: struct @prelim_drm_xe_oa_unit array returned for this device.
	 * Written below as a u64 array to avoid problems with nested flexible
	 * arrays with some compilers
	 */
	__u64 oa_units[];
};

/** enum prelim_drm_xe_oa_format_type - OA format types */
enum prelim_drm_xe_oa_format_type {
	PRELIM_DRM_XE_OA_FMT_TYPE_OAG = 128,
	PRELIM_DRM_XE_OA_FMT_TYPE_OAR,
	PRELIM_DRM_XE_OA_FMT_TYPE_OAM,
	PRELIM_DRM_XE_OA_FMT_TYPE_OAC,
	PRELIM_DRM_XE_OA_FMT_TYPE_OAM_MPEC,
	PRELIM_DRM_XE_OA_FMT_TYPE_PEC,
};

/**
 * enum prelim_drm_xe_oa_property_id - OA stream property id's
 *
 * Stream params are specified as a chain of @drm_xe_ext_set_property
 * struct's, with @property values from enum @drm_xe_oa_property_id and
 * @xe_user_extension base.name set to @DRM_XE_OA_EXTENSION_SET_PROPERTY.
 * @param field in struct @drm_xe_perf_param points to the first
 * @drm_xe_ext_set_property struct.
 *
 * Exactly the same mechanism is also used for stream reconfiguration using
 * the @DRM_XE_PERF_IOCTL_CONFIG perf fd ioctl, though only a subset of
 * properties below can be specified for stream reconfiguration.
 */
enum prelim_drm_xe_oa_property_id {
#define PRELIM_DRM_XE_OA_EXTENSION_SET_PROPERTY	128
	/**
	 * @PRELIM_DRM_XE_OA_PROPERTY_OA_UNIT_ID: ID of the OA unit on which to open
	 * the OA stream, see @oa_unit_id in 'struct
	 * prelim_drm_xe_query_oa_units'. Defaults to 0 if not provided.
	 */
	PRELIM_DRM_XE_OA_PROPERTY_OA_UNIT_ID = 129,

	/**
	 * @PRELIM_DRM_XE_OA_PROPERTY_SAMPLE_OA: A value of 1 requests inclusion of raw
	 * OA unit reports or stream samples in a global buffer attached to an
	 * OA unit.
	 */
	PRELIM_DRM_XE_OA_PROPERTY_SAMPLE_OA,

	/**
	 * @PRELIM_DRM_XE_OA_PROPERTY_OA_METRIC_SET: OA metrics defining contents of OA
	 * reports, previously added via @PRELIM_DRM_XE_PERF_OP_ADD_CONFIG.
	 */
	PRELIM_DRM_XE_OA_PROPERTY_OA_METRIC_SET,

	/** @PRELIM_DRM_XE_OA_PROPERTY_OA_FORMAT: Perf counter report format */
	PRELIM_DRM_XE_OA_PROPERTY_OA_FORMAT,
	/*
	 * OA_FORMAT's are specified the same way as in PRM/Bspec 52198/60942,
	 * in terms of the following quantities: a. enum @drm_xe_oa_format_type
	 * b. Counter select c. Counter size and d. BC report. Also refer to the
	 * oa_formats array in drivers/gpu/drm/xe/xe_oa.c.
	 */
#define PRELIM_DRM_XE_OA_FORMAT_MASK_FMT_TYPE		(0xff << 0)
#define PRELIM_DRM_XE_OA_FORMAT_MASK_COUNTER_SEL	(0xff << 8)
#define PRELIM_DRM_XE_OA_FORMAT_MASK_COUNTER_SIZE	(0xff << 16)
#define PRELIM_DRM_XE_OA_FORMAT_MASK_BC_REPORT		(0xff << 24)

	/**
	 * @PRELIM_DRM_XE_OA_PROPERTY_OA_EXPONENT: Requests periodic OA unit sampling
	 * with sampling frequency proportional to 2^(period_exponent + 1)
	 */
	PRELIM_DRM_XE_OA_PROPERTY_OA_EXPONENT,

	/**
	 * @PRELIM_DRM_XE_OA_PROPERTY_OA_DISABLED: A value of 1 will open the OA
	 * stream in a DISABLED state (see @PRELIM_DRM_XE_PERF_IOCTL_ENABLE).
	 */
	PRELIM_DRM_XE_OA_PROPERTY_OA_DISABLED,

	/**
	 * @PRELIM_DRM_XE_OA_PROPERTY_EXEC_QUEUE_ID: Open the stream for a specific
	 * @exec_queue_id. Perf queries can be executed on this exec queue.
	 */
	PRELIM_DRM_XE_OA_PROPERTY_EXEC_QUEUE_ID,

	/**
	 * @PRELIM_DRM_XE_OA_PROPERTY_OA_ENGINE_INSTANCE: Optional engine instance to
	 * pass along with @PRELIM_DRM_XE_OA_PROPERTY_EXEC_QUEUE_ID or will default to 0.
	 */
	PRELIM_DRM_XE_OA_PROPERTY_OA_ENGINE_INSTANCE,

	/** @PRELIM_DRM_XE_OA_PROPERTY_MAX: non-ABI */
	PRELIM_DRM_XE_OA_PROPERTY_MAX
};

/**
 * struct prelim_drm_xe_oa_config - OA metric configuration
 *
 * Multiple OA configs can be added using @PRELIM_DRM_XE_PERF_OP_ADD_CONFIG. A
 * particular config can be specified when opening an OA stream using
 * @PRELIM_DRM_XE_OA_PROPERTY_OA_METRIC_SET property.
 */
struct prelim_drm_xe_oa_config {
	/** @extensions: Pointer to the first extension struct, if any */
	__u64 extensions;

	/** @uuid: String formatted like "%\08x-%\04x-%\04x-%\04x-%\012x" */
	char uuid[36];

	/** @n_regs: Number of regs in @regs_ptr */
	__u32 n_regs;

	/**
	 * @regs_ptr: Pointer to (register address, value) pairs for OA config
	 * registers. Expected length of buffer is: (2 * sizeof(u32) * @n_regs).
	 */
	__u64 regs_ptr;
};

/**
 * struct prelim_drm_xe_oa_stream_status - OA stream status returned from
 * @PRELIM_DRM_XE_PERF_IOCTL_STATUS perf fd ioctl
 */
struct prelim_drm_xe_oa_stream_status {
	/** @extensions: Pointer to the first extension struct, if any */
	__u64 extensions;

	/** @oa_status: OA status register as specified in PRM/Bspec 46717/61226 */
	__u64 oa_status;
#define PRELIM_DRM_XE_OASTATUS_MMIO_TRG_Q_FULL		(1 << 6)
#define PRELIM_DRM_XE_OASTATUS_COUNTER_OVERFLOW		(1 << 2)
#define PRELIM_DRM_XE_OASTATUS_BUFFER_OVERFLOW		(1 << 1)
#define PRELIM_DRM_XE_OASTATUS_REPORT_LOST		(1 << 0)

	/** @reserved: reserved for future use */
	__u64 reserved[3];
};

/**
 * struct prelim_drm_xe_oa_stream_info - OA stream info returned from
 * @PRELIM_DRM_XE_PERF_IOCTL_INFO perf fd ioctl
 */
struct prelim_drm_xe_oa_stream_info {
	/** @extensions: Pointer to the first extension struct, if any */
	__u64 extensions;

	/** @oa_buf_size: OA buffer size */
	__u64 oa_buf_size;

	/** @reserved: reserved for future use */
	__u64 reserved[3];
};

/*
 * Top bits of every counter are GT id.
 */
#define __PRELIM_XE_GENL_GT_SHIFT (56)

/**
 * DOC: uevent generated by xe on it's pci node.
 *
 * PRELIM_DRM_XE_RESET_REQUIRED_UEVENT - Event is generated when device needs reset.
 * The REASON is provided along with the event for which reset is required.
 * On the basis of REASONS, additional information might be supplied.
 */
#define PRELIM_DRM_XE_RESET_REQUIRED_UEVENT "DEVICE_STATUS=NEEDS_RESET"

/**
 * PRELIM_DRM_XE_RESET_REQUIRED_UEVENT_REASON_GT - Reason provided to PRELIM_DRM_XE_RESET_REQUIRED_UEVENT
 * incase of gt reset failure. The additional information supplied is tile id and
 * gt id of the gt unit for which reset has failed.
 */
#define PRELIM_DRM_XE_RESET_REQUIRED_UEVENT_REASON_GT "REASON=GT_RESET_FAILED"

/**
 * PRELIM_DRM_XE_RESET_REQUIRED_UEVENT_REASON_GSC - Reason provided to PRELIM_DRM_XE_RESET_REQUIRED_UEVENT
 * incase of GSC HW reporting Uncorrectable errors. The GSC errors are reported only
 * on TILE0, therefore no additional information is supplied for this reason.
 */
#define PRELIM_DRM_XE_RESET_REQUIRED_UEVENT_REASON_GSC "REASON=GSC_HW_ERROR"

/**
 * PRELIM_XE_FS1_UEVENT_THROTTLE_START - Event is generated whenever the throttling is triggered
 * by the device.
 */
#define PRELIM_XE_FS1_UEVENT_THROTTLE_START "THROTTLE_EVENT_START=1"

/**
 * PRELIM_XE_FS1_UEVENT_THROTTLE_END - Event is generated whenever the throttling ends.
 */
#define PRELIM_XE_FS1_UEVENT_THROTTLE_END "THROTTLE_EVENT_END=1"

/**
 * PRELIM_XE_FS1_THROTTLE_REASON - Throttling can occur due to  Power or Thermal crossing their
 * thresholds this reports the reason for throttling.
 */
#define PRELIM_XE_FS1_THROTTLE_REASON "REASON"

/**
 * DOC: XE GENL netlink event IDs
 * TODO: Add more details
 */
#define PRELIM_DRM_XE_HW_ERROR(gt, id) \
	((id) | ((__u64)(gt) << __PRELIM_XE_GENL_GT_SHIFT))

#define PRELIM_DRM_XE_GENL_GT_ERROR_CORRECTABLE_L3_SNG		(0)
#define PRELIM_DRM_XE_GENL_GT_ERROR_CORRECTABLE_GUC			(1)
#define PRELIM_DRM_XE_GENL_GT_ERROR_CORRECTABLE_SAMPLER		(2)
#define PRELIM_DRM_XE_GENL_GT_ERROR_CORRECTABLE_SLM			(3)
#define PRELIM_DRM_XE_GENL_GT_ERROR_CORRECTABLE_EU_IC		(4)
#define PRELIM_DRM_XE_GENL_GT_ERROR_CORRECTABLE_EU_GRF		(5)
#define PRELIM_DRM_XE_GENL_GT_ERROR_FATAL_ARR_BIST			(6)
#define PRELIM_DRM_XE_GENL_GT_ERROR_FATAL_L3_DOUB			(7)
#define PRELIM_DRM_XE_GENL_GT_ERROR_FATAL_L3_ECC_CHK		(8)
#define PRELIM_DRM_XE_GENL_GT_ERROR_FATAL_GUC			(9)
#define PRELIM_DRM_XE_GENL_GT_ERROR_FATAL_IDI_PAR			(10)
#define PRELIM_DRM_XE_GENL_GT_ERROR_FATAL_SQIDI			(11)
#define PRELIM_DRM_XE_GENL_GT_ERROR_FATAL_SAMPLER			(12)
#define PRELIM_DRM_XE_GENL_GT_ERROR_FATAL_SLM			(13)
#define PRELIM_DRM_XE_GENL_GT_ERROR_FATAL_EU_IC			(14)
#define PRELIM_DRM_XE_GENL_GT_ERROR_FATAL_EU_GRF			(15)
#define PRELIM_DRM_XE_GENL_GT_ERROR_FATAL_FPU			(16)
#define PRELIM_DRM_XE_GENL_GT_ERROR_FATAL_TLB			(17)
#define PRELIM_DRM_XE_GENL_GT_ERROR_FATAL_L3_FABRIC			(18)
#define PRELIM_DRM_XE_GENL_GT_ERROR_CORRECTABLE_SUBSLICE		(19)
#define PRELIM_DRM_XE_GENL_GT_ERROR_CORRECTABLE_L3BANK		(20)
#define PRELIM_DRM_XE_GENL_GT_ERROR_FATAL_SUBSLICE			(21)
#define PRELIM_DRM_XE_GENL_GT_ERROR_FATAL_L3BANK			(22)
#define PRELIM_DRM_XE_GENL_SGUNIT_ERROR_CORRECTABLE			(23)
#define PRELIM_DRM_XE_GENL_SGUNIT_ERROR_NONFATAL			(24)
#define PRELIM_DRM_XE_GENL_SGUNIT_ERROR_FATAL			(25)
#define PRELIM_DRM_XE_GENL_SOC_ERROR_NONFATAL_CSC_PSF_CMD		(26)
#define PRELIM_DRM_XE_GENL_SOC_ERROR_NONFATAL_CSC_PSF_CMP		(27)
#define PRELIM_DRM_XE_GENL_SOC_ERROR_NONFATAL_CSC_PSF_REQ		(28)
#define PRELIM_DRM_XE_GENL_SOC_ERROR_NONFATAL_ANR_MDFI		(29)
#define PRELIM_DRM_XE_GENL_SOC_ERROR_NONFATAL_MDFI_T2T		(30)
#define PRELIM_DRM_XE_GENL_SOC_ERROR_NONFATAL_MDFI_T2C		(31)
#define PRELIM_DRM_XE_GENL_SOC_ERROR_FATAL_CSC_PSF_CMD		(32)
#define PRELIM_DRM_XE_GENL_SOC_ERROR_FATAL_CSC_PSF_CMP		(33)
#define PRELIM_DRM_XE_GENL_SOC_ERROR_FATAL_CSC_PSF_REQ		(34)
#define PRELIM_DRM_XE_GENL_SOC_ERROR_FATAL_PUNIT			(35)
#define PRELIM_DRM_XE_GENL_SOC_ERROR_FATAL_PCIE_PSF_CMD		(36)
#define PRELIM_DRM_XE_GENL_SOC_ERROR_FATAL_PCIE_PSF_CMP		(37)
#define PRELIM_DRM_XE_GENL_SOC_ERROR_FATAL_PCIE_PSF_REQ		(38)
#define PRELIM_DRM_XE_GENL_SOC_ERROR_FATAL_ANR_MDFI			(39)
#define PRELIM_DRM_XE_GENL_SOC_ERROR_FATAL_MDFI_T2T			(40)
#define PRELIM_DRM_XE_GENL_SOC_ERROR_FATAL_MDFI_T2C			(41)
#define PRELIM_DRM_XE_GENL_SOC_ERROR_FATAL_PCIE_AER			(42)
#define PRELIM_DRM_XE_GENL_SOC_ERROR_FATAL_PCIE_ERR			(43)
#define PRELIM_DRM_XE_GENL_SOC_ERROR_FATAL_UR_COND			(44)
#define PRELIM_DRM_XE_GENL_SOC_ERROR_FATAL_SERR_SRCS		(45)

#define PRELIM_DRM_XE_GENL_SOC_ERROR_NONFATAL_HBM(ss, n)\
		(PRELIM_DRM_XE_GENL_SOC_ERROR_FATAL_SERR_SRCS + 0x1 + (ss) * 0x10 + (n))
#define PRELIM_DRM_XE_GENL_SOC_ERROR_FATAL_HBM(ss, n)\
		(PRELIM_DRM_XE_GENL_SOC_ERROR_NONFATAL_HBM(1, 15) + 0x1 + (ss) * 0x10 + (n))

/* 109 is the last ID used by SOC errors */
#define PRELIM_DRM_XE_GENL_GSC_ERROR_CORRECTABLE_SRAM_ECC		(110)
#define PRELIM_DRM_XE_GENL_GSC_ERROR_NONFATAL_MIA_SHUTDOWN		(111)
#define PRELIM_DRM_XE_GENL_GSC_ERROR_NONFATAL_MIA_INTERNAL		(112)
#define PRELIM_DRM_XE_GENL_GSC_ERROR_NONFATAL_SRAM_ECC		(113)
#define PRELIM_DRM_XE_GENL_GSC_ERROR_NONFATAL_WDG_TIMEOUT		(114)
#define PRELIM_DRM_XE_GENL_GSC_ERROR_NONFATAL_ROM_PARITY		(115)
#define PRELIM_DRM_XE_GENL_GSC_ERROR_NONFATAL_UCODE_PARITY		(116)
#define PRELIM_DRM_XE_GENL_GSC_ERROR_NONFATAL_VLT_GLITCH		(117)
#define PRELIM_DRM_XE_GENL_GSC_ERROR_NONFATAL_FUSE_PULL		(118)
#define PRELIM_DRM_XE_GENL_GSC_ERROR_NONFATAL_FUSE_CRC_CHECK	(119)
#define PRELIM_DRM_XE_GENL_GSC_ERROR_NONFATAL_SELF_MBIST		(120)
#define PRELIM_DRM_XE_GENL_GSC_ERROR_NONFATAL_AON_RF_PARITY		(121)
#define PRELIM_DRM_XE_GENL_SGGI_ERROR_NONFATAL			(122)
#define PRELIM_DRM_XE_GENL_SGLI_ERROR_NONFATAL			(123)
#define PRELIM_DRM_XE_GENL_SGCI_ERROR_NONFATAL			(124)
#define PRELIM_DRM_XE_GENL_MERT_ERROR_NONFATAL			(125)
#define PRELIM_DRM_XE_GENL_SGGI_ERROR_FATAL				(126)
#define PRELIM_DRM_XE_GENL_SGLI_ERROR_FATAL				(127)
#define PRELIM_DRM_XE_GENL_SGCI_ERROR_FATAL				(128)
#define PRELIM_DRM_XE_GENL_MERT_ERROR_FATAL				(129)

#define __PRELIM_DRM_XE_GENL_DRV_GT_ERROR_OFFSET		(PRELIM_DRM_XE_HW_ERROR(0, 0) + \
								 3000)
#define PRELIM_DRM_XE_GENL_DRV_GT_ERROR(gt, id) \
	((__PRELIM_DRM_XE_GENL_DRV_GT_ERROR_OFFSET + (id)) | \
	((__u64)(gt) << __PRELIM_XE_GENL_GT_SHIFT))
/* Driver GT error IDs */
#define PRELIM_DRM_XE_GENL_DRV_GT_ERROR_GUC_COMMUNICATION		(0)
#define PRELIM_DRM_XE_GENL_DRV_GT_ERROR_ENGINE_OTHER		(1)
#define PRELIM_DRM_XE_GENL_DRV_GT_ERROR_OTHER			(2)

#define __PRELIM_DRM_XE_GENL_DRV_TILE_ERROR_OFFSET		(PRELIM_DRM_XE_HW_ERROR(0, 0) + \
								 5000)
#define PRELIM_DRM_XE_GENL_DRV_TILE_ERROR(tile, id) \
	((__PRELIM_DRM_XE_GENL_DRV_TILE_ERROR_OFFSET + (id)) | \
	 ((__u64)(tile) << __PRELIM_XE_GENL_GT_SHIFT))
/* Driver Tile error IDs */
#define PRELIM_DRM_XE_GENL_DRV_TILE_ERROR_GTT			(0)
#define PRELIM_DRM_XE_GENL_DRV_TILE_ERROR_INTERRUPT		(1)

/**
 * struct prelim_drm_xe_ext_msix - allocate and release msix interrupts
 */
struct prelim_drm_xe_ext_msix {
	/** @extensions: Pointer to the first extension struct, if any */
	__u64 extensions;

#define PRELIM_DRM_XE_EXT_MSIX_ALLOC	(1 << 0)
#define PRELIM_DRM_XE_EXT_MSIX_RELEASE	(1 << 1)
	__u32 flags;

	/**
	 * @msix_id:
	 * PRELIM_DRM_XE_EXT_MSIX_ALLOC: returned msix id
	 * PRELIM_DRM_XE_EXT_MSIX_RELEASE: msix id to release
	 */
	__u32 msix_id;

	/** @reserved: Reserved */
	__u64 reserved[2];
};

/**
 * @PRELIM_DRM_XE_EXEC_QUEUE_DEDICATED_INTERRUPT_HINT - Flag to request a
 * dedicated interrupt vector during exec queue creation.
 *
 * Should be set in the `flags` field of `struct drm_xe_exec_queue_create` when
 * a dedicated interrupt vector for better performance is desired.
 */
#define PRELIM_DRM_XE_EXEC_QUEUE_DEDICATED_INTERRUPT_HINT	(1 << 31)

struct prelim_drm_xe_vm_bind_op_ext_attach_debug {
	/** @base: base user extension */
	struct drm_xe_user_extension base;

	/** @id: Debug object id from create metadata */
	__u64 metadata_id;

	/** @flags: Flags */
	__u64 flags;

	/** @cookie: Cookie */
	__u64 cookie;

	/** @reserved: Reserved */
	__u64 reserved;
};

#define PRELIM_XE_VM_BIND_OP_EXTENSIONS_ATTACH_DEBUG 0
#define   PRELIM_DRM_XE_EXEC_QUEUE_SET_PROPERTY_EUDEBUG		3
#define     PRELIM_DRM_XE_EXEC_QUEUE_EUDEBUG_FLAG_ENABLE		(1 << 0)
#define     PRELIM_DRM_XE_EXEC_QUEUE_EUDEBUG_FLAG_PAGEFAULT_ENABLE	(1 << 1)
/*
 * Debugger ABI (ioctl and events) Version History:
 * 0 - No debugger available
 * 1 - Initial version
 */
#define PRELIM_DRM_XE_EUDEBUG_VERSION 1
struct prelim_drm_xe_eudebug_connect {
	/** @extensions: Pointer to the first extension struct, if any */
	__u64 extensions;

	__u64 pid; /* input: Target process ID */
	__u32 flags; /* MBZ */

	__u32 version; /* output: current ABI (ioctl / events) version */
};

/*
 * struct prelim_drm_xe_debug_metadata_create - Create debug metadata
 *
 * Add a region of user memory to be marked as debug metadata.
 * When the debugger attaches, the metadata regions will be delivered
 * for debugger. Debugger can then map these regions to help decode
 * the program state.
 *
 * Returns handle to created metadata entry.
 */
struct prelim_drm_xe_debug_metadata_create {
	/** @extensions: Pointer to the first extension struct, if any */
	__u64 extensions;

#define PRELIM_DRM_XE_DEBUG_METADATA_ELF_BINARY     0
#define PRELIM_DRM_XE_DEBUG_METADATA_PROGRAM_MODULE 1
#define PRELIM_WORK_IN_PROGRESS_DRM_XE_DEBUG_METADATA_MODULE_AREA 2
#define PRELIM_WORK_IN_PROGRESS_DRM_XE_DEBUG_METADATA_SBA_AREA 3
#define PRELIM_WORK_IN_PROGRESS_DRM_XE_DEBUG_METADATA_SIP_AREA 4
#define PRELIM_WORK_IN_PROGRESS_DRM_XE_DEBUG_METADATA_NUM (1 + \
	  PRELIM_WORK_IN_PROGRESS_DRM_XE_DEBUG_METADATA_SIP_AREA)

	/** @type: Type of metadata */
	__u64 type;

	/** @user_addr: pointer to start of the metadata */
	__u64 user_addr;

	/** @len: length, in bytes of the medata */
	__u64 len;

	/** @metadata_id: created metadata handle (out) */
	__u32 metadata_id;
};

/**
 * struct prelim_drm_xe_debug_metadata_destroy - Destroy debug metadata
 *
 * Destroy debug metadata.
 */
struct prelim_drm_xe_debug_metadata_destroy {
	/** @extensions: Pointer to the first extension struct, if any */
	__u64 extensions;

	/** @metadata_id: metadata handle to destroy */
	__u32 metadata_id;
};


/**
 * Do a eudebug event read for a debugger connection.
 *
 * This ioctl is available in debug version 1.
 */
#define PRELIM_DRM_XE_EUDEBUG_IOCTL_READ_EVENT		_IO('j', 0x0)
#define PRELIM_DRM_XE_EUDEBUG_IOCTL_EU_CONTROL		_IOWR('j', 0x2, struct prelim_drm_xe_eudebug_eu_control)
#define PRELIM_DRM_XE_EUDEBUG_IOCTL_ACK_EVENT		_IOW('j', 0x4, struct prelim_drm_xe_eudebug_ack_event)
#define PRELIM_DRM_XE_EUDEBUG_IOCTL_VM_OPEN		_IOW('j', 0x1, struct prelim_drm_xe_eudebug_vm_open)
#define PRELIM_DRM_XE_EUDEBUG_IOCTL_READ_METADATA	_IOWR('j', 0x3, struct prelim_drm_xe_eudebug_read_metadata)

/* XXX: Document events to match their internal counterparts when moved to xe_drm.h */
struct prelim_drm_xe_eudebug_event {
	__u32 len;

	__u16 type;
#define PRELIM_DRM_XE_EUDEBUG_EVENT_NONE			0
#define PRELIM_DRM_XE_EUDEBUG_EVENT_READ			1
#define PRELIM_DRM_XE_EUDEBUG_EVENT_OPEN			2
#define PRELIM_DRM_XE_EUDEBUG_EVENT_VM				3
#define PRELIM_DRM_XE_EUDEBUG_EVENT_EXEC_QUEUE			4
#define PRELIM_DRM_XE_EUDEBUG_EVENT_EU_ATTENTION		5
#define PRELIM_DRM_XE_EUDEBUG_EVENT_VM_BIND			6
#define PRELIM_DRM_XE_EUDEBUG_EVENT_VM_BIND_OP			7
#define PRELIM_DRM_XE_EUDEBUG_EVENT_VM_BIND_UFENCE		8
#define PRELIM_DRM_XE_EUDEBUG_EVENT_METADATA			9
#define PRELIM_DRM_XE_EUDEBUG_EVENT_VM_BIND_OP_METADATA 	10
#define PRELIM_DRM_XE_EUDEBUG_EVENT_PAGEFAULT			11
#define PRELIM_DRM_XE_EUDEBUG_EVENT_SYNC_HOST			12
#define PRELIM_DRM_XE_EUDEBUG_EVENT_EXEC_QUEUE_PLACEMENTS	13

	__u16 flags;
#define PRELIM_DRM_XE_EUDEBUG_EVENT_CREATE		(1 << 0)
#define PRELIM_DRM_XE_EUDEBUG_EVENT_DESTROY		(1 << 1)
#define PRELIM_DRM_XE_EUDEBUG_EVENT_STATE_CHANGE	(1 << 2)
#define PRELIM_DRM_XE_EUDEBUG_EVENT_NEED_ACK		(1 << 3)

	__u64 seqno;
	__u64 reserved;
};

struct prelim_drm_xe_eudebug_event_client {
	struct prelim_drm_xe_eudebug_event base;

	__u64 client_handle; /* This is unique per debug connection */
};

struct prelim_drm_xe_eudebug_event_vm {
	struct prelim_drm_xe_eudebug_event base;

	__u64 client_handle;
	__u64 vm_handle;
};

struct prelim_drm_xe_eudebug_event_exec_queue {
	struct prelim_drm_xe_eudebug_event base;

	__u64 client_handle;
	__u64 vm_handle;
	__u64 exec_queue_handle;
	__u32 engine_class;
	__u32 width;
	__u64 lrc_handle[];
};

struct prelim_drm_xe_eudebug_event_eu_attention {
	struct prelim_drm_xe_eudebug_event base;

	__u64 client_handle;
	__u64 exec_queue_handle;
	__u64 lrc_handle;
	__u32 flags;
	__u32 bitmask_size;
	__u8 bitmask[];
};

struct prelim_drm_xe_eudebug_eu_control {
	__u64 client_handle;

#define PRELIM_DRM_XE_EUDEBUG_EU_CONTROL_CMD_INTERRUPT_ALL	0
#define PRELIM_DRM_XE_EUDEBUG_EU_CONTROL_CMD_STOPPED		1
#define PRELIM_DRM_XE_EUDEBUG_EU_CONTROL_CMD_RESUME		2
#define PRELIM_DRM_XE_EUDEBUG_EU_CONTROL_CMD_UNLOCK		3
	__u32 cmd;
	__u32 flags;

	__u64 seqno;

	__u64 exec_queue_handle;
	__u64 lrc_handle;
	__u32 reserved;
	__u32 bitmask_size;
	__u64 bitmask_ptr;
};

/*
 *  When client (debuggee) does vm_bind_ioctl() following event
 *  sequence will be created (for the debugger):
 *
 *  ┌───────────────────────┐
 *  │  EVENT_VM_BIND        ├───────┬─┬─┐
 *  └───────────────────────┘       │ │ │
 *      ┌───────────────────────┐   │ │ │
 *      │ EVENT_VM_BIND_OP #1   ├───┘ │ │
 *      └───────────────────────┘     │ │
 *                 ...                │ │
 *      ┌───────────────────────┐     │ │
 *      │ EVENT_VM_BIND_OP #n   ├─────┘ │
 *      └───────────────────────┘       │
 *                                      │
 *      ┌───────────────────────┐       │
 *      │ EVENT_UFENCE          ├───────┘
 *      └───────────────────────┘
 *
 * All the events below VM_BIND will reference the VM_BIND
 * they associate with, by field .vm_bind_ref_seqno.
 * event_ufence will only be included if the client did
 * attach sync of type UFENCE into its vm_bind_ioctl().
 *
 * When EVENT_UFENCE is sent by the driver, all the OPs of
 * the original VM_BIND are completed and the [addr,range]
 * contained in them are present and modifiable through the
 * vm accessors. Accessing [addr, range] before related ufence
 * event will lead to undefined results as the actual bind
 * operations are async and the backing storage might not
 * be there on a moment of receiving the event.
 *
 * Client's UFENCE sync will be held by the driver: client's
 * drm_xe_wait_ufence will not complete and the value of the ufence
 * won't appear until ufence is acked by the debugger process calling
 * PRELIM_DRM_XE_EUDEBUG_IOCTL_ACK_EVENT with the event_ufence.base.seqno.
 * This will signal the fence, .value will update and the wait will
 * complete allowing the client to continue.
 *
 */

struct prelim_drm_xe_eudebug_event_vm_bind {
	struct prelim_drm_xe_eudebug_event base;

	__u64 client_handle;
	__u64 vm_handle;

	__u32 flags;
#define PRELIM_DRM_XE_EUDEBUG_EVENT_VM_BIND_FLAG_UFENCE (1 << 0)

	__u32 num_binds;
};

struct prelim_drm_xe_eudebug_event_vm_bind_op {
	struct prelim_drm_xe_eudebug_event base;
	__u64 vm_bind_ref_seqno; /* *_event_vm_bind.base.seqno */
	__u64 num_extensions;

	__u64 addr; /* XXX: Zero for unmap all? */
	__u64 range; /* XXX: Zero for unmap all? */
};

struct prelim_drm_xe_eudebug_event_vm_bind_ufence {
	struct prelim_drm_xe_eudebug_event base;
	__u64 vm_bind_ref_seqno; /* *_event_vm_bind.base.seqno */
};

struct prelim_drm_xe_eudebug_ack_event {
	__u32 type;
	__u32 flags; /* MBZ */
	__u64 seqno;
};

struct prelim_drm_xe_eudebug_vm_open {
	/** @extensions: Pointer to the first extension struct, if any */
	__u64 extensions;

	/** @client_handle: id of client */
	__u64 client_handle;

	/** @vm_handle: id of vm */
	__u64 vm_handle;

	/** @flags: flags */
	__u64 flags;

#define PRELIM_DRM_XE_EUDEBUG_VM_SYNC_MAX_TIMEOUT_NSECS (10ULL * NSEC_PER_SEC)
	/** @timeout_ns: Timeout value in nanoseconds operations (fsync) */
	__u64 timeout_ns;
};

struct prelim_drm_xe_eudebug_read_metadata {
	__u64 client_handle;
	__u64 metadata_handle;
	__u32 flags;
	__u32 reserved;
	__u64 ptr;
	__u64 size;
};

struct prelim_drm_xe_eudebug_event_metadata {
	struct prelim_drm_xe_eudebug_event base;

	__u64 client_handle;
	__u64 metadata_handle;
	/* XXX: Refer to xe_drm.h for fields */
	__u64 type;
	__u64 len;
};

struct prelim_drm_xe_eudebug_event_vm_bind_op_metadata {
	struct prelim_drm_xe_eudebug_event base;
	__u64 vm_bind_op_ref_seqno; /* *_event_vm_bind_op.base.seqno */

	__u64 metadata_handle;
	__u64 metadata_cookie;
};

struct prelim_drm_xe_eudebug_event_pagefault {
	struct prelim_drm_xe_eudebug_event base;

	__u64 client_handle;
	__u64 exec_queue_handle;
	__u64 lrc_handle;
	__u32 flags;
	__u32 bitmask_size;
	__u64 pagefault_address;
	__u8 bitmask[];
};

struct prelim_drm_xe_eudebug_event_sync_host {
	struct prelim_drm_xe_eudebug_event base;
	__u64 client_handle;
	__u64 exec_queue_handle;
	__u64 lrc_handle;
};

struct prelim_drm_xe_eudebug_event_exec_queue_placements {
	struct prelim_drm_xe_eudebug_event base;
	__u64 client_handle;
	__u64 vm_handle;
	__u64 exec_queue_handle;
	__u64 lrc_handle;
	__u32 num_placements;
	__u32 pad;
	/**
	 * @instances: user pointer to num_placements sized array of struct
	 * drm_xe_engine_class_instance
	 */
	__u64 instances[];
};

/**
 * PXP Tag format:
 * bits   0-6: session id
 * bit      7: rsvd
 * bits  8-15: instance id
 * bit     16: session enabled
 * bit     17: mode hm
 * bit     18: rsvd
 * bit     19: mode sm
 * bits 20-31: rsvd
 */
#define PRELIM_DRM_XE_PXP_TAG_SESSION_ID_MASK		(0x7f)
#define PRELIM_DRM_XE_PXP_TAG_INSTANCE_ID_MASK		(0xff << 8)
#define PRELIM_DRM_XE_PXP_TAG_SESSION_ENABLED		(0x1 << 16)
#define PRELIM_DRM_XE_PXP_TAG_SESSION_HM		(0x1 << 17)
#define PRELIM_DRM_XE_PXP_TAG_SESSION_SM		(0x1 << 19)

/**
 * struct prelim_drm_xe_pxp_query_host_session_handle
 * Contains params to get a host-session-handle that the user-space
 * process uses for all communication with the GSC-FW.
 *
 * - Each user space process is provided a single host_session_handle.
 *   A user space process that repeats a request for a host_session_handle
 *   will be successfully serviced but returned the same host_session_handle
 *   that was generated (a random number) on the first request.
 * - When the user space process exits, the kernel driver will send a cleanup
 *   cmd to the gsc firmware. There is no need (and no mechanism) for the user
 *   space process to explicitly request to release its host_session_handle.
 * - The host_session_handle remains valid through any suspend/resume cycles
 *   and through PXP hw-session-slot teardowns (essentially they are
 *   decoupled from the hw session-slots)
 *
 * This operation can only fail if something goes wrong in the prep steps, in
 * which case the ioctl will fail. Therefore, if the ioctl succeeds the
 * pxp_ops.status will always be PRELIM_DRM_XE_PXP_OP_STATUS_SUCCESS
 */
struct prelim_drm_xe_pxp_query_host_session_handle {
	__u64 host_session_handle; /* out - returned host_session_handle */
} __attribute__((packed));

/**
 * struct prelim_drm_xe_pxp_session_op - Params to reserve or release a PXP
 * session.
 */
struct prelim_drm_xe_pxp_session_op {
	/** @action: session operation to perform (reserve or release). */
	__u32 action;
#define PRELIM_DRM_XE_PXP_SESSION_RESERVE 0
#define PRELIM_DRM_XE_PXP_SESSION_RELEASE 1

	/**
	 * @pxp_tag: when reserving a session, this variable MBZ as input and
	 * will be filled with the pxp_tag as output (see defines in
	 * struct prelim_drm_xe_pxp_query_tag for the format of the tag). When
	 * releasing a session this must be set to either the full tag or
	 * just the ID of the session to be released.
	 */
	__u32 pxp_tag;

	/**
	 * @session_type: When reserving a PXP session, specify the type of
	 * session. The only supported value is DRM_XE_PXP_TYPE_HWDRM. Ignored
	 * when releasing the session.
	 */
	__u32 session_type;

	/**
	 * @session_mode: When reserving a PXP session, specify the protection
	 * mode. This information is stored in the PXP tag. Ignored when
	 * releasing the session.
	 */
	__u32 session_mode;
#define PRELIM_DRM_XE_PXP_MODE_LM 0 /* Light */
#define PRELIM_DRM_XE_PXP_MODE_HM 1 /* Heavy */
#define PRELIM_DRM_XE_PXP_MODE_SM 2 /* Stout */
} __attribute__((packed));

/**
 * struct prelim_drm_xe_pxp_query_tag - Params to query the PXP tag of specified
 * session id and whether the session is alive from PXP state machine.
 */
struct prelim_drm_xe_pxp_query_tag {
	/**
	 * @pxp_tag: as input, this variable must be set to either the pxp_tag
	 * returned by the session reservation or to the session id. The value
	 * will be overwritten with the current tag of matching session.
	 */
	__u32 pxp_tag;

	/**
	 * @session_is_alive: Returns whether the session is alive in HW, based
	 * on the value in the KCR_SIP register.
	 */
	__u32 session_is_alive;
} __attribute__((packed));

/**
 * struct prelim_drm_xe_pxp_io_message - Params to send/receive message to/from TEE.
 */
struct prelim_drm_xe_pxp_io_message {
	/** @msg_in: pointer to memory containing input message */
	__u64 msg_in;
	/** @msg_in_size: input message size */
	__u32 msg_in_size;
	/** @msg_out: pointer to memory to store the output message */
	__u64 msg_out;
	/** @msg_out_buf_size: size of the memory available for msg_out */
	__u32 msg_out_buf_size;
	/** @msg_out_ret_size: actual size of the message returned from the TEE */
	__u32 msg_out_ret_size;
} __attribute__((packed));

/**
 * prelim_drm_xe_pxp_ops
 *
 * PXP is an Xe component, that helps user space to establish the hardware
 * protected session and manage the status of each alive software session,
 * as well as the life cycle of each session.
 *
 * This ioctl is to allow user space driver to create, set, and destroy each
 * session. It also provides the communication channel to TEE (Trusted
 * Execution Environment) for the protected hardware session creation.
 */
struct prelim_drm_xe_pxp_ops {
	/** @extensions: MBZ, as no extension are currently defined for this ioctl */
	__u64 extensions;

	/** @action: operation to perform */
	__u32 action;
#define PRELIM_DRM_XE_PXP_ACTION_HOST_SESSION_HANDLE_REQ 0
#define PRELIM_DRM_XE_PXP_ACTION_SESSION_OP 1
#define PRELIM_DRM_XE_PXP_ACTION_QUERY_PXP_TAG 2
#define PRELIM_DRM_XE_PXP_ACTION_TEE_IO_MESSAGE 3

	/** @status: returned outcome of the operation */
	__u32 status;
#define PRELIM_DRM_XE_PXP_OP_STATUS_SUCCESS 0
#define PRELIM_DRM_XE_PXP_OP_STATUS_RETRY_REQUIRED 1
#define PRELIM_DRM_XE_PXP_OP_STATUS_SESSION_NOT_AVAILABLE 2
#define PRELIM_DRM_XE_PXP_OP_STATUS_POWER_OFF 3

	/*
	 * in/out: action-specific data. Must fill the structure matching the
	 * selected action.
	 */
	union {
		/** @query_handle: parameters for the HOST_SESSION_HANDLE_REQ action */
		struct prelim_drm_xe_pxp_query_host_session_handle query_handle;
		/** @session_op: parameters for the SESSION_OP action */
		struct prelim_drm_xe_pxp_session_op session_op;
		/** @query_tag: parameters for the QUERY_PXP_TAG action */
		struct prelim_drm_xe_pxp_query_tag query_tag;
		/** @io_message: parameters for the TEE_IO_MESSAGE action */
		struct prelim_drm_xe_pxp_io_message io_message;
	};
} __attribute__((packed));

#endif /* _UAPI_XE_DRM_PRELIM_H_ */
