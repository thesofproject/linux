// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
//
// This file is provided under a dual BSD/GPLv2 license.  When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2026 Intel Corporation
//
// USB Audio Offload Link (UAOL) DAIs: the DSP drives a USB audio endpoint
// over the UAOL instead of the host.
//

#include <linux/usb/ch9.h>
#include <linux/usb.h>
#include <sound/pcm_params.h>
#include <sound/hdaudio_ext.h>
#include <sound/hda-mlink.h>
#include <sound/jack.h>
#include <sound/soc-usb.h>
#include <sound/sof/ipc4/header.h>
#include <sound/usb-offload.h>
#include <uapi/sound/sof/header.h>
#include "../ipc4-priv.h"
#include "../ipc4-topology.h"
#include "../sof-priv.h"
#include "../sof-audio.h"
#include "hda.h"

/**
 * struct hda_uaol_dev - an offload capable USB audio device
 * @sdev: the device identity handed to the ASoC USB framework
 * @udev: USB device this record stands for, the key used on disconnect
 * @node: link on hdev->uaol_devices, this backend's own record of what it
 *	 has allocated - not the routing list, see hdev->uaol_usb's
 *	 available_devices for that
 * @ppcm: backing store for @sdev.ppcm_idx
 * @cpcm: backing store for @sdev.cpcm_idx
 * @ppcm_names: backing store for @sdev.ppcm_names
 * @cpcm_names: backing store for @sdev.cpcm_names
 * @ppcm_name_storage: string storage @ppcm_names points into
 * @cpcm_name_storage: string storage @cpcm_names points into
 *
 * The offload library reports the identity as a value, while the ASoC USB
 * framework wants a list node it can keep, so the record is owned here.
 */
struct hda_uaol_dev {
	struct snd_soc_usb_device sdev;
	struct usb_device *udev;
	struct list_head node;
	unsigned int ppcm[USB_OFFLOAD_MAX_PCM];
	unsigned int cpcm[USB_OFFLOAD_MAX_PCM];
	const char *ppcm_names[USB_OFFLOAD_MAX_PCM];
	const char *cpcm_names[USB_OFFLOAD_MAX_PCM];
	char ppcm_name_storage[USB_OFFLOAD_MAX_PCM][USB_OFFLOAD_PCM_NAME_LEN];
	char cpcm_name_storage[USB_OFFLOAD_MAX_PCM][USB_OFFLOAD_PCM_NAME_LEN];
};

/* Caller holds hdev->uaol_mutex. */
static struct hda_uaol_dev *hda_uaol_find(struct sof_intel_hda_dev *hdev,
					  struct usb_device *udev)
{
	struct hda_uaol_dev *dev;

	list_for_each_entry(dev, &hdev->uaol_devices, node) {
		if (dev->udev == udev)
			return dev;
	}

	return NULL;
}

/*
 * The identity is everything the port is told about the device; a re-notify
 * with all of it unchanged carries nothing new.
 */
static bool hda_uaol_dev_identity_equal(const struct hda_uaol_dev *a,
					const struct hda_uaol_dev *b)
{
	return a->sdev.card_idx == b->sdev.card_idx &&
	       a->sdev.chip_idx == b->sdev.chip_idx &&
	       a->sdev.num_playback == b->sdev.num_playback &&
	       a->sdev.num_capture == b->sdev.num_capture &&
	       !memcmp(a->ppcm, b->ppcm, sizeof(a->ppcm)) &&
	       !memcmp(a->cpcm, b->cpcm, sizeof(a->cpcm)) &&
	       !memcmp(a->ppcm_name_storage, b->ppcm_name_storage,
		       sizeof(a->ppcm_name_storage)) &&
	       !memcmp(a->cpcm_name_storage, b->cpcm_name_storage,
		       sizeof(a->cpcm_name_storage));
}

/*
 * Whether a stream is already open on @route of @direction, consulted by the
 * route kcontrol before it lets a selection change - a link a running stream
 * owns is never switched out from under it.
 */
static bool hda_uaol_is_route_busy(struct snd_soc_usb *usb, int direction,
				   unsigned int route)
{
	struct sof_intel_hda_dev *hdev = usb->priv_data;
	int i;

	for (i = 0; i < ARRAY_SIZE(hdev->uaol_streams); i++) {
		struct hda_uaol_stream *stream = &hdev->uaol_streams[i];

		if (stream->dai && stream->direction == direction &&
		    stream->route == route && stream->udev)
			return true;
	}

	return false;
}

/* The backend state of a UAOL CPU DAI, claimed when the DAI probed */
static struct hda_uaol_stream *hda_uaol_stream_get(struct sof_intel_hda_dev *hdev,
						   struct snd_soc_dai *dai)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(hdev->uaol_streams); i++) {
		if (hdev->uaol_streams[i].dai == dai)
			return &hdev->uaol_streams[i];
	}

	return NULL;
}

