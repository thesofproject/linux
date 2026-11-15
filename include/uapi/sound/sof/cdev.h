/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause) */
/*
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * Copyright(c) 2026 Intel Corporation. All rights reserved.
 *
 * Sound Open Firmware (SOF) Character Device UAPI (/dev/sof-dspN)
 */

#ifndef __INCLUDE_UAPI_SOUND_SOF_CDEV_H
#define __INCLUDE_UAPI_SOUND_SOF_CDEV_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define SOF_CDEV_IOC_MAGIC		'S'
#define SOF_CDEV_ABI_VERSION		0x00010000

/*
 * SRAM Memory Window Selectors
 * Used in struct sof_cdev_win_io.win_id and mmap() page offset (win_id << 20)
 */
#define SOF_CDEV_WIN_FW_REGS		0	/* Window 0: FW Regs (0x0..0xfff) + Inbox (+0x1000) */
#define SOF_CDEV_WIN_OUTBOX		1	/* Window 1: Host Downlink Outbox */
#define SOF_CDEV_WIN_DEBUG		2	/* Window 2: Debug Window (16 x 4 KB pages) */
#define SOF_CDEV_WIN_PERF		3	/* Window 3: Performance Measurements Window (ADSP_PMW) */
#define SOF_CDEV_WIN_BY_SLOT_TYPE	0xFF	/* Select Window 2 slot by slot_type + slot_index */

#define SOF_CDEV_MAX_WINDOWS		4
#define SOF_CDEV_MAX_DEBUG_SLOTS	16

/* Standard IPC4 Window 2 Debug Slot Types */
#define SOF_CDEV_SLOT_UNUSED		0x00000000U
#define SOF_CDEV_SLOT_CRITICAL_LOG	0x54524300U
#define SOF_CDEV_SLOT_DEBUG_LOG		0x474f4c00U
#define SOF_CDEV_SLOT_GDB_STUB		0x42444700U
#define SOF_CDEV_SLOT_TELEMETRY		0x4c455400U
#define SOF_CDEV_SLOT_BROKEN		0x44414544U
#define SOF_CDEV_SLOT_SHELL		0x73686c6cU /* 'shll' Zephyr winstream shell */
#define SOF_CDEV_SLOT_LLEXT_LOAD	0x4c454c44U /* 'LELD' Shell LLEXT load shm */

/**
 * struct sof_cdev_slot_desc - Window 2 debug slot descriptor
 * @slot_index:	Slot index (0..14 for full 4 KB pages, 15 for partial page 0)
 * @res_id:	Resource / owner ID reported by firmware
 * @type:	Slot type magic (e.g. SOF_CDEV_SLOT_TELEMETRY)
 * @vma:	DSP virtual memory address of the slot
 * @win_offset:	Byte offset of the slot payload relative to Window 2 start
 * @bar_offset:	Byte offset of the slot payload relative to DSP BAR
 * @size:	Usable byte capacity of the slot (4096 for 0..14, 3072 for 15)
 * @reserved:	Reserved for alignment
 */
struct sof_cdev_slot_desc {
	__u32 slot_index;
	__u32 res_id;
	__u32 type;
	__u32 vma;
	__u32 win_offset;
	__u32 bar_offset;
	__u32 size;
	__u32 reserved;
};

/**
 * struct sof_cdev_win_desc - SRAM Window descriptor
 * @win_id:	Window index (0..3)
 * @bar_offset:	Byte offset relative to DSP BAR (sdev->mailbox_bar)
 * @size:	Window byte size
 * @flags:	Access flags (bit 0 = valid/mapped)
 */
struct sof_cdev_win_desc {
	__u32 win_id;
	__u32 bar_offset;
	__u32 size;
	__u32 flags;
};

/**
 * struct sof_cdev_info - Device, firmware, and window capability summary
 */
struct sof_cdev_info {
	__u32 abi_version;
	__u32 ipc_type;			/* 0 = IPC3, 1 = IPC4 */
	__u32 fw_state;			/* enum sof_fw_state */
	__u32 dsp_power_state;		/* enum sof_dsp_power_states (0=U0, 1=D0, 2=D1, 3=D2, 4=D3) */
	__u32 pm_runtime_active;	/* 1 if PCI/ACPI device is currently in D0 */
	__u32 num_cores;		/* Total DSP cores on platform */
	__u32 enabled_cores_mask;	/* Bitmask of currently enabled DSP cores */
	__u32 max_payload_size;		/* Max IPC mailbox payload size in bytes */
	__u16 fw_ver_major;
	__u16 fw_ver_minor;
	__u16 fw_ver_micro;
	__u16 fw_ver_build;
	__u32 hw_ip_version;		/* enum sof_intel_hw_ip_version */
	char  platform[32];		/* Platform / driver identifier */
	char  fw_filename[64];		/* Loaded firmware file path */
	struct sof_cdev_win_desc windows[SOF_CDEV_MAX_WINDOWS];
	struct sof_cdev_slot_desc debug_slots[SOF_CDEV_MAX_DEBUG_SLOTS];
};

