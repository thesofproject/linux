// SPDX-License-Identifier: GPL-2.0-only
/*
 * USB audio offload library - USB-audio device ownership + candidate filter.
 *
 * snd-usb-audio stays in charge of the device. This file hooks the
 * snd_usb_platform_ops connect/disconnect notifications of the usb-audio
 * core (sound/usb/card.c), checks the device and the PCMs usb-audio set up to decide
 * whether a device can be offloaded, and for the candidates it bridges the
 * hotplug event into the library via usb_offload_connect() /
 * usb_offload_disconnect().
 *
 * The xHCI audio sideband resources and the DSP gateway configuration
 * happen later, on the PCM data path: usb_offload_acquire_stream() starts
 * the stream on the USB side and claims the endpoint for the DSP.
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/usb/audio.h>
#include <linux/usb/xhci-sideband.h>
#include <uapi/linux/usb/audio.h>
#include <uapi/linux/usb/ch9.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>

/* PORTSC Port Speed ID of a high-speed root port, xHCI 7.2.2.1.1 */
#define USB_OFFLOAD_PORT_SPEED_HIGH	3

/*
 * snd_usb_audio and the snd_usb_platform_ops bridge live in the private
 * usb-audio headers one directory up. The Qualcomm in-tree consumer
 * (sound/usb/qcom/) includes them the same way.
 */
#include "../usbaudio.h"
#include "../card.h"
#include "../endpoint.h"
#include "../pcm.h"
#include "../power.h"

#include "offload.h"

static bool offload_disable;
module_param(offload_disable, bool, 0444);
MODULE_PARM_DESC(offload_disable,
		 "Disable USB audio offload candidate selection (default: 0)");

static bool allow_feedback;
module_param(allow_feedback, bool, 0444);
MODULE_PARM_DESC(allow_feedback,
		 "Offload asynchronous playback streams also when the host keeps them (default: 0)");

/*
 * An OUT endpoint with implicit feedback is clocked by a capture data endpoint
 * the DSP cannot use, and the hardware has no way to follow it: that stream
 * would run blind. It always stays on the host. A quirk can give an endpoint
 * which is not declared asynchronous implicit feedback, so that is checked
 * for on its own.
 *
 * An asynchronous OUT endpoint runs off the device's own clock and needs a
 * feedback source for the sender to follow it. A host whose DSP applies that
 * feedback in a way which degrades the audio keeps such streams to itself
 * (usb_offload_res.async_playback_on_host), unless allowed for testing.
 */
static bool usb_offload_playback_kept(const struct usb_offload_ctx *ctx,
				      const struct audioformat *fp)
{
	if (fp->implicit_fb)
		return true;

	if (!ctx->async_playback_on_host || allow_feedback)
		return false;

	return (fp->ep_attr & USB_ENDPOINT_SYNCTYPE) == USB_ENDPOINT_SYNC_ASYNC;
}

/*
 * The capture endpoint a playback format with implicit feedback syncs to is
 * that playback's clock. Giving it to the DSP would leave a playback stream
 * which the host keeps without one, so the host keeps such a capture format
 * as well.
 *
 * A capture format with implicit feedback of its own, which a quirk gives to
 * a device that only captures while its playback endpoint streams, stays on
 * the host too: the DSP only gets the capture endpoint and nothing would
 * stream the playback one.
 */
static bool usb_offload_fmt_offloadable(const struct usb_offload_ctx *ctx,
					struct snd_usb_audio *chip,
					const struct audioformat *fp, int direction)
{
	const struct audioformat *pfp;
	struct snd_usb_stream *as;

	if (direction == SNDRV_PCM_STREAM_PLAYBACK)
		return !usb_offload_playback_kept(ctx, fp);

	if (fp->implicit_fb)
		return false;

	list_for_each_entry(as, &chip->pcm_list, list) {
		/* the list of a direction a stream does not have is not set up */
		if (!as->substream[SNDRV_PCM_STREAM_PLAYBACK].num_formats)
			continue;

		list_for_each_entry(pfp, &as->substream[SNDRV_PCM_STREAM_PLAYBACK].fmt_list, list) {
			if (pfp->implicit_fb && pfp->sync_ep == fp->endpoint &&
			    usb_offload_playback_kept(ctx, pfp))
				return false;
		}
	}

	return true;
}

static bool usb_offload_subs_offloadable(const struct usb_offload_ctx *ctx,
					 const struct snd_usb_substream *subs)
{
	const struct audioformat *fp;

	list_for_each_entry(fp, &subs->fmt_list, list)
		if (usb_offload_fmt_offloadable(ctx, subs->stream->chip, fp,
						subs->direction))
			return true;

	return false;
}

/*
 * Candidate filter
 *
 * snd-usb-audio has parsed the descriptors by the time the connect callback
 * runs and left the result in chip->pcm_list, with the malformed and the
 * unsupported streams already left out. All that is left to decide here is
 * whether the device itself can be offloaded; which of its PCMs can is up to
 * usb_offload_ua_build_id().
 */
