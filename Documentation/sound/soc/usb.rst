================
ASoC USB support
================

Overview
========
In order to leverage the existing USB sound device support in ALSA, the
ASoC USB APIs are introduced to allow the subsystems to exchange
configuration information.

One potential use case would be to support USB audio offloading, which is
an implementation that allows for an alternate power-optimized path in the audio
subsystem to handle the transfer of audio data over the USB bus.  This would
let the main processor to stay in lower power modes for longer duration.  The
following is an example design of how the ASoC and ALSA pieces can be connected
together to achieve this:

::

               USB                   |            ASoC
                                     |  _________________________
                                     | |   ASoC Platform card    |
                                     | |_________________________|
                                     |         |           |
                                     |      ___V____   ____V____
                                     |     |ASoC BE | |ASoC FE  |
                                     |     |DAI LNK | |DAI LNK  |
                                     |     |________| |_________|
                                     |         ^  ^        ^
                                     |         |  |________|
                                     |      ___V____    |
                                     |     |SoC-USB |   |
     ________       ________               |        |   |
    |USB SND |<--->|USBSND  |<------------>|________|   |
    |(card.c)|     |offld   |<----------                |
    |________|     |________|___     | |                |
        ^               ^       |    | |    ____________V_________
        |               |       |    | |   |IPC                   |
     __ V_______________V_____  |    | |   |______________________|
    |USB SND (endpoint.c)     | |    | |              ^
    |_________________________| |    | |              |
                ^               |    | |   ___________V___________
                |               |    | |->|audio DSP              |
     ___________V_____________  |    |    |_______________________|
    |XHCI HCD                 |<-    |
    |_________________________|      |


SoC USB driver
==============
Structures
----------
``struct snd_soc_usb``

  - ``list``: list head for SND SoC struct list
  - ``component``: reference to ASoC component
  - ``connection_status_cb``: callback to notify connection events
  - ``update_offload_route_info``: callback to fetch selected USB sound card/PCM
    device
  - ``is_route_busy``: callback reporting whether the backend has a stream open
    on a route, consulted before a route selection is changed
  - ``available_devices``: the connected ``snd_soc_usb_device`` entries
  - ``routes``, ``num_routes``: the offload routes of each direction, see
    `Platform Card Route Kcontrols`_
  - ``priv_data``: driver data

The snd_soc_usb structure can be referenced using the ASoC platform card
device, or a USB device (udev->dev).  This is created by the ASoC BE DAI
link, and the USB sound entity will be able to pass information to the
ASoC BE DAI link using this structure.  On device tree platforms the USB
device is tied to the port through the ``usb-soc-be`` phandle; without a
device tree node the port is found by the device its component was
registered with, so the caller passes that device.

``struct snd_soc_usb_device``

  - ``card_idx``: sound card index associated with USB sound device
  - ``chip_idx``: USB sound chip array index
  - ``cpcm_idx``: capture pcm device indexes associated with the USB sound device
  - ``ppcm_idx``: playback pcm device indexes associated with the USB sound device
  - ``cpcm_names``, ``ppcm_names``: optional human readable names of the PCMs,
    parallel to the index arrays, shown as the items of the route kcontrols
  - ``num_playback``: number of playback streams
  - ``num_capture``: number of capture streams
  - ``list``: list head for the backend's own use
  - ``available_node``: list head on ``available_devices``, owned by SoC USB

The struct snd_soc_usb_device is created by the USB sound offload driver.
This will carry basic parameters/limitations that will be used to
determine the possible offloading paths for this USB audio device.

Functions
---------
.. code-block:: rst

	int snd_soc_usb_find_supported_format(int card_idx,
			struct snd_pcm_hw_params *params, int direction)
..

  - ``card_idx``: the index into the USB sound chip array.
  - ``params``: Requested PCM parameters from the USB DPCM BE DAI link
  - ``direction``: capture or playback

**snd_soc_usb_find_supported_format()** ensures that the requested audio profile
being requested by the external DSP is supported by the USB device.

Returns 0 on success, and -EOPNOTSUPP on failure.

.. code-block:: rst

	int snd_soc_usb_connect(struct device *usbdev, struct snd_soc_usb_device *sdev)
..

  - ``usbdev``: the usb device that was discovered
  - ``sdev``: capabilities of the device

**snd_soc_usb_connect()** notifies the ASoC USB DPCM BE DAI link of a USB
audio device detection.  This can be utilized in the BE DAI
driver to keep track of available USB audio devices.  This is intended
to be called by the USB offload driver residing in USB SND.  The device is
added to ``available_devices`` and the route controls are notified. A route
nothing was picked for follows its default pair, see the selection rules
below.

