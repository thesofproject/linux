/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * USB audio offload.
 *
 * An offloaded USB audio stream is driven by a DSP instead of by the
 * host: the host controller routes the endpoint's traffic to the DSP over
 * a dedicated link, an xHCI Audio Sideband resource.
 *
 * The USB audio offload library tracks offload capable USB audio devices
 * and forwards their lifecycle to the driver owning the DSP. That driver
 * installs a struct usb_offload_ops and turns each callback into whatever
 * its DSP expects, normally an IPC message. The library builds no IPC
 * message, touches no register and owns no interrupt.
 *
 * Lifecycle:
 *
 *	ctx = usb_offload_register(&res);
 *	...devices are reported via ops->notify_connect/notify_disconnect...
 *	usb_offload_unregister(ctx);
 *
 * One context stands for one offload engine, the link over which a DSP
 * reaches a host controller. A host with several of them registers one
 * context for each.
 *
 * The data path is not driven from here. The host's backend DAI reserves
 * the endpoint resources itself with usb_offload_acquire_stream().
 */

#ifndef __SOUND_USB_OFFLOAD_H
#define __SOUND_USB_OFFLOAD_H

#include <linux/types.h>

#include <sound/asound.h>

struct device;
struct snd_pcm_hw_params;
struct usb_device;
struct usb_offload_ctx;

#define USB_OFFLOAD_MAX_PCM		8
#define USB_OFFLOAD_PCM_NAME_LEN	64

/**
 * struct usb_offload_usb_dev_info - identity of an offloadable USB device
 * @card_idx: snd_card number
 * @chip_idx: snd_usb_audio index
 * @num_playback: populated @ppcm_idx entries
 * @num_capture: populated @cpcm_idx entries
 * @ppcm_idx: playback PCM device numbers
 * @cpcm_idx: capture PCM device numbers
 * @ppcm_names: playback PCM names, parallel to @ppcm_idx - "<card>,<pcm> |
 *	       <product> - <pcm name>" so that a device offering several
 *	       destinations, and several devices offering the same generic PCM
 *	       name, both stay distinguishable
 * @cpcm_names: capture PCM names, parallel to @cpcm_idx, same convention
 *
 * Filled in by the snd-usb-audio ownership bridge, which holds the
 * struct snd_usb_audio chip, and handed to the host on connect.
 */
struct usb_offload_usb_dev_info {
	int		card_idx;
	int		chip_idx;
	int		num_playback;
	int		num_capture;
	unsigned int	ppcm_idx[USB_OFFLOAD_MAX_PCM];
	unsigned int	cpcm_idx[USB_OFFLOAD_MAX_PCM];
	char		ppcm_names[USB_OFFLOAD_MAX_PCM][USB_OFFLOAD_PCM_NAME_LEN];
	char		cpcm_names[USB_OFFLOAD_MAX_PCM][USB_OFFLOAD_PCM_NAME_LEN];
};

/**
 * struct usb_offload_ops - host callback table, library to host
 * @notify_connect: an offloadable device appeared
 * @notify_disconnect: an offloaded device went away
 * @get_resource_range: the sideband resources an endpoint may be placed on,
 *		      as an inclusive range of xHCI Audio Sideband resource
 *		      IDs, @first to @last. @direction is the SNDRV_PCM_STREAM_*
 *		      value of the stream and @feedback tells a playback stream's
 *		      clock feedback endpoint from its data endpoint. Optional:
 *		      an offload engine whose firmware ties a function to
 *		      particular resources answers, one that does not care
 *		      leaves it unset and the lowest free resource is taken.
 *		      An error fails the acquisition of the stream.
 *
 * @notify_connect and @notify_disconnect are mandatory. All callbacks run in
 * process context.
 */
struct usb_offload_ops {
	int (*notify_connect)(struct device *dev, struct usb_device *udev,
			      const struct usb_offload_usb_dev_info *id);
	int (*notify_disconnect)(struct device *dev, struct usb_device *udev);

	int (*get_resource_range)(struct device *dev, int direction, bool feedback,
				  unsigned int *first, unsigned int *last);
};

/**
 * struct usb_offload_res - what the host hands the library at registration
 * @dev: the host's device, passed back to the @ops callbacks
 * @ops: host callback table
 * @s24_3le_as_s24_le: treat a device that only offers S24_3LE as if it also
 *		     offered S24_LE when refining a stream's format, and have
 *		     usb_offload_acquire_stream() run the stream in S24_3LE for
 *		     the device, see usb_offload_stream_hw.format. Set this when
 *		     the host's own pipeline only has a 24-bit representation
 *		     as S24_LE (24 valid bits in a 4-byte container) and cannot
 *		     take S24_3LE directly; leave false otherwise.
 * @async_playback_on_host: keep playback streams whose asynchronous OUT
 *		     endpoint needs explicit clock feedback on the host: their
 *		     PCMs are not reported in the device identity when no
 *		     other format can be offloaded, the formats are skipped
 *		     when refining hw_params, and usb_offload_acquire_stream()
 *		     refuses them. Set this when the DSP applies the feedback
 *		     in a way that degrades the audio. The allow_feedback
 *		     module parameter of the library overrides it for testing.
 *		     Playback with implicit feedback, and the capture it syncs
 *		     to, stays on the host whatever this says: the DSP cannot
 *		     follow that clock.
 */