int hda_uaol_notify_connect(struct device *dev, struct usb_device *udev,
			    const struct usb_offload_usb_dev_info *id)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(dev);
	struct sof_intel_hda_dev *hdev = sdev->pdata->hw_pdata;
	struct hda_uaol_dev *offload_dev, *old;
	int ret;

	offload_dev = kzalloc_obj(*offload_dev, GFP_KERNEL);
	if (!offload_dev)
		return -ENOMEM;

	offload_dev->udev = udev;
	INIT_LIST_HEAD(&offload_dev->sdev.available_node);
	offload_dev->sdev.card_idx = id->card_idx;
	offload_dev->sdev.chip_idx = id->chip_idx;
	offload_dev->sdev.num_playback = id->num_playback;
	offload_dev->sdev.num_capture = id->num_capture;
	memcpy(offload_dev->ppcm, id->ppcm_idx, sizeof(offload_dev->ppcm));
	memcpy(offload_dev->cpcm, id->cpcm_idx, sizeof(offload_dev->cpcm));
	memcpy(offload_dev->ppcm_name_storage, id->ppcm_names,
	       sizeof(offload_dev->ppcm_name_storage));
	memcpy(offload_dev->cpcm_name_storage, id->cpcm_names,
	       sizeof(offload_dev->cpcm_name_storage));
	for (int i = 0; i < USB_OFFLOAD_MAX_PCM; i++) {
		offload_dev->ppcm_names[i] = offload_dev->ppcm_name_storage[i];
		offload_dev->cpcm_names[i] = offload_dev->cpcm_name_storage[i];
	}
	offload_dev->sdev.ppcm_idx = offload_dev->ppcm;
	offload_dev->sdev.cpcm_idx = offload_dev->cpcm;
	offload_dev->sdev.ppcm_names = offload_dev->ppcm_names;
	offload_dev->sdev.cpcm_names = offload_dev->cpcm_names;

	/*
	 * Held across the connect, so that the card removal, which takes the
	 * port away under the same lock, cannot slip in between the check for
	 * the port and the device being listed.
	 */
	guard(mutex)(&hdev->uaol_mutex);

	/*
	 * Without a card there is nothing to attach the device to. The port
	 * rediscovers the already enumerated devices when it comes up, so the
	 * event is not lost.
	 */
	if (!hdev->uaol_usb) {
		kfree(offload_dev);
		return 0;
	}

	/*
	 * A notification for a listed device carries its updated identity: the
	 * PCM list grows as further interfaces of a multi-function device are
	 * probed. Replace the stale entry; an unchanged identity is left alone,
	 * a disconnect would drop the routes picked for the device.
	 */
	old = hda_uaol_find(hdev, udev);
	if (old) {
		if (hda_uaol_dev_identity_equal(old, offload_dev)) {
			kfree(offload_dev);
			return 0;
		}

		list_del(&old->node);
		snd_soc_usb_disconnect(sdev->dev, &old->sdev);
		kfree(old);
	}

	ret = snd_soc_usb_connect(sdev->dev, &offload_dev->sdev);
	if (ret) {
		kfree(offload_dev);
		return ret;
	}

	list_add_tail(&offload_dev->node, &hdev->uaol_devices);
	if (hdev->uaol_jack.jack)
		snd_jack_report(hdev->uaol_jack.jack, SND_JACK_USB);

	return 0;
}
EXPORT_SYMBOL_NS(hda_uaol_notify_connect, "SND_SOC_SOF_INTEL_HDA_COMMON");

/*
 * Nothing else tells a frontend that the device it plays to has been unplugged:
 * the link keeps running into a dead endpoint and the application keeps
 * feeding it. End its stream the way the USB card ends its own users' streams
 * when it goes away, so the application sees the device disappear instead of
 * silence.
 */
static void hda_uaol_stop_frontends(struct sof_intel_hda_dev *hdev,
				    struct usb_device *udev)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(hdev->uaol_streams); i++) {
		struct hda_uaol_stream *stream = &hdev->uaol_streams[i];
		struct snd_pcm_substream *be_substream = READ_ONCE(stream->be);
		int dir = stream->direction;
		struct snd_soc_pcm_runtime *be;
		struct snd_soc_dpcm *dpcm;

		if (!be_substream)
			continue;

		be = snd_soc_substream_to_rtd(be_substream);

		/* the backend's hw_params and hw_free run under this, so the pair is stable */
		snd_soc_dpcm_mutex_lock(be);
		if (stream->udev != udev || stream->be != be_substream) {
			snd_soc_dpcm_mutex_unlock(be);
			continue;
		}

		for_each_dpcm_fe(be, dir, dpcm) {
			struct snd_pcm_substream *fe_substream =
				snd_soc_dpcm_get_substream(dpcm->fe, dir);

			snd_pcm_stream_lock_irq(fe_substream);
			if (fe_substream->runtime) {
				if (snd_pcm_running(fe_substream))
					snd_pcm_stop(fe_substream, SNDRV_PCM_STATE_DISCONNECTED);
				else
					__snd_pcm_set_state(fe_substream->runtime,
							    SNDRV_PCM_STATE_DISCONNECTED);
			}
			snd_pcm_stream_unlock_irq(fe_substream);
		}

		/*
		 * The library has already given the USB side back and the device
		 * goes away with the disconnect. Forget it, so that the trigger and
		 * hw_free of the frontends' teardown do not hand the stale pointer
		 * back to the library, where a device reusing the address would
		 * match.
		 */
		stream->udev = NULL;
		stream->be = NULL;
		snd_soc_dpcm_mutex_unlock(be);
	}
}

int hda_uaol_notify_disconnect(struct device *dev, struct usb_device *udev)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(dev);
	struct sof_intel_hda_dev *hdev = sdev->pdata->hw_pdata;
	struct hda_uaol_dev *offload_dev;

	hda_uaol_stop_frontends(hdev, udev);

	scoped_guard(mutex, &hdev->uaol_mutex) {
		offload_dev = hda_uaol_find(hdev, udev);
		if (offload_dev)
			list_del(&offload_dev->node);
	}
	if (!offload_dev)
		return 0;

	snd_soc_usb_disconnect(sdev->dev, &offload_dev->sdev);
	kfree(offload_dev);

	/*
	 * The jack follows the list and not this one event, so that removing
	 * one of several connected devices still leaves it reported. The list
	 * is read where the jack is reported, a device connecting in between
	 * would otherwise be reported as gone.
	 */
	scoped_guard(mutex, &hdev->uaol_mutex) {
		if (hdev->uaol_jack.jack)
			snd_jack_report(hdev->uaol_jack.jack,
					list_empty(&hdev->uaol_devices) ? 0 : SND_JACK_USB);
	}

	return 0;
}
EXPORT_SYMBOL_NS(hda_uaol_notify_disconnect, "SND_SOC_SOF_INTEL_HDA_COMMON");

/*
 * The ASoC USB port is published once the UAOL DAI is bound into a card,
 * which is the point where an offloaded device has somewhere to be routed.
 * The firmware has booted long before that, so the offload library cannot
 * do this itself.
 */