static bool usb_offload_chip_is_candidate(struct snd_usb_audio *chip)
{
	if (offload_disable)
		return false;

	switch (chip->dev->speed) {
	case USB_SPEED_FULL:
	case USB_SPEED_HIGH:
		break;
	default:
		return false;
	}

	/* the audio function the sideband is registered on */
	return chip->ctrl_intf &&
	       chip->ctrl_intf->desc.bInterfaceClass == USB_CLASS_AUDIO;
}

/*
 * Device-ownership bridge (snd_usb_platform_ops)
 *
 * One module-global registration; the usb-audio core allows only a
 * single set of platform ops. We map each offloaded snd_usb_audio chip
 * to a record of its offload state.
 */

/**
 * struct usb_offload_ua_stream - one acquired stream of a device.
 * @node:		link on usb_offload_dev.streams.
 * @subs:		the claimed usb-audio substream.
 * @direction:		SNDRV_PCM_STREAM_* of the stream.
 * @pcm_index:		USB SND PCM device number of @subs.
 * @resource:		sideband resource ID of the data endpoint.
 * @fb_resource:	sideband resource ID of the feedback endpoint, 0 if none.
 * @data_host_ep:	the data endpoint.
 * @feedback_host_ep:	the feedback endpoint, NULL if none.
 *
 * A device can have several PCMs per direction, and each can be offloaded
 * on a stream of its own at the same time.
 */
struct usb_offload_ua_stream {
	struct list_head		node;
	struct snd_usb_substream	*subs;
	int				direction;
	int				pcm_index;
	u8				resource;
	u8				fb_resource;
	struct usb_host_endpoint	*data_host_ep;
	struct usb_host_endpoint	*feedback_host_ep;
};

/**
 * struct usb_offload_dev - per-offloaded-device record.
 * @node:		link on usb_offload_ua_devs.
 * @chip:		the owning usb-audio chip.
 * @udev:		shortcut to chip->dev.
 * @sb:			xHCI sideband instance for this device.
 * @connected:		true once usb_offload_connect() succeeded.
 * @streams:		the streams currently acquired on the device.
 */
struct usb_offload_dev {
	struct list_head		node;
	struct snd_usb_audio		*chip;
	struct usb_device		*udev;
	struct xhci_sideband		*sb;
	bool				connected;
	struct list_head		streams;
};

static DEFINE_MUTEX(usb_offload_ua_lock);
static struct usb_offload_ctx *usb_offload_ua_ctx;	/* active offload ctx */
static LIST_HEAD(usb_offload_ua_devs);

/* Caller holds usb_offload_ua_lock. */
static struct usb_offload_dev *usb_offload_ua_find(struct snd_usb_audio *chip)
{
	struct usb_offload_dev *dev;

	list_for_each_entry(dev, &usb_offload_ua_devs, node)
		if (dev->chip == chip)
			return dev;
	return NULL;
}

/* Caller holds usb_offload_ua_lock. */
static struct usb_offload_dev *usb_offload_ua_find_udev(struct usb_device *udev)
{
	struct usb_offload_dev *dev;

	list_for_each_entry(dev, &usb_offload_ua_devs, node)
		if (dev->udev == udev)
			return dev;
	return NULL;
}

/* Caller holds usb_offload_ua_lock. */
static struct usb_offload_ua_stream *
usb_offload_ua_find_stream(struct usb_offload_dev *dev, int direction, int pcm_index)
{
	struct usb_offload_ua_stream *stream;

	list_for_each_entry(stream, &dev->streams, node)
		if (stream->direction == direction && stream->pcm_index == pcm_index)
			return stream;
	return NULL;
}

static void usb_offload_fill_ep(struct usb_offload_ep_info *desc,
				const struct usb_host_endpoint *host_ep)
{
	desc->ep_addr = host_ep->desc.bEndpointAddress;
	desc->maxpacket = usb_endpoint_maxp(&host_ep->desc);
	desc->interval = host_ep->desc.bInterval;
}

/*
 * Start the device streaming so that its endpoints, and with them their
 * xHCI transfer rings, exist. This is what snd_usb_pcm_prepare() does for
 * a host driven stream. Caller holds usb_offload_ua_lock.
 */
static int usb_offload_ua_start_stream(struct usb_offload_dev *dev,
				       struct snd_usb_substream *subs,
				       struct snd_pcm_hw_params *params)
{
	struct snd_usb_audio *chip = dev->chip;
	int ret;

	ret = snd_usb_offload_claim_substream(subs);
	if (ret)
		return ret;

	ret = snd_usb_autoresume(chip);
	if (ret < 0)
		goto err_release;

	ret = snd_usb_hw_params(subs, params);
	if (ret < 0)
		goto err_suspend;

	if (atomic_read(&chip->shutdown)) {
		ret = -ENODEV;
		goto err_hw_free;
	}

	{
		CLASS(snd_usb_lock, pm)(chip);

		if (pm.err < 0) {
			ret = pm.err;
			goto err_hw_free;
		}

		if (subs->sync_endpoint) {
			ret = snd_usb_endpoint_prepare(chip, subs->sync_endpoint);
			if (ret < 0)
				goto err_hw_free;
		}

		ret = snd_usb_endpoint_prepare(chip, subs->data_endpoint);
		if (ret < 0)
			goto err_hw_free;
	}

