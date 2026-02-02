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
#include <linux/pci.h>

#include "xhci.h"

/* sideband internal helpers */
static int
sideband_res_id_to_ep_index(struct xhci_sideband *sb, unsigned int res_id)
{
	int i;

	if (res_id == 0)
		return -EINVAL;

	for (i = 0; i < EP_CTX_PER_DEV; i++) {
		if (sb->eps_res_id[i] == res_id)
			return i;
	}

	return -ENODEV;
}

static int
xhci_sideband_hw_max_resources(struct xhci_hcd *xhci)
{
	union xhci_extprop_ctx *ctx;
	unsigned int res_max;
	int ret;

	ctx = kzalloc_obj(*ctx, GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ret = xhci_get_ext_property(xhci, 0, 0, PROP_ECI_AUDIO,
				    AUDIO_SUBTYPE_GET_RES, ctx, sizeof(*ctx));
	if (ret < 0) {
		kfree(ctx);
		return ret;
	}

	res_max = ctx->generic.byte[0];
	kfree(ctx);

	/* Intel hosts sideband resources are capped at 6 */
	if (xhci->quirks & XHCI_INTEL_HOST)
		res_max = min(res_max, 6);

	return res_max;
}

static bool
xhci_sideband_hw_capable(struct xhci_hcd *xhci)
{
	u16 buf = 0;
	int res_max;
	int ret;

	/* fastpath, all checks are done if resource limits are set */
	if (xhci->sideband_idr.id_lim.max > 0)
		return true;

	/* xHCI 1.2 and later can support GSC bit in hcc_params2 register */
	if ((xhci->hci_version < 0x120) || !(xhci->hcc_params2 & HCC2_GSC))
		return false;
	/*
	 * Note, normally xHC hardware writes directly to buf, so buf needs to
	 * be DMA mappable. PROP_ECI_ALL is special and returns 16 bits data in
	 * the command completion event, which is then copied to buf.
	 */
	ret = xhci_get_ext_property(xhci, 0, 0, PROP_ECI_ALL, 0, &buf, sizeof(buf));
	if (ret < 0 || !(buf & PROP_ECI_AUDIO))
		return false;

	/* check xHC has at least one resource for endpoint sideband use */
	res_max = xhci_sideband_hw_max_resources(xhci);
	if (res_max > 0) {
		xhci->sideband_idr.id_lim.min = 1;
		xhci->sideband_idr.id_lim.max = res_max;
		return true;
	}

	return false;
}

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

static bool
xhci_sideband_endpoint_belongs(struct xhci_sideband *sb,
			       struct usb_host_endpoint *host_ep)
{
	unsigned int epnum;

	if (!sb || !host_ep)
		return false;

	epnum = usb_endpoint_num(&host_ep->desc);

	if (usb_endpoint_dir_in(&host_ep->desc))
		return sb->udev->ep_in[epnum] == host_ep;

	return sb->udev->ep_out[epnum] == host_ep;
}

/*
 * Return the current virt ep of sb->udev at ep_index,
 * or NULL if vdev was freed or reallocated
 */
static struct xhci_virt_ep *
sideband_to_ep(struct xhci_sideband *sb, unsigned int ep_index)
{
	struct xhci_virt_device *vdev;

	lockdep_assert_held(&sb->mutex);
	lockdep_assert_held(&sb->xhci->lock);

	vdev = sideband_to_vdev(sb);
	if (!vdev)
		return NULL;

	return &vdev->eps[ep_index];
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

/*
 * Hand an endpoint over to the xHC audio sideband using resource @res_id, or
 * give it back to xhci if @res_id is 0 (xHCI 7.9.2.1). The vdev is validated
 * under the same xhci->lock that queues the command. ep->sideband and
 * sb->eps[] are only updated once the command completed successfully and the
 * vdev has been validated again.
 *
 * Caller must hold sb->mutex
 */
static int
sideband_set_ep_resource_sync(struct xhci_sideband *sb, unsigned int ep_index,
			      u32 res_id)
{
	struct xhci_command *cmd;
	struct xhci_virt_ep *ep;
	unsigned long flags;
	int ret = 0;

	lockdep_assert_held(&sb->mutex);

	cmd = xhci_alloc_command(sb->xhci, true, GFP_KERNEL);
	if (!cmd)
		ret = -ENOMEM;

	spin_lock_irqsave(&sb->xhci->lock, flags);

	/* validate that xhci didn't free or reallocate vdev */
	ep = sideband_to_ep(sb, ep_index);
	if (!ep) {
		ret = -ENODEV;
		goto out_unlock;
	}

	/* fail add if ep is already in sideband use, or uses streams */
	if (res_id) {
		if (ep->sideband)
			ret = -EBUSY;
		else if (ep->ep_state & EP_HAS_STREAMS)
			ret = -EINVAL;
	} else if (ep->sideband != sb) {
		/* fail remove if endpoint isn't part of this sideband */
		ret = -ENODEV;
	}

	if (ret)
		goto out_unlock;

	ret = xhci_queue_set_ext_property(sb->xhci, cmd, sb->udev->slot_id,
					  ep_index + 1, 0, PROP_ECI_AUDIO,
					  AUDIO_SUBTYPE_SET_RES, res_id);
	if (ret)
		goto out_unlock;

	xhci_ring_cmd_db(sb->xhci);
	spin_unlock_irqrestore(&sb->xhci->lock, flags);

	wait_for_completion(cmd->completion);

	spin_lock_irqsave(&sb->xhci->lock, flags);
	ep = sideband_to_ep(sb, ep_index);
	if (!ep) {
		ret = -ENODEV;
		goto out_unlock;
	}

	if (cmd->status != COMP_SUCCESS) {
		ret = -EIO;
		goto out_unlock;
	}

	ep->sideband = res_id ? sb : NULL;
	sb->eps[ep_index] = res_id ? ep : NULL;

out_unlock:
	spin_unlock_irqrestore(&sb->xhci->lock, flags);
	if (cmd)
		xhci_free_command(sb->xhci, cmd);

	return ret;
}

/* Caller must hold sb->mutex */
static int
__xhci_sideband_remove_endpoint(struct xhci_sideband *sb, unsigned int ep_index)
{
	int ret = 0;

	lockdep_assert_held(&sb->mutex);

	if (sb->type == XHCI_SIDEBAND_VENDOR) {
		ret = sideband_stop_endpoint_sync(sb, ep_index, true);
		sb->eps[ep_index] = NULL;
	}

	if (sb->type == XHCI_SIDEBAND_AUDIO) {
		ret = sideband_set_ep_resource_sync(sb, ep_index, 0);
		/* removed on success or when the endpoint is no longer ours */
		if (ret == 0 || ret == -ENODEV)
			sb->eps[ep_index] = NULL;
	}

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
 * xhci_sideband_get_ep_properties - query xHC properties for an endpoint
 * @sb: sideband instance for this usb device
 * @host_ep: usb host endpoint
 * @properties: returned properties.
 *
 * Return: 0 on success, negative error otherwise
 */
int
xhci_sideband_get_ep_properties(struct xhci_sideband *sb,
				struct usb_host_endpoint *host_ep,
				struct xhci_sideband_ep_properties *properties)
{
	union xhci_extprop_ctx *ctx;
	unsigned int ep_index;
	struct device *dev;
	u32 ep_id;
	int ret = 0;

	guard(mutex)(&sb->mutex);

	if (!sideband_to_vdev(sb))
		return -ENODEV;

	if (!xhci_sideband_endpoint_belongs(sb, host_ep))
		return -EINVAL;

	if (sb->type != XHCI_SIDEBAND_AUDIO || !properties)
		return -EINVAL;

	memset(properties, 0, sizeof(*properties));
	ep_index = xhci_get_endpoint_index(&host_ep->desc);
	dev = xhci_to_hcd(sb->xhci)->self.controller;
	ep_id = ep_index + 1;

	/* get the PCI B:D:F data of parent xHC */
	if (dev_is_pci(dev)) {
		struct pci_dev *pdev = to_pci_dev(dev);

		properties->bus = pdev->bus->number;
		properties->device = PCI_SLOT(pdev->devfn);
		properties->function = PCI_FUNC(pdev->devfn);
	}

	properties->slot_id = sb->udev->slot_id;

	properties->reserved_res_id = sb->eps_res_id[ep_index];

	/* no resource id set for endpoint, don't query ep property */
	if (properties->reserved_res_id == 0)
		return 0;

	ctx = kzalloc_obj(*ctx, GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	/* query extended endpoint property from xHC */
	ret = xhci_get_ext_property(sb->xhci, sb->udev->slot_id, ep_id,
				    PROP_ECI_AUDIO, AUDIO_SUBTYPE_GET_EP_PROP,
				    ctx, sizeof(*ctx));
	if (!ret) {
		properties->rh_port = ctx->audio_ep.rh_port;
		properties->res_id = ctx->audio_ep.res_id;
		properties->port_speed = ctx->audio_ep.port_speed_tt & 0xf;
		properties->tt = !!(ctx->audio_ep.port_speed_tt & 0x10);
	}

	kfree(ctx);

	return ret;
}
EXPORT_SYMBOL_GPL(xhci_sideband_get_ep_properties);

/**
 * xhci_sideband_alloc_resource_id - reserve a resource id to offload an endpoint
 * @sb: sideband instance for this usb device
 * @host_ep: usb host endpoint
 * @min: lowest resource id acceptable to the caller, 0 for no preference
 * @max: highest resource id acceptable to the caller, 0 for no preference
 *
 * Reserves a sideband resource id for this endpoint from the limited set of
 * xHC resource ids. Only valid for XHCI_SIDEBAND_AUDIO sideband types.
 * Once an endpoint has a valid resource id then it can be assigned to audio
 * sideband by calling xhci_sideband_add_endpoint()
 *
 * The xHC limits which resources an endpoint of a direction may use. An
 * offload engine which ties a function to particular resources narrows that
 * further with @min and @max, which have to lie within the xHC's limits for
 * the endpoint; the first free resource of the intersection is reserved.
 * With both 0 the first free resource the xHC allows is reserved.
 *
 * An endpoint holds one reservation at a time; it has to be released with
 * xhci_sideband_free_resource_id() before another one can be made.
 *
 * Returns: positive resource id on success, -EBUSY if all resources of the
 * range are taken or the endpoint already holds a reservation, negative
 * error otherwise.
 */
int
xhci_sideband_alloc_resource_id(struct xhci_sideband *sb,
				struct usb_host_endpoint *host_ep,
				unsigned int min, unsigned int max)
{
	struct xhci_sideband_idr *res;
	struct xa_limit lim;
	unsigned int ep_index;
	unsigned int res_id;
	int ret;

	if (!xhci_sideband_endpoint_belongs(sb, host_ep))
		return -EINVAL;

	guard(mutex)(&sb->mutex);

	if (sb->type != XHCI_SIDEBAND_AUDIO)
		return -EINVAL;

	ep_index = xhci_get_endpoint_index(&host_ep->desc);
	res = &sb->xhci->sideband_idr;
	lim = res->id_lim;

	/*
	 * Intel xHC hardware limits resource IDs 1 and 2 to output endpoints.
	 * IDs 3-6 are for input endpoints
	 */
	if (sb->xhci->quirks & XHCI_INTEL_HOST) {
		if (usb_endpoint_dir_out(&host_ep->desc))
			lim.max = min(lim.max, 2);
		else
			lim.min = 3;
	}

	if (min) {
		if (min < lim.min || min > lim.max)
			return -EINVAL;
		lim.min = min;
	}

	if (max) {
		if (max < lim.min || max > lim.max)
			return -EINVAL;
		lim.max = max;
	}

	if (sb->eps_res_id[ep_index]) {
		dev_dbg(&sb->udev->dev, "ep %u already holds resource id %u\n",
			ep_index, sb->eps_res_id[ep_index]);
		return -EBUSY;
	}

	ret = xa_alloc(&res->id_xa, &res_id, sb, lim, GFP_KERNEL);
	if (ret)
		return ret;

	sb->eps_res_id[ep_index] = res_id;

	dev_dbg(&sb->udev->dev, "allocated sideband resource %u for ep %u\n",
		res_id, ep_index);

	return res_id;
}
EXPORT_SYMBOL_GPL(xhci_sideband_alloc_resource_id);

/**
 * xhci_sideband_free_resource_id - release a resource id
 * @sb: sideband instance for this usb device
 * @res_id: resource id to free
 *
 * Free a previously reserved sideband resource id, making it available for
 * endpoints. Don't release the resource id if the endpoint is in active
 * sideband use.
 *
 * Returns: 0 on success, negative error otherwise.
 */
int
xhci_sideband_free_resource_id(struct xhci_sideband *sb,
			       unsigned int res_id)
{
	struct xhci_sideband_idr *res;
	int ep_index;

	if (!sb)
		return -EINVAL;

	res = &sb->xhci->sideband_idr;

	guard(mutex)(&sb->mutex);

	ep_index = sideband_res_id_to_ep_index(sb, res_id);
	if (ep_index < 0)
		return -ENODEV;

	if (sb != xa_load(&res->id_xa, res_id))
		return -ENODEV;
	if (sb->eps[ep_index])
		return -EBUSY;

	xa_erase(&res->id_xa, res_id);
	sb->eps_res_id[ep_index] = 0;

	dev_dbg(&sb->udev->dev, "erase ep %u resource id %u\n",
		ep_index, res_id);

	return 0;
}
EXPORT_SYMBOL_GPL(xhci_sideband_free_resource_id);

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
	struct xhci_virt_ep *ep;
	unsigned int ep_index;
	unsigned int res_id;
	unsigned long flags;
	int ret = 0;

	guard(mutex)(&sb->mutex);

	if (!xhci_sideband_endpoint_belongs(sb, host_ep))
		return -EINVAL;

	ep_index = xhci_get_endpoint_index(&host_ep->desc);

	if (sb->eps[ep_index])
		return -EBUSY;

	if (sb->type == XHCI_SIDEBAND_AUDIO) {
		res_id = sb->eps_res_id[ep_index];
		if (res_id == 0) {
			dev_dbg(&sb->udev->dev,
				"Can't offload endpoint %u, missing resource id\n",
				ep_index);
			return -EINVAL;
		}

		return sideband_set_ep_resource_sync(sb, ep_index, res_id);
	}

	/*
	 * Note, we don't know the DMA mask of the audio DSP device, if its
	 * smaller than for xhci it won't be able to access the endpoint ring
	 * buffer. This could be solved by not allowing the audio class driver
	 * to add the endpoint the normal way, but instead offload it immediately,
	 * and let this function add the endpoint and allocate the ring buffer
	 * with the smallest common DMA mask
	 */
	spin_lock_irqsave(&sb->xhci->lock, flags);

	ep = sideband_to_ep(sb, ep_index);
	if (!ep) {
		ret = -ENODEV;
	} else if (ep->sideband) {
		ret = -EBUSY;
	} else if (ep->ep_state & EP_HAS_STREAMS) {
		ret = -EINVAL;
	} else {
		/* success */
		ep->sideband = sb;
		sb->eps[ep_index] = ep;
	}

	spin_unlock_irqrestore(&sb->xhci->lock, flags);

	return ret;
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

	guard(mutex)(&sb->mutex);

	ep_index = xhci_get_endpoint_index(&host_ep->desc);

	return __xhci_sideband_remove_endpoint(sb, ep_index);
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

	/* Make sure the usb device is connected to a xhci controller. */
	if (!udev->slot_id)
		return NULL;

	switch (type) {
	case XHCI_SIDEBAND_VENDOR:
		break;
	case XHCI_SIDEBAND_AUDIO:
		if (!xhci_sideband_hw_capable(xhci)) {
			dev_info(&udev->dev, "Host not sideband capable\n");
			return NULL;
		}
		break;
	default:
		return NULL;
	}

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

	dev_dbg(&udev->dev, "Registered for sideband use\n");

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
	struct xhci_virt_ep *ep;
	struct usb_device *udev;
	struct xhci_hcd *xhci;
	int res_id;
	int i;

	if (!sb)
		return;

	xhci = sb->xhci;
	udev = sb->udev;

	mutex_lock(&sb->mutex);

	for (i = 0; i < EP_CTX_PER_DEV; i++) {
		if (sb->eps[i])
			__xhci_sideband_remove_endpoint(sb, i);

		if (sb->type != XHCI_SIDEBAND_AUDIO)
			continue;

		res_id = sb->eps_res_id[i];
		if (res_id && sb == xa_load(&xhci->sideband_idr.id_xa, res_id)) {
			dev_dbg(&udev->dev, "%s erase ep %u res_id %u\n",
				__func__, i, res_id);
			xa_erase(&xhci->sideband_idr.id_xa, res_id);
		}
	}

	__xhci_sideband_remove_interrupter(sb);

	spin_lock_irq(&xhci->lock);

	/* clear endpoint sideband pointers for endpoints that failed removal */
	for (i = 0; i < EP_CTX_PER_DEV; i++) {
		ep = sideband_to_ep(sb, i);
		if (ep && ep->sideband == sb)
			ep->sideband = NULL;
		sb->eps[i] = NULL;
	}
	vdev = sideband_to_vdev(sb);
	if (vdev)
		vdev->sideband = NULL;
	sb->udev = NULL;
	sb->xhci = NULL;
	spin_unlock_irq(&xhci->lock);

	mutex_unlock(&sb->mutex);

	dev_dbg(&udev->dev, "sideband unregistered\n");

	kfree(sb);
}
EXPORT_SYMBOL_GPL(xhci_sideband_unregister);
MODULE_DESCRIPTION("xHCI sideband driver for secondary interrupter management");
MODULE_LICENSE("GPL");