static int uaol_dai_probe(struct snd_soc_dai *dai)
{
	struct snd_sof_dev *sdev = snd_soc_component_get_drvdata(dai->component);
	struct sof_intel_hda_dev *hdev = sdev->pdata->hw_pdata;
	struct hda_uaol_stream *stream;
	struct snd_soc_usb *usb;
	unsigned int route;
	int direction, ret, i;

	/*
	 * The pin number of the DAI, "UAOL0 Playback Pin1", is the ordinal of
	 * the backend among those of its direction, which is the ASoC USB
	 * route it plays to.
	 */
	if (sscanf(dai->driver->name, "UAOL%*u %*s Pin%u", &route) != 1)
		return -EINVAL;
	direction = dai->driver->playback.channels_min ? SNDRV_PCM_STREAM_PLAYBACK :
							  SNDRV_PCM_STREAM_CAPTURE;

	/* a free slot has no DAI */
	stream = hda_uaol_stream_get(hdev, NULL);
	if (!stream)
		return -ENOSPC;
	stream->hdev = hdev;
	stream->dai = dai;
	stream->direction = direction;
	stream->route = route;

	if (!hdev->uaol) {
		/*
		 * The UAOL PCMs exist when the controller and firmware
		 * support offload, even if the offload library itself failed to
		 * probe. Give userspace a jack it can read instead of a control
		 * that silently doesn't exist.
		 */
		if (hdev->uaol_jack.jack)
			return 0;

		ret = snd_soc_card_jack_new(dai->component->card, "USB Offload Jack",
					    SND_JACK_USB, &hdev->uaol_jack);
		if (ret)
			goto err_free_slot;

		snd_jack_report(hdev->uaol_jack.jack, 0);
		return 0;
	}

	/* the port belongs to the controller, every UAOL CPU DAI probes here */
	if (hdev->uaol_usb)
		return 0;

	usb = snd_soc_usb_allocate_port(dai->component, hdev);
	if (IS_ERR(usb)) {
		ret = PTR_ERR(usb);
		goto err_free_slot;
	}

	usb->is_route_busy = hda_uaol_is_route_busy;

	/* a route per backend of a direction, each with a device of its own */
	for (i = 0; i <= SNDRV_PCM_STREAM_LAST; i++) {
		ret = snd_soc_usb_set_num_routes(usb, i, max(hdev->uaol_stream_num[i], 1U));
		if (ret)
			goto err_free_port;
	}

	/*
	 * The UAOL PCMs exist whether or not a device is there to play to, and
	 * nothing else tells userspace which of the two it is. The jack has to
	 * be in place before the port goes up, as that is what reports the
	 * devices which are already connected.
	 */
	ret = snd_soc_card_jack_new(dai->component->card, "USB Offload Jack",
				    SND_JACK_USB, &hdev->uaol_jack);
	if (ret)
		goto err_free_port;

	ret = snd_soc_usb_setup_offload_route_ctl(dai->component, usb);
	if (ret)
		goto err_free_port;

	scoped_guard(mutex, &hdev->uaol_mutex)
		hdev->uaol_usb = usb;

	/* rediscovers the devices which enumerated before the card existed */
	snd_soc_usb_add_port(usb);

	return 0;

err_free_port:
	snd_soc_usb_free_port(usb);
	/*
	 * ASoC does not call remove for a DAI whose probe failed, and the card
	 * frees the jack of the failed bring-up: do not leave it for the
	 * connect notifications to report on.
	 */
	scoped_guard(mutex, &hdev->uaol_mutex)
		hdev->uaol_jack.jack = NULL;
err_free_slot:
	/* ASoC does not call remove for a DAI whose probe failed */
	stream->dai = NULL;
	return ret;
}

static int uaol_dai_remove(struct snd_soc_dai *dai)
{
	struct snd_sof_dev *sdev = snd_soc_component_get_drvdata(dai->component);
	struct sof_intel_hda_dev *hdev = sdev->pdata->hw_pdata;
	struct hda_uaol_stream *stream = hda_uaol_stream_get(hdev, dai);
	struct hda_uaol_dev *dev, *tmp;
	struct snd_soc_usb *usb;

	if (stream)
		stream->dai = NULL;

	/*
	 * Taken away under the lock so that a connect racing with the card
	 * going away sees no port to attach to. The jack itself belongs to the
	 * card and goes away with it.
	 */
	scoped_guard(mutex, &hdev->uaol_mutex) {
		usb = hdev->uaol_usb;
		hdev->uaol_usb = NULL;
		hdev->uaol_jack.jack = NULL;
	}
	if (!usb)
		return 0;

	snd_soc_usb_remove_port(usb);
	snd_soc_usb_free_port(usb);

	/* the port is gone, no disconnect event will report these anymore */
	scoped_guard(mutex, &hdev->uaol_mutex) {
		list_for_each_entry_safe(dev, tmp, &hdev->uaol_devices, node) {
			list_del(&dev->node);
			kfree(dev);
		}
	}

	return 0;
}

/*
 * Which device a backend plays to is the ASoC USB route kcontrol's job to
 * decide, per route (defaulting to the Nth connected pair for route N, see
 * snd_soc_usb_get_selected_device()) - this
 * just resolves that selection back to the Intel-side record that carries
 * the actual usb_device pointer.
 */
static struct usb_device *hda_uaol_selected(struct sof_intel_hda_dev *hdev,
					    int direction, unsigned int route,
					    int *pcm_index)
{
	struct snd_soc_usb_device *sdev;
	struct hda_uaol_dev *dev;
	int ret;

	ret = snd_soc_usb_get_selected_device(hdev->uaol_usb, direction, route,
					      &sdev, pcm_index);
	if (ret || !sdev)
		return NULL;

	scoped_guard(mutex, &hdev->uaol_mutex) {
		list_for_each_entry(dev, &hdev->uaol_devices, node) {
			if (&dev->sdev == sdev)
				return dev->udev;
		}
	}

	return NULL;
}

/*
 * The gateway stages a service interval worth of payload in the link FIFO, so
 * the space a stream takes is its byte rate per interval. Two intervals have to
 * fit for the DSP to be able to refill the FIFO while the link drains it.
 */
#define SOF_UAOL_FIFO_INTERVALS		2

static const int uaol_params[] = {
	SNDRV_PCM_HW_PARAM_RATE,
	SNDRV_PCM_HW_PARAM_CHANNELS,
	SNDRV_PCM_HW_PARAM_FORMAT,
};