/*
 * Power Management Control Commands (SOF_IOC_PM_CTL)
 */
#define SOF_PM_OP_QUERY		0	/* Query current power & core state without waking */
#define SOF_PM_OP_WAKE_HOLD	1	/* Resume DSP to D0 and hold PM reference on this fd */
#define SOF_PM_OP_RELEASE_HOLD	2	/* Release PM reference held on this fd */
#define SOF_PM_OP_BOOT_DSP	3	/* Ensure DSP firmware is booted (sof_client_boot_dsp) */
#define SOF_PM_OP_CORE_GET	4	/* Power up specific DSP core (core_id) */
#define SOF_PM_OP_CORE_PUT	5	/* Power down specific DSP core (core_id) */

struct sof_cdev_pm_ctl {
	__u32 op;			/* SOF_PM_OP_* */
	__u32 core_id;			/* Used for CORE_GET / CORE_PUT */
	__u32 fw_state;			/* Out: current enum sof_fw_state */
	__u32 dsp_power_state;		/* Out: current DSP power state */
	__u32 pm_runtime_active;	/* Out: 1 if runtime PM is active (D0) */
	__u32 enabled_cores_mask;	/* Out: current enabled_cores_mask */
	__u32 fd_holding_d0;		/* Out: 1 if this fd currently holds a D0 reference */
	__u32 reserved;
};

/*
 * Atomic IPC Transaction (SOF_IOC_IPC_TX)
 */
#define SOF_CDEV_IPC_FL_NO_REPLY	(1U << 0) /* Do not expect payload reply */

struct sof_cdev_ipc_msg {
	__u32 primary;			/* In: IPC4 primary header (or IPC3 cmd) */
	__u32 extension;		/* In: IPC4 extension header */
	__u32 tx_size;			/* In: TX payload byte length */
	__u32 rx_size;			/* In: RX buffer capacity; Out: bytes received */
	__u32 reply_primary;		/* Out: IPC4 reply primary header */
	__u32 reply_extension;		/* Out: IPC4 reply extension header */
	__s32 ipc_status;		/* Out: Firmware reply status (0 = success) */
	__u32 flags;			/* In: SOF_CDEV_IPC_FL_* */
	__u64 tx_data_ptr;		/* In: Userspace pointer to TX payload (or 0) */
	__u64 rx_data_ptr;		/* In: Userspace pointer to RX buffer (or 0) */
};

/*
 * IPC4 Large Config Get / Set (SOF_IOC_IPC_LARGE_CFG)
 */
#define SOF_CDEV_LARGE_CFG_GET		0
#define SOF_CDEV_LARGE_CFG_SET		1

struct sof_cdev_large_cfg {
	__u32 module_id;		/* 0 for BaseFW, or target module_id */
	__u32 instance_id;		/* 0 for BaseFW, or target instance_id */
	__u32 param_id;			/* IPC4 param ID (e.g. 14=MEMORY_STATE, 15=POWER_STATE) */
	__u32 set_get;			/* SOF_CDEV_LARGE_CFG_GET (0) or SET (1) */
	__u32 data_size;		/* In: buffer size; Out: actual bytes read/written */
	__u32 flags;
	__u64 data_ptr;			/* Userspace buffer pointer */
};

/*
 * Memory Window & Debug Slot I/O (SOF_IOC_WIN_INFO, SOF_IOC_WIN_READ, SOF_IOC_WIN_WRITE)
 */
#define SOF_CDEV_WIN_FL_WAKE_DSP	(1U << 0) /* Wake DSP to D0 if currently suspended */

struct sof_cdev_win_info {
	struct sof_cdev_win_desc windows[SOF_CDEV_MAX_WINDOWS];
	struct sof_cdev_slot_desc debug_slots[SOF_CDEV_MAX_DEBUG_SLOTS];
};

struct sof_cdev_win_io {
	__u32 win_id;			/* SOF_CDEV_WIN_0..3 or SOF_CDEV_WIN_BY_SLOT_TYPE */
	__u32 slot_type;		/* Used when win_id == SOF_CDEV_WIN_BY_SLOT_TYPE */
	__u32 slot_index;		/* Match Nth slot of slot_type (0 = first match) */
	__u32 offset;			/* Byte offset within window or slot */
	__u32 size;			/* Bytes to read or write */
	__u32 flags;			/* SOF_CDEV_WIN_FL_* */
	__u64 data_ptr;			/* Userspace buffer pointer */
};

/*
 * Zephyr Winstream Slot I/O (SOF_IOC_WINSTREAM_IO)
 * Used for ADSP_DW_SLOT_SHELL (0x73686c6c) and GDB stub (0x42444700)
 */