	return 0;

err_hw_free:
	snd_usb_hw_free(subs);
err_suspend:
	snd_usb_autosuspend(chip);
err_release:
	snd_usb_offload_release_substream(subs);

	return ret;
}

/* Caller holds usb_offload_ua_lock. */
static void usb_offload_ua_stop_stream(struct usb_offload_dev *dev,
				       struct snd_usb_substream *subs)
{
	snd_usb_hw_free(subs);
	snd_usb_autosuspend(dev->chip);
	snd_usb_offload_release_substream(subs);
}

/*
 * Detach @host_ep from its sideband resource and free the resource. -ENODEV
 * means the endpoint is not attached to the sideband (any more), nothing
 * holds on to the resource then. On any other failure the xHC may still have
 * the endpoint on the resource, so the reservation is kept rather than given
 * to the next stream: the endpoint cannot be offloaded again until the
 * sideband goes away with the device, but no two endpoints share a resource.
 */
static void usb_offload_ua_unclaim_ep(struct usb_offload_dev *dev,
				      struct usb_host_endpoint *host_ep, int res_id)
{
	int ret;

	ret = xhci_sideband_remove_endpoint(dev->sb, host_ep);
	if (ret && ret != -ENODEV) {
		dev_warn(&dev->udev->dev,
			 "USB offload: ep %#x not detached (%d), keeping resource %d\n",
			 host_ep->desc.bEndpointAddress, ret, res_id);
		return;
	}

	ret = xhci_sideband_free_resource_id(dev->sb, res_id);
	if (ret)
		dev_warn(&dev->udev->dev,
			 "USB offload: ep %#x resource %d not freed: %d\n",
			 host_ep->desc.bEndpointAddress, res_id, ret);
}

/*
 * Detach the stream's endpoints from their sideband resources, give the
 * substream back to the host and drop the record. Caller holds
 * usb_offload_ua_lock.
 */
static void usb_offload_ua_release(struct usb_offload_dev *dev,
				   struct usb_offload_ua_stream *stream)
{
	if (stream->feedback_host_ep)
		usb_offload_ua_unclaim_ep(dev, stream->feedback_host_ep,
					  stream->fb_resource);
	if (stream->data_host_ep)
		usb_offload_ua_unclaim_ep(dev, stream->data_host_ep, stream->resource);

	usb_offload_ua_stop_stream(dev, stream->subs);

	list_del(&stream->node);
	kfree(stream);
}

/*
 * Reserve the sideband resource @host_ep is offloaded on. The host says
 * where an endpoint of this function belongs when its engine ties the
 * resources to functions, and the first free one of those is taken; a host
 * without an opinion gets the first free resource of the controller.
 * Caller holds usb_offload_ua_lock.
 */
static int usb_offload_ua_alloc_resource(struct usb_offload_dev *dev,
					 struct usb_host_endpoint *host_ep,
					 int direction, bool feedback)
{
	unsigned int first, last;
	int ret;

	ret = usb_offload_resource_range(usb_offload_ua_ctx, direction, feedback,
					 &first, &last);
	if (ret)
		return ret;

	ret = xhci_sideband_alloc_resource_id(dev->sb, host_ep, first, last);
	if (ret == -EBUSY)
		dev_dbg(&dev->udev->dev, "USB offload: no free %s resource in %u-%u\n",
			feedback ? "feedback" : "data", first, last);

	return ret;
}

/*
 * Reserve a sideband resource for @host_ep, attach the endpoint to it and read
 * back what the xHC knows about it. Undone by usb_offload_ua_unclaim_ep().
 * Caller holds usb_offload_ua_lock.
 */
static int usb_offload_ua_claim_ep(struct usb_offload_dev *dev,
				   struct usb_host_endpoint *host_ep, int direction,
				   bool feedback, int *res_id,
				   struct xhci_sideband_ep_properties *prop)
{
	int ret;

	ret = usb_offload_ua_alloc_resource(dev, host_ep, direction, feedback);
	if (ret < 0)
		return ret;
	*res_id = ret;

	/* nothing is attached when the add fails, only the reservation is undone */
	ret = xhci_sideband_add_endpoint(dev->sb, host_ep);
	if (ret) {
		xhci_sideband_free_resource_id(dev->sb, *res_id);
		return ret;
	}

	ret = xhci_sideband_get_ep_properties(dev->sb, host_ep, prop);
	if (ret)
		usb_offload_ua_unclaim_ep(dev, host_ep, *res_id);

	return ret;
}

/*
 * A host which has only S24_LE for 24-bit audio (usb_offload_res.s24_3le_as_s24_le)
 * asks for it also of a device which packs 24-bit samples into 3 bytes and
 * will not recognise S24_LE. Rewrite @params to that container, the DSP
 * packs the samples on their way to the link. Returns false when the host
 * did not ask for S24_LE or has no use for the rewrite.
 */
static bool usb_offload_s24_container(const struct usb_offload_ctx *ctx,
				      struct snd_pcm_hw_params *params)
{
	struct snd_mask *fmt = hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT);

	if (!ctx->s24_3le_as_s24_le || !snd_mask_test_format(fmt, SNDRV_PCM_FORMAT_S24_LE))
		return false;

	snd_mask_none(fmt);
	snd_mask_set_format(fmt, SNDRV_PCM_FORMAT_S24_3LE);

	return true;
}