/*
 * The gateway is programmed with the valid bits of the format and the link
 * packs the samples to that width, so the container is not what occupies the
 * FIFO. S24_LE is 3 bytes: the HDA DMA only knows a 32 bit container, of which
 * it takes the 24 MSBs and drops the LSB byte. Only a full 32 bit format
 * takes 4 bytes.
 */
static unsigned int uaol_sample_bytes(snd_pcm_format_t format)
{
	int width = snd_pcm_format_width(format);

	return width > 0 ? DIV_ROUND_UP(width, BITS_PER_BYTE) : 0;
}

static unsigned int uaol_min_sample_bytes(const struct snd_mask *formats)
{
	unsigned int min_bytes = UINT_MAX;
	snd_pcm_format_t format;

	pcm_for_each_format(format) {
		unsigned int bytes;

		if (!snd_mask_test_format(formats, format))
			continue;

		bytes = uaol_sample_bytes(format);
		if (bytes)
			min_bytes = min(min_bytes, bytes);
	}

	return min_bytes == UINT_MAX ? 0 : min_bytes;
}

/*
 * The budget is one dimensional in the byte rate, so the bound it puts on any
 * one of the three parameters depends on the other two. Taking the smallest
 * value the other two can still take keeps each bound the loosest one that is
 * valid, and the refining converges on the combinations that fit.
 */
static int uaol_rule_rate_channels(struct snd_pcm_hw_params *params,
				   struct snd_pcm_hw_rule *rule)
{
	/* the product of the two is what the budget bounds, so one limits the other */
	int other = rule->var == SNDRV_PCM_HW_PARAM_RATE ? SNDRV_PCM_HW_PARAM_CHANNELS :
							   SNDRV_PCM_HW_PARAM_RATE;
	struct snd_interval *other_i = hw_param_interval(params, other);
	unsigned int budget = (uintptr_t)rule->private;
	unsigned int bytes;
	struct snd_interval t = { .integer = 1 };

	bytes = uaol_min_sample_bytes(hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT));
	if (!bytes || !other_i->min)
		return 0;

	t.max = budget / (other_i->min * bytes);

	return snd_interval_refine(hw_param_interval(params, rule->var), &t);
}

static int uaol_rule_format(struct snd_pcm_hw_params *params, struct snd_pcm_hw_rule *rule)
{
	struct snd_interval *channels = hw_param_interval(params, SNDRV_PCM_HW_PARAM_CHANNELS);
	struct snd_interval *rate = hw_param_interval(params, SNDRV_PCM_HW_PARAM_RATE);
	unsigned int budget = (uintptr_t)rule->private;
	snd_pcm_format_t format;
	struct snd_mask m;

	if (!rate->min || !channels->min)
		return 0;

	snd_mask_none(&m);
	pcm_for_each_format(format) {
		unsigned int bytes = uaol_sample_bytes(format);

		if (bytes && (u64)channels->min * bytes * rate->min <= budget)
			snd_mask_set_format(&m, format);
	}

	return snd_mask_refine(hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT), &m);
}

/*
 * The share of the link FIFO a stream gets is fixed when the gateway is
 * configured, and the rate, the channel count and the format together decide
 * whether what the stream produces fits in it. Overrunning it is not reported
 * anywhere, the audio simply comes out corrupted, so keep the combinations that
 * do not fit off the table.
 *
 * This is a property of the link and not of the connected device, hence it is
 * applied whether or not there is a device to narrow to.
 */
static void hda_uaol_set_fifo_constraint(struct snd_sof_dev *sdev,
					 struct snd_soc_dai *be_dai,
					 struct snd_pcm_substream *substream)
{
	struct snd_soc_dapm_widget *w = snd_soc_dai_get_widget(be_dai, substream->stream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct sof_ipc4_copier *ipc4_copier;
	unsigned int budget;
	int size;

	if (!w)
		return;

	ipc4_copier = widget_to_copier(w);

	size = sof_ipc4_uaol_get_fifo_size(sdev, ipc4_copier->dai_index, substream->stream);
	if (size <= 0)
		return;

	/*
	 * The real service interval is only known once a device's altsetting is
	 * actually resolved, so this early negotiation-time rule can only offer a
	 * coarse pre-filter: assume the shortest possible interval (125 us), which
	 * keeps high-bandwidth devices from being needlessly capped here.
	 */
	budget = size / SOF_UAOL_FIFO_INTERVALS * (USEC_PER_SEC / 125);

	snd_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_RATE,
			    uaol_rule_rate_channels, (void *)(uintptr_t)budget,
			    SNDRV_PCM_HW_PARAM_CHANNELS, SNDRV_PCM_HW_PARAM_FORMAT, -1);
	snd_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_CHANNELS,
			    uaol_rule_rate_channels, (void *)(uintptr_t)budget,
			    SNDRV_PCM_HW_PARAM_RATE, SNDRV_PCM_HW_PARAM_FORMAT, -1);
	snd_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_FORMAT,
			    uaol_rule_format, (void *)(uintptr_t)budget,
			    SNDRV_PCM_HW_PARAM_RATE, SNDRV_PCM_HW_PARAM_CHANNELS, -1);
}

/*
 * The device is resolved on every call rather than once at open, so that a
 * device connected or removed, or the route kcontrol changing which device
 * is selected, while a frontend sits open is picked up on the next
 * negotiation.
 */
static int uaol_rule_device(struct snd_pcm_hw_params *params,
			    struct snd_pcm_hw_rule *rule)
{
	struct hda_uaol_stream *stream = rule->private;
	struct sof_intel_hda_dev *hdev = stream->hdev;
	struct usb_device *udev;
	int pcm_index;

	udev = hda_uaol_selected(hdev, stream->direction, stream->route, &pcm_index);
	if (!udev)
		return 0;

	return usb_offload_refine_hw_params(hdev->uaol, udev, stream->direction,
					    pcm_index, rule->var, params);
}

/*
 * A frontend sharing the backend with one which has it open, the deep buffer,
 * can only run what the first one opened the backend with: the two share the
 * endpoint and the mixer. Clone those parameters as the only ones allowed, so
 * userspace negotiates against the running stream and not against what the
 * topology offers.
 *
 * What the first frontend opened the backend with is only known once its
 * hw_params ran, hence rules and not plain constraints.
 */
