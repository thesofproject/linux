/* SPDX-License-Identifier: GPL-2.0 */
/*
 * xHCI host controller sideband support
 *
 * Copyright (c) 2023-2025, Intel Corporation.
 *
 * Author: Mathias Nyman <mathias.nyman@linux.intel.com>
 */
#ifndef __LINUX_XHCI_SIDEBAND_H
#define __LINUX_XHCI_SIDEBAND_H

#include <linux/scatterlist.h>
#include <linux/usb.h>
#include <linux/usb/hcd.h>

#define	EP_CTX_PER_DEV		31	/* FIXME defined twice, from xhci.h */

struct xhci_sideband;

enum xhci_sideband_type {
	XHCI_SIDEBAND_AUDIO,
	XHCI_SIDEBAND_VENDOR,
};

enum xhci_sideband_notify_type {
	XHCI_SIDEBAND_XFER_RING_FREE,
};

/**
 * struct xhci_sideband_event - sideband event
 * @type: notifier type
 * @evt_data: event data
 */
struct xhci_sideband_event {
	enum xhci_sideband_notify_type type;
	void *evt_data;
};

/**
 * struct xhci_sideband_ep_properties - controller properties of an endpoint.
 * @bus: PCI bus number if xHCI is on a PCI bus
 * @device: PCI device number if xHCI is on a PCI bus
 * @function: PCI function number if xHCI is on a PCI bus
 * @slot_id: xHCI slot_id of the usb device this endpoint is part of
 * @reserved_res_id: driver reserved resource number for this endpoint
 * @rh_port: root hub port number
 * @res_id: xHCI sideband resource number in use by offloaded endpoint
 * @port_speed: PORTSC protocol speed ID
 * @tt: endpoint is behind a USB 2.0 transaction translator
 *
 * reserved_res_id is set after calling xhci_sideband_alloc_resource_id(),
 * res_id is set after offloading the endpoint when xhci_sideband_add_endpoint()
 * is called, and should match reserved_res_id at that point.
 */
struct xhci_sideband_ep_properties {
	/* PCI bus, device and function */
	u8 bus;
	u8 device;
	u8 function;

	/* xHCI usb device slot_id */
	int slot_id;
	u8 reserved_res_id;

	/* Properties queried with Get Endpoint Properties Command */
	u8 rh_port;
	u8 res_id;
	u8 port_speed;
	bool tt;
};

/**
 * struct xhci_sideband - representation of a sideband accessed usb device.
 * @xhci: The xhci host controller the usb device is connected to
 * @udev: the usb device accessed via sideband
 * @eps: array of endpoints controlled via sideband
 * @eps_res_id: array of resource ids for endpoints
 * @ir: event handling and buffer for sideband accessed device
 * @type: xHCI sideband type
 * @mutex: mutex for sideband operations
 * @intf: USB sideband client interface
 * @notify_client: callback for xHCI sideband sequences
 */
struct xhci_sideband {
	struct xhci_hcd                 *xhci;
	struct usb_device		*udev;
	struct xhci_virt_ep             *eps[EP_CTX_PER_DEV];
	unsigned int			eps_res_id[EP_CTX_PER_DEV];
	struct xhci_interrupter         *ir;
	enum xhci_sideband_type		type;

	/* Synchronizing xHCI sideband operations with client drivers operations */
	struct mutex			mutex;

	struct usb_interface		*intf;
	int (*notify_client)(struct usb_interface *intf,
			     struct xhci_sideband_event *evt);
};

struct xhci_sideband *
xhci_sideband_register(struct usb_interface *intf, enum xhci_sideband_type type,
		       int (*notify_client)(struct usb_interface *intf,
				    struct xhci_sideband_event *evt));
void
xhci_sideband_unregister(struct xhci_sideband *sb);
int
xhci_sideband_add_endpoint(struct xhci_sideband *sb,
			   struct usb_host_endpoint *host_ep);
int
xhci_sideband_remove_endpoint(struct xhci_sideband *sb,
			      struct usb_host_endpoint *host_ep);
int
xhci_sideband_stop_endpoint(struct xhci_sideband *sb,
			    struct usb_host_endpoint *host_ep);
struct sg_table *
xhci_sideband_get_endpoint_buffer(struct xhci_sideband *sb,
				  struct usb_host_endpoint *host_ep);
struct sg_table *
xhci_sideband_get_event_buffer(struct xhci_sideband *sb);

#if IS_ENABLED(CONFIG_USB_XHCI_SIDEBAND)
bool xhci_sideband_check(struct usb_hcd *hcd);
#else
static inline bool xhci_sideband_check(struct usb_hcd *hcd)
{ return false; }
#endif /* IS_ENABLED(CONFIG_USB_XHCI_SIDEBAND) */

int
xhci_sideband_create_interrupter(struct xhci_sideband *sb, int num_seg,
				 bool ip_autoclear, u32 imod_interval, int intr_num);
void
xhci_sideband_remove_interrupter(struct xhci_sideband *sb);
int
xhci_sideband_interrupter_id(struct xhci_sideband *sb);
int
xhci_sideband_alloc_resource_id(struct xhci_sideband *sb,
				struct usb_host_endpoint *host_ep,
				unsigned int min, unsigned int max);
int
xhci_sideband_free_resource_id(struct xhci_sideband *sb,
			       unsigned int res_id);
int
xhci_sideband_get_ep_properties(struct xhci_sideband *sb,
				struct usb_host_endpoint *host_ep,
				struct xhci_sideband_ep_properties *properties);

#if IS_ENABLED(CONFIG_USB_XHCI_SIDEBAND)
void xhci_sideband_notify_ep_ring_free(struct xhci_sideband *sb,
				       unsigned int ep_index);
#else
static inline void xhci_sideband_notify_ep_ring_free(struct xhci_sideband *sb,
						     unsigned int ep_index)
{ }
#endif /* IS_ENABLED(CONFIG_USB_XHCI_SIDEBAND) */
#endif /* __LINUX_XHCI_SIDEBAND_H */