Returns 0 on success, -ENODEV when no port is registered for ``usbdev`` (the
device is then not listed and must not be reported as disconnected later),
-EEXIST when ``sdev`` is connected already, negative error code on other
failures.

.. code-block:: rst

	int snd_soc_usb_disconnect(struct device *usbdev, struct snd_soc_usb_device *sdev)
..

  - ``usbdev``: the usb device that was removed
  - ``sdev``: capabilities to free

**snd_soc_usb_disconnect()** notifies the ASoC USB DPCM BE DAI link of a USB
audio device removal.  This is intended to be called by the USB offload
driver that resides in USB SND.  Every route that was picked for the device
goes back to its default pair.

.. code-block:: rst

	void *snd_soc_usb_find_priv_data(struct device *usbdev)
..

  - ``usbdev``: the usb device to reference to find private data

**snd_soc_usb_find_priv_data()** fetches the private data saved to the SoC USB
device.

Returns pointer to priv_data on success, NULL on failure.

.. code-block:: rst

	int snd_soc_usb_setup_offload_jack(struct snd_soc_component *component,
					struct snd_soc_jack *jack)
..

  - ``component``: ASoC component to add the jack
  - ``jack``: jack component to populate

**snd_soc_usb_setup_offload_jack()** is a helper to add a sound jack control to
the platform sound card.  This will allow for consistent naming to be used on
designs that support USB audio offloading.  Additionally, this will enable the
jack to notify of changes.

Returns 0 on success, negative otherwise.

.. code-block:: rst

	int snd_soc_usb_update_offload_route(struct device *dev, int card, int pcm,
					     int direction, enum snd_soc_usb_kctl path,
					     long *route)
..

  - ``dev``: USB device to look up offload path mapping
  - ``card``: USB sound card index
  - ``pcm``: USB sound PCM device index
  - ``direction``: direction to fetch offload routing information
  - ``path``: kcontrol selector - pcm device or card index
  - ``route``: mapping of sound card and pcm indexes for the offload path.  This is
	       an array of two integers that will carry the card and pcm device indexes
	       in that specific order.  This can be used as the array for the kcontrol
	       output.

**snd_soc_usb_update_offload_route()** calls a registered callback to the USB BE DAI
link to fetch the information about the mapped ASoC devices for executing USB audio
offload for the device. ``route`` may be a pointer to a kcontrol value output array,
which carries values when the kcontrol is read.

Returns 0 on success, negative otherwise.

.. code-block:: rst

	struct snd_soc_usb *snd_soc_usb_allocate_port(struct snd_soc_component *component,
			void *data);
..

  - ``component``: DPCM BE DAI link component
  - ``data``: private data

**snd_soc_usb_allocate_port()** allocates a SoC USB device and populates standard
parameters that is used for further operations.  The port starts out with one
offload route per direction.

Returns a pointer to struct soc_usb on success, negative on error.

.. code-block:: rst

	int snd_soc_usb_set_num_routes(struct snd_soc_usb *usb, int direction,
				       unsigned int num_routes);
..

  - ``usb``: SoC USB device, not yet added with snd_soc_usb_add_port()
  - ``direction``: SNDRV_PCM_STREAM_PLAYBACK or SNDRV_PCM_STREAM_CAPTURE
  - ``num_routes``: streams the backend can offload at a time in ``direction``

**snd_soc_usb_set_num_routes()** gives a backend which runs several offloaded
streams of a direction at a time a route, and with
snd_soc_usb_setup_offload_route_ctl() a kcontrol, per stream, so that each can
be pointed at a (device, PCM) pair of its own.  To be called before the route
kcontrols are created and the port is added.

Returns 0 on success, negative on error.

.. code-block:: rst

	int snd_soc_usb_setup_offload_route_ctl(struct snd_soc_component *component,
						struct snd_soc_usb *usb);
..

  - ``component``: DPCM BE DAI link component
  - ``usb``: SoC USB device to attach the controls to

**snd_soc_usb_setup_offload_route_ctl()** creates one writable enumerated
kcontrol per route of each direction on the platform sound card, see
`Platform Card Route Kcontrols`_.

Returns 0 on success, negative on error.

.. code-block:: rst

	int snd_soc_usb_get_selected_device(struct snd_soc_usb *usb, int direction,
					    unsigned int route,
					    struct snd_soc_usb_device **sdev,
					    int *pcm_index);
