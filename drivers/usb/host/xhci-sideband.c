// SPDX-License-Identifier: GPL-2.0

/*
 * xHCI host controller sideband support
 *
 * Copyright (c) 2023-2025, Intel Corporation.
 *
 * Author: Mathias Nyman
 */

#include <linux/usb/xhci-sideband.h>
#include <linux/dma-direct.h>

#include "xhci.h"

/* sideband internal helpers */
static struct sg_table *
xhci_ring_to_sgtable(struct xhci_sideband *sb, struct xhci_ring *ring)
{
	struct xhci_segment *seg;
	struct sg_table	*sgt;
	unsigned int n_pages;
	struct page **pages;
	struct device *dev;
	size_t sz;
	int i;

	dev = xhci_to_hcd(sb->xhci)->self.sysdev;
	sz = ring->num_segs * TRB_SEGMENT_SIZE;
	n_pages = PAGE_ALIGN(sz) >> PAGE_SHIFT;
	pages = kvmalloc_objs(struct page *, n_pages);
	if (!pages)
		return NULL;

	sgt = kzalloc_obj(*sgt);
	if (!sgt) {
		kvfree(pages);
		return NULL;
	}

	seg = ring->first_seg;
	if (!seg)
		goto err;
	/*
	 * Rings can potentially have multiple segments, create an array that
	 * carries page references to allocated segments.  Utilize the
	 * sg_alloc_table_from_pages() to create the sg table, and to ensure
	 * that page links are created.
	 */
	for (i = 0; i < ring->num_segs; i++) {
		dma_get_sgtable(dev, sgt, seg->trbs, seg->dma,
				TRB_SEGMENT_SIZE);
		pages[i] = sg_page(sgt->sgl);
		sg_free_table(sgt);
		seg = seg->next;
	}

	if (sg_alloc_table_from_pages(sgt, pages, n_pages, 0, sz, GFP_KERNEL))
		goto err;

	kvfree(pages);

	/*
	 * Save first segment dma address to sg dma_address field for the sideband
	 * client to have access to the IOVA of the ring.
	 */
	sg_dma_address(sgt->sgl) = ring->first_seg->dma;

	return sgt;

err:
	kvfree(pages);
	kfree(sgt);

	return NULL;
}

/* Return the current vdev of sb->udev, or NULL if it was freed or reallocated */
static struct xhci_virt_device *
sideband_to_vdev(struct xhci_sideband *sb)
{
	struct xhci_virt_device *vdev;

	if (!sb->udev || !sb->udev->slot_id)
		return NULL;

	vdev = sb->xhci->devs[sb->udev->slot_id];
	if (vdev && vdev->sideband == sb)
		return vdev;

	return NULL;
}

/*
 * Synchronous xHCI stop endpoint helper. Issues a stop endpoint command and
 * waits for the command completion before returning. This does not call
 * xhci_handle_cmd_stop_ep(), which has additional handling for 'context error'
 * cases, along with transfer ring cleanup.
 *
 * Caller must hold sb->mutex
 */
static int
sideband_stop_endpoint_sync(struct xhci_sideband *sb, unsigned int ep_index,
			    bool remove)
{
	struct xhci_virt_device *vdev;
	struct xhci_command *cmd;
	struct xhci_virt_ep *ep;
	struct xhci_ep_ctx *ctx;
	unsigned long flags;
	int ret = -ENODEV;

	lockdep_assert_held(&sb->mutex);

	cmd = xhci_alloc_command(sb->xhci, true, GFP_KERNEL);

	spin_lock_irqsave(&sb->xhci->lock, flags);

	vdev = sideband_to_vdev(sb);
	ep = sb->eps[ep_index];

	/* validate that xhci didn't reallocate vdev during reset or resume */
	if (!vdev || !ep || ep->sideband != sb)
		goto out_unlock;

	if (remove) {
		ep->sideband = NULL;
		/* endpoint is removed, not running is not an error */
		ret = 0;
	}

	if (!ep->ring)
		goto out_unlock;

	/* Stop the endpoint if it's running */
	ctx = xhci_get_ep_ctx(sb->xhci, vdev->out_ctx, ep_index);
	if (GET_EP_CTX_STATE(ctx) == EP_STATE_RUNNING) {
		if (!cmd) {
			ret = -ENOMEM;
			goto out_unlock;
		}
		ret = xhci_queue_stop_endpoint(sb->xhci, cmd, vdev->slot_id,
					       ep_index, 0);
		if (ret)
			goto out_unlock;

		xhci_ring_cmd_db(sb->xhci);
		spin_unlock_irqrestore(&sb->xhci->lock, flags);
		wait_for_completion(cmd->completion);

		if (cmd->status == COMP_COMMAND_ABORTED ||
		    cmd->status == COMP_COMMAND_RING_STOPPED) {
			xhci_warn(sb->xhci, "Timeout while waiting for stop endpoint command\n");
			ret = -ETIME;
		}
		xhci_free_command(sb->xhci, cmd);
		return ret;
	}

out_unlock:
	spin_unlock_irqrestore(&sb->xhci->lock, flags);
	if (cmd)
		xhci_free_command(sb->xhci, cmd);

	return ret;
}