static int uaol_rule_shared(struct snd_pcm_hw_params *params,
			    struct snd_pcm_hw_rule *rule)
{
	struct hda_uaol_stream *stream = rule->private;
	struct snd_interval t = { .integer = 1 };
	struct snd_mask m;

	/* pairs with the release which publishes the first frontend's parameters */
	if (!smp_load_acquire(&stream->params_valid))
		return 0;

	switch (rule->var) {
	case SNDRV_PCM_HW_PARAM_RATE:
		t.min = stream->rate;
		t.max = stream->rate;
		break;
	case SNDRV_PCM_HW_PARAM_CHANNELS:
		t.min = stream->channels;
		t.max = stream->channels;
		break;
	default:
		snd_mask_none(&m);
		snd_mask_set_format(&m, stream->format);

		return snd_mask_refine(hw_param_mask(params, rule->var), &m);
	}

	return snd_interval_refine(hw_param_interval(params, rule->var), &t);
}

/*
 * A UAOL frontend advertises what the topology configured the link for, which
 * has nothing to do with the device that happens to be connected. Narrow it to
 * what that device can be run at, so that userspace negotiates against the
 * device instead of finding out at hw_params time that its choice is
 * impossible.
 *
 * Rules and not plain constraints, because what one parameter can be depends on
 * the other two: a device can well have a multichannel stream at one rate and a
 * stereo one at another, and the ranges alone would offer the combination of the
 * two that it does not have. Each rule reports what the parameters still allow,
 * so the refining settles on a combination a single alternate setting can serve,
 * which is what the acquire in uaol_dai_start_stream() goes looking for. The
 * rules are given the backend they run for, which lives as long as the card.
 */
static void hda_uaol_set_constraint(struct snd_sof_dev *sdev,
				    struct snd_soc_dai *be_dai,
				    struct snd_pcm_substream *substream)
{
	struct sof_intel_hda_dev *hdev = sdev->pdata->hw_pdata;
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct hda_uaol_stream *ctx;
	int i;

	hda_uaol_set_fifo_constraint(sdev, be_dai, substream);

	if (!hdev->uaol)
		return;

	ctx = hda_uaol_stream_get(hdev, be_dai);
	if (!ctx)
		return;

	for (i = 0; i < ARRAY_SIZE(uaol_params); i++) {
		/* what each of the three can be depends on all of them */
		snd_pcm_hw_rule_add(runtime, 0, uaol_params[i], uaol_rule_device, ctx,
				    SNDRV_PCM_HW_PARAM_RATE, SNDRV_PCM_HW_PARAM_CHANNELS,
				    SNDRV_PCM_HW_PARAM_FORMAT, -1);
		snd_pcm_hw_rule_add(runtime, 0, uaol_params[i], uaol_rule_shared, ctx,
				    uaol_params[i], -1);
	}
}

static u32 uaol_ep_info(const struct usb_offload_stream_hw *hw,
			const struct usb_offload_ep_info *ep)
{
	return FIELD_PREP(SOF_IPC4_UAOL_EP_INFO_DIRECTION,
			  !!(ep->ep_addr & USB_DIR_IN)) |
	       FIELD_PREP(SOF_IPC4_UAOL_EP_INFO_NUMBER,
			  ep->ep_addr & USB_ENDPOINT_NUMBER_MASK) |
	       FIELD_PREP(SOF_IPC4_UAOL_EP_INFO_SLOT, hw->slot_id) |
	       FIELD_PREP(SOF_IPC4_UAOL_EP_INFO_SPLIT, hw->split) |
	       FIELD_PREP(SOF_IPC4_UAOL_EP_INFO_HIGH_SPEED, hw->high_speed) |
	       FIELD_PREP(SOF_IPC4_UAOL_EP_INFO_MAX_PACKET, ep->maxpacket);
}

/*
 * The gateway only knows which USB endpoint to drive once the device is
 * acquired, so complete the configuration the topology set up.
 */
static int uaol_dai_set_endpoint(struct snd_soc_dai *cpu_dai, int direction,
				 const struct usb_offload_stream_hw *hw)
{
	struct snd_soc_dapm_widget *w = snd_soc_dai_get_widget(cpu_dai, direction);
	struct sof_ipc4_uaol_configuration_blob *blob;
	struct sof_ipc4_copier *ipc4_copier;
	u32 stream = hw->data.resource - 1;
	int link_stream;

	link_stream = sof_ipc4_uaol_link_stream(stream);
	if (link_stream < 0) {
		dev_err(cpu_dai->dev, "invalid UAOL stream %u\n", stream);
		return link_stream;
	}

	ipc4_copier = widget_to_copier(w);
	blob = (struct sof_ipc4_uaol_configuration_blob *)ipc4_copier->copier_config;

	blob->config &= ~SOF_IPC4_UAOL_CONFIG_STREAM_IDX;
	blob->config |= FIELD_PREP(SOF_IPC4_UAOL_CONFIG_STREAM_IDX, stream);

	ipc4_copier->data.gtw_cfg.node_id &= ~SOF_IPC4_NODE_INDEX_MASK;
	ipc4_copier->data.gtw_cfg.node_id |= SOF_IPC4_NODE_INDEX(link_stream);

	blob->xhci_bdf = FIELD_PREP(SOF_IPC4_UAOL_XHCI_BDF_BUS,
				    PCI_BUS_NUM(hw->bus_devid)) |
			 FIELD_PREP(SOF_IPC4_UAOL_XHCI_BDF_DEVICE,
				    PCI_SLOT(hw->bus_devid)) |
			 FIELD_PREP(SOF_IPC4_UAOL_XHCI_BDF_FUNCTION,
				    PCI_FUNC(hw->bus_devid));

	blob->ep_info = uaol_ep_info(hw, &hw->data);

	/* Service interval from the USB endpoint descriptor */
	blob->service_interval_us = hw->service_interval_us;

	/* the feedback fields are left over from the previously offloaded device */
	if (hw->has_feedback) {
		blob->feedback_ep_info = uaol_ep_info(hw, &hw->feedback);
		blob->config &= ~(SOF_IPC4_UAOL_CONFIG_FEEDBACK_IDX |
				  SOF_IPC4_UAOL_CONFIG_FEEDBACK_PERIOD);
		blob->config |= FIELD_PREP(SOF_IPC4_UAOL_CONFIG_FEEDBACK_IDX,
					   hw->feedback.resource - 1) |
				/* bInterval form, the firmware polls every 2^(n-1) (micro)frames */
				FIELD_PREP(SOF_IPC4_UAOL_CONFIG_FEEDBACK_PERIOD,
					   hw->feedback.interval);
	} else {
		blob->feedback_ep_info = 0;
		blob->config &= ~(SOF_IPC4_UAOL_CONFIG_FEEDBACK_IDX |
				  SOF_IPC4_UAOL_CONFIG_FEEDBACK_PERIOD);
	}

	return 0;
}