/*
 * The substream of @dev which can run @params and the host lets the DSP
 * have. @kept is set when one could, but the host keeps it. Caller holds
 * usb_offload_ua_lock.
 *
 * snd_usb_find_suppported_substream() would do this, but it takes the
 * usb-audio register_mutex to turn a card index into the chip. The chip is
 * already known here, and the caller is a backend DAI holding the ASoC
 * card's pcm_mutex, which that mutex nests inside.
 */
static struct snd_usb_substream *
usb_offload_find_subs(const struct usb_offload_ctx *ctx, struct usb_offload_dev *dev,
		      int direction, int pcm_index, struct snd_pcm_hw_params *params,
		      int *pcm_device, bool *kept)
{
	const struct audioformat *format;
	struct snd_usb_stream *as;

	list_for_each_entry(as, &dev->chip->pcm_list, list) {
		if (!as->pcm || (pcm_index >= 0 && as->pcm->device != pcm_index))
			continue;
		if (!as->substream[direction].num_formats)
			continue;

		format = snd_usb_find_substream_format(&as->substream[direction], params);
		if (!format)
			continue;
		/* find_format() already prefers a non-async altsetting if one matches */
		if (!usb_offload_fmt_offloadable(ctx, dev->chip, format, direction)) {
			*kept = true;
			continue;
		}

		*pcm_device = as->pcm->device;

		return &as->substream[direction];
	}

	return NULL;
}

int usb_offload_acquire_stream(struct usb_offload_ctx *ctx,
			       struct usb_device *udev, int direction, int pcm_index,
			       struct snd_pcm_hw_params *params,
			       struct usb_offload_stream_hw *hw)
{
	struct snd_pcm_hw_params dev_params;
	struct usb_host_endpoint *host_ep;
	struct usb_host_endpoint *fb_host_ep = NULL;
	struct usb_offload_ua_stream *stream;
	struct snd_usb_substream *subs;
	struct usb_offload_dev *dev;
	struct xhci_sideband_ep_properties ep_prop;
	struct xhci_sideband_ep_properties fb_ep_prop;
	bool kept = false;
	int pcm_device;
	int fb_res_id = 0;
	u8 interval;
	int res_id;
	int ret;

	if (!ctx || !udev || !params || !hw)
		return -EINVAL;

	if (direction < 0 || direction > SNDRV_PCM_STREAM_LAST)
		return -EINVAL;

	guard(mutex)(&usb_offload_ua_lock);

	if (usb_offload_ua_ctx != ctx)
		return -ENODEV;

	dev = usb_offload_ua_find_udev(udev);
	if (!dev || !dev->connected)
		return -ENODEV;

	/* the container the device has, not necessarily the one the host asked for */
	dev_params = *params;
	subs = usb_offload_find_subs(ctx, dev, direction, pcm_index, &dev_params,
				     &pcm_device, &kept);
	if (!subs && usb_offload_s24_container(ctx, &dev_params))
		subs = usb_offload_find_subs(ctx, dev, direction, pcm_index, &dev_params,
					     &pcm_device, &kept);
	if (!subs && kept) {
		dev_dbg(&udev->dev,
			"USB offload: %uch %uHz %s stays with the host, not offloaded\n",
			params_channels(params), params_rate(params),
			direction == SNDRV_PCM_STREAM_PLAYBACK ? "playback" : "capture");
		return -EOPNOTSUPP;
	}
	if (!subs) {
		dev_dbg(&udev->dev, "USB offload: no %s format for %s %uch %uHz\n",
			direction == SNDRV_PCM_STREAM_PLAYBACK ? "playback" : "capture",
			snd_pcm_format_name(params_format(params)),
			params_channels(params), params_rate(params));
		return -EOPNOTSUPP;
	}

	/* a PCM is offloaded once; another PCM of the device may well be running */
	if (usb_offload_ua_find_stream(dev, direction, pcm_device))
		return -EBUSY;

	stream = kzalloc_obj(*stream);
	if (!stream)
		return -ENOMEM;
	stream->subs = subs;
	stream->direction = direction;
	stream->pcm_index = pcm_device;

	ret = usb_offload_ua_start_stream(dev, subs, &dev_params);
	if (ret)
		goto err_free;

	host_ep = usb_pipe_endpoint(udev, subs->data_endpoint->pipe);
	if (!host_ep) {
		ret = -ENXIO;
		goto err_stop;
	}

	/* the endpoint exists only now, so the resource is reserved here */
	ret = usb_offload_ua_claim_ep(dev, host_ep, direction, false, &res_id, &ep_prop);
	if (ret)
		goto err_stop;

	/*
	 * Playback with implicit feedback is never offloaded, see
	 * usb_offload_playback_kept(), so the sync endpoint a stream has here
	 * is an explicit feedback one. The flag is checked nonetheless: the
	 * endpoint such a stream would sync to carries audio samples, not a
	 * feedback value the DSP can interpret.
	 */
	if (subs->sync_endpoint && !subs->data_endpoint->implicit_fb_sync) {
		fb_host_ep = usb_pipe_endpoint(udev, subs->sync_endpoint->pipe);
		if (!fb_host_ep) {
			ret = -ENXIO;
			goto err_unclaim_data;
		}

		ret = usb_offload_ua_claim_ep(dev, fb_host_ep, direction, true,
					      &fb_res_id, &fb_ep_prop);
		if (ret)
			goto err_unclaim_data;
	}