/* Caller must hold sb->mutex */
static void
__xhci_sideband_remove_interrupter(struct xhci_sideband *sb)
{
	lockdep_assert_held(&sb->mutex);

	if (!sb->ir)
		return;

	xhci_remove_secondary_interrupter(xhci_to_hcd(sb->xhci), sb->ir);
	sb->ir = NULL;
}

/* sideband api functions */

/**
 * xhci_sideband_notify_ep_ring_free - notify client of xfer ring free
 * @sb: sideband instance for this usb device
 * @ep_index: usb endpoint index
 *
 * Notifies the xHCI sideband client driver of a xHCI transfer ring free
 * routine.  This will allow for the client to ensure that all transfers
 * are completed.
 *
 * The callback should be synchronous, as the ring free happens after.
 */
void xhci_sideband_notify_ep_ring_free(struct xhci_sideband *sb,
				       unsigned int ep_index)
{
	struct xhci_sideband_event evt;

	evt.type = XHCI_SIDEBAND_XFER_RING_FREE;
	evt.evt_data = &ep_index;

	if (sb->notify_client)
		sb->notify_client(sb->intf, &evt);
}
EXPORT_SYMBOL_GPL(xhci_sideband_notify_ep_ring_free);

/**
 * xhci_sideband_add_endpoint - add endpoint to sideband access list
 * @sb: sideband instance for this usb device
 * @host_ep: usb host endpoint
 *
 * Adds an endpoint to the list of sideband accessed endpoints for this usb
 * device.
 * After an endpoint is added the sideband client can get the endpoint transfer
 * ring buffer by calling xhci_sideband_endpoint_buffer()
 *
 * Return: 0 on success, negative error otherwise.
 */
int
xhci_sideband_add_endpoint(struct xhci_sideband *sb,
			   struct usb_host_endpoint *host_ep)
{
	struct xhci_virt_device *vdev;
	struct xhci_virt_ep *ep;
	unsigned int ep_index;

	guard(mutex)(&sb->mutex);

	vdev = sideband_to_vdev(sb);
	if (!vdev)
		return -ENODEV;

	ep_index = xhci_get_endpoint_index(&host_ep->desc);
	ep = &vdev->eps[ep_index];

	if (ep->ep_state & EP_HAS_STREAMS)
		return -EINVAL;

	/*
	 * Note, we don't know the DMA mask of the audio DSP device, if its
	 * smaller than for xhci it won't be able to access the endpoint ring
	 * buffer. This could be solved by not allowing the audio class driver
	 * to add the endpoint the normal way, but instead offload it immediately,
	 * and let this function add the endpoint and allocate the ring buffer
	 * with the smallest common DMA mask
	 */
	if (sb->eps[ep_index] || ep->sideband)
		return -EBUSY;

	ep->sideband = sb;
	sb->eps[ep_index] = ep;

	return 0;
}
EXPORT_SYMBOL_GPL(xhci_sideband_add_endpoint);

/**
 * xhci_sideband_remove_endpoint - remove endpoint from sideband access list
 * @sb: sideband instance for this usb device
 * @host_ep: usb host endpoint
 *
 * Removes an endpoint from the list of sideband accessed endpoints for this usb
 * device.
 * sideband client should no longer touch the endpoint transfer buffer after
 * calling this.
 *
 * Return: 0 on success, negative error otherwise.
 */
int
xhci_sideband_remove_endpoint(struct xhci_sideband *sb,
			      struct usb_host_endpoint *host_ep)
{
	unsigned int ep_index;
	int ret;

	guard(mutex)(&sb->mutex);

	ep_index = xhci_get_endpoint_index(&host_ep->desc);

	ret = sideband_stop_endpoint_sync(sb, ep_index, true);
	sb->eps[ep_index] = NULL;

	return ret;
}
EXPORT_SYMBOL_GPL(xhci_sideband_remove_endpoint);

int
xhci_sideband_stop_endpoint(struct xhci_sideband *sb,
			    struct usb_host_endpoint *host_ep)
{
	unsigned int ep_index;

