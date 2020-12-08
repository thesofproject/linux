===========================
SoundWire Subsystem Summary
===========================

SoundWire is a new interface ratified in 2015 by the MIPI Alliance.
SoundWire is used for transporting data typically related to audio
functions. SoundWire interface is optimized to integrate audio devices in
mobile or mobile inspired systems.

SoundWire is a 2-pin multi-drop interface with data and clock line. It
facilitates development of low cost, efficient, high performance systems.
Broad level key features of SoundWire interface include:

 (1) Transporting all of payload data channels, control information, and setup
     commands over a single two-pin interface.

 (2) Lower clock frequency, and hence lower power consumption, by use of DDR
     (Dual Data Rate) data transmission.

 (3) Clock scaling and optional multiple data lanes to give wide flexibility
     in data rate to match system requirements.

 (4) Device status monitoring, including interrupt-style alerts to the Manager.

The SoundWire protocol supports up to eleven Peripheral interfaces. All the
interfaces share the common Bus containing data and clock line. Each of the
Peripherals can support up to 14 Data Ports. 13 Data Ports are dedicated to audio
transport. Data Port0 is dedicated to transport of Bulk control information,
each of the audio Data Ports (1..14) can support up to 8 Channels in
transmit or receiving mode (typically fixed direction but configurable
direction is enabled by the specification).  Bandwidth restrictions to
~19.2..24.576Mbits/s don't however allow for 11*13*8 channels to be
transmitted simultaneously.

Below figure shows an example of connectivity between a SoundWire Manager and
two Peripheral devices. ::

        +---------------+                                       +---------------+
        |               |                       Clock Signal    |               |
        |    Manager    |-------+-------------------------------|   Peripheral  |
        |   Interface   |       |               Data Signal     |  Interface 1  |
        |               |-------|-------+-----------------------|               |
        +---------------+       |       |                       +---------------+
                                |       |
                                |       |
                                |       |
                             +--+-------+--+
                             |             |
                             |  Peripheral |
                             | Interface 2 |
                             |             |
                             +-------------+


Terminology
===========

The MIPI SoundWire specification uses the term 'device' to refer to a Manager
or Peripheral interface, which of course can be confusing. In this summary and
code we use the term interface only to refer to the hardware. We follow the
Linux device model by mapping each Peripheral interface connected on the bus as a
device managed by a specific driver. The Linux SoundWire subsystem provides
a framework to implement a SoundWire Peripheral driver with an API allowing
3rd-party vendors to enable implementation-defined functionality while
common setup/configuration tasks are handled by the bus.

Bus:
Implements SoundWire Linux Bus which handles the SoundWire protocol.
Programs all the MIPI-defined Peripheral registers. Represents a SoundWire
Manager. Multiple instances of Bus may be present in a system.

Peripheral:
Registers as SoundWire Peripheral device (Linux Device). Multiple Peripheral devices
can register to a Bus instance.

Peripheral driver:
Driver controlling the Peripheral device. MIPI-specified registers are controlled
directly by the Bus (and transmitted through the Manager driver/interface).
Any implementation-defined Peripheral register is controlled by Peripheral driver. In
practice, it is expected that the Peripheral driver relies on regmap and does not
request direct register access.

Programming interfaces (SoundWire Manager interface Driver)
==========================================================

SoundWire Bus supports programming interfaces for the SoundWire Manager
implementation and SoundWire Peripheral devices. All the code uses the "sdw"
prefix commonly used by SoC designers and 3rd party vendors.

Each of the SoundWire Manager interfaces needs to be registered to the Bus.
Bus implements API to read standard Manager MIPI properties and also provides
callback in Manager ops for Manager driver to implement its own functions that
provides capabilities information. DT support is not implemented at this
time but should be trivial to add since capabilities are enabled with the
``device_property_`` API.

The Manager interface along with the Manager interface capabilities are
registered based on board file, DT or ACPI.

