// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2022-2025 Qualcomm Innovation Center, Inc. All rights reserved.
 */
#include <linux/of.h>
#include <linux/usb.h>

#include <sound/jack.h>
#include <sound/soc-usb.h>

#include "../usb/card.h"

static DEFINE_MUTEX(ctx_mutex);
static LIST_HEAD(usb_ctx_list);

static struct device_node *snd_soc_find_phandle(struct device *dev)
{
	struct device_node *node;

	node = of_parse_phandle(dev->of_node, "usb-soc-be", 0);
	if (!node)
		return ERR_PTR(-ENODEV);

	return node;
}

static struct snd_soc_usb *snd_soc_usb_ctx_lookup_node(struct device_node *node)
{
	struct snd_soc_usb *ctx;

	if (!node)
		return NULL;

	list_for_each_entry(ctx, &usb_ctx_list, list) {
		if (ctx->component->dev->of_node == node)
			return ctx;
	}

	return NULL;
}

static struct snd_soc_usb *snd_soc_usb_ctx_lookup(struct device *dev)
{
	struct snd_soc_usb *ctx;

	if (!dev)
		return NULL;

	if (dev->of_node)
		return snd_soc_usb_ctx_lookup_node(dev->of_node);

	/* ACPI/PCI platforms have no of_node, match on the device itself */
	list_for_each_entry(ctx, &usb_ctx_list, list) {
		if (ctx->component->dev == dev)
			return ctx;
	}

	return NULL;
}

static struct snd_soc_usb *snd_soc_find_usb_ctx(struct device *dev)
{
	struct snd_soc_usb *ctx;
	struct device_node *node;

	node = snd_soc_find_phandle(dev);
	if (!IS_ERR(node)) {
		ctx = snd_soc_usb_ctx_lookup_node(node);
		of_node_put(node);

		return ctx;
	}

	return snd_soc_usb_ctx_lookup(dev);
}

/**
 * struct snd_soc_usb_route_entry - one (device, pcm) pair offloadable for a
 *				    direction
 * @sdev: the device this pcm belongs to
 * @pcm_idx: the USB SND PCM device index within @sdev
 * @name: human readable name for this entry, borrowed from @sdev, valid only
 *	 as long as @sdev stays in usb->available_devices
 */
struct snd_soc_usb_route_entry {
	struct snd_soc_usb_device *sdev;
	int pcm_idx;
	const char *name;
};

/*
 * Caller holds ctx_mutex. Walks usb->available_devices in order, treating
 * every (device, pcm) pair for @direction across every device as one flat,
 * positionally indexed list - this positional index is what the route
 * kcontrol's enumerated item number means. Returns false once @index runs
 * past the end.
 */
static bool snd_soc_usb_route_entry_at(struct snd_soc_usb *usb, int direction,
				       unsigned int index,
				       struct snd_soc_usb_route_entry *entry)
{
	struct snd_soc_usb_device *sdev;
	unsigned int *pcm_idx;
	const char * const *pcm_names;
	int num, i;

	list_for_each_entry(sdev, &usb->available_devices, available_node) {
		if (direction == SNDRV_PCM_STREAM_PLAYBACK) {
			num = sdev->num_playback;
			pcm_idx = sdev->ppcm_idx;
			pcm_names = sdev->ppcm_names;
		} else {
			num = sdev->num_capture;
			pcm_idx = sdev->cpcm_idx;
			pcm_names = sdev->cpcm_names;
		}

		for (i = 0; i < num; i++) {
			if (index == 0) {
				entry->sdev = sdev;
				entry->pcm_idx = pcm_idx[i];
				entry->name = (pcm_names && pcm_names[i]) ?
					      pcm_names[i] : "USB PCM";
				return true;
			}
			index--;
		}
	}

	return false;
}

/*
 * Caller holds ctx_mutex. The pair @route of @direction currently points at,
 * and its position in the flat list: the one picked for it, or while nothing
 * is picked (or the picked pair is gone) the one it gets by default. By
 * default route N gets the Nth pair, wrapping around when there are fewer
 * pairs than routes, so that the routes spread over what is connected. The
 * default is worked out here on every call instead of being stored, it
 * follows the devices as they come and go. Returns false when no pair is
 * available.
 */
