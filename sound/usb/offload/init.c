// SPDX-License-Identifier: GPL-2.0-only
/*
 * USB audio offload library - top level lifecycle.
 *
 * Exported in the USB_AUDIO_OFFLOAD module namespace.
 */
#include <linux/device.h>
#include <linux/err.h>
#include <linux/export.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/usb.h>

#include "offload.h"

/**
 * usb_offload_register - register an offload engine with the library
 * @res: the host's device and callbacks
 *
 * Starts tracking the offload capable USB audio devices for the host and
 * reports the ones that are already connected through res->ops.
 *
 * Return: the context to pass to the other API calls, or an ERR_PTR().
 */
struct usb_offload_ctx *usb_offload_register(const struct usb_offload_res *res)
{
	struct usb_offload_ctx *ctx;
	int ret;

	if (!res || !res->dev || !res->ops || !res->ops->notify_connect ||
	    !res->ops->notify_disconnect)
		return ERR_PTR(-EINVAL);

	ctx = kzalloc_obj(*ctx);
	if (!ctx)
		return ERR_PTR(-ENOMEM);

	ctx->dev = res->dev;
	ctx->ops = res->ops;

	ret = usb_offload_usb_audio_init(ctx);
	if (ret) {
		kfree(ctx);
		return ERR_PTR(ret);
	}

	/*
	 * Backfill the USB audio devices that enumerated before the bridge
	 * registered, e.g. when the DSP came up after USB enumeration finished.
	 */
	usb_offload_usb_audio_rediscover();

	return ctx;
}
EXPORT_SYMBOL_NS_GPL(usb_offload_register, "USB_AUDIO_OFFLOAD");

/**
 * usb_offload_unregister - unregister an offload engine
 * @ctx: the context usb_offload_register() returned
 *
 * Gives every stream still held back to the host, reports the devices as
 * disconnected and frees the context.
 */
void usb_offload_unregister(struct usb_offload_ctx *ctx)
{
	if (IS_ERR_OR_NULL(ctx))
		return;

	usb_offload_usb_audio_exit(ctx);
	kfree(ctx);
}
EXPORT_SYMBOL_NS_GPL(usb_offload_unregister, "USB_AUDIO_OFFLOAD");

/*
 * Hotplug bridge - called by the USB SND side
 *
 * USB audio devices appear and disappear at runtime. Each event is
 * delivered to the host via ops->notify_connect / notify_disconnect. On
 * connect the USB-SND device identity (@id, card / chip / PCM indices)
 * travels with the event so the host can build a struct snd_soc_usb_device
 * without reaching into the USB-audio core.
 */
int usb_offload_connect(struct usb_offload_ctx *ctx, struct usb_device *udev,
			const struct usb_offload_usb_dev_info *id)
{
	int ret;

	ret = ctx->ops->notify_connect(ctx->dev, udev, id);
	if (ret) {
		dev_warn(ctx->dev, "USB offload: %s: connect rejected: %d\n",
			 dev_name(&udev->dev), ret);
		return ret;
	}

	dev_dbg(ctx->dev, "USB offload: %s connected\n", dev_name(&udev->dev));
	return 0;
}

void usb_offload_disconnect(struct usb_offload_ctx *ctx, struct usb_device *udev)
{
	ctx->ops->notify_disconnect(ctx->dev, udev);

	dev_dbg(ctx->dev, "USB offload: %s disconnected\n", dev_name(&udev->dev));
}

/*
 * Resource placement
 *
 * Runs from the stream acquisition path, under the usb-audio side's lock.
 */
int usb_offload_resource_range(struct usb_offload_ctx *ctx, int direction,
			       unsigned int *first, unsigned int *last)
{
	if (ctx->ops->get_resource_range)
		return ctx->ops->get_resource_range(ctx->dev, direction,
						    first, last);

	*first = 0;
	*last = 0;

	return 0;
}

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("USB audio offload library");
