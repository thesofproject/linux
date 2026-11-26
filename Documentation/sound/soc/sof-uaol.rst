.. SPDX-License-Identifier: GPL-2.0

===================================
SOF USB Audio Offload Link (UAOL)
===================================

Scope
=====

This document describes the Linux implementation of USB audio offload on
Intel platforms with SOF. It covers the Linux host side, including the
configuration the host hands to the DSP firmware. It does not describe
firmware internals and is not a hardware specification.

For the generic ASoC USB framework, see :doc:`usb`. Of the two control
models it describes, SOF uses the platform card route controls: writable
``USB Offload <n> Playback Route`` and ``USB Offload <n> Capture Route``
controls, one per backend, on the *SOF platform* card, which select the USB
device the offloaded stream of that backend goes to. The read-only
``USB Offload ... Card/PCM Route`` controls on the *USB* sound card are not
created.

.. contents:: In this document
   :local:
   :depth: 2

Architecture and design principles
==================================

The core idea: a second path into the same USB PCM
--------------------------------------------------

Offload does not replace the normal USB audio path, and does not change the
USB sound card. snd-usb-audio still creates the USB card and its PCMs, and
those PCMs still move audio the usual way: the CPU submits URBs to the xHCI
controller. This document calls that the *URB path*.

The SOF card adds a second way into the same USB PCM, the *offload path*.
A SOF frontend PCM carries the audio through the DSP and the UAOL link
directly into the USB PCM that a route control has selected.