	memset(hw, 0, sizeof(*hw));
	hw->pcm_index = stream->pcm_index;
	hw->format = params_format(&dev_params);
	hw->slot_id = ep_prop.slot_id;
	hw->bus_devid = PCI_DEVID(ep_prop.bus,
				  PCI_DEVFN(ep_prop.device, ep_prop.function));
	usb_offload_fill_ep(&hw->data, host_ep);
	hw->data.resource = ep_prop.res_id;
	stream->resource = res_id;
	stream->fb_resource = fb_res_id;
	stream->data_host_ep = host_ep;
	stream->feedback_host_ep = fb_host_ep;
	if (fb_host_ep) {
		usb_offload_fill_ep(&hw->feedback, fb_host_ep);
		hw->feedback.resource = fb_ep_prop.res_id;
		/*
		 * The period the feedback value is refreshed at, the one
		 * snd-usb-audio polls its sync endpoint at: bRefresh of a UAC1
		 * endpoint, otherwise 2 frames at full speed and bInterval at
		 * high speed, 1 ms when that is not usable either. It is kept
		 * as log2 in syncinterval, here in bInterval form.
		 */
		hw->feedback.interval = subs->sync_endpoint->syncinterval + 1;
		hw->has_feedback = true;
	}
	hw->split = ep_prop.tt;
	/*
	 * The root port's speed is what the link runs at, also for a full
	 * speed device behind a high speed hub; the service interval is in
	 * the device's own frame units, so that one goes by udev->speed.
	 */
	hw->high_speed = ep_prop.port_speed == USB_OFFLOAD_PORT_SPEED_HIGH;

	/*
	 * An isochronous bInterval is an exponent at both speeds, in frames
	 * (1 ms) at full speed and in microframes (125 us) at high speed, which
	 * is how the xHC schedules the endpoint (xhci_parse_exponent_interval).
	 */
	interval = clamp_t(u8, hw->data.interval, 1, 16);
	hw->service_interval_us = (udev->speed == USB_SPEED_HIGH ? 125 : 1000) <<
				  (interval - 1);

	list_add_tail(&stream->node, &dev->streams);

	dev_dbg(&udev->dev, "USB offload: acquired pcm=%d ep=%#x\n",
		hw->pcm_index, hw->data.ep_addr);

	return 0;

err_unclaim_data:
	usb_offload_ua_unclaim_ep(dev, host_ep, res_id);
err_stop:
	usb_offload_ua_stop_stream(dev, subs);
err_free:
	kfree(stream);
	return ret;
}
EXPORT_SYMBOL_NS_GPL(usb_offload_acquire_stream, "USB_AUDIO_OFFLOAD");

int usb_offload_release_stream(struct usb_offload_ctx *ctx,
			       struct usb_device *udev, int direction, int pcm_index)
{
	struct usb_offload_ua_stream *stream;
	struct usb_offload_dev *dev;

	if (!ctx || !udev)
		return -EINVAL;

	if (direction < 0 || direction > SNDRV_PCM_STREAM_LAST)
		return -EINVAL;

	guard(mutex)(&usb_offload_ua_lock);

	if (usb_offload_ua_ctx != ctx)
		return -ENODEV;

	dev = usb_offload_ua_find_udev(udev);
	if (!dev)
		return -ENODEV;

	stream = usb_offload_ua_find_stream(dev, direction, pcm_index);
	if (!stream)
		return -ENODEV;

	usb_offload_ua_release(dev, stream);

	return 0;
}
EXPORT_SYMBOL_NS_GPL(usb_offload_release_stream, "USB_AUDIO_OFFLOAD");

static u64 usb_offload_add_s24_le(struct usb_offload_ctx *ctx, u64 formats)
{
	if (ctx->s24_3le_as_s24_le && (formats & SNDRV_PCM_FMTBIT_S24_3LE))
		formats |= SNDRV_PCM_FMTBIT_S24_LE;

	return formats;
}

static bool usb_offload_fmt_in_range(const struct audioformat *fp,
				     struct snd_pcm_hw_params *params)
{
	struct snd_interval *c = hw_param_interval(params,
						    SNDRV_PCM_HW_PARAM_CHANNELS);
	struct snd_interval *r = hw_param_interval(params,
						    SNDRV_PCM_HW_PARAM_RATE);
	unsigned int i;

	if (fp->channels < c->min || fp->channels > c->max)
		return false;

	if (fp->rate_min > r->max ||
	    (fp->rate_min == r->max && r->openmax))
		return false;
	if (fp->rate_max < r->min ||
	    (fp->rate_max == r->min && r->openmin))
		return false;

	/*
	 * A format with a rate table can have none of its rates in the
	 * interval even though the interval is within its bounds. FORMAT is
	 * chosen before RATE, so offering it would leave no rate to choose.
	 */
	if ((fp->rates & SNDRV_PCM_RATE_CONTINUOUS) || !fp->rate_table || !fp->nr_rates)
		return true;