static void uaol_dai_stop_stream(struct snd_pcm_substream *substream,
				 struct hda_uaol_stream *stream);

/*
 * The negotiation-time rule assumes the shortest service interval. Once the
 * device's endpoint is known, check the payload of SOF_UAOL_FIFO_INTERVALS
 * actual service intervals, in whole sample frames, against the FIFO share.
 * A stream taking exactly the whole share is valid, e.g. 384 kHz stereo on a
 * 500 us endpoint, only a payload that is larger than the share overruns it.
 */
static int uaol_check_fifo_fit(struct snd_sof_dev *sdev, struct snd_soc_dai *cpu_dai,
			       struct snd_pcm_substream *substream,
			       struct snd_pcm_hw_params *params,
			       const struct usb_offload_stream_hw *hw)
{
	struct snd_soc_dapm_widget *w = snd_soc_dai_get_widget(cpu_dai, substream->stream);
	struct sof_ipc4_copier *ipc4_copier;
	u64 frames, bytes;
	int size;

	if (!w || !hw->service_interval_us)
		return 0;

	ipc4_copier = widget_to_copier(w);

	size = sof_ipc4_uaol_get_fifo_size(sdev, ipc4_copier->dai_index, substream->stream);
	if (size <= 0)
		return 0;

	frames = DIV_ROUND_UP_ULL((u64)params_rate(params) * hw->service_interval_us,
				  USEC_PER_SEC);
	bytes = frames * params_channels(params) *
		uaol_sample_bytes(params_format(params)) * SOF_UAOL_FIFO_INTERVALS;

	if (bytes > size) {
		dev_err(cpu_dai->dev,
			"stream needs %llu bytes of link FIFO, only %d available\n",
			bytes, size);
		return -EINVAL;
	}

	return 0;
}

/*
 * Index of the DMA config TLV describing the feedback link DMA. A UAOL
 * backend has a single CPU DAI, so the audio link DMA is always TLV 0 and
 * the firmware takes the second one as the feedback (host_dma_config[1]).
 */
#define SOF_UAOL_FB_DMA_TLV_IDX	1

static void uaol_dai_release_feedback(struct snd_sof_dev *sdev,
				      struct hda_uaol_stream *stream)
{
	struct snd_soc_dapm_widget *w = snd_soc_dai_get_widget(stream->dai, stream->direction);
	struct sof_ipc4_copier *ipc4_copier = widget_to_copier(w);

	/* a stale TLV would be sent along with the next, feedback-less, device */
	memset(&ipc4_copier->dma_config_tlv[SOF_UAOL_FB_DMA_TLV_IDX], 0,
	       sizeof(ipc4_copier->dma_config_tlv[SOF_UAOL_FB_DMA_TLV_IDX]));

	if (!stream->fb_hext_stream)
		return;

	hda_uaol_release_fb_link_stream(sdev, stream->fb_hext_stream);
	stream->fb_hext_stream = NULL;
}

/*
 * The feedback endpoint is read by the DSP through a link DMA of its own, in
 * the capture direction. Its stream tag is what the firmware needs to bind
 * the UAOL feedback stream to, handed over as a second DMA config TLV after
 * the one of the audio link DMA.
 */
static int uaol_dai_setup_feedback(struct snd_pcm_substream *substream,
				   struct snd_pcm_hw_params *params,
				   struct hda_uaol_stream *stream)
{
	struct snd_soc_dai *cpu_dai = stream->dai;
	struct snd_soc_dapm_widget *w = snd_soc_dai_get_widget(cpu_dai, substream->stream);
	struct snd_sof_dev *sdev = dai_to_sdev(substream, cpu_dai);
	struct sof_ipc4_copier *ipc4_copier = widget_to_copier(w);
	struct sof_ipc4_dma_config_tlv *dma_config_tlv;
	struct sof_ipc4_dma_config *dma_config;
	struct hdac_ext_stream *hext_stream;
	unsigned int format_val;
	unsigned int format_bits;
	int stream_tag;

	if (sdev->dspless_mode_selected)
		return 0;

	/* one 32-bit feedback value per service interval */
	format_bits = snd_hdac_stream_format_bits(SNDRV_PCM_FORMAT_S32_LE,
						  SNDRV_PCM_SUBFORMAT_STD, 32);
	format_val = snd_hdac_stream_format(1, format_bits, params_rate(params));

	hext_stream = hda_uaol_assign_fb_link_stream(sdev, substream, format_val);
	if (!hext_stream) {
		dev_err(cpu_dai->dev, "no link DMA available for the USB feedback\n");
		return -EBUSY;
	}

	stream->fb_hext_stream = hext_stream;
	stream_tag = hdac_stream(hext_stream)->stream_tag;

	dma_config_tlv = &ipc4_copier->dma_config_tlv[SOF_UAOL_FB_DMA_TLV_IDX];
	dma_config_tlv->type = SOF_IPC4_GTW_DMA_CONFIG_ID;
	dma_config_tlv->length = sizeof(dma_config_tlv->dma_config);

	dma_config = &dma_config_tlv->dma_config;
	dma_config->dma_method = SOF_IPC4_DMA_METHOD_HDA;
	dma_config->pre_allocated_by_host = 1;
	dma_config->dma_channel_id = stream_tag - 1;
	dma_config->stream_id = stream_tag;
	dma_config->dma_stream_channel_map.device_count = 1;
	dma_config->dma_priv_config_size = 0;