static bool snd_soc_usb_route_resolve(struct snd_soc_usb *usb, int direction,
				      unsigned int route,
				      struct snd_soc_usb_route_entry *entry,
				      unsigned int *index)
{
	struct snd_soc_usb_route *r = &usb->routes[direction][route];
	unsigned int i, count = 0;

	for (i = 0; snd_soc_usb_route_entry_at(usb, direction, i, entry); i++) {
		if (r->sdev && entry->sdev == r->sdev && entry->pcm_idx == r->pcm_idx) {
			*index = i;
			return true;
		}
		count++;
	}

	if (!count)
		return false;

	*index = route % count;

	return snd_soc_usb_route_entry_at(usb, direction, *index, entry);
}

/* SOC USB sound kcontrols */
/**
 * snd_soc_usb_setup_offload_jack() - Create USB offloading jack
 * @component: USB DPCM backend DAI component
 * @jack: jack structure to create
 *
 * Creates a jack device for notifying userspace of the availability
 * of an offload capable device.
 *
 * Returns 0 on success, negative on error.
 *
 */
int snd_soc_usb_setup_offload_jack(struct snd_soc_component *component,
				   struct snd_soc_jack *jack)
{
	int ret;

	ret = snd_soc_card_jack_new(component->card, "USB Offload Jack",
				    SND_JACK_USB, jack);
	if (ret < 0) {
		dev_err(component->card->dev, "Unable to add USB offload jack: %d\n",
			ret);
		return ret;
	}