The diagram below shows, for playback, how a sound server such as PipeWire
or PulseAudio could choose between the two paths for a USB headset, and the
route the audio then takes. Capture uses the same paths in the opposite
direction::

                    +--------------------------------------------+
                    |        Desktop / Sound Server (PW / PA)    |
                    |   Target: USB Headset (Card #1, PCM #0)    |
                    +---------------------+----------------------+
                                          |
                        +-----------------+-----------------+
                        | 'Use USB Offload' Checkbox?       |
                   [ UNCHECKED ]                      [ CHECKED ]
                        |                                   |
                        |                        +----------v----------+
                        |                        | 1. Has 'USB Offload |
                        |                        |    Jack' (Plugged)? |
                        |                        +----+-----------+----+
                        |                          No |       Yes |
                        |                             |           v
                        |                             |  +-----------------+
                        |                             |  | 2. Is Headset   |
                        |                             |  |    in Route?    |
                        |                             |  +----+-------+----+
                        |                             |    No |   Yes |
                        |                             |<------+       |
                        |                             |               v
                        v                             v         [ UAOL Path ]
             [ Standard URB Path ] <------------------+         1. Set Route
          Open: hw:<USB Card>,<PCM>                             2. Open: hw:<SOF Card>,<FE>
                        |                                             |
                        v                                             v
             +--------------------+                         +--------------------+
             |   snd-usb-audio    |                         |    SOF Frontend    |
             |    Host Driver     |                         +---------+----------+
             +---------+----------+                                   | Host DMA
                       |                                              v
               Submits | URBs                               +--------------------+
                       v                                    |  Intel Audio DSP   |
             +--------------------+                         |  pipeline -> UAOL  |
             |  System Memory     |                         |  DAI copier (IPC4  |
             |  Transfer Rings    |                         |  gateway, set up   |
             +---------+----------+                         |  by the UAOL BE)   |
                       |                                    +---------+----------+
                  Periodic DMA                                        | Link DMA over
                       |                                              | the dedicated
                       |                                              | UAOL link
                       |                                              |
                       v                                              v
             +---------------------------------------------------------------------+
             |                       xHCI Controller Engine                        |
             |    [ System Memory Rings (URB) ]   vs.   [ Audio Sideband (UAOL) ]  |
             +----------------------------------+----------------------------------+
                                                |
                                       Isochronous Packets
                                                v
                                +-------------------------------+
                                | Physical USB Headset Endpoint |
                                +-------------------------------+

The decision part of the diagram is userspace policy, shown as an example.
The "Use USB Offload" switch belongs to the sound server; the kernel provides
no such setting. What the kernel provides are the inputs to the decision and
the means to act on it:

1. The ``USB Offload Jack`` tells whether any USB device can be offloaded at
   all.
2. The items of the ``USB Offload <n> Playback Route`` and ``USB Offload <n>
   Capture Route`` controls tell which ones. Each item name starts with
   ``<card>,<pcm>``, the USB card and PCM numbers, so the sound server can
   match the device it targets.
3. Selecting the item in the route control of a backend and opening the SOF
   frontend PCM routed to that backend starts offload. The selection fails
   with ``-EBUSY`` while an offloaded stream is set up on that backend.

Opening the SOF frontend can still fail later, in ``hw_params``. The usual
reasons are that the USB card's PCM is already open, or that the device
cannot run the requested format over UAOL. The sound server should then fall
back to the URB path, just as it does when the jack or the route item is
missing.

For userspace the difference between the two paths is which PCM device it
opens:

* **Same device, same card, same controls.** The USB device, its USB card,
  and the card's mixer controls stay as they are. Device controls such as the
  USB device's own volume and mute remain on the USB card and apply to
  whichever path is in use, because snd-usb-audio still owns the device.
* **Same userspace rules.** Whatever userspace applies to the USB card, for
  example its UCM configuration, applies to the offload path as well. Only
  the PCM device differs: the SOF card's frontend instead of the USB card's
  PCM.
* **One path at a time per USB PCM and direction.** Both paths share the USB
  substream's ownership flag. While the offload path holds it, opening the
  USB card's PCM fails with ``-EBUSY``. While the USB card's PCM is open, the
  offload stream's ``hw_params`` fails with ``-EBUSY``. A running stream is
  never moved from one path to the other.
* **Offload is optional.** The URB path is always available, and is the
  fallback whenever the offload path is not.

What is offloaded?
------------------

UAOL (USB Audio Offload Link) is a dedicated hardware link between the Intel
audio controller and the xHCI controller's Audio Sideband interface. Once an
isochronous endpoint is assigned an Audio Sideband resource, the xHCI
controller exchanges that endpoint's audio data with the DSP over UAOL instead
of with transfer rings in system memory. The xHCI controller still schedules
the USB transactions; the DSP produces or consumes the audio.

The CPU remains in charge of everything else: USB enumeration, control
requests and interface configuration, PCM negotiation, resource allocation,
and the stream lifecycle. The goal is to take the CPU off the steady-state
audio data path so that the host can stay idle for longer.

This is not compressed-audio offload: the DSP carries PCM audio. Audio is
offloaded only when an application opens a SOF PCM that the topology routes
to a UAOL backend; opening the USB card's own PCM always uses the URB path.

Two views of the same playback stream are useful. Capture is the mirror image,
with audio flowing upwards::

    ASoC / ALSA view                   Audio data path
    ----------------                   ---------------
    Application
        |
        v
    FE: SOF platform-card PCM  ......  Host DMA (system memory -> DSP)
        |                                  |
        | DPCM, connected by               v
        | topology routes              DSP pipeline (host copier,
        |                              processing modules)
        v                                  |
    BE: UAOL0-Play0 (no PCM) ........  UAOL DAI copier (IPC4 gateway)
        CPU DAI "UAOL0 Playback Pin0"      |
                                           v
                                       Audio link DMA
                                           |
                                           v
                                       UAOL link
                                           |
                                           v
                                       xHCI Audio Sideband
                                           |
                                           v
                                       USB isochronous OUT endpoint

    Explicit playback feedback (separate return path):
    USB feedback IN endpoint -> xHCI Audio Sideband -> UAOL link
        -> feedback link DMA (capture direction) -> UAOL DAI copier

The frontend (FE) is the PCM that the application opens. The backend (BE) is
the ASoC DAI link on the UAOL side. Topology routes connect the FE to the BE
through DPCM. The dotted lines link each ASoC object to the part of the data
path it controls. The FE controls the host DMA. The BE's CPU DAI callbacks
acquire the USB endpoints, set up the link DMAs, and configure the UAOL DAI
copier, which is the DSP-side gateway to the link. The BE is not a separate
processing stage.

Control and setup run on the CPU, apart from the audio path::

    Hotplug (device discovery):
      snd-usb-audio enumeration / platform callbacks
        -> USB offload library (candidate filter, sideband registration)
        -> SOF hotplug callbacks
        -> ASoC USB device list, route controls, and SOF jack

    Stream setup (BE hw_params):
      SOF UAOL BE DAI
        -> USB offload library
        -> snd-usb-audio endpoint preparation + xHCI sideband resource binding
        -> endpoint/resource identity returned to SOF
        -> IPC4 gateway configuration and link DMA setup

Unlike the Qualcomm USB offload design, the DSP does not get the xHCI transfer
and event rings and does not queue TRBs itself. The SOF path calls none of the
sideband ring-export or secondary-interrupter APIs; the endpoint-to-resource
assignment alone routes the data.

Ownership boundaries
--------------------

The central rule is to retain each subsystem's existing responsibility:

* **snd-usb-audio owns the USB device.** No competing USB class driver claims
  it. Its descriptor parsing, format lists, interface configuration, endpoint
  setup, and runtime-PM helpers are reused.
* **The USB offload library owns the USB-side offload records.** It filters
  candidates, registers Audio Sideband access, claims USB substreams, and
  acquires/releases endpoint resources. It does not build SOF IPC messages,
  program audio registers, or own the ASoC USB port.
* **SOF owns the DSP and audio-controller side.** It publishes the ASoC USB
  port, translates hotplug identities into ASoC USB devices, manages the jack,
  allocates link DMAs, and completes the firmware gateway configuration.
* **The ASoC USB framework owns route policy/state.** It keeps the available
  device list and the selected USB device/PCM pair for each direction.
* **The xHCI driver owns controller-side endpoint binding.** It reserves Audio
  Sideband resources and attaches them to endpoints with xHCI
  extended-property commands.
* **The topology and machine driver define the audio graph.** A route control
  only chooses which USB device a UAOL backend talks to. It does not create a
  frontend, load a topology, or change the DSP processing graph.

A USB device moves through three states, which are easily confused:

Available
  The device passed the candidate filter, registered for Audio Sideband use,
  and was published to the SOF port. It appears in the route controls and
  counts toward the jack state.

Selected
  A direction's route control points at one of the device's USB PCMs.
  Selecting a route does not claim the USB substream or allocate any endpoint
  resource.

Acquired
  The BE's ``hw_params`` has claimed the USB substream and bound its active
  endpoints to Audio Sideband resources. The actual endpoint and resource
  identities are only known from this point.

A device can be available and selected and still fail acquisition, for example
because its USB PCM is already open on the USB card. A stream that is already
running on the USB card is never moved to the DSP.

Platform discovery and card construction
========================================

Build and runtime gates
-----------------------

``CONFIG_SND_SOC_SOF_INTEL_USB_OFFLOAD`` enables SOF's integration. Its
dependencies include ``SND_SOC_SOF_HDA_GENERIC``, ``SND_SOC_SOF_HDA_LINK``,
``SND_USB_OFFLOAD``, and ``SND_SOC_USB``; Kconfig also prevents incompatible
built-in/module combinations. The USB library requires ``SND_USB_AUDIO``,
``USB_XHCI_HCD``, and ``USB_XHCI_SIDEBAND``.

Runtime discovery has two independent inputs:

1. HDA extended-multilink enumeration finds Intel UAOL alternate links.
2. The booted IPC4 firmware reports ``SOF_IPC4_FW_CFG_UAOL_SUPPORT`` and
   ``SOF_IPC4_HW_CFG_INTEL_UAOL_CAPS``. The latter supplies the link count,
   stream capabilities, and transmit/receive FIFO sizes.

The loader checks the size of the capability payload before keeping it.
``hda_uaol_link_caps()`` returns the cached capabilities of a link, and
``hda_uaol_available()`` tells whether the firmware supports UAOL at all.

``hda_machine_register()`` registers with the offload library
(``usb_offload_register()``) after the firmware configuration has been
parsed, then registers the machine driver. The registration passes a
``struct usb_offload_res``: the SOF device, the hotplug and resource-range
callbacks, and two policy flags, ``s24_3le_as_s24_le`` and
``async_playback_on_host`` (see `Negotiation rules`_ and `Asynchronous
playback stays on the host`_). The library knows nothing about UAOL links or
xHCI controllers: it has one context per registered host, and the host
answers for the resources.

A failure to set up offload does not stop the rest of the sound card from
registering; it is logged at debug level only. A working sound card
therefore does not prove that USB offload is working.

Backend links and topology
--------------------------

``mach_params.uaol_link_mask`` tells the machine driver which UAOL links
exist, and ``mach_params.uaol_playback_num`` and ``uaol_capture_num`` how
many streams per direction each link runs at a time. ``hda_finalize_mach_params()``
fills the stream counts in from the firmware's link capabilities once the
firmware has booted: a link with ``T`` output and ``I`` input streams carries
``T`` playback and ``I - T`` capture streams, the remaining ``T`` input
streams being the clock feedback of the playback streams (see `FIFO
partitioning and constraints`_). The static DAI count is the ceiling. It
clears the mask if UAOL support is not built in or the firmware does not
support it. The ``snd_sof_intel_hda_generic.uaol_link_mask`` module
parameter, set at load time, can override the discovered mask. Setting bits
for links that were not discovered produces a warning; it cannot add hardware
that is not there.

The generic HDA board helper and the SoundWire machine driver add one
unidirectional backend per stream of each enabled link. With two streams per
direction:

.. list-table::
   :header-rows: 1
   :widths: 20 30 50

   * - Direction
     - Backend link name
     - CPU DAI name
   * - Playback
     - ``UAOL0-Play0``, ``UAOL0-Play1``
     - ``UAOL0 Playback Pin0``, ``UAOL0 Playback Pin1``
   * - Capture
     - ``UAOL0-Cap0``, ``UAOL0-Cap1``
     - ``UAOL0 Capture Pin0``, ``UAOL0 Capture Pin1``

These are no-PCM backend links with a dummy codec. They are not user-visible
PCMs and not USB sound cards. In the names, the first ``0`` is the UAOL link
index and the trailing number the ordinal of the stream among those of its
direction. The ordinal only tells the backends apart: it is **not** the UAOL
stream the backend runs on, which is decided by the sideband resource its
endpoint gets at acquisition (see `Resource IDs, UAOL stream indices, and DMA
tags`_). Two backends of a direction are interchangeable.

The backends are created ordinal first, playback before capture within an
ordinal: ``Play0``, ``Cap0``, ``Play1``, ``Cap1``. The first pair therefore
keeps two consecutive backend IDs whatever the stream count, and a topology
describing one stream per direction stays valid; a backend the topology
leaves out is simply unused.

Each direction gets its own backend and DAI because the backend ``hw_params``
fixup is not told the stream direction. A duplex backend could not tell
whether to use the topology's playback or capture DAI copier.

The backend names are abbreviated on purpose. The topology names the DAI
copier widget ``dai-copier.UAOL.<backend>.<direction>``, and alsa-lib keeps
element ids in ``SNDRV_CTL_ELEM_ID_NAME_MAXLEN`` bytes: from 38 characters on,
the ids of the widget's ``dai.word`` and ``dai.string`` tuple sections collide
and one set is silently dropped from the topology.

A matching topology must provide the frontends, the routes, and the
``SOF_DAI_INTEL_UAOL`` DAI copiers. The topology decides the frontend PCM
numbers and the processing graph; the backend names above do not.

In this implementation the HDA code limits the usable UAOL link count to one.
The multilink code has no per-instance offset for UAOL, so further sublinks
would all alias to sublink zero. Within the link, every backend has an ASoC
USB route of its own, so with two devices connected, or one device with two
PCM pairs, both backends of a direction can run at the same time, each on its
own device or PCM.

Publishing the ASoC USB port
----------------------------

The ASoC USB port (``struct snd_soc_usb``) is where USB devices are published
and where the route state lives. There is one per audio controller. The first
UAOL CPU DAI to probe in the card creates it, and the other UAOL DAIs reuse
it. Every UAOL CPU DAI also claims a ``struct hda_uaol_stream`` of its own at
probe time; that is where the acquired device, the backend substream, the
feedback link DMA and the frontend narrowing rules of its backend live, and
which ASoC USB route of its direction the backend plays to. The link and the
route are parsed from the DAI name, ``UAOL<link> <Direction> Pin<route>``:
``UAOL0 Playback Pin1`` is playback route 1 of link 0.

The probe sets the ``is_route_busy`` callback, which reports whether the
backend of a route holds an acquired stream, gives the port as many routes
per direction as the link has streams (``snd_soc_usb_set_num_routes()``),
creates the ``USB Offload Jack``, creates the route controls, stores the
port in ``hdev->uaol_usb``, and finally calls ``snd_soc_usb_add_port()``. The
jack has to exist before the port is added, because adding the port
rediscovers devices that were connected earlier, and those report the jack.

ASoC USB normally finds the port through a device-tree ``usb-soc-be``
phandle. On these ACPI/PCI platforms there is none, so the port is found by
its component's device: SOF passes ``sdev->dev`` to
``snd_soc_usb_connect()`` and ``snd_soc_usb_disconnect()``.

If the offload library is not running, the probe still creates the jack and
leaves it unplugged, but adds no port and no route controls.

USB discovery and route population
==================================

Candidate detection
-------------------

The library registers a ``snd_usb_platform_ops`` table with connect and
disconnect callbacks. snd-usb-audio keeps driving the device and calls these
callbacks as its interfaces are probed and removed.

The bridge accepts a device only if it is an audio-class function usb-audio
has set up PCMs for *and* registers for xHCI Audio Sideband use:

* The device must be full-speed or high-speed. SuperSpeed devices are rejected.
* The descriptors are not parsed again: the streams, formats and endpoints are
  the ones snd-usb-audio found, with whatever it rejected as malformed already
  left out. A device without any PCM that can be offloaded is rejected.
* ``snd_usb_offload.offload_disable=1`` disables candidate selection.

Sideband registration needs a valid xHCI slot and a controller with Audio
Sideband capability: xHCI 1.2 or later, extended-property support, and at
least one audio resource.

Registration uses the ``XHCI_SIDEBAND_AUDIO`` type, not the vendor sideband.
It does not allocate any endpoint resources; that happens only at
acquisition.

Asynchronous playback stays on the host
---------------------------------------

An asynchronous OUT endpoint runs off the device's own clock, and the sender
has to follow that clock through the feedback the device provides. Whether
the DSP applies that feedback well enough is the host's call: the SOF
firmware spreads samples linearly over the service interval, which is
audible on the kind of equipment that uses asynchronous playback in the
first place: audio interfaces and mixers. Implicit feedback is no better: its
sync source is a capture data endpoint, which the DSP cannot use, so such a
stream would run with no rate tracking at all.

SOF therefore registers with ``async_playback_on_host`` set, and the library
keeps asynchronous playback on the URB path. The decision is made per
``audioformat``, on the sync type of the data endpoint and its implicit
feedback flag, so it covers explicit and implicit feedback alike and does not
depend on the snd-usb-audio quirks that choose between the two:

* A playback PCM whose formats all need feedback is left out of the device
  identity, so it never appears in the playback route controls. The
  library logs ``PCM <n> playback needs clock feedback, not offloaded`` once
  per connect.
* Formats that need feedback are skipped during negotiation, so a PCM that
  mixes adaptive and asynchronous alternate settings only negotiates the
  adaptive ones.
* ``usb_offload_acquire_stream()`` refuses a format that still needs
  feedback with ``-EOPNOTSUPP``.

Capture is not affected by its own sync type: an asynchronous IN endpoint
needs no feedback. It is affected when its data endpoint is the implicit
feedback source of a playback format the host keeps, as with the Behringer
UMC204HD's 24-bit playback: that endpoint is the playback's clock, and once
the DSP owned it the host could not run the playback at all. Such a capture
format stays on the host as well, and the library logs ``PCM <n> capture is
tied to the playback, not offloaded``. A device left with no PCM
to offload in either direction is not reported to SOF at all, the library
logs ``no PCM can be offloaded``.

``snd_usb_offload.allow_feedback=1`` overrides the host's flag for testing
the feedback path. Explicit feedback is then claimed as described under
`Acquiring the USB endpoints`_; implicit feedback is still left unused.

From a USB chip to enumerated control items
-------------------------------------------

The connection sequence is::

    snd-usb-audio connect_cb(chip)
      usb_offload_chip_is_candidate()      speed/class check
      xhci_sideband_register()             controller eligibility
      usb_offload_ua_build_id()            card/chip/PCM identity
      usb_offload_connect()
        hda_uaol_notify_connect()
          allocate hda_uaol_dev
          copy identity and name storage
          snd_soc_usb_connect(SOF device, &record->sdev)
            append to available_devices
            notify control INFO / VALUE changes
          report SND_JACK_USB

``usb_offload_ua_build_id()`` walks ``chip->pcm_list``. A USB PCM adds a
playback or capture entry if it has formats for that direction that can be
offloaded (see `Asynchronous playback stays on the host`_). The identity
passed to SOF holds the ALSA card number, the USB chip index, and the PCM
device numbers and names. At most eight PCMs per direction are exported.

Each entry's display name has this form, with the product name cut to 24
characters and the PCM name to 16::

    <card>,<pcm> | <product> - <PCM name>

so that several PCMs on one device, and identical devices on different cards,
can be told apart. The library passes the identity by value, and only for the
duration of the callback. ASoC USB, however, keeps pointers to it and links it
into its device list, so SOF copies it, names included, into its own
``hda_uaol_dev`` record.

There is no control per device. A published port has one *route* per
backend of each direction, and a control per route:

* ``USB Offload 0 Playback Route``, ``USB Offload 1 Playback Route``, ...
* ``USB Offload 0 Capture Route``, ``USB Offload 1 Capture Route``, ...

The number in the name is the backend ordinal, the same as in
``UAOL0-Play<n>``: route ``n`` of a direction is what backend ``n`` of that
direction plays to. The number is always present, also for a direction with
a single backend, so the names do not depend on the stream count. The
topologies name the frontend PCMs with the same ordinal, ``USB Offload 0``
and ``USB Offload 1`` (``Deepbuffer USB Offload <n>`` for the deep buffers),
so that a PCM, its route controls and the ``USB Offload Jack`` share a name
and pair up by number.

Each is a writable enumerated control on the SOF platform card. Its item list
is rebuilt on every query. ASoC USB lists every available ``(device, USB
PCM)`` pair for that direction, in device-connection order and then PCM
order. A playback-only device adds no capture items, and the reverse. All
routes of a direction list the same items.

An item number is a position in this live list. It is **not** a card number,
USB PCM number, endpoint number, or sideband resource ID. Hotplug can shift
positions even while the selected device stays the same, so userspace should
re-read the item list when it gets a control notification.

Selection policy and changes
----------------------------

Any route can be pointed at any available pair, including a pair another
route of the direction already points at. A ``(device, USB PCM)`` pair can
only be offloaded once, though: of two routes pointing at the same pair, the
first backend to acquire it gets it and the second fails ``hw_params`` with
``-EBUSY`` until the first releases it. Swapping two devices between two
routes is therefore a plain sequence of two writes, through an intermediate
state where both routes point at the same device.

A route nothing was picked for follows its default, which ASoC USB works out
whenever the route is queried: route N gets the Nth available pair of its
direction, wrapping around when there are fewer pairs than routes. A pick,
by a write to the control, overrides it until the device goes away. With two
backends per direction, one connected device offering one PCM pair puts both
routes on it; when a second device, or a second PCM of the first, appears,
route 1 follows to it, unless it was picked. Playback and capture routes are
independent of each other.

When no pair is available, the control has one item, ``None``, and reads as
item zero. ``None`` only marks an empty list. Once devices are available it
disappears, so it cannot be used to turn offload off.

A write resolves the item number to a device/PCM pair:

* Writing the pair that is already selected does nothing.
* Writing an item number past the end of the list fails with ``-EINVAL``,
  except item zero on an empty list, which does nothing.
* Selecting a different pair fails with ``-EBUSY`` while SOF holds an
  acquired stream on that route's backend, from BE ``hw_params`` until
  ``hw_free`` or the unplug of the device, whether or not the stream is
  running.
* A write which changes the pair while the route's backend is acquiring its
  stream is not refused, the backend is not busy yet. The backend reads the
  selection back once it holds the stream and, finding it changed, releases
  the stream again and fails its ``hw_params`` (or ``prepare`` after a
  system suspend) with ``-EBUSY``.
* Otherwise the new pair becomes the selection. The next acquisition uses it.
  The write itself allocates no USB or xHCI resources.

A successful selection does not reserve any xHCI resource.

On disconnect, the device's entries are removed. Every route that pointed at
the device is cleared, and then every empty route gets its default again, if
any pair is left. Connect and disconnect send
``SNDRV_CTL_EVENT_MASK_INFO`` to every route control of the direction and
add ``SNDRV_CTL_EVENT_MASK_VALUE`` where the selection changed. A stream
already acquired on the removed device is not moved to the new selection; see
`Physical disconnect`_.

Probe ordering and rediscovery
------------------------------

USB enumeration, DSP startup, and machine-card binding need not occur in a
fixed order. Two rediscovery points close the gaps:

1. Registering with the library installs the USB platform callbacks and
   re-emits connect callbacks for USB chips that already exist.
2. Port publication calls ``snd_usb_rediscover_devices()`` again, after the
   SOF card has somewhere to route them.

If the SOF port does not exist yet, ``hda_uaol_notify_connect()`` publishes
nothing but still returns success. The library keeps its record of the
device and notifies SOF again on the next rediscovery. Both the library and
SOF ignore devices they already know, so rediscovery does not create
duplicate entries.

PCM negotiation and offload acquisition
=======================================

Negotiation rules
-----------------

What the topology allows on a frontend is not necessarily what the selected
USB device can play. When a frontend that routes to a UAOL backend is opened,
``hda_apply_constraint_from_be()`` adds hardware rules derived from that
backend.

There are three sets of rules, each covering rate, channels, and format:

* **Link FIFO rules** limit the byte rate to what fits in the stream's share
  of the UAOL FIFO. They apply even when no USB device is selected. The
  service interval is not known yet, so they assume the shortest one,
  125 us, and the real fit is checked after the acquisition (see
  `Acquiring the USB endpoints`_).
* **USB device rules** limit the parameters to the selected USB PCM's
  ``audioformat`` list, leaving out formats that need clock feedback (see
  `Asynchronous playback stays on the host`_). They look up the current
  selection each time
  they run, so a route change or hotplug after open is taken into account.
  Each rule also depends on the parameter it refines, so a request which
  changes only that one is checked as well.
* **Shared backend rules** apply to the second frontend of a backend, the
  deep buffer. They allow only the rate, channels, and format the backend
  was opened with, and allow everything while the backend is idle.

The rules depend on each other: choosing a channel count, for example, can
narrow the allowed rates and formats. The USB rules produce ranges for rate
and channels and a mask for format; they do not reproduce gaps in a device's
discrete rate list. Acquisition then does an exact format lookup and can
still reject a combination that passed negotiation.

SOF's pipeline represents 24-bit audio only as ``S24_LE`` (24 valid bits in
a 4-byte container), while many USB devices pack 24-bit audio in 3 bytes
(``S24_3LE``). SOF therefore sets ``s24_3le_as_s24_le`` when registering with
the library, and devices offering ``S24_3LE`` are also offered as ``S24_LE`` during
negotiation. Acquisition then looks for the format in every PCM of the device
first as the frontend asked for it, and only then as ``S24_3LE``, so a device
with ``S24_LE`` at the chosen rate and channel count keeps it. It reports the
format the stream runs in as ``usb_offload_stream_hw.format``. SOF sets the BE
parameters to it right after the acquire, and the DSP packs the samples on
their way to the link. DPCM hands the BE ``hw_params`` a copy of the
parameters, while the common prepare programs the link DMA again after a stop
from the BE's own copy, so ``uaol_dai_prepare()`` sets that copy to the format
the stream was acquired with; otherwise a stream recovering from an xrun would
run its link DMA with the 4-byte container against the 3-byte one of the
device.