..

  - ``usb``: SoC USB device
  - ``direction``: SNDRV_PCM_STREAM_PLAYBACK or SNDRV_PCM_STREAM_CAPTURE
  - ``route``: the route of ``direction``
  - ``sdev``: set to the routed device, NULL if nothing is selected
  - ``pcm_index``: set to the routed USB SND PCM device index

**snd_soc_usb_get_selected_device()** returns the (device, PCM) pair a route
currently points at, for the backend to consult when it starts a stream
instead of picking a device itself.

Returns 0 on success (also when nothing is selected), negative on error.

.. code-block:: rst

	void snd_soc_usb_free_port(struct snd_soc_usb *usb);
..

  - ``usb``: SoC USB device to free

**snd_soc_usb_free_port()** frees a SoC USB device.

.. code-block:: rst

	void snd_soc_usb_add_port(struct snd_soc_usb *usb);
..

  - ``usb``: SoC USB device to add

**snd_soc_usb_add_port()** add an allocated SoC USB device to the SOC USB framework.
Once added, this device can be referenced by further operations.

.. code-block:: rst

	void snd_soc_usb_remove_port(struct snd_soc_usb *usb);
..

  - ``usb``: SoC USB device to remove

**snd_soc_usb_remove_port()** removes a SoC USB device from the SoC USB framework.
After removing a device, any SOC USB operations would not be able to reference the
device removed.

How to Register to SoC USB
--------------------------
The ASoC DPCM USB BE DAI link is the entity responsible for allocating and
registering the SoC USB device on the component bind.  Likewise, it will
also be responsible for freeing the allocated resources.  An example can
be shown below:

.. code-block:: rst

	static int q6usb_component_probe(struct snd_soc_component *component)
	{
		...
		data->usb = snd_soc_usb_allocate_port(component, 1, &data->priv);
		if (!data->usb)
			return -ENOMEM;

		usb->connection_status_cb = q6usb_alsa_connection_cb;

		ret = snd_soc_usb_add_port(usb);
		if (ret < 0) {
			dev_err(component->dev, "failed to add usb port\n");
			goto free_usb;
		}
		...
	}

	static void q6usb_component_remove(struct snd_soc_component *component)
	{
		...
		snd_soc_usb_remove_port(data->usb);
		snd_soc_usb_free_port(data->usb);
	}

	static const struct snd_soc_component_driver q6usb_dai_component = {
		.probe = q6usb_component_probe,
		.remove = q6usb_component_remove,
		.name = "q6usb-dai-component",
		...
	};
..

BE DAI links can pass along vendor specific information as part of the
call to allocate the SoC USB device.  This will allow any BE DAI link
parameters or settings to be accessed by the USB offload driver that
resides in USB SND.

USB Audio Device Connection Flow
--------------------------------
USB devices can be hotplugged into the USB ports at any point in time.
The BE DAI link should be aware of the current state of the physical USB
port, i.e. if there are any USB devices with audio interface(s) connected.
connection_status_cb() can be used to notify the BE DAI link of any change.

This is called whenever there is a USB SND interface bind or remove event,
using snd_soc_usb_connect() or snd_soc_usb_disconnect():

.. code-block:: rst

	static void qc_usb_audio_offload_probe(struct snd_usb_audio *chip)
	{
		...
		snd_soc_usb_connect(usb_get_usb_backend(udev), sdev);
		...
	}

	static void qc_usb_audio_offload_disconnect(struct snd_usb_audio *chip)
	{
		...
		snd_soc_usb_disconnect(usb_get_usb_backend(chip->dev), dev->sdev);
		...
	}
..

In order to account for conditions where driver or device existence is
not guaranteed, USB SND exposes snd_usb_rediscover_devices() to resend the
connect events for any identified USB audio interfaces.  Consider the
the following situation:

	**usb_audio_probe()**
	  | --> USB audio streams allocated and saved to usb_chip[]
	  | --> Propagate connect event to USB offload driver in USB SND
	  | --> **snd_soc_usb_connect()** exits as USB BE DAI link is not ready

	BE DAI link component probe
	  | --> DAI link is probed and SoC USB port is allocated
	  | --> The USB audio device connect event is missed

To ensure connection events are not missed, **snd_usb_rediscover_devices()**
is executed when the SoC USB device is registered.  Now, when the BE DAI
link component probe occurs, the following highlights the sequence:

	BE DAI link component probe
	  | --> DAI link is probed and SoC USB port is allocated
	  | --> SoC USB device added, and **snd_usb_rediscover_devices()** runs

	**snd_usb_rediscover_devices()**
	  | --> Traverses through usb_chip[] and for non-NULL entries issue
	  |     **connection_status_cb()**

