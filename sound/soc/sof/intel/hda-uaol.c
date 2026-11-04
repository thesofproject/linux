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

int hda_uaol_notify_disconnect(struct device *dev, struct usb_device *udev)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(dev);
	struct sof_intel_hda_dev *hdev = sdev->pdata->hw_pdata;
	struct hda_uaol_dev *offload_dev;

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
	struct snd_soc_usb *usb;
	int ret, i;

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
			return ret;

		snd_jack_report(hdev->uaol_jack.jack, 0);
		return 0;
	}

	/* the port belongs to the controller, every UAOL CPU DAI probes here */
	if (hdev->uaol_usb)
		return 0;

	usb = snd_soc_usb_allocate_port(dai->component, hdev);
	if (IS_ERR(usb))
		return PTR_ERR(usb);

	/* a route per backend of a direction, each with a device of its own */
	for (i = 0; i <= SNDRV_PCM_STREAM_LAST; i++) {
		ret = snd_soc_usb_set_num_routes(usb, i, max(hdev->uaol_stream_num[i], 1U));
		if (ret) {
			snd_soc_usb_free_port(usb);
			return ret;
		}
	}

	/*
	 * The UAOL PCMs exist whether or not a device is there to play to, and
	 * nothing else tells userspace which of the two it is. The jack has to
	 * be in place before the port goes up, as that is what reports the
	 * devices which are already connected.
	 */
	ret = snd_soc_card_jack_new(dai->component->card, "USB Offload Jack",
				    SND_JACK_USB, &hdev->uaol_jack);
	if (ret) {
		snd_soc_usb_free_port(usb);
		return ret;
	}

	ret = snd_soc_usb_setup_offload_route_ctl(dai->component, usb);
	if (ret) {
		snd_soc_usb_free_port(usb);
		return ret;
	}

	scoped_guard(mutex, &hdev->uaol_mutex)
		hdev->uaol_usb = usb;

	/* rediscovers the devices which enumerated before the card existed */
	snd_soc_usb_add_port(usb);

	return 0;
}

static int uaol_dai_remove(struct snd_soc_dai *dai)
{
	struct snd_sof_dev *sdev = snd_soc_component_get_drvdata(dai->component);
	struct sof_intel_hda_dev *hdev = sdev->pdata->hw_pdata;
	struct hda_uaol_dev *dev, *tmp;
	struct snd_soc_usb *usb;

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

/* The link DMA is run like the one of the SSP and DMIC DAIs */
static const struct snd_soc_dai_ops uaol_dai_ops = {
	.probe = uaol_dai_probe,
	.remove = uaol_dai_remove,
	.hw_params = non_hda_dai_hw_params,
	.hw_free = hda_dai_hw_free,
	.trigger = hda_dai_trigger,
	.prepare = non_hda_dai_prepare,
};

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