Acquiring the USB endpoints
---------------------------

``uaol_dai_hw_params()`` acquires the USB side first, through
``uaol_dai_start_stream()``, and then runs the common link DMA ``hw_params``
setup also used for SSP and DMIC. A backend fed by two frontends, the
playback PCM and its deep buffer, runs ``hw_params`` once per frontend while
the first one has not started yet; the USB side is acquired by the first call
only and kept until the backend's ``hw_free``, which DPCM runs when the last
frontend lets go.

The two frontends share one endpoint and one mixer, so the second one can
only run what the first one opened the backend with. The backend stores the
rate, channels, and format of the first call and refuses a later one that
differs with ``-EBUSY``, before the core prepares anything for it. A
frontend failing further down would otherwise take the widgets the first one
prepared with it. The shared backend rules above keep this from being
reached in the normal case.

1. Map the selected ASoC USB device back to its ``usb_device`` and the
   selected USB PCM number. If nothing is selected, or the library is not
   running, this fails with ``-ENODEV``.
2. Call ``usb_offload_acquire_stream()`` with the direction, USB PCM number,
   and BE parameters; it settles the 24-bit container itself (see above).
3. The library looks only at the selected USB PCM for a format that exactly
   matches. If there is none, or the match needs clock feedback the host
   keeps to itself, this fails with ``-EOPNOTSUPP``. A USB PCM is
   offloaded once, so a PCM already acquired fails with ``-EBUSY``; the other
   PCMs of the same device are not affected and can be acquired alongside.