	ret = snd_soc_component_set_jack(component, jack, NULL);
	if (ret) {
		dev_err(component->card->dev, "Failed to set jack: %d\n", ret);
		return ret;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_usb_setup_offload_jack);

/**
 * snd_soc_usb_update_offload_route - Find active USB offload path
 * @dev: USB device to get offload status
 * @card: USB card index
 * @pcm: USB PCM device index
 * @direction: playback or capture direction
 * @path: pcm or card index
 * @route: pointer to route output array
 *
 * Fetch the current status for the USB SND card and PCM device indexes
 * specified.  The "route" argument should be an array of integers being
 * used for a kcontrol output.  The first element should have the selected
 * card index, and the second element should have the selected pcm device
 * index.
 */
int snd_soc_usb_update_offload_route(struct device *dev, int card, int pcm,
				     int direction, enum snd_soc_usb_kctl path,
				     long *route)
{
	struct snd_soc_usb *ctx;
	int ret = -ENODEV;

	mutex_lock(&ctx_mutex);
	ctx = snd_soc_find_usb_ctx(dev);
	if (!ctx)
		goto exit;

	if (ctx->update_offload_route_info)
		ret = ctx->update_offload_route_info(ctx->component, card, pcm,
						     direction, path, route);
exit:
	mutex_unlock(&ctx_mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(snd_soc_usb_update_offload_route);

/**
 * snd_soc_usb_get_selected_device() - Get the device/pcm a route points at
 * @usb: soc usb port
 * @direction: SNDRV_PCM_STREAM_PLAYBACK or SNDRV_PCM_STREAM_CAPTURE
 * @route: the route of @direction, below usb->num_routes[direction]
 * @sdev: set to the routed device, or NULL if nothing is selected
 * @pcm_index: set to the routed device's USB SND PCM device index
 *
 * A backend consults this at stream acquire time instead of picking a device
 * itself, so the stream goes to whatever the route kcontrol (or the
 * connect-time default) currently points at.
 *
 * Return: 0 on success (even if nothing is selected - check @sdev), negative
 * on error.
 */
int snd_soc_usb_get_selected_device(struct snd_soc_usb *usb, int direction,
				    unsigned int route,
				    struct snd_soc_usb_device **sdev,
				    int *pcm_index)
{
	struct snd_soc_usb_route_entry entry;
	unsigned int index;

	if (!usb || direction < 0 || direction > SNDRV_PCM_STREAM_LAST)
		return -EINVAL;

	if (route >= usb->num_routes[direction])
		return -EINVAL;

	guard(mutex)(&ctx_mutex);
	if (snd_soc_usb_route_resolve(usb, direction, route, &entry, &index)) {
		*sdev = entry.sdev;
		*pcm_index = entry.pcm_idx;
	} else {
		*sdev = NULL;
		*pcm_index = -1;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_usb_get_selected_device);

/**
 * snd_soc_usb_find_priv_data() - Retrieve private data stored
 * @usbdev: device reference
 *
 * Fetch the private data stored in the USB SND SoC structure.
 *
 */
void *snd_soc_usb_find_priv_data(struct device *usbdev)
{
	struct snd_soc_usb *ctx;

	mutex_lock(&ctx_mutex);
	ctx = snd_soc_find_usb_ctx(usbdev);
	mutex_unlock(&ctx_mutex);

	return ctx ? ctx->priv_data : NULL;
}
EXPORT_SYMBOL_GPL(snd_soc_usb_find_priv_data);

/**
 * snd_soc_usb_find_supported_format() - Check if audio format is supported
 * @card_idx: USB sound chip array index
 * @params: PCM parameters
 * @direction: capture or playback
 *
 * Ensure that a requested audio profile from the ASoC side is able to be
 * supported by the USB device.
 *
 * Return 0 on success, negative on error.
 *
 */
int snd_soc_usb_find_supported_format(int card_idx,
				      struct snd_pcm_hw_params *params,
				      int direction)
{
	struct snd_usb_stream *as;

	as = snd_usb_find_suppported_substream(card_idx, params, direction);
	if (!as)
		return -EOPNOTSUPP;

	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_usb_find_supported_format);

/* Not under ctx_mutex: the port is not yet, or no longer, on usb_ctx_list */
static int snd_soc_usb_alloc_routes(struct snd_soc_usb *usb, int direction,
				    unsigned int num_routes)
{
	struct snd_soc_usb_route *routes;
	unsigned int i;

	if (!num_routes)
		return -EINVAL;

	routes = kcalloc(num_routes, sizeof(*routes), GFP_KERNEL);
	if (!routes)
		return -ENOMEM;

	for (i = 0; i < num_routes; i++)
		routes[i].pcm_idx = -1;

	kfree(usb->routes[direction]);
	usb->routes[direction] = routes;
	usb->num_routes[direction] = num_routes;

	return 0;
}

/**
 * snd_soc_usb_allocate_port() - allocate a SoC USB port for offloading support
 * @component: USB DPCM backend DAI component
 * @data: private data
 *
 * Allocate and initialize a SoC USB port.  The SoC USB port is used to communicate
 * different USB audio devices attached, in order to start audio offloading handled
 * by an ASoC entity.  USB device plug in/out events are signaled with a
 * notification, but don't directly impact the memory allocated for the SoC USB
 * port. The port starts out with one route per direction, see
 * snd_soc_usb_set_num_routes().
 *
 */
struct snd_soc_usb *snd_soc_usb_allocate_port(struct snd_soc_component *component,
					      void *data)
{
	struct snd_soc_usb *usb;
	int i;

	usb = kzalloc_obj(*usb);
	if (!usb)
		return ERR_PTR(-ENOMEM);

	usb->component = component;
	usb->priv_data = data;

	INIT_LIST_HEAD(&usb->available_devices);
	for (i = 0; i <= SNDRV_PCM_STREAM_LAST; i++) {
		if (snd_soc_usb_alloc_routes(usb, i, 1)) {
			snd_soc_usb_free_port(usb);
			return ERR_PTR(-ENOMEM);
		}
	}

	return usb;
}
EXPORT_SYMBOL_GPL(snd_soc_usb_allocate_port);

/**
 * snd_soc_usb_set_num_routes() - Set the number of offload routes of a direction
 * @usb: allocated SoC USB port, not yet added with snd_soc_usb_add_port()
 * @direction: SNDRV_PCM_STREAM_PLAYBACK or SNDRV_PCM_STREAM_CAPTURE
 * @num_routes: streams the backend can offload at a time in @direction
 *
 * A backend running several offloaded streams in a direction at a time gets
 * a route, and with snd_soc_usb_setup_offload_route_ctl() a kcontrol, per
 * stream, so that each can be pointed at a (device, pcm) pair of its own.
 * To be called before the route kcontrols are created and the port is added.
 *
 * Return: 0 on success, negative on error.
 */
int snd_soc_usb_set_num_routes(struct snd_soc_usb *usb, int direction,
			       unsigned int num_routes)
{
	if (!usb || direction < 0 || direction > SNDRV_PCM_STREAM_LAST)
		return -EINVAL;

	return snd_soc_usb_alloc_routes(usb, direction, num_routes);
}
EXPORT_SYMBOL_GPL(snd_soc_usb_set_num_routes);

/**
 * snd_soc_usb_free_port() - free a SoC USB port used for offloading support
 * @usb: allocated SoC USB port
 *
 * Free and remove the SoC USB port from the available list of ports.  This will
 * ensure that the communication between USB SND and ASoC is halted.
 *
 */
void snd_soc_usb_free_port(struct snd_soc_usb *usb)
{
	int i;

	snd_soc_usb_remove_port(usb);
	for (i = 0; i <= SNDRV_PCM_STREAM_LAST; i++)
		kfree(usb->routes[i]);
	kfree(usb);
}
EXPORT_SYMBOL_GPL(snd_soc_usb_free_port);

/**
 * snd_soc_usb_add_port() - Add a USB backend port
 * @usb: soc usb port to add
 *
 * Register a USB backend DAI link to the USB SoC framework.  Memory is allocated
 * as part of the USB backend DAI link.
 *
 */
void snd_soc_usb_add_port(struct snd_soc_usb *usb)
{
	mutex_lock(&ctx_mutex);
	list_add_tail(&usb->list, &usb_ctx_list);
	mutex_unlock(&ctx_mutex);

	snd_usb_rediscover_devices();
}
EXPORT_SYMBOL_GPL(snd_soc_usb_add_port);

/**
 * snd_soc_usb_remove_port() - Remove a USB backend port
 * @usb: soc usb port to remove
 *
 * Remove a USB backend DAI link from USB SoC.  Memory is freed when USB backend
 * DAI is removed, or when snd_soc_usb_free_port() is called.
 *
 */
void snd_soc_usb_remove_port(struct snd_soc_usb *usb)
{
	struct snd_soc_usb *ctx, *tmp;

	mutex_lock(&ctx_mutex);
	list_for_each_entry_safe(ctx, tmp, &usb_ctx_list, list) {
		if (ctx == usb) {
			list_del(&ctx->list);
			break;
		}
	}
	mutex_unlock(&ctx_mutex);
}
EXPORT_SYMBOL_GPL(snd_soc_usb_remove_port);

/*
 * Caller holds ctx_mutex. The device list changed: the items of every route
 * control did, and so can the pair a route which was not picked for follows.
 */
static void snd_soc_usb_notify_routes_changed(struct snd_soc_usb *ctx)
{
	int direction;

	if (!ctx->component || !ctx->component->card)
		return;

	for (direction = 0; direction <= SNDRV_PCM_STREAM_LAST; direction++) {
		unsigned int route;

		for (route = 0; route < ctx->num_routes[direction]; route++) {
			struct snd_kcontrol *kctl = ctx->routes[direction][route].kctl;

			if (kctl)
				snd_ctl_notify(ctx->component->card->snd_card,
					       SNDRV_CTL_EVENT_MASK_INFO |
					       SNDRV_CTL_EVENT_MASK_VALUE, &kctl->id);
		}
	}
}

/* Caller holds ctx_mutex. Whether @sdev is on the port's available_devices. */
static bool snd_soc_usb_device_listed(struct snd_soc_usb *ctx,
				      struct snd_soc_usb_device *sdev)
{
	struct snd_soc_usb_device *entry;

	list_for_each_entry(entry, &ctx->available_devices, available_node) {
		if (entry == sdev)
			return true;
	}

	return false;
}

/**
 * snd_soc_usb_connect() - Notification of USB device connection
 * @usbdev: USB bus device
 * @sdev: USB SND device to add
 *
 * Notify of a new USB SND device connection.  The sdev->card_idx can be used to
 * handle how the DPCM backend selects, which device to enable USB offloading
 * on.
 *
 * Return: 0 on success, -ENODEV if no port is registered for @usbdev (the
 * device is then not listed and must not be reported as disconnected later),
 * -EEXIST if @sdev is connected already.
 */
int snd_soc_usb_connect(struct device *usbdev, struct snd_soc_usb_device *sdev)
{
	struct snd_soc_usb *ctx;
	int ret = 0;

	if (!usbdev)
		return -ENODEV;

	mutex_lock(&ctx_mutex);
	ctx = snd_soc_find_usb_ctx(usbdev);
	if (!ctx) {
		ret = -ENODEV;
		goto exit;
	}

	/* the device is linked into the list through itself, it can only be listed once */
	if (WARN_ON(snd_soc_usb_device_listed(ctx, sdev))) {
		ret = -EEXIST;
		goto exit;
	}

	if (ctx->connection_status_cb)
		ctx->connection_status_cb(ctx, sdev, true);

	list_add_tail(&sdev->available_node, &ctx->available_devices);

	snd_soc_usb_notify_routes_changed(ctx);

exit:
	mutex_unlock(&ctx_mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(snd_soc_usb_connect);

/**
 * snd_soc_usb_disconnect() - Notification of USB device disconnection
 * @usbdev: USB bus device
 * @sdev: USB SND device to remove
 *
 * Notify of a new USB SND device disconnection to the USB backend.
 *
 */
int snd_soc_usb_disconnect(struct device *usbdev, struct snd_soc_usb_device *sdev)
{
	struct snd_soc_usb *ctx;
	int direction;

	if (!usbdev)
		return -ENODEV;

	mutex_lock(&ctx_mutex);
	ctx = snd_soc_find_usb_ctx(usbdev);
	if (!ctx)
		goto exit;

	if (ctx->connection_status_cb)
		ctx->connection_status_cb(ctx, sdev, false);

	/* a device the platform never connected has nothing to take back */
	if (!snd_soc_usb_device_listed(ctx, sdev))
		goto exit;

	list_del(&sdev->available_node);

	/* a route picked for the device falls back to its default */
	for (direction = 0; direction <= SNDRV_PCM_STREAM_LAST; direction++) {
		unsigned int route;

		for (route = 0; route < ctx->num_routes[direction]; route++) {
			struct snd_soc_usb_route *r = &ctx->routes[direction][route];

			if (r->sdev == sdev) {
				r->sdev = NULL;
				r->pcm_idx = -1;
			}
		}
	}

	snd_soc_usb_notify_routes_changed(ctx);

exit:
	mutex_unlock(&ctx_mutex);

	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_usb_disconnect);

/* The kcontrol's private_value carries the direction and the route */
#define SND_SOC_USB_KCTL_VALUE(direction, route)	((direction) | ((route) << 8))
#define SND_SOC_USB_KCTL_DIRECTION(value)		((value) & 0xff)
#define SND_SOC_USB_KCTL_ROUTE(value)			((value) >> 8)

static int snd_soc_usb_route_info(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_info *uinfo)
{
	struct snd_soc_usb *usb = snd_kcontrol_chip(kcontrol);
	int direction = SND_SOC_USB_KCTL_DIRECTION(kcontrol->private_value);
	struct snd_soc_usb_route_entry entry;
	unsigned int count;

	uinfo->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	uinfo->count = 1;

	guard(mutex)(&ctx_mutex);

	for (count = 0; snd_soc_usb_route_entry_at(usb, direction, count, &entry); count++)
		;

	/* with no route available the only item is "None" */
	if (!count) {
		uinfo->value.enumerated.items = 1;
		uinfo->value.enumerated.item = 0;
		strscpy(uinfo->value.enumerated.name, "None",
			sizeof(uinfo->value.enumerated.name));
		return 0;
	}

	uinfo->value.enumerated.items = count;

	if (uinfo->value.enumerated.item >= count)
		uinfo->value.enumerated.item = count - 1;

	snd_soc_usb_route_entry_at(usb, direction, uinfo->value.enumerated.item, &entry);
	strscpy(uinfo->value.enumerated.name, entry.name,
		sizeof(uinfo->value.enumerated.name));

	return 0;
}

static int snd_soc_usb_route_get(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_usb *usb = snd_kcontrol_chip(kcontrol);
	int direction = SND_SOC_USB_KCTL_DIRECTION(kcontrol->private_value);
	unsigned int route = SND_SOC_USB_KCTL_ROUTE(kcontrol->private_value);
	struct snd_soc_usb_route_entry entry;
	unsigned int index = 0;

	scoped_guard(mutex, &ctx_mutex) {
		/* item 0 is "None" when nothing is available */
		if (!snd_soc_usb_route_resolve(usb, direction, route, &entry, &index))
			index = 0;
	}

	ucontrol->value.enumerated.item[0] = index;

	return 0;
}

static int snd_soc_usb_route_put(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_usb *usb = snd_kcontrol_chip(kcontrol);
	int direction = SND_SOC_USB_KCTL_DIRECTION(kcontrol->private_value);
	unsigned int route = SND_SOC_USB_KCTL_ROUTE(kcontrol->private_value);
	unsigned int requested = ucontrol->value.enumerated.item[0];
	struct snd_soc_usb_route *r = &usb->routes[direction][route];
	struct snd_soc_usb_route_entry entry, current_entry;
	unsigned int current_index;
	bool changed;

	guard(mutex)(&ctx_mutex);

	/* item 0 is "None" when nothing is available, accept it as a no-op */
	if (!snd_soc_usb_route_entry_at(usb, direction, requested, &entry))
		return requested ? -EINVAL : 0;

	changed = !snd_soc_usb_route_resolve(usb, direction, route, &current_entry,
					     &current_index) ||
		  current_index != requested;

	if (changed && usb->is_route_busy && usb->is_route_busy(usb, direction, route))
		return -EBUSY;

	/* picking what the route points at already pins it there */
	r->sdev = entry.sdev;
	r->pcm_idx = entry.pcm_idx;

	return changed;
}

static const struct snd_kcontrol_new snd_soc_usb_route_ctl_template = {
	.iface = SNDRV_CTL_ELEM_IFACE_CARD,
	.info = snd_soc_usb_route_info,
	.get = snd_soc_usb_route_get,
	.put = snd_soc_usb_route_put,
};

/**
 * snd_soc_usb_setup_offload_route_ctl() - Create the offload route kcontrols
 * @component: USB DPCM backend DAI component
 * @usb: soc usb port to attach the controls to
 *
 * Creates one writable enumerated kcontrol per route of each direction,
 * "USB Offload 0 Playback Route", "USB Offload 1 Playback Route", "USB Offload
 * 0 Capture Route" and so on, letting userspace pick which of the currently
 * connected devices' PCMs the offloaded stream of that route is routed to.
 * The number is the route and matches the backend's ordinal, whatever the
 * number of routes a direction has. The item list is rebuilt live from
 * usb->available_devices on every query, so it always reflects what is
 * currently connected; with nothing connected the single item is "None".
 *
 * Return: 0 on success, negative on error.
 */
int snd_soc_usb_setup_offload_route_ctl(struct snd_soc_component *component,
					struct snd_soc_usb *usb)
{
	static const char * const names[] = {
		[SNDRV_PCM_STREAM_PLAYBACK] = "Playback",
		[SNDRV_PCM_STREAM_CAPTURE]  = "Capture",
	};
	int direction, ret;

	for (direction = 0; direction <= SNDRV_PCM_STREAM_LAST; direction++) {
		unsigned int route;

		for (route = 0; route < usb->num_routes[direction]; route++) {
			struct snd_kcontrol_new kctl = snd_soc_usb_route_ctl_template;
			char name[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
			struct snd_kcontrol *kcontrol;

			snprintf(name, sizeof(name), "USB Offload %u %s Route",
				 route, names[direction]);

			kctl.name = name;
			kctl.private_value = SND_SOC_USB_KCTL_VALUE(direction, route);

			kcontrol = snd_ctl_new1(&kctl, usb);
			if (!kcontrol)
				return -ENOMEM;

			ret = snd_ctl_add(component->card->snd_card, kcontrol);
			if (ret < 0)
				return ret;

			usb->routes[direction][route].kctl = kcontrol;
		}
	}

	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_usb_setup_offload_route_ctl);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("SoC USB driver for offloading");