	guard(mutex)(&sb->mutex);

	ep_index = xhci_get_endpoint_index(&host_ep->desc);

	return sideband_stop_endpoint_sync(sb, ep_index, false);
}
EXPORT_SYMBOL_GPL(xhci_sideband_stop_endpoint);

/**
 * xhci_sideband_get_endpoint_buffer - gets the endpoint transfer buffer address
 * @sb: sideband instance for this usb device
 * @host_ep: usb host endpoint
 *
 * Returns the address of the endpoint buffer where xHC controller reads queued
 * transfer TRBs from. This is the starting address of the ringbuffer where the
 * sideband client should write TRBs to.
 *
 * Caller needs to free the returned sg_table
 *
 * Return: struct sg_table * if successful. NULL otherwise.
 */
struct sg_table *
xhci_sideband_get_endpoint_buffer(struct xhci_sideband *sb,
				  struct usb_host_endpoint *host_ep)
{
	struct xhci_virt_ep *ep;
	unsigned int ep_index;

	ep_index = xhci_get_endpoint_index(&host_ep->desc);
	ep = sb->eps[ep_index];

	if (!sideband_to_vdev(sb) || !ep || !ep->ring || ep->sideband != sb)
		return NULL;

	return xhci_ring_to_sgtable(sb, ep->ring);
}
EXPORT_SYMBOL_GPL(xhci_sideband_get_endpoint_buffer);

/**
 * xhci_sideband_get_event_buffer - return the event buffer for this device
 * @sb: sideband instance for this usb device
 *
 * If a secondary xhci interupter is set up for this usb device then this
 * function returns the address of the event buffer where xHC writes
 * the transfer completion events.
 *
 * Caller needs to free the returned sg_table
 *
 * Return: struct sg_table * if successful. NULL otherwise.
 */
struct sg_table *
xhci_sideband_get_event_buffer(struct xhci_sideband *sb)
{
	if (!sb || !sb->ir)
		return NULL;

	return xhci_ring_to_sgtable(sb, sb->ir->event_ring);
}
EXPORT_SYMBOL_GPL(xhci_sideband_get_event_buffer);

/**
 * xhci_sideband_check - check the existence of active sidebands
 * @hcd: the host controller driver associated with the target host controller
 *
 * Allow other drivers, such as usb controller driver, to check if there are
 * any sideband activity on the host controller. This information could be used
 * for power management or other forms of resource management. The caller should
 * ensure downstream usb devices are all marked as "offload_pm_locked" to ensure
 * the correctness of the return value.
 *
 * Returns true on any active sideband existence, false otherwise.
 */
bool xhci_sideband_check(struct usb_hcd *hcd)
{
	struct usb_device *udev = hcd->self.root_hub;
	bool active;

	usb_lock_device(udev);
	active = usb_offload_check(udev);
	usb_unlock_device(udev);

	return active;
}
EXPORT_SYMBOL_GPL(xhci_sideband_check);

/**
 * xhci_sideband_create_interrupter - creates a new interrupter for this sideband
 * @sb: sideband instance for this usb device
 * @num_seg: number of event ring segments to allocate
 * @ip_autoclear: IP autoclearing support such as MSI implemented
 *
 * Sets up a xhci interrupter that can be used for this sideband accessed usb
 * device. Transfer events for this device can be routed to this interrupters
 * event ring by setting the 'Interrupter Target' field correctly when queueing
 * the transfer TRBs.
 * Once this interrupter is created the interrupter target ID can be obtained
 * by calling xhci_sideband_interrupter_id()
 *
 * Returns 0 on success, negative error otherwise
 */
int
xhci_sideband_create_interrupter(struct xhci_sideband *sb, int num_seg,
				 bool ip_autoclear, u32 imod_interval, int intr_num)
{
	if (!sb || !sb->xhci)
		return -ENODEV;

	guard(mutex)(&sb->mutex);

	if (!sideband_to_vdev(sb))
		return -ENODEV;

	if (sb->ir)
		return -EBUSY;

	sb->ir = xhci_create_secondary_interrupter(xhci_to_hcd(sb->xhci),
						   num_seg, imod_interval,
						   intr_num);
	if (!sb->ir)
		return -ENOMEM;

	sb->ir->ip_autoclear = ip_autoclear;

	return 0;
}
EXPORT_SYMBOL_GPL(xhci_sideband_create_interrupter);

/**
 * xhci_sideband_remove_interrupter - remove the interrupter from a sideband
 * @sb: sideband instance for this usb device
 *
 * Removes a registered interrupt for a sideband.  This would allow for other
 * sideband users to utilize this interrupter.
 */