4. Claim the USB substream with ``snd_usb_offload_claim_substream()``. This
   uses the same ``subs->opened`` flag, under ``chip->mutex``, as opening the
   USB card's PCM, so only one of the two can own the substream.
5. Resume the USB device, call ``snd_usb_hw_params()``, claim the data
   endpoint and an explicit feedback endpoint with
   ``snd_usb_endpoint_claim_offload()``, and prepare the sync endpoint, if
   any, and the data endpoint. This selects the streaming alternate setting,
   which makes the xHCI driver set up the endpoint. No URBs are submitted:
   the host does not stream the data itself. A claimed endpoint cannot be
   opened by another stream, the host's ``hw_params`` fails with ``-EBUSY``.
   An endpoint which another stream has open already cannot be claimed and
   the acquisition fails with ``-EBUSY``: with implicit feedback the capture
   data endpoint is also the sync source of the device's playback. A freshly
   prepared data endpoint gets the device's format quirk run, as
   ``snd_usb_pcm_prepare()`` does, which sets the sample rate of devices that
   take it through vendor requests. A quirk which leaves the host to drop
   bytes from the stream or its packets to align the samples, as for the
   MacroSilicon MS2100/MS2109 capture cards, fails the acquisition with
   ``-EOPNOTSUPP``: only the URB path does that.