In the case where the USB offload driver is unbound, while USB SND is ready,
the **snd_usb_rediscover_devices()** is called during module init.  This allows
for the offloading path to also be enabled with the following flow:

	**usb_audio_probe()**
	  | --> USB audio streams allocated and saved to usb_chip[]
	  | --> Propagate connect event to USB offload driver in USB SND
	  | --> USB offload driver **NOT** ready!

	BE DAI link component probe
	  | --> DAI link is probed and SoC USB port is allocated
	  | --> No USB connect event due to missing USB offload driver

	USB offload driver probe
	  | --> **qc_usb_audio_offload_init()**
	  | --> Calls **snd_usb_rediscover_devices()** to notify of devices

USB Offload Related Kcontrols
=============================
Details
-------
A set of kcontrols can be utilized by applications to help select the proper sound
devices to enable USB audio offloading.  Two models are available: read-only
controls on the USB sound card reporting where a USB PCM is offloaded to
(`USB Sound Card Route Kcontrols`_), and writable controls on the platform
sound card selecting which USB device an offloaded stream goes to
(`Platform Card Route Kcontrols`_).  A backend uses the one that fits how
its DSP is driven; the two can coexist.

USB Sound Card Route Kcontrols
------------------------------
SoC USB exposes the get_offload_dev() callback that designs can use to ensure
that the proper indices are returned to the application.

Implementation
--------------

**Example:**

  **Sound Cards**:

	::

	  0 [SM8250MTPWCD938]: sm8250 - SM8250-MTP-WCD9380-WSA8810-VA-D
						SM8250-MTP-WCD9380-WSA8810-VA-DMIC
	  1 [Seri           ]: USB-Audio - Plantronics Blackwire 3225 Seri
						Plantronics Plantronics Blackwire
						3225 Seri at usb-xhci-hcd.1.auto-1.1,
						full sp
	  2 [C320M          ]: USB-Audio - Plantronics C320-M
                      Plantronics Plantronics C320-M at usb-xhci-hcd.1.auto-1.2, full speed

  **PCM Devices**:

	::

	  card 0: SM8250MTPWCD938 [SM8250-MTP-WCD9380-WSA8810-VA-D], device 0: MultiMedia1 (*) []
	  Subdevices: 1/1
	  Subdevice #0: subdevice #0
	  card 0: SM8250MTPWCD938 [SM8250-MTP-WCD9380-WSA8810-VA-D], device 1: MultiMedia2 (*) []
	  Subdevices: 1/1
	  Subdevice #0: subdevice #0
	  card 1: Seri [Plantronics Blackwire 3225 Seri], device 0: USB Audio [USB Audio]
	  Subdevices: 1/1
	  Subdevice #0: subdevice #0
	  card 2: C320M [Plantronics C320-M], device 0: USB Audio [USB Audio]
	  Subdevices: 1/1
	  Subdevice #0: subdevice #0

  **USB Sound Card** - card#1:

	::

	  USB Offload Playback Card Route PCM#0   -1 (range -1->32)
	  USB Offload Playback PCM Route PCM#0    -1 (range -1->255)

  **USB Sound Card** - card#2:

	::

	  USB Offload Playback Card Route PCM#0   0 (range -1->32)
	  USB Offload Playback PCM Route PCM#0    1 (range -1->255)

