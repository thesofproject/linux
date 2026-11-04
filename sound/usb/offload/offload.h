/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Internal definitions for the USB audio offload library.
 *
 * The host callback table lives in <sound/usb-offload.h>; the host turns
 * those callbacks into IPC messages. The ASoC USB framework port is not
 * owned here: the host owns the snd_soc_usb port and drives the
 * connection callbacks, the library only forwards hotplug events through
 * ops->notify_connect / notify_disconnect.
 */

#ifndef __SOUND_USB_OFFLOAD_LIB_H
#define __SOUND_USB_OFFLOAD_LIB_H

#include <sound/usb-offload.h>

/*
 * One offload engine. Returned to the host by usb_offload_register() and
 * passed back in to every other API call.
 */
struct usb_offload_ctx {
	struct device			*dev;
	const struct usb_offload_ops	*ops;
};

/* Where the host wants a stream's endpoint placed (called from usb-audio.c) */
int  usb_offload_resource_range(struct usb_offload_ctx *ctx, int direction,
				bool feedback, unsigned int *first,
				unsigned int *last);

/*
 * Candidate filter + USB-audio device ownership (usb-audio.c)
 *
 * No dedicated USB driver claims the device: snd-usb-audio keeps
 * ownership and notifies us via snd_usb_platform_ops. We check the
 * PCMs it has set up and bridge the hotplug event into the library for
 * offloadable devices.
 */

/*
 * USB hotplug bridge: how the usb-audio side hands a device to the library,
 * which propagates it to the host via ops->notify_connect / notify_disconnect.
 */
int usb_offload_connect(struct usb_offload_ctx *ctx, struct usb_device *udev,
			const struct usb_offload_usb_dev_info *id);
void usb_offload_disconnect(struct usb_offload_ctx *ctx, struct usb_device *udev);

/*
 * Register / unregister the snd_usb_platform_ops bridge. init() is called
 * from usb_offload_register(), exit() from usb_offload_unregister(). Both
 * manage the per-device offload records.
 */
int  usb_offload_usb_audio_init(struct usb_offload_ctx *ctx);
void usb_offload_usb_audio_exit(struct usb_offload_ctx *ctx);

/*
 * Re-emit connect events for USB audio devices that enumerated before the
 * platform ops were registered. Wraps the usb-audio core's
 * snd_usb_rediscover_devices(), which owns the private sound/usb headers; the
 * connect callback is idempotent so this is safe to call repeatedly.
 */
void usb_offload_usb_audio_rediscover(void);

#endif /* __SOUND_USB_OFFLOAD_LIB_H */