	dev_dbg(cpu_dai->dev, "USB feedback on link DMA stream %d\n", stream_tag);

	return 0;
}

static void uaol_set_be_format(struct snd_pcm_hw_params *params, snd_pcm_format_t format)
{
	struct snd_mask *fmt = hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT);

	snd_mask_none(fmt);
	snd_mask_set_format(fmt, format);
}

/*
 * Take the device away from the host and finish the gateway configuration
 * with what the acquire reports. The two belong together: the sideband
 * resource the endpoint lands on is what the stream index and the node
 * index are derived from, and it need not come back the same. The stream
 * is started from hw_params and, after a system suspend, from prepare,
 * which is the only callback the resume path runs.
 */
static int uaol_dai_start_stream(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *params,
				 struct hda_uaol_stream *stream)
{
	struct snd_soc_dai *cpu_dai = stream->dai;
	struct snd_sof_dev *sdev = dai_to_sdev(substream, cpu_dai);
	struct sof_intel_hda_dev *hdev = sdev->pdata->hw_pdata;
	int direction = substream->stream;
	struct usb_offload_stream_hw hw;
	struct usb_device *udev;
	int pcm_index;
	int ret;

	if (!hdev->uaol)
		return -ENODEV;

	udev = hda_uaol_selected(hdev, direction, stream->route, &pcm_index);
	if (!udev) {
		dev_err(cpu_dai->dev, "no USB device selected for offload\n");
		return -ENODEV;
	}

	ret = usb_offload_acquire_stream(hdev->uaol, udev, direction, pcm_index,
					 params, &hw);
	if (ret) {
		dev_err(cpu_dai->dev, "failed to acquire the USB stream: %d\n", ret);
		return ret;
	}

	/*
	 * The device may run the stream in another container than the one the
	 * frontend negotiated, SOF only ever hands S24_LE for 24 bits and a
	 * device packing them into 3 bytes will not recognise it. The link DMA
	 * is set up from the params, so it runs the container of the device,
	 * the DSP packs the samples on their way to the link. On hw_params
	 * those are a copy DPCM made for this call, prepare sets them again
	 * for itself.
	 */
	stream->be_format = hw.format;
	uaol_set_be_format(params, hw.format);

	ret = uaol_check_fifo_fit(sdev, cpu_dai, substream, params, &hw);
	if (ret)
		goto err_release;

	ret = uaol_dai_set_endpoint(cpu_dai, direction, &hw);
	if (ret)
		goto err_release;

	if (hw.has_feedback) {
		ret = uaol_dai_setup_feedback(substream, params, stream);
		if (ret)
			goto err_release;
	}

	stream->udev = udev;
	stream->pcm_index = hw.pcm_index;
	stream->be = substream;

	/*
	 * The route control refuses a change once the stream is set here, and
	 * it decides that under the lock the selection is read back with: a
	 * change which went through while the device was being acquired is
	 * seen now, a later one finds the route busy. Back out of the former,
	 * the stream would play to a device the route no longer selects.
	 */
	if (hda_uaol_selected(hdev, direction, stream->route, &pcm_index) != udev ||
	    pcm_index != hw.pcm_index) {
		uaol_dai_stop_stream(substream, stream);
		return -EBUSY;
	}

	return 0;

err_release:
	usb_offload_release_stream(hdev->uaol, udev, direction, hw.pcm_index);
	return ret;
}

static void uaol_dai_stop_stream(struct snd_pcm_substream *substream,
				 struct hda_uaol_stream *stream)
{
	struct snd_sof_dev *sdev = dai_to_sdev(substream, stream->dai);
	struct sof_intel_hda_dev *hdev = sdev->pdata->hw_pdata;
	struct usb_device *udev = stream->udev;

	/* also after an unplug, which takes only the USB side away */
	if (stream->fb_hext_stream)
		uaol_dai_release_feedback(sdev, stream);

	if (!udev)
		return;

	stream->udev = NULL;
	stream->be = NULL;
	usb_offload_release_stream(hdev->uaol, udev, stream->direction, stream->pcm_index);
}

static int uaol_dai_hw_params(struct snd_pcm_substream *substream,
			      struct snd_pcm_hw_params *params,
			      struct snd_soc_dai *cpu_dai)
{
	struct snd_sof_dev *sdev = dai_to_sdev(substream, cpu_dai);
	struct hda_uaol_stream *stream = hda_uaol_stream_get(sdev->pdata->hw_pdata, cpu_dai);
	unsigned int rate, channels;
	snd_pcm_format_t format;
	int ret;

	if (!stream)
		return -ENODEV;

	/*
	 * A second frontend joining the stream's mixer, the deep buffer, runs
	 * the backend's hw_params again while the first one still has it. The
	 * USB stream is acquired once, by whichever frontend came first, and
	 * stays until the last one is gone.
	 *
	 * The two share the one endpoint and the one mixer, so the second one
	 * can only run what the first one opened the backend with. Refuse it
	 * here, before the core prepares anything for it: a frontend failing
	 * further down would take what the first one prepared with it.
	 */
	if (stream->udev) {
		/* pairs with the release once the first frontend's parameters are stored */
		if (!smp_load_acquire(&stream->params_valid) ||
		    params_rate(params) != stream->rate ||
		    params_channels(params) != stream->channels ||
		    params_format(params) != stream->format) {
			dev_err(cpu_dai->dev,
				"stream is open at %u Hz, %u ch, format %d, not %u Hz, %u ch, format %d\n",
				stream->rate, stream->channels, stream->format,
				params_rate(params), params_channels(params),
				params_format(params));
			return -EBUSY;
		}

		/* the link DMA runs the format of the device, not the frontend's */
		uaol_set_be_format(params, stream->be_format);

		return non_hda_dai_hw_params(substream, params, cpu_dai);
	}

	/* the params are set to the format of the device, keep what the frontend asked for */
	rate = params_rate(params);
	channels = params_channels(params);
	format = params_format(params);

	ret = uaol_dai_start_stream(substream, params, stream);
	if (ret)
		return ret;