6. Reserve an Audio Sideband resource for the data endpoint and attach it
   with ``xhci_sideband_add_endpoint()``. From now on the controller routes
   this endpoint's data over UAOL. Which resources the endpoint may use is
   SOF's choice, see `Resource IDs, UAOL stream indices, and DMA tags`_.
7. If the stream has an explicit feedback endpoint, reserve and attach a
   resource for it too. An implicit-feedback source is **not** claimed: it is
   another interface's audio data endpoint, not a feedback value the DSP can
   use.
8. Query the xHCI endpoint properties and return them in
   ``usb_offload_stream_hw``: the resource IDs actually assigned, the xHCI
   controller's PCI bus/device/function, the slot ID, endpoint addresses,
   maximum packet sizes, intervals, speed, whether a transaction translator
   is used, and the USB PCM number the stream was acquired on, which names
   the stream to ``usb_offload_quiesce_stream()``,
   ``usb_offload_restart_stream()`` and ``usb_offload_release_stream()``
   together with the direction.
9. Back in SOF, check that the payload of two service intervals, rounded to
   whole sample frames, fits in the stream's share of the link FIFO. The
   interval is the one the library reports. A payload exactly as large as the
   share fits; a larger one is refused with ``-EINVAL``, as an overrun is not
   reported anywhere and only corrupts the audio.
10. Fill in the runtime fields of the UAOL gateway configuration, allocate
    the feedback link DMA if needed, and record the acquired device and BE
    substream.
11. Allocate the audio link DMA and add its IPC4 DMA configuration TLV.

If any step fails, the steps already done are undone, including the USB
claim, and a failure in step 11 releases the USB side as well. On success,
the claim and the USB runtime-PM reference are held until the stream is
released.

Resource IDs, UAOL stream indices, and DMA tags
-----------------------------------------------

Several kinds of index are involved. They are independent of each other and
must not be mixed up:

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Identifier
     - Meaning
   * - USB card / USB PCM number
     - ALSA identity of the device, used by route selection.
   * - USB endpoint address / xHCI slot ID
     - The endpoint and its device, as the xHCI controller knows them.
   * - xHCI Audio Sideband resource ID
     - Allocated at acquisition. On Intel hosts, IDs 1--2 are for OUT
       endpoints and IDs 3--6 for IN endpoints; SOF narrows these further
       by function, see below.
   * - UAOL stream index
     - Sideband resource ID minus one.
   * - Gateway node index
     - The HDA link stream that hardware ties to a UAOL stream:
       UAOL streams 0--5 map to link streams 13, 14, 15, 16, 29, 30.
   * - HDA link DMA stream tag
     - Allocated separately for the audio and feedback link DMAs and passed
       to the firmware in DMA configuration TLVs.

Resource IDs come from one allocator per xHCI controller, shared by all
devices on it, and are assigned when a stream is acquired. The firmware ties
the link's streams to functions through its FIFO layout: the ``T`` output
streams carry playback, the first ``I - T`` input streams carry capture, and
the last ``T`` input streams carry the clock feedback of the playback
streams. An endpoint on a stream outside its function's range would be
served from the wrong FIFO area. SOF therefore answers the library's
``get_resource_range`` callback with the resources of the endpoint's
function, and the library passes that range to
``xhci_sideband_alloc_resource_id()``, which reserves the first free one of
it; when all are taken the acquisition fails with ``-EBUSY``. With two output and four input streams this places
playback on resources 1--2, capture on 3--4, and feedback on 5--6.