	for (i = 0; i < fp->nr_rates; i++) {
		if (snd_interval_test(r, fp->rate_table[i]))
			return true;
	}

	return false;
}

static bool usb_offload_fmt_usable(struct usb_offload_ctx *ctx,
				   const struct audioformat *fp,
				   struct snd_pcm_hw_params *params)
{
	struct snd_mask fmts;
	u64 formats = usb_offload_add_s24_le(ctx, fp->formats);

	snd_mask_none(&fmts);
	fmts.bits[0] = lower_32_bits(formats);
	fmts.bits[1] = upper_32_bits(formats);
	snd_mask_intersect(&fmts,
			   hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT));
	if (snd_mask_empty(&fmts))
		return false;

	return usb_offload_fmt_in_range(fp, params);
}

static void usb_offload_fmt_rates(const struct audioformat *fp,
				  struct snd_pcm_hw_params *params,
				  unsigned int *rmin, unsigned int *rmax)
{
	struct snd_interval *r = hw_param_interval(params,
						    SNDRV_PCM_HW_PARAM_RATE);
	unsigned int i;

	if (!fp->rate_table || !fp->nr_rates) {
		*rmin = min(*rmin, fp->rate_min);
		*rmax = max(*rmax, fp->rate_max);
		return;
	}

	for (i = 0; i < fp->nr_rates; i++) {
		if (!snd_interval_test(r, fp->rate_table[i]))
			continue;

		*rmin = min(*rmin, fp->rate_table[i]);
		*rmax = max(*rmax, fp->rate_table[i]);
	}
}

int usb_offload_refine_hw_params(struct usb_offload_ctx *ctx,
				 struct usb_device *udev, int direction,
				 int pcm_index, int var,
				 struct snd_pcm_hw_params *params)
{
	unsigned int rmin = UINT_MAX, rmax = 0;
	struct snd_usb_substream *subs;
	struct usb_offload_dev *dev;
	const struct audioformat *fp;
	struct snd_usb_stream *as;
	struct snd_interval t;
	struct snd_mask m;
	u64 formats = 0;

	if (!ctx || !udev || !params)
		return -EINVAL;

	if (direction < 0 || direction > SNDRV_PCM_STREAM_LAST)
		return -EINVAL;

	if (var != SNDRV_PCM_HW_PARAM_RATE &&
	    var != SNDRV_PCM_HW_PARAM_CHANNELS &&
	    var != SNDRV_PCM_HW_PARAM_FORMAT)
		return -EINVAL;

	guard(mutex)(&usb_offload_ua_lock);

	if (usb_offload_ua_ctx != ctx)
		return 0;

	dev = usb_offload_ua_find_udev(udev);
	if (!dev || !dev->connected)
		return 0;

	list_for_each_entry(as, &dev->chip->pcm_list, list) {
		if (pcm_index >= 0 &&
		    (!as->pcm || as->pcm->device != pcm_index))
			continue;

		subs = &as->substream[direction];
		if (!subs->ep_num)
			continue;

		list_for_each_entry(fp, &subs->fmt_list, list) {
			if (!usb_offload_fmt_offloadable(ctx, dev->chip, fp, direction))
				continue;

			/*
			 * Resolving FORMAT is what decides whether a format
			 * is usable in the first place - e.g. an alternate
			 * setting whose only format is S24_3LE must still be
			 * found here so that it can stand in for S24_LE
			 * below, so this is checked on rate and channels
			 * alone, unlike the other two parameters.
			 */
			if (var == SNDRV_PCM_HW_PARAM_FORMAT) {
				if (usb_offload_fmt_in_range(fp, params))
					formats |= fp->formats;
				continue;
			}

			if (!usb_offload_fmt_usable(ctx, fp, params))
				continue;

			switch (var) {
			case SNDRV_PCM_HW_PARAM_RATE:
				usb_offload_fmt_rates(fp, params, &rmin, &rmax);
				break;
			case SNDRV_PCM_HW_PARAM_CHANNELS:
				rmin = min(rmin, fp->channels);
				rmax = max(rmax, fp->channels);
				break;
			}
		}
	}

	if (var == SNDRV_PCM_HW_PARAM_FORMAT) {
		formats = usb_offload_add_s24_le(ctx, formats);

		snd_mask_none(&m);
		m.bits[0] = lower_32_bits(formats);
		m.bits[1] = upper_32_bits(formats);

		return snd_mask_refine(hw_param_mask(params, var), &m);
	}

	if (rmin > rmax) {
		hw_param_interval(params, var)->empty = 1;
		return -EINVAL;
	}

	t = (struct snd_interval){ .min = rmin, .max = rmax, .integer = 1 };

	return snd_interval_refine(hw_param_interval(params, var), &t);
}
EXPORT_SYMBOL_NS_GPL(usb_offload_refine_hw_params, "USB_AUDIO_OFFLOAD");

