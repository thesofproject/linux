/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (c) 2022-2025 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef __LINUX_SND_SOC_USB_H
#define __LINUX_SND_SOC_USB_H

#include <sound/soc.h>

enum snd_soc_usb_kctl {
	SND_SOC_USB_KCTL_CARD_ROUTE,
	SND_SOC_USB_KCTL_PCM_ROUTE,
};

/**
 * struct snd_soc_usb_device - SoC USB representation of a USB sound device
 * @card_idx: sound card index associated with USB device
 * @chip_idx: USB sound chip array index
 * @cpcm_idx: capture PCM index array associated with USB device
 * @ppcm_idx: playback PCM index array associated with USB device
 * @cpcm_names: capture PCM name array, parallel to @cpcm_idx
 * @ppcm_names: playback PCM name array, parallel to @ppcm_idx
 * @num_capture: number of capture streams
 * @num_playback: number of playback streams
 * @list: list head for the backend's own use
 * @available_node: link on snd_soc_usb.available_devices, owned by soc-usb
 **/
struct snd_soc_usb_device {
	int card_idx;
	int chip_idx;

	/* PCM index arrays */
	unsigned int *cpcm_idx; /* TODO: capture path is not tested yet */
	unsigned int *ppcm_idx;
	/* human readable PCM name arrays, parallel to the index arrays above */
	const char * const *cpcm_names;
	const char * const *ppcm_names;
	int num_capture; /* TODO: capture path is not tested yet */
	int num_playback;

	struct list_head list;
	struct list_head available_node;
};

/**
 * struct snd_soc_usb_route - one offload route of a direction
 * @sdev: the device picked for the route, NULL while nothing is picked and the
 *	  route follows its default
 * @pcm_idx: the PCM index of @sdev picked, -1 while nothing is picked
 *
 * A backend able to run several offloaded streams in a direction at a time
 * has a route per stream. Two routes may point at the same pair; only one of
 * them can offload it at a time, the other fails when it tries.
 **/
struct snd_soc_usb_route {
	struct snd_soc_usb_device *sdev;
	int pcm_idx;
};

/**
 * struct snd_soc_usb - representation of a SoC USB backend entity
 * @list: list head for SND SOC struct list
 * @component: reference to ASoC component
 * @connection_status_cb: callback to notify connection events
 * @update_offload_route_info: callback to fetch mapped ASoC card and pcm
 *			       device pair.  This is unrelated to the concept
 *			       of DAPM route.  The "route" argument carries
 *			       an array used for a kcontrol output for either
 *			       the card or pcm index.  "path" determines the
 *			       which entry to look for. (ie mapped card or pcm)
 * @available_devices: list of currently connected snd_soc_usb_device entries
 * @routes: the routes of each direction, @num_routes of them
 * @num_routes: number of routes per direction, one unless the backend set
 *		 more with snd_soc_usb_set_num_routes()
 * @priv_data: driver data
 **/
struct snd_soc_usb {
	struct list_head list;
	struct snd_soc_component *component;
	int (*connection_status_cb)(struct snd_soc_usb *usb,
				    struct snd_soc_usb_device *sdev,
				    bool connected);
	int (*update_offload_route_info)(struct snd_soc_component *component,
					 int card, int pcm, int direction,
					 enum snd_soc_usb_kctl path,
					 long *route);

	struct list_head available_devices;
	struct snd_soc_usb_route *routes[SNDRV_PCM_STREAM_LAST + 1];
	unsigned int num_routes[SNDRV_PCM_STREAM_LAST + 1];

	void *priv_data;
};

#if IS_ENABLED(CONFIG_SND_SOC_USB)
int snd_soc_usb_find_supported_format(int card_idx,
				      struct snd_pcm_hw_params *params,
				      int direction);

int snd_soc_usb_connect(struct device *usbdev, struct snd_soc_usb_device *sdev);
int snd_soc_usb_disconnect(struct device *usbdev, struct snd_soc_usb_device *sdev);
void *snd_soc_usb_find_priv_data(struct device *usbdev);

int snd_soc_usb_setup_offload_jack(struct snd_soc_component *component,
				   struct snd_soc_jack *jack);
int snd_soc_usb_update_offload_route(struct device *dev, int card, int pcm,
				     int direction, enum snd_soc_usb_kctl path,
				     long *route);

struct snd_soc_usb *snd_soc_usb_allocate_port(struct snd_soc_component *component,
					      void *data);
void snd_soc_usb_free_port(struct snd_soc_usb *usb);
int snd_soc_usb_set_num_routes(struct snd_soc_usb *usb, int direction,
			       unsigned int num_routes);
void snd_soc_usb_add_port(struct snd_soc_usb *usb);
void snd_soc_usb_remove_port(struct snd_soc_usb *usb);

int snd_soc_usb_get_selected_device(struct snd_soc_usb *usb, int direction,
				    unsigned int route,
				    struct snd_soc_usb_device **sdev,
				    int *pcm_index);
#else
static inline int
snd_soc_usb_find_supported_format(int card_idx, struct snd_pcm_hw_params *params,
				  int direction)
{
	return -EINVAL;
}

static inline int snd_soc_usb_connect(struct device *usbdev,
				      struct snd_soc_usb_device *sdev)
{
	return -ENODEV;
}

static inline int snd_soc_usb_disconnect(struct device *usbdev,
					 struct snd_soc_usb_device *sdev)
{
	return -EINVAL;
}

static inline void *snd_soc_usb_find_priv_data(struct device *usbdev)
{
	return NULL;
}

static inline int snd_soc_usb_setup_offload_jack(struct snd_soc_component *component,
						 struct snd_soc_jack *jack)
{
	return 0;
}

static inline int
snd_soc_usb_update_offload_route(struct device *dev, int card, int pcm,
				 int direction, enum snd_soc_usb_kctl path,
				 long *route)
{
	return -ENODEV;
}

static inline struct snd_soc_usb *
snd_soc_usb_allocate_port(struct snd_soc_component *component, void *data)
{
	return ERR_PTR(-ENOMEM);
}

static inline void snd_soc_usb_free_port(struct snd_soc_usb *usb)
{ }

static inline int snd_soc_usb_set_num_routes(struct snd_soc_usb *usb, int direction,
					     unsigned int num_routes)
{
	return -ENODEV;
}

static inline void snd_soc_usb_add_port(struct snd_soc_usb *usb)
{ }

static inline void snd_soc_usb_remove_port(struct snd_soc_usb *usb)
{ }

static inline int snd_soc_usb_get_selected_device(struct snd_soc_usb *usb, int direction,
						  unsigned int route,
						  struct snd_soc_usb_device **sdev,
						  int *pcm_index)
{
	return -ENODEV;
}
#endif /* IS_ENABLED(CONFIG_SND_SOC_USB) */
#endif /*__LINUX_SND_SOC_USB_H */