Within a range, neither the order in which devices were connected nor their
position in the route control predicts which ID a stream gets. SOF therefore
rebuilds the gateway binding from the values returned by each acquisition.

``xhci_sideband_add_endpoint()`` issues the ``PROP_ECI_AUDIO`` /
``AUDIO_SUBTYPE_SET_RES`` extended-property command for the endpoint. This
command is what routes the endpoint to its sideband resource. Registering
the device or selecting a route does not.

Firmware configuration and link DMA
===================================

Two-stage gateway configuration
-------------------------------

The UAOL DAI copier's gateway configuration is filled in two stages, because
part of it depends on which USB device the stream ends up on.

1. At topology load, ``sof_ipc4_widget_setup_uaol()`` allocates a
   ``sof_ipc4_uaol_configuration_blob`` and fills in what does not depend on
   the device: the link index, the FIFO layout, the timestamp conversion, and
   all TLV headers.
2. At every acquisition, ``uaol_dai_set_endpoint()`` fills in the rest from
   the ``usb_offload_stream_hw`` the library returned.

The blob is a plain list of type/length/value records; lengths are in bytes.
All seven TLVs are always present, with the feedback fields zeroed when there
is no feedback endpoint:

.. list-table::
   :header-rows: 1
   :widths: 10 30 60

   * - Type
     - Value
     - Source
   * - 0
     - xHCI bus/device/function
     - PCI address of the xHCI controller, from the endpoint properties.
   * - 1
     - Link/stream binding
     - Topology link index; data resource ID minus one; if present, the
       feedback resource ID minus one and the feedback endpoint's interval.
   * - 2
     - FIFO start offsets
     - Computed from the FIFO sizes and stream counts reported by the
       firmware (see below).
   * - 3
     - Data endpoint information
     - Direction, endpoint number, slot ID, split/high-speed flags, maximum
       packet size.
   * - 4
     - Feedback endpoint information
     - Same layout for the feedback endpoint, or zero if there is none.
   * - 5
     - ART conversion
     - Multiplier and divider both 1: the xHCI global time is already
       counted in DSP timer ticks.
   * - 6
     - Service interval, microseconds
     - Derived from the data endpoint's ``bInterval``.

For a high-speed data endpoint the service interval is
``125 * 2^(bInterval - 1)`` microseconds, for a full-speed one
``1000 * 2^(bInterval - 1)``: an isochronous ``bInterval`` is an exponent at
both speeds. ``bInterval`` is first clamped to 1--16.

The copier's gateway node index is the link stream mapped from the data
resource ID (see the table above), not the audio link DMA's stream tag. When
the copier is created, the common IPC4 code appends the DMA configuration
TLVs to this blob and sends the result to the firmware.

FIFO partitioning and constraints
---------------------------------

The UAOL link has one transmit and one receive FIFO, shared by all of its
streams. Which stream slot a USB endpoint uses follows from the sideband
resource ID it gets at acquisition. Even a single active stream can land on
any slot, so the FIFOs are divided among all the streams the hardware has,
not just the ones the topology uses.

The firmware reports ``T`` output (transmit) streams and ``I`` input (receive)
streams. The receive FIFO also carries one eight-byte clock-feedback word per
transmit stream, at its start, and those feedback channels are counted in
``I``. The layout is::

    audio_receive_streams = I - T
    tx_size = round_down(max_tx_fifo_size / T, 8)
    rx_base = T * 8
    rx_size = round_down((max_rx_fifo_size - rx_base) /
                         audio_receive_streams, 8)

All start offsets are multiples of the eight-byte bus width. A link with no
transmit streams, or no receive streams left for audio, is rejected.

A stream's share must hold two service intervals of audio, so that the DSP
can refill it while the link drains it. Samples occupy their valid width in
the FIFO, rounded up to whole bytes, not their container size. The
requirement is::

    2 * rate * channels * sample_bytes * service_interval_seconds
        <= per_stream_fifo_size

The service interval is only known once a device's alternate setting has
been chosen, which happens after negotiation. The frontend rules therefore
assume the shortest possible interval, 125 microseconds::

    rate * channels * sample_bytes <= (fifo_size / 2) * 8000

This is only a coarse filter: with a longer service interval, a combination
that passes it can still overflow the FIFO. SOF does not repeat the check
with the real interval at acquisition. An overflow is not reported anywhere;
the audio is simply corrupted. Keep this in mind when a combination that
negotiated successfully produces corrupted audio.

Audio and explicit-feedback DMAs
--------------------------------

UAOL uses the same IPC4 link DMA setup as SSP and DMIC, with a UAOL-specific
lookup of the multilink. The audio link DMA is described in DMA TLV slot
zero: HDA DMA method, preallocated by the host, ``dma_channel_id = tag - 1``,
and ``stream_id = tag``.

A playback stream with an explicit feedback endpoint needs a second link DMA,
in the capture direction, to bring the feedback values to the DSP. It is part
of the playback stream, not a capture stream of its own:

* It carries one 32-bit (``S32_LE``) mono value at the stream's rate.
* It is described in DMA TLV slot one; the firmware treats the second DMA as
  the feedback channel.
* It is not the DAI's DMA data and is not registered as a ``link_substream``,
  so the common code does not handle it. The UAOL DAI starts, stops, and
  frees it itself.
* When it is freed, its TLV is cleared, so that a later device without a
  feedback endpoint does not inherit a stale feedback DMA.

Both link DMAs come from the shared HDA link-stream allocator. On ACE2+ it
avoids two hardware errata: a SoundWire stream cannot share a stream index
with an HDA, iDisp, or UAOL stream of the opposite direction, and a playback
stream index once used by HDA or iDisp cannot be used for another link type
until the controller is reset. Free sideband resources therefore do not
guarantee that the link DMAs can be allocated.

Trigger, teardown, and power transitions
========================================

Start and pause
---------------

On ``START`` and ``PAUSE_RELEASE``, the UAOL DAI starts the feedback link DMA,
if there is one, and then runs the common HDA DAI trigger. That starts the
audio link DMA and sets the IPC4 pipeline to running.

On ``PAUSE_PUSH``, the common trigger pauses the DSP pipeline and stops the
audio link DMA, and the UAOL DAI also stops the feedback link DMA. Unlike
``STOP`` and ``SUSPEND``, pausing leaves the USB endpoints running and keeps
all resources.

Stop ordering is a hardware requirement
---------------------------------------

On ``STOP`` and ``SUSPEND`` the steps run in this order::

    common HDA DAI trigger:
      pause the DSP pipeline (the DSP stops its UAOL stream)
      stop the audio link DMA, common link DMA cleanup
    usb_offload_quiesce_stream():
      stop the xHCI feedback endpoint, if any
      stop the xHCI data endpoint
    stop the feedback link DMA
    ... later: DAI copier freed, UAOL link powered down ...
    hw_free:
      free the audio link DMA
      free the feedback link DMA, clear its TLV
      detach endpoints and free sideband resources, USB hw_free,
      drop the USB runtime-PM reference, release the substream claim