	ret = non_hda_dai_hw_params(substream, params, cpu_dai);
	if (ret) {
		uaol_dai_stop_stream(substream, stream);
		return ret;
	}

	stream->rate = rate;
	stream->channels = channels;
	stream->format = format;
	/* publish the parameters to the frontend rules and to a joining frontend */
	smp_store_release(&stream->params_valid, true);

	return 0;
}

static int uaol_dai_hw_free(struct snd_pcm_substream *substream,
			    struct snd_soc_dai *cpu_dai)
{
	struct snd_sof_dev *sdev = dai_to_sdev(substream, cpu_dai);
	struct hda_uaol_stream *stream = hda_uaol_stream_get(sdev->pdata->hw_pdata, cpu_dai);
	int ret = hda_dai_hw_free(substream, cpu_dai);

	if (stream) {
		/* the next frontend to open the backend decides the parameters */
		smp_store_release(&stream->params_valid, false);
		uaol_dai_stop_stream(substream, stream);
		/* a stream closed while suspended has nothing left to reacquire */
		stream->suspended = false;
	}

	return ret;
}

/*
 * A system suspend resets the pipeline and frees the DAI copier, and takes
 * the device's streaming altsetting with it, while the stream itself stays
 * open. The resume path only runs prepare on the backend, so nothing
 * reestablishes the USB side on its own and the stream is acquired again
 * from here, before the pipeline is rebuilt.
 */
static int uaol_dai_prepare(struct snd_pcm_substream *substream,
			    struct snd_soc_dai *cpu_dai)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_sof_dev *sdev = dai_to_sdev(substream, cpu_dai);
	struct sof_intel_hda_dev *hdev = sdev->pdata->hw_pdata;
	struct hda_uaol_stream *stream = hda_uaol_stream_get(hdev, cpu_dai);
	struct snd_pcm_hw_params *params;
	int direction = substream->stream;
	int ret;

	if (!stream)
		return non_hda_dai_prepare(substream, cpu_dai);

	params = &rtd->dpcm[direction].hw_params;

	/* left set until the stream is back, for a failed prepare to retry */
	if (stream->suspended) {
		uaol_dai_stop_stream(substream, stream);

		ret = uaol_dai_start_stream(substream, params, stream);
		if (ret)
			return ret;

		stream->suspended = false;
	}

	/* the device is gone, the gateway would run on a released resource */
	if (!stream->udev)
		return -ENODEV;

	/*
	 * The common prepare programs the link DMA again after a stop, from
	 * the backend's own copy of the params, which DPCM keeps at what the
	 * frontend negotiated. Set the format the stream was acquired with,
	 * as it was for hw_params.
	 */
	uaol_set_be_format(params, stream->be_format);

	return non_hda_dai_prepare(substream, cpu_dai);
}

/*
 * The backend is triggered on the way into a system suspend, which is the
 * only notice this driver gets that the stream will have to be reacquired.
 * The feedback link DMA is not the DAI's DMA data, so the common trigger
 * does not know about it and it is run from here, alongside the audio one.
 */
static int uaol_dai_trigger(struct snd_pcm_substream *substream, int cmd,
			    struct snd_soc_dai *cpu_dai)
{
	struct snd_sof_dev *sdev = dai_to_sdev(substream, cpu_dai);
	struct hda_uaol_stream *stream = hda_uaol_stream_get(sdev->pdata->hw_pdata, cpu_dai);
	struct hdac_ext_stream *fb_hext_stream = NULL;
	int ret;

	if (!stream)
		return hda_dai_trigger(substream, cmd, cpu_dai);

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		fb_hext_stream = stream->fb_hext_stream;

	if (cmd == SNDRV_PCM_TRIGGER_SUSPEND)
		stream->suspended = true;

	if (fb_hext_stream &&
	    (cmd == SNDRV_PCM_TRIGGER_START || cmd == SNDRV_PCM_TRIGGER_PAUSE_RELEASE))
		snd_hdac_ext_stream_start(fb_hext_stream);

	ret = hda_dai_trigger(substream, cmd, cpu_dai);

	/* nothing was started: do not leave the feedback stream running */
	if (ret && fb_hext_stream &&
	    (cmd == SNDRV_PCM_TRIGGER_START || cmd == SNDRV_PCM_TRIGGER_PAUSE_RELEASE))
		snd_hdac_ext_stream_clear(fb_hext_stream);

	if (fb_hext_stream &&
	    (cmd == SNDRV_PCM_TRIGGER_STOP || cmd == SNDRV_PCM_TRIGGER_SUSPEND ||
	     cmd == SNDRV_PCM_TRIGGER_PAUSE_PUSH))
		snd_hdac_ext_stream_clear(fb_hext_stream);

	return ret;
}

static const struct snd_soc_dai_ops uaol_dai_ops = {
	.probe = uaol_dai_probe,
	.remove = uaol_dai_remove,
	.hw_params = uaol_dai_hw_params,
	.hw_free = uaol_dai_hw_free,
	.trigger = uaol_dai_trigger,
	.prepare = uaol_dai_prepare,
};

/*
 * Called for every frontend the platform component opens, so the backends it
 * routes to are what says whether there is anything to do here.
 */
void hda_apply_constraint_from_be(struct snd_sof_dev *sdev,
				  struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dpcm *dpcm;

	for_each_dpcm_be(rtd, substream->stream, dpcm) {
		struct snd_soc_dai *be_dai = snd_soc_rtd_to_cpu(dpcm->be, 0);

		if (be_dai->driver->ops == &uaol_dai_ops) {
			hda_uaol_set_constraint(sdev, be_dai, substream);
			return;
		}
	}
}

void hda_uaol_set_dai_drv_ops(struct snd_sof_dev *sdev, struct snd_sof_dsp_ops *ops)
{
	const struct sof_intel_dsp_desc *chip;
	int i;

	chip = get_chip_info(sdev->pdata);

	if (chip->hw_ip_version >= SOF_INTEL_ACE_2_0) {
		for (i = 0; i < ops->num_drv; i++) {
			if (strstr(ops->drv[i].name, "UAOL"))
				ops->drv[i].ops = &uaol_dai_ops;
		}
	}
}

MODULE_IMPORT_NS("USB_AUDIO_OFFLOAD");