/* Tear down one record. Caller holds usb_offload_ua_lock. */
static void usb_offload_ua_teardown(struct usb_offload_dev *dev)
{
	struct usb_offload_ctx *ctx = usb_offload_ua_ctx;
	struct usb_offload_ua_stream *stream, *tmp;

	/*
	 * The host is told first: it ends the streams it runs on the device,
	 * which stops the DSP driving the endpoints, and quiesces them through
	 * the stream records, so these have to be still there. Nothing can
	 * acquire the device anymore once it is not connected.
	 *
	 * Runs before the chip is marked shut down, so what the host has not
	 * given back by the time it returns can still be released properly.
	 */
	if (dev->connected && ctx) {
		dev->connected = false;
		mutex_unlock(&usb_offload_ua_lock);
		usb_offload_disconnect(ctx, dev->udev);
		mutex_lock(&usb_offload_ua_lock);
	}

	list_for_each_entry_safe(stream, tmp, &dev->streams, node)
		usb_offload_ua_release(dev, stream);

	xhci_sideband_unregister(dev->sb);
	list_del(&dev->node);
	kfree(dev);
}

/*
 * Build the USB-SND device identity from the usb-audio chip. This is the
 * only place the chip's card / PCM topology is read; the result travels
 * to the host (via usb_offload_connect) so that it can construct a
 * struct snd_soc_usb_device for the ASoC USB framework without the
 * library ever including <sound/soc-usb.h>.
 *
 * @log: report PCMs left out of the offload list; the caller passes true
 * only on the first connect of a device, as this runs on every re-notify.
 */
static void usb_offload_ua_build_id(const struct usb_offload_ctx *ctx,
				    struct snd_usb_audio *chip,
				    struct usb_offload_usb_dev_info *id, bool log)
{
	struct snd_usb_substream *subs;
	struct snd_usb_stream *as;
	const char *product;
	const char *pcm_name;
	int index;

	memset(id, 0, sizeof(*id));
	id->card_idx = chip->card ? chip->card->number : -1;
	id->chip_idx = chip->index;
	product = chip->dev->product ?: "USB Audio";

	list_for_each_entry(as, &chip->pcm_list, list) {
		if (!as->pcm)
			continue;
		pcm_name = as->pcm->name[0] ? as->pcm->name : "USB PCM";

		subs = &as->substream[SNDRV_PCM_STREAM_PLAYBACK];
		if (subs->num_formats && !usb_offload_subs_offloadable(ctx, subs)) {
			if (log)
				dev_dbg(&chip->dev->dev,
					"USB offload: PCM %u playback needs clock feedback, not offloaded\n",
					as->pcm->device);
		} else if (subs->num_formats &&
			   id->num_playback < USB_OFFLOAD_MAX_PCM) {
			index = id->num_playback++;
			id->ppcm_idx[index] = as->pcm->device;
			snprintf(id->ppcm_names[index], USB_OFFLOAD_PCM_NAME_LEN,
				 "%d,%u | %.24s - %.16s", id->card_idx,
				 as->pcm->device, product, pcm_name);
		}

		subs = &as->substream[SNDRV_PCM_STREAM_CAPTURE];
		if (subs->num_formats && !usb_offload_subs_offloadable(ctx, subs)) {
			if (log)
				dev_dbg(&chip->dev->dev,
					"USB offload: PCM %u capture is tied to the playback, not offloaded\n",
					as->pcm->device);
		} else if (subs->num_formats &&
			   id->num_capture < USB_OFFLOAD_MAX_PCM) {
			index = id->num_capture++;
			id->cpcm_idx[index] = as->pcm->device;
			snprintf(id->cpcm_names[index], USB_OFFLOAD_PCM_NAME_LEN,
				 "%d,%u | %.24s - %.16s", id->card_idx,
				 as->pcm->device, product, pcm_name);
		}
	}
}