void
xhci_sideband_remove_interrupter(struct xhci_sideband *sb)
{
	if (!sb)
		return;

	guard(mutex)(&sb->mutex);

	__xhci_sideband_remove_interrupter(sb);
}
EXPORT_SYMBOL_GPL(xhci_sideband_remove_interrupter);

/**
 * xhci_sideband_interrupter_id - return the interrupter target id
 * @sb: sideband instance for this usb device
 *
 * If a secondary xhci interrupter is set up for this usb device then this
 * function returns the ID used by the interrupter. The sideband client
 * needs to write this ID to the 'Interrupter Target' field of the transfer TRBs
 * it queues on the endpoints transfer ring to ensure transfer completion event
 * are written by xHC to the correct interrupter event ring.
 *
 * Returns interrupter id on success, negative error othgerwise
 */
int
xhci_sideband_interrupter_id(struct xhci_sideband *sb)
{
	if (!sb || !sb->ir)
		return -ENODEV;

	return sb->ir->intr_num;
}
EXPORT_SYMBOL_GPL(xhci_sideband_interrupter_id);

/**
 * xhci_sideband_register - register a sideband for a usb device
 * @intf: usb interface associated with the sideband device
 *
 * Allows for clients to utilize XHCI interrupters and fetch transfer and event
 * ring parameters for executing data transfers.
 *
 * Return: pointer to a new xhci_sideband instance if successful. NULL otherwise.
 */
struct xhci_sideband *
xhci_sideband_register(struct usb_interface *intf, enum xhci_sideband_type type,
		       int (*notify_client)(struct usb_interface *intf,
				    struct xhci_sideband_event *evt))
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct usb_hcd *hcd = bus_to_hcd(udev->bus);
	struct xhci_hcd *xhci = hcd_to_xhci(hcd);
	struct xhci_virt_device *vdev;
	struct xhci_sideband *sb;

	/*
	 * Make sure the usb device is connected to a xhci controller.  Fail
	 * registration if the type is anything other than  XHCI_SIDEBAND_VENDOR,
	 * as this is the only type that is currently supported by xhci-sideband.
	 */
	if (!udev->slot_id || type != XHCI_SIDEBAND_VENDOR)
		return NULL;

	sb = kzalloc_node(sizeof(*sb), GFP_KERNEL, dev_to_node(hcd->self.sysdev));
	if (!sb)
		return NULL;

	mutex_init(&sb->mutex);

	/* check this device isn't already controlled via sideband */
	spin_lock_irq(&xhci->lock);

	vdev = xhci->devs[udev->slot_id];

	if (!vdev || vdev->sideband) {
		xhci_warn(xhci, "XHCI sideband for slot %d already in use\n",
			  udev->slot_id);
		spin_unlock_irq(&xhci->lock);
		kfree(sb);
		return NULL;
	}

	sb->xhci = xhci;
	sb->udev = udev;
	sb->intf = intf;
	sb->type = type;
	sb->notify_client = notify_client;
	vdev->sideband = sb;

	spin_unlock_irq(&xhci->lock);

	return sb;
}
EXPORT_SYMBOL_GPL(xhci_sideband_register);

/**
 * xhci_sideband_unregister - unregister sideband access to a usb device
 * @sb: sideband instance to be unregistered
 *
 * Unregisters sideband access to a usb device and frees the sideband
 * instance.
 * After this the endpoint and interrupter event buffers should no longer
 * be accessed via sideband. The xhci driver can now take over handling
 * the buffers.
 */
void
xhci_sideband_unregister(struct xhci_sideband *sb)
{
	struct xhci_virt_device *vdev;
	struct xhci_hcd *xhci;
	int i;

	if (!sb)
		return;

	xhci = sb->xhci;

	mutex_lock(&sb->mutex);

	for (i = 0; i < EP_CTX_PER_DEV; i++) {
		if (sb->eps[i]) {
			sideband_stop_endpoint_sync(sb, i, true);
			sb->eps[i] = NULL;
		}
	}
	__xhci_sideband_remove_interrupter(sb);

	spin_lock_irq(&xhci->lock);
	vdev = sideband_to_vdev(sb);
	if (vdev)
		vdev->sideband = NULL;
	sb->udev = NULL;
	sb->xhci = NULL;
	spin_unlock_irq(&xhci->lock);

	mutex_unlock(&sb->mutex);

	kfree(sb);
}
EXPORT_SYMBOL_GPL(xhci_sideband_unregister);
MODULE_DESCRIPTION("xHCI sideband driver for secondary interrupter management");
MODULE_LICENSE("GPL");