Following is the Bus API to register the SoundWire Bus:

.. code-block:: c

	int sdw_bus_manager_add(struct sdw_bus *bus,
				struct device *parent,
				struct fwnode_handle)
	{
		sdw_manager_device_add(bus, parent, fwnode);

		mutex_init(&bus->lock);
		INIT_LIST_HEAD(&bus->peripherals);

		/* Check ACPI for Peripheral devices */
		sdw_acpi_find_peripherals(bus);

		/* Check DT for Peripheral devices */
		sdw_of_find_peripherals(bus);

		return 0;
	}

This will initialize sdw_bus object for Manager device. "sdw_manager_ops" and
"sdw_manager_port_ops" callback functions are provided to the Bus.

"sdw_manager_ops" is used by Bus to control the Bus in the hardware specific
way. It includes Bus control functions such as sending the SoundWire
read/write messages on Bus, setting up clock frequency & Stream
Synchronization Point (SSP). The "sdw_manager_ops" structure abstracts the
hardware details of the Manager from the Bus.

"sdw_manager_port_ops" is used by Bus to setup the Port parameters of the
Manager interface Port. Manager interface Port register map is not defined by
MIPI specification, so Bus calls the "sdw_manager_port_ops" callback
function to do Port operations like "Port Prepare", "Port Transport params
set", "Port enable and disable". The implementation of the Manager driver can
then perform hardware-specific configurations.

Programming interfaces (SoundWire Peripheral Driver)
===============================================

The MIPI specification requires each Peripheral interface to expose a unique
48-bit identifier, stored in 6 read-only dev_id registers. This dev_id
identifier contains vendor and part information, as well as a field enabling
to differentiate between identical components. An additional class field is
currently unused. Peripheral driver is written for a specific vendor and part
identifier, Bus enumerates the Peripheral device based on these two ids.
Peripheral device and driver match is done based on these two ids . Probe
of the Peripheral driver is called by Bus on successful match between device and
driver id. A parent/child relationship is enforced between Manager and Peripheral
devices (the logical representation is aligned with the physical
connectivity).

The information on Manager/Peripheral dependencies is stored in platform data,
board-file, ACPI or DT. The MIPI Software specification defines additional
link_id parameters for controllers that have multiple Manager interfaces. The
dev_id registers are only unique in the scope of a link, and the link_id
unique in the scope of a controller. Both dev_id and link_id are not
necessarily unique at the system level but the parent/child information is
used to avoid ambiguity.

.. code-block:: c

	static const struct sdw_device_id peripheral_id[] = {
	        SDW_PERIPHERAL_ENTRY(0x025d, 0x700, 0),
	        {},
	};
	MODULE_DEVICE_TABLE(sdw, peripheral_id);

	static struct sdw_driver peripheral_sdw_driver = {
	        .driver = {
	                   .name = "peripheral_xxx",
	                   .pm = &peripheral_runtime_pm,
	                   },
		.probe = peripheral_sdw_probe,
		.remove = peripheral_sdw_remove,
		.ops = &peripheral_peripheral_ops,
		.id_table = peripheral_id,
	};


For capabilities, Bus implements API to read standard Peripheral MIPI properties
and also provides callback in Peripheral ops for Peripheral driver to implement own
function that provides capabilities information. Bus needs to know a set of
Peripheral capabilities to program Peripheral registers and to control the Bus
reconfigurations.

Future enhancements to be done
==============================

 (1) Bulk Register Access (BRA) transfers.


 (2) Multiple data lane support.

Links
=====

SoundWire MIPI specification 1.1 is available at:
https://members.mipi.org/wg/All-Members/document/70290

SoundWire MIPI DisCo (Discovery and Configuration) specification is
available at:
https://www.mipi.org/specifications/mipi-disco-soundwire

(publicly accessible with registration or directly accessible to MIPI
members)

MIPI Alliance Manufacturer ID Page: mid.mipi.org