The xHCI endpoints must be stopped in a narrow window:

* **Not before the DSP has stopped.** The DSP cannot bring its UAOL stream to
  idle while the xHCI controller no longer services an endpoint still bound
  to the stream, and the stop times out. An endpoint already detached from
  its resource, as on a disconnect, does not hold the stop up.
* **Not as late as hw_free.** By then the DAI copier has been freed and the
  UAOL link powered down. An IN endpoint the controller still services, in
  particular the feedback endpoint of a playback stream, has nowhere to
  deliver its data and can lock up the controller.

``usb_offload_quiesce_stream()`` only stops the endpoints. It keeps the
sideband resources and the substream claim, and
``usb_offload_release_stream()`` must still follow. Release detaches the
feedback endpoint before the data endpoint, frees their resource IDs, then
runs USB hw_free, drops the runtime-PM reference, and releases the claim.

A stream can also be started again without ``hw_free`` in between, which is
how an xrun is recovered from: ``STOP``, ``prepare``, ``START``. A stopped
xHCI endpoint only runs again once its doorbell is rung, and starting the DSP
side of the link does not do that, so the ``START`` trigger of a quiesced
stream first restarts the endpoints with ``usb_offload_restart_stream()``:
the data endpoint, then the feedback endpoint. They then run before the DSP
starts its UAOL stream, as they do on the first start. A release forgets
that the endpoints were quiesced; after a system suspend the stream is
acquired again with fresh endpoints instead.

Quiesce logs a failure to stop an endpoint only at debug level and still
returns success, and the trigger does not check its return value. A
successful stop therefore does not prove that the endpoints are idle.

System suspend and resume
-------------------------

System suspend frees the DSP pipeline and DAI copier, and resets the USB
device's streaming alternate setting, while the application keeps the PCM
open. The ``SUSPEND`` trigger records that the direction has to be acquired
again. ALSA only triggers a running stream, so a stream which is paused, or
not running, when the system suspends is not acquired again.

On resume the backend gets only ``prepare``, not ``hw_params``. So
``uaol_dai_prepare()`` releases the old acquisition, acquires again with the
saved BE parameters, which rebuilds the endpoint binding, the gateway
configuration, and the feedback link DMA, and then runs the common prepare.
The resource IDs may differ from before the suspend. The widget stays
prepared over the suspend, so its IPC payload is not built again; the copier
setup refreshes the gateway configuration in it from the one the DAI
rewrote, along with the node ID and the DMA configuration TLVs.

The flag is only cleared once the stream is acquired again, a failed
``prepare`` tries again the next time. A backend without a device, because
it was unplugged, fails ``prepare`` with ``-ENODEV`` rather than starting the
DSP on a gateway whose resources are gone. If the stream is closed while
suspended, the flag is cleared so that nothing is acquired again later.
Audio does not keep flowing through system suspend: the stream stops, and is
set up again when the PCM is prepared after resume.

Jack semantics, unplug, and lifetime
====================================

What the jack means
-------------------

SOF creates one card-wide jack, ``USB Offload Jack``, of type
``SND_JACK_USB``, and reports it directly with ``snd_jack_report()``. It does
not use the generic ``snd_soc_usb_setup_offload_jack()`` helper, which would
also hand the jack to the component through ``set_jack``.

The jack answers one question: **is there at least one USB device that
offload can be routed to?**

* It is reported plugged when a device is published to the SOF port.
* On disconnect it stays plugged as long as SOF still knows of another
  device, and is reported unplugged when the last one goes away.
* A playback-only or a capture-only device is enough to plug it.
* It does not say which device is selected, whether a stream is running,
  whether both directions are available, or whether acquisition will succeed.

It is not the headset's own headphone or microphone jack, and there is no
jack per device. It switches no DAPM pins and moves no streams. Userspace
needs the route controls, and PCM negotiation, to know what it can play.

With a working port and no offloadable device, both route controls show
``None`` and the jack is unplugged. A device rejected by the candidate filter
or by sideband registration, or one whose PCMs all stay with the host, never
plugs the jack. If the library failed to
probe altogether, the UAOL DAI still creates the jack, permanently unplugged,
but no route controls.

Physical disconnect
-------------------

Unplugging a device that has an acquired stream does not follow the orderly
stop sequence above. The USB side goes first, while the DSP may still be
running:

1. snd-usb-audio calls the bridge's disconnect callback before marking the
   chip as shut down. The bridge detaches the endpoints from their sideband
   resources, frees the resources, and releases the USB substream.
2. The bridge then notifies SOF. ``hda_uaol_stop_frontends()`` finds the BE
   substreams acquired on this device and, under the BE's DPCM lock, sets
   every connected FE to ``SNDRV_PCM_STATE_DISCONNECTED``, stopping it first
   if it is running. Without this the application would keep writing to a
   PCM whose USB device no longer exists. The BE then forgets the device:
   the USB side is already released and the ``usb_device`` goes away with
   the disconnect, so the FE's later stop, ``hw_free``, and close do not
   hand its stale pointer back to the bridge, where a newly plugged device
   reusing the address could match. The feedback link DMA stays with the BE
   until its ``hw_free``.
3. SOF removes its device record from ASoC USB, which drops the device's
   route items and selects another device if one is available. The jack is
   then updated from the remaining devices.
4. Finally the bridge unregisters the device's sideband handle.

Card and library removal
------------------------

When the UAOL DAI is removed, SOF clears its port and jack pointers under its
UAOL mutex, removes and frees the port, and then frees the device records it
still holds. The jack belongs to the sound card and is freed with it.

``usb_offload_unregister()`` lets snd-usb-audio run the disconnect callback
for every connected device and unregister the USB platform callbacks, both
under its ``register_mutex`` with ``snd_usb_release_platform_ops()``. The
bridge's device records are released and the host is told of the
disconnects as on an unplug, and no device can be freed under a record in
the meantime. Once it returns no callback can be running or start, and the
context is freed.

Synchronization model
=====================

The implementation has several distinct locks rather than one global stack
lock:

* USB-audio ``register_mutex`` serializes platform-callback registration,
  rediscovery, and USB-chip callbacks.
* ``usb_offload_ua_lock`` protects the active library context, per-device
  bridge records, and USB-side stream acquisition/release. The library has
  no lock of its own: the host callbacks are dispatched from the bridge.
* SOF ``uaol_mutex`` protects its published-port/device-record bookkeeping.
* ASoC USB ``ctx_mutex`` protects ports, available-device lists, selections,
  and route-control operations.
* ``chip->mutex`` decides who owns a USB substream: the USB card's own PCM or
  the offload path.
* The backend DPCM mutex stabilizes frontend/backend associations during
  disconnect, with PCM stream locks used to change frontend state.
* The HDA bus register lock protects link-DMA allocator masks; each sideband
  handle has its own mutex for sideband operations.