struct sof_cdev_winstream_io {
	__u32 slot_type;		/* Default: SOF_CDEV_SLOT_SHELL (0x73686c6c) */
	__u32 rx_win_size;		/* Host->DSP winstream region size (default 256) */
	__u32 tx_len;			/* Bytes to write from tx_ptr to DSP (0 = read only) */
	__u32 rx_buf_len;		/* Max bytes to read into rx_ptr (0 = write only) */
	__u32 rx_bytes_read;		/* Out: Actual bytes read from DSP winstream */
	__u32 seq_in_out;		/* In/Out: Host reader sequence number */
	__u64 tx_ptr;			/* Userspace pointer to TX data */
	__u64 rx_ptr;			/* Userspace pointer to RX buffer */
};

/*
 * Atomic LLEXT Module Loading (SOF_IOC_LLEXT_LOAD)
 */
#define SOF_LLEXT_MODE_SHELL_SHM	0 /* 2-step handshake via SOF_CDEV_SLOT_LLEXT_LOAD */
#define SOF_LLEXT_MODE_IPC4_DIRECT	1 /* Direct IPC4 LOAD_LIBRARY with explicit/auto lib_id */

struct sof_cdev_llext_load {
	__u32 mode;			/* SOF_LLEXT_MODE_* */
	__u32 lib_id;			/* In/Out: Library slot ID (1..15; 0 = from shm or auto) */
	__u32 data_size;		/* Size of .ri / .llext binary in bytes */
	__s32 fw_state;			/* Out: Final slot state or IPC status */
	__u64 data_ptr;			/* Userspace pointer to library binary */
};

/*
 * SOF Probes Control (SOF_IOC_PROBE_CTL)
 */
#define SOF_PROBE_OP_INIT		1
#define SOF_PROBE_OP_DEINIT		2
#define SOF_PROBE_OP_INFO_ACTIVE	3
#define SOF_PROBE_OP_INFO_AVAILABLE	4
#define SOF_PROBE_OP_POINTS_ADD		5
#define SOF_PROBE_OP_POINTS_REMOVE	6

struct sof_cdev_probe_point {
	__u32 buffer_id;		/* Probe point ID (mod_id, inst_id, type, idx) */
	__u32 purpose;			/* 0 = EXTRACTION, 1 = INJECTION */
	__u32 stream_tag;		/* HDA stream tag (or 0 if disconnected) */
	__u32 reserved;
	char  widget_name[32];		/* Resolved topology widget name (output on INFO) */
};

struct sof_cdev_probe_ctl {
	__u32 op;			/* SOF_PROBE_OP_* */
	__u32 stream_tag;		/* Used for SOF_PROBE_OP_INIT */
	__u32 dma_buffer_size;		/* Used for SOF_PROBE_OP_INIT */
	__u32 num_points;		/* In: capacity / count; Out: returned count */
	__u64 points_ptr;		/* Userspace pointer to struct sof_cdev_probe_point[] */
};

/*
 * Async Event Notification Record read() from /dev/sof-dspN
 */
#define SOF_CDEV_EVT_FW_STATE		1
#define SOF_CDEV_EVT_IPC_NOTIFICATION	2

struct sof_cdev_event {
	__u64 timestamp_ns;		/* ktime_get_ns() */
	__u32 event_type;		/* SOF_CDEV_EVT_* */
	__u32 fw_state;			/* Current enum sof_fw_state */
	__u32 primary;			/* IPC4 notification primary header */
	__u32 extension;		/* IPC4 notification extension header */
};

/* IOCTL Definitions */
#define SOF_IOC_GET_INFO	_IOR(SOF_CDEV_IOC_MAGIC, 0x01, struct sof_cdev_info)
#define SOF_IOC_PM_CTL		_IOWR(SOF_CDEV_IOC_MAGIC, 0x02, struct sof_cdev_pm_ctl)
#define SOF_IOC_IPC_TX		_IOWR(SOF_CDEV_IOC_MAGIC, 0x10, struct sof_cdev_ipc_msg)
#define SOF_IOC_IPC_LARGE_CFG	_IOWR(SOF_CDEV_IOC_MAGIC, 0x11, struct sof_cdev_large_cfg)
#define SOF_IOC_WIN_INFO	_IOR(SOF_CDEV_IOC_MAGIC, 0x20, struct sof_cdev_win_info)
#define SOF_IOC_WIN_READ	_IOWR(SOF_CDEV_IOC_MAGIC, 0x21, struct sof_cdev_win_io)
#define SOF_IOC_WIN_WRITE	_IOW(SOF_CDEV_IOC_MAGIC, 0x22, struct sof_cdev_win_io)
#define SOF_IOC_WINSTREAM_IO	_IOWR(SOF_CDEV_IOC_MAGIC, 0x23, struct sof_cdev_winstream_io)
#define SOF_IOC_LLEXT_LOAD	_IOWR(SOF_CDEV_IOC_MAGIC, 0x30, struct sof_cdev_llext_load)
#define SOF_IOC_PROBE_CTL	_IOWR(SOF_CDEV_IOC_MAGIC, 0x40, struct sof_cdev_probe_ctl)

#endif /* __INCLUDE_UAPI_SOUND_SOF_CDEV_H */