static void usb_offload_ua_connect_cb(struct snd_usb_audio *chip)
{
	struct usb_offload_usb_dev_info *id __free(kfree) = NULL;
	struct usb_offload_ctx *ctx;
	struct usb_offload_dev *dev;
	struct usb_interface *audctrl;
	struct usb_device *udev;
	int ret;

	if (!chip || !chip->dev)
		return;
	udev = chip->dev;

	if (!usb_offload_chip_is_candidate(chip)) {
		dev_dbg(&udev->dev, "USB offload: device not a candidate\n");
		return;
	}

	id = kzalloc_obj(*id);
	if (!id)
		return;

	mutex_lock(&usb_offload_ua_lock);

	ctx = usb_offload_ua_ctx;
	if (!ctx)
		goto unlock;

	/* connect_cb fires per interface; offload the chip only once. */
	dev = usb_offload_ua_find(chip);
	if (dev) {
		bool connected = dev->connected;

		mutex_unlock(&usb_offload_ua_lock);

		if (connected) {
			usb_offload_ua_build_id(ctx, chip, id, false);
			ret = usb_offload_connect(ctx, udev, id);
			if (ret)
				dev_dbg(&udev->dev,
					"USB offload: re-notify failed: %d\n",
					ret);
		}
		return;
	}

	/* a device whose PCMs all stay with the host has nothing to offload */
	usb_offload_ua_build_id(ctx, chip, id, true);
	if (!id->num_playback && !id->num_capture) {
		dev_dbg(&udev->dev, "USB offload: no PCM can be offloaded\n");
		goto unlock;
	}

	dev = kzalloc_obj(*dev);
	if (!dev)
		goto unlock;

	dev->chip		= chip;
	dev->udev		= udev;
	INIT_LIST_HEAD(&dev->streams);
	audctrl = usb_ifnum_to_if(udev, chip->ctrl_intf->desc.bInterfaceNumber);
	if (!audctrl) {
		ret = -ENODEV;
		goto free_dev_unlock;
	}

	/*
	 * The first registration on a controller asks it for its sideband
	 * support with a command, which it cannot run while suspended. A
	 * device found again by the rediscovery may be autosuspended by then,
	 * and the controller with it.
	 */
	ret = usb_autopm_get_interface(audctrl);
	if (ret)
		goto free_dev_unlock;

	dev->sb = xhci_sideband_register(audctrl, XHCI_SIDEBAND_AUDIO, NULL);
	usb_autopm_put_interface(audctrl);
	if (!dev->sb) {
		ret = -EOPNOTSUPP;
		goto free_dev_unlock;
	}

	list_add_tail(&dev->node, &usb_offload_ua_devs);
	mutex_unlock(&usb_offload_ua_lock);

	/* Inform the DSP and the ASoC USB framework. */
	ret = usb_offload_connect(ctx, udev, id);

	mutex_lock(&usb_offload_ua_lock);
	if (ret) {
		list_del(&dev->node);
		goto unregister_sb;
	}
	dev->connected = true;
	mutex_unlock(&usb_offload_ua_lock);

	dev_dbg(&udev->dev,
		"USB offload: device offloaded (UAC protocol %#x)\n",
		chip->ctrl_intf->desc.bInterfaceProtocol);
	return;

unregister_sb:
	xhci_sideband_unregister(dev->sb);
free_dev_unlock:
	dev_dbg(&udev->dev, "USB offload: sideband setup failed: %d\n", ret);
	kfree(dev);
unlock:
	mutex_unlock(&usb_offload_ua_lock);
}

static void usb_offload_ua_disconnect_cb(struct snd_usb_audio *chip)
{
	struct usb_offload_dev *dev;

	if (!chip)
		return;

	guard(mutex)(&usb_offload_ua_lock);

	dev = usb_offload_ua_find(chip);
	if (dev) {
		dev_dbg(&dev->udev->dev, "USB offload: device removed\n");
		usb_offload_ua_teardown(dev);
	}
}

static struct snd_usb_platform_ops usb_offload_ua_ops = {
	.connect_cb	= usb_offload_ua_connect_cb,
	.disconnect_cb	= usb_offload_ua_disconnect_cb,
};

int usb_offload_usb_audio_init(struct usb_offload_ctx *ctx)
{
	int ret;

	if (!ctx)
		return -EINVAL;

	scoped_guard(mutex, &usb_offload_ua_lock) {
		if (usb_offload_ua_ctx)
			return -EEXIST;
		usb_offload_ua_ctx = ctx;
	}

	/*
	 * register/unregister take the usb-audio register_mutex; do not hold
	 * usb_offload_ua_lock across them to avoid an A-B / B-A inversion with
	 * the connect/disconnect callbacks (which run under register_mutex and
	 * then take usb_offload_ua_lock).
	 */
	ret = snd_usb_register_platform_ops(&usb_offload_ua_ops);
	if (ret) {
		scoped_guard(mutex, &usb_offload_ua_lock)
			usb_offload_ua_ctx = NULL;
		return ret;
	}

	dev_dbg(ctx->dev, "USB offload: usb-audio bridge registered\n");
	return 0;
}

void usb_offload_usb_audio_exit(struct usb_offload_ctx *ctx)
{
	struct usb_offload_dev *dev, *tmp;

	scoped_guard(mutex, &usb_offload_ua_lock) {
		if (usb_offload_ua_ctx != ctx)
			return;
	}

	/*
	 * Let go of every device while snd-usb-audio still reports their
	 * disconnect, and stop the callbacks. snd_usb_release_platform_ops()
	 * runs our disconnect callback for each device and unregisters under
	 * the usb-audio register_mutex, so a device cannot be freed in between
	 * unnoticed, and once it returns no callback is running and none can
	 * start.
	 */
	snd_usb_release_platform_ops();

	scoped_guard(mutex, &usb_offload_ua_lock) {
		/* every record is of a connected chip, there should be none left */
		list_for_each_entry_safe(dev, tmp, &usb_offload_ua_devs, node)
			usb_offload_ua_teardown(dev);
		usb_offload_ua_ctx = NULL;
	}

	dev_dbg(ctx->dev, "USB offload: usb-audio bridge unregistered\n");
}

void usb_offload_usb_audio_rediscover(void)
{
	/*
	 * Walk the usb-audio core's connected-chip array and re-run our
	 * connect_cb for each. snd_usb_rediscover_devices() does this under
	 * the usb-audio register_mutex; our callback only re-notifies for
	 * chips already offloaded rather than setting them up again, and is
	 * a no-op if the platform ops were never registered, so this is
	 * always safe.
	 */
	snd_usb_rediscover_devices();
}