The above example shows a scenario where the system has one ASoC platform card
(card#0) and two USB sound devices connected (card#1 and card#2).  When reading
the available kcontrols for each USB audio device, the following kcontrols lists
the mapped offload card and pcm device indexes for the specific USB device:

	``USB Offload Playback Card Route PCM#*``

	``USB Offload Playback PCM Route PCM#*``

The kcontrol is indexed, because a USB audio device could potentially have
several PCM devices.  The above kcontrols are defined as:

  - ``USB Offload Playback Card Route PCM#`` **(R)**: Returns the ASoC platform sound
    card index for a mapped offload path.  The output **"0"** (card index) signifies
    that there is an available offload path for the USB SND device through card#0.
    If **"-1"** is seen, then no offload path is available for the USB SND device.
    This kcontrol exists for each USB audio device that exists in the system, and
    its expected to derive the current status of offload based on the output value
    for the kcontrol along with the PCM route kcontrol.

  - ``USB Offload Playback PCM Route PCM#`` **(R)**: Returns the ASoC platform sound
    PCM device index for a mapped offload path.  The output **"1"** (PCM device index)
    signifies that there is an available offload path for the USB SND device through
    PCM device#0. If **"-1"** is seen, then no offload path is available for the USB\
    SND device.  This kcontrol exists for each USB audio device that exists in the
    system, and its expected to derive the current status of offload based on the
    output value for this kcontrol, in addition to the card route kcontrol.

USB Offload Playback Route Kcontrol
-----------------------------------
In order to allow for vendor specific implementations on audio offloading device
selection, the SoC USB layer exposes the following:

.. code-block:: rst

	int (*update_offload_route_info)(struct snd_soc_component *component,
					 int card, int pcm, int direction,
					 enum snd_soc_usb_kctl path,
					 long *route)
..

These are specific for the **USB Offload Playback Card Route PCM#** and **USB
Offload PCM Route PCM#** kcontrols.

When users issue get calls to the kcontrol, the registered SoC USB callbacks will
execute the registered function calls to the DPCM BE DAI link.

**Callback Registration:**

.. code-block:: rst

	static int q6usb_component_probe(struct snd_soc_component *component)
	{
	...
	usb = snd_soc_usb_allocate_port(component, 1, &data->priv);
	if (IS_ERR(usb))
		return -ENOMEM;

	usb->connection_status_cb = q6usb_alsa_connection_cb;
	usb->update_offload_route_info = q6usb_get_offload_dev;

	ret = snd_soc_usb_add_port(usb);
..

Platform Card Route Kcontrols
-----------------------------
A backend whose offloaded PCMs exist on the platform card whether or not a
USB device is connected, and which can offload any connected device, lets
the user choose the device instead: it has a *route* per stream it can
offload at a time in a direction (see snd_soc_usb_set_num_routes()), and
snd_soc_usb_setup_offload_route_ctl() creates a writable enumerated kcontrol
per route on the platform card:

	``USB Offload <n> Playback Route``

	``USB Offload <n> Capture Route``

``<n>`` is the route and is always present, also with a single route per
direction, so the names do not depend on how many streams a backend runs.
A backend pairs its PCMs with the controls by naming them the same way,
``USB Offload <n>``.

The items of a control are every (device, PCM) pair of the direction over
all connected devices, in connection order and then PCM order, named by the
backend through ``ppcm_names``/``cpcm_names``; a name should start with the
USB card and PCM numbers, ``<card>,<pcm>``, so that a sound server can match
the device it targets.  The item list is rebuilt on every query, so it
always reflects what is connected; with nothing connected the single item is
``None``.

Selection rules:

  - A route nothing was picked for follows its default: route N gets the Nth
    available pair of its direction, wrapping around when there are fewer
    pairs than routes, so that the routes spread over what is connected.
    The default is worked out on every query, it is not stored, so it moves
    as devices connect and disconnect.
  - A write picks a pair for the route. Picking the pair the route already
    points at pins it there.
  - On disconnect, routes picked for the device go back to their default.
  - A write is refused with ``-EBUSY`` while the backend reports the route
    busy through ``is_route_busy()``, i.e. while a stream is open on it.
    Writing the current selection is a no-op.  A selection by itself claims
    no USB or DSP resource; the backend reads it with
    snd_soc_usb_get_selected_device() when a stream starts.

Control ``INFO`` and ``VALUE`` change notifications are sent whenever a
connect, a disconnect or a write changes the item list or the selection.
For a backend using this model see :doc:`sof-uaol`.

Existing USB Sound Kcontrol
---------------------------
With the introduction of USB offload support, the above USB offload kcontrol
will be added to the pre existing list of kcontrols identified by the USB sound
framework.  These kcontrols are still the main controls that are used to
modify characteristics pertaining to the USB audio device.

	::

	  Number of controls: 9
	  ctl     type    num     name                                    value
	  0       INT     2       Capture Channel Map                     0, 0 (range 0->36)
	  1       INT     2       Playback Channel Map                    0, 0 (range 0->36)
	  2       BOOL    1       Headset Capture Switch                  On
	  3       INT     1       Headset Capture Volume                  10 (range 0->13)
	  4       BOOL    1       Sidetone Playback Switch                On
	  5       INT     1       Sidetone Playback Volume                4096 (range 0->8192)
	  6       BOOL    1       Headset Playback Switch                 On
	  7       INT     2       Headset Playback Volume                 20, 20 (range 0->24)
	  8       INT     1       USB Offload Playback Card Route PCM#0   0 (range -1->32)
	  9       INT     1       USB Offload Playback PCM Route PCM#0    1 (range -1->255)

Since USB audio device controls are handled over the USB control endpoint, use the
existing mechanisms present in the USB mixer to set parameters, such as volume.