struct usb_offload_res {
	struct device			*dev;
	const struct usb_offload_ops	*ops;
	bool				s24_3le_as_s24_le;
	bool				async_playback_on_host;
};

struct usb_offload_ctx *usb_offload_register(const struct usb_offload_res *res);
void usb_offload_unregister(struct usb_offload_ctx *ctx);

/**
 * struct usb_offload_ep_info - an endpoint handed to the DSP
 * @ep_addr: bEndpointAddress of the endpoint
 * @resource: xHCI Audio Sideband resource ID the endpoint is bound to
 * @maxpacket: wMaxPacketSize of the endpoint
 * @interval: period of the endpoint as an isochronous bInterval, 2^(n-1)
 *	      frames (full speed) or microframes (high speed). For a feedback
 *	      endpoint this is the refresh period the host would poll it at,
 *	      which a UAC1 device states in bRefresh rather than bInterval.
 */
struct usb_offload_ep_info {
	u8				ep_addr;
	u8				resource;
	u16				maxpacket;
	u8				interval;
};

/**
 * struct usb_offload_stream_hw - resources reserved for one offloaded stream
 * @pcm_index: USB SND PCM device number the stream was acquired on, to be
 *	       passed back to usb_offload_release_stream()
 * @bus_devid: PCI bus, device and function of the xHCI controller, as
 *	       PCI_DEVID() lays them out
 * @format: the sample format the device runs the stream in. It is the format of
 *	    the params handed to usb_offload_acquire_stream(), except for the
 *	    container of a 24-bit stream, see @s24_3le_as_s24_le
 * @slot_id: xHCI slot ID of the USB device
 * @high_speed: the root port the device hangs off runs at high speed
 * @split: the endpoint is reached through a USB 2.0 transaction translator
 * @data: the isochronous data endpoint
 * @feedback: the explicit clock feedback endpoint of a playback stream,
 *	      valid when @has_feedback is set
 * @has_feedback: @feedback has been reserved for the DSP
 * @service_interval_us: service interval of @data in microseconds: a multiple
 *			 of a 1 ms frame for a full speed device, of a 125 us
 *			 microframe for a high speed one
 *
 * Filled in by usb_offload_acquire_stream() and released again by
 * usb_offload_release_stream().
 */
struct usb_offload_stream_hw {
	int				pcm_index;
	snd_pcm_format_t		format;
	u16				bus_devid;
	u8				slot_id;
	bool				high_speed;
	bool				split;
	struct usb_offload_ep_info	data;
	struct usb_offload_ep_info	feedback;
	bool				has_feedback;
	unsigned int			service_interval_us;
};

/*
 * Data-path resource acquisition
 *
 * acquire_stream() starts the stream on the USB side and reports the
 * connected device's data endpoint so that the caller can program its DSP.
 * The USB PCM is taken away from the host for as long as it is held; the
 * other PCMs of the device are not affected and can be acquired alongside.
 * release_stream() reverses it, naming the stream by its direction and the
 * USB PCM device number acquire_stream() reported in
 * usb_offload_stream_hw.pcm_index.
 *
 * @params is the format to run the device at and @direction an
 * SNDRV_PCM_STREAM_* value. @pcm_index pins the search to one USB SND PCM
 * device of a multi-stream device (e.g. via snd_soc_usb_get_selected_device()),
 * or is negative to consider every PCM stream of the device.
 */
int usb_offload_acquire_stream(struct usb_offload_ctx *ctx,
			       struct usb_device *udev, int direction,
			       int pcm_index,
			       struct snd_pcm_hw_params *params,
			       struct usb_offload_stream_hw *hw);
int usb_offload_release_stream(struct usb_offload_ctx *ctx,
			       struct usb_device *udev, int direction,
			       int pcm_index);

/*
 * Narrow one of SNDRV_PCM_HW_PARAM_RATE, _CHANNELS or _FORMAT (@var) in
 * @params to what the selected USB PCM of @udev can run at, considering the
 * other two parameters, as a hardware rule of the host's PCM would. The
 * selection is the same as for usb_offload_acquire_stream().
 */
int usb_offload_refine_hw_params(struct usb_offload_ctx *ctx,
				 struct usb_device *udev, int direction,
				 int pcm_index, int var,
				 struct snd_pcm_hw_params *params);

#endif /* __SOUND_USB_OFFLOAD_H */