Registration and rediscovery are never done while holding the library's
controller lock, and the bridge does not hold ``usb_offload_ua_lock`` while
it registers or unregisters with snd-usb-audio. The disconnect path drops
``usb_offload_ua_lock`` while it notifies SOF, because stopping the frontends
triggers the BE, which calls back into the library. Keep these rules in mind
when adding callbacks: a hotplug notification runs with locks held and can
re-enter other parts of the stack.

Current limits and troubleshooting
==================================

Current limits:

* One usable UAOL link, with two streams per direction. A ``(device, USB
  PCM)`` pair is offloaded once: the two backends of a direction need two
  devices, or a device with two PCM pairs, to both run.
* One set of USB platform callbacks system-wide, and one active library
  context, so one registered host. There is no mapping from xHCI
  controllers to UAOL links yet: every eligible device on any controller is
  published to the one host.
* On Intel hosts the xHCI controller has six Audio Sideband resources: two
  for OUT and four for IN endpoints, of which SOF uses two for capture and
  two for clock feedback. An explicit feedback endpoint uses one of the
  feedback resources plus a capture-direction link DMA.
* A device that appears in the route controls is not guaranteed to work in
  every format.
* SOF keeps asynchronous playback on the host unless ``allow_feedback`` is
  set; see `Asynchronous playback stays on the host`_.
* Implicit feedback is not supported; such a sync source is left unused even
  with ``allow_feedback``. Without it, the playback and the capture it syncs
  to both stay on the host. With it, the two directions cannot be split, as
  the capture data endpoint is also the sync source of the playback: while
  the capture is offloaded, the device's playback cannot be opened on the
  host, nor offloaded, and the other way around. A capture format with
  implicit feedback of its own, which snd-usb-audio gives to a device that
  only captures while its playback endpoint streams (the Behringer FCA1616),
  always stays on the host: the DSP only gets the capture endpoint.

When bring-up fails, work through the stack from the bottom up:

1. **No UAOL backend:** check the Kconfig options, HDA multilink discovery,
   the firmware's UAOL capability messages, the final ``uaol_link_mask``, and
   that the topology's DAI names match the backends.
2. **No route controls:** check that SOF registered with the library and
   that the UAOL DAI is bound in the card. A jack without route controls
   means the library is not running.
3. **Route controls show only None:** check that the platform callbacks are
   registered, the candidate-filter debug messages, the speed rule,
   the controller's sideband capability, and the rediscovery after the card registered. A playback route that stays
   ``None`` while the capture route lists the device points at an
   asynchronous playback endpoint; the library's debug message names the
   PCM.
4. **Route selected but hw_params fails:** check the selected direction and
   USB PCM, whether the USB card's own PCM is open, exact format support,
   free sideband resources, and free link DMAs, including for feedback.
5. **Corrupted audio with accepted parameters:** check sample packing, the
   device's real service interval against the FIFO share (negotiation
   assumes 125 microseconds), and the endpoint fields in the gateway
   configuration.
6. **Stop timeout or xHCI controller hang:** check the stop order: DSP
   pipeline, then xHCI endpoints including feedback, then copier free and
   link power-down.
7. **Failure after resume:** check that ``prepare`` acquired the stream again
   and rebuilt the bindings, rather than reusing the old resource IDs.

The route controls and the jack are card-interface controls, so ``amixer``
needs ``iface=CARD``. With ``$SOF_CARD`` set to the SOF card number::

    amixer -c "$SOF_CARD" controls | grep 'USB Offload'
    amixer -c "$SOF_CARD" cget iface=CARD,name='USB Offload 0 Playback Route'
    amixer -c "$SOF_CARD" cget iface=CARD,name='USB Offload 0 Capture Route'
    amixer -c "$SOF_CARD" cget iface=CARD,name='USB Offload Jack'

To point the first playback backend at item zero of the current list, and
the second at item one::

    amixer -c "$SOF_CARD" cset iface=CARD,name='USB Offload 0 Playback Route' 0
    amixer -c "$SOF_CARD" cset iface=CARD,name='USB Offload 1 Playback Route' 1

To exercise offload, open the SOF frontend PCM that the topology routes to
that UAOL backend, not the USB card's PCM. Item numbers change with hotplug,
so do not store them as device identities; re-read the list instead.

Source map
==========

The following paths are relative to the kernel source root:

.. list-table::
   :header-rows: 1
   :widths: 55 45

   * - Source
     - Primary responsibility
   * - :file:`include/sound/usb-offload.h`
     - Library API, host callbacks, device and stream-resource identities.
   * - :file:`sound/usb/offload/init.c`
     - Host registration, context lifecycle and hotplug dispatch.
   * - :file:`sound/usb/offload/usb-audio.c`
     - Candidate filter, platform-ops bridge, format refinement, resource
       acquisition, quiesce, restart, release.
   * - :file:`sound/usb/card.c`, :file:`sound/usb/pcm.c`
     - USB platform callbacks and rediscovery; arbitration of substream
       ownership between the USB card's PCM and offload.
   * - :file:`sound/soc/soc-usb.c`, :file:`include/sound/soc-usb.h`
     - Port lookup, available devices, route lists, controls and policy.
   * - :file:`sound/soc/sof/intel/hda.c`
     - Discovery, firmware gates, library callbacks, machine registration.
   * - :file:`sound/soc/sof/intel/hda-uaol.c`
     - SOF records, port, jack, constraints, endpoint-to-gateway binding,
       backend lifecycle and disconnect handling.
   * - :file:`sound/soc/sof/intel/hda-dai.c`,
       :file:`sound/soc/sof/intel/hda-dai-ops.c`
     - The DAI callbacks the UAOL DAI builds on, link DMA allocation,
       feedback DMA, IPC4 trigger machinery.
   * - :file:`sound/soc/sof/intel/hda-pcm.c`
     - Applying backend-derived constraints during frontend open.
   * - :file:`sound/soc/sof/intel/hda-mlink.c`,
       :file:`sound/soc/sof/intel/lnl.c`
     - UAOL multilink lookup and alternate-link offload enablement.
   * - :file:`sound/soc/sof/ipc4-loader.c`
     - Firmware UAOL support/capability parsing.
   * - :file:`sound/soc/sof/ipc4-topology.c`,
       :file:`sound/soc/sof/ipc4-topology.h`
     - Gateway TLV layout, FIFO partitioning and copier IPC assembly.
   * - :file:`sound/soc/intel/boards/sof_board_helpers.c`,
       :file:`sound/soc/intel/boards/skl_hda_dsp_generic.c`,
       :file:`sound/soc/intel/boards/sof_sdw.c`,
       :file:`sound/soc/intel/boards/sof_uaol_common.h`
     - Machine backend links and shared naming.
   * - :file:`drivers/usb/host/xhci-sideband.c`,
       :file:`include/linux/usb/xhci-sideband.h`
     - Controller eligibility, sideband resource reservation, endpoint
       binding/properties, and endpoint stop.
