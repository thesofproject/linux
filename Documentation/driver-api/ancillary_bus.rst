.. SPDX-License-Identifier: GPL-2.0-only

=============
Ancillary Bus
=============

In some subsystems, the functionality of the core device (PCI/ACPI/other) is
too complex for a single device to be managed as a monolithic block or a part of
the functionality needs to be exposed to a different subsystem.  Splitting the
functionality into smaller orthogonal devices would make it easier to manage
data, power management and domain-specific interaction with the hardware. A key
requirement for such a split is that there is no dependency on a physical bus,
device, register accesses or regmap support. These individual devices split from
the core cannot live on the platform bus as they are not physical devices that
are controlled by DT/ACPI. The same argument applies for not using MFD in this
scenario as MFD relies on individual function devices being physical devices
that are DT enumerated.

An example for this kind of requirement is the audio subsystem where a single
IP is handling multiple entities such as HDMI, Soundwire, local devices such as
mics/speakers etc. The split for the core's functionality can be arbitrary or
be defined by the DSP firmware topology and include hooks for test/debug. This
allows for the audio core device to be minimal and focused on hardware-specific
control and communication.

The ancillary bus is intended to be minimal, generic and avoid domain-specific
assumptions. Each ancillary_device represents a part of its parent
functionality. The generic behavior can be extended and specialized as needed
by encapsulating an ancillary_device within other domain-specific structures and
the use of .ops callbacks. Devices on the ancillary bus do not share any
structures and the use of a communication channel with the parent is
domain-specific.

When Should the Ancillary Bus Be Used
=====================================

The ancillary bus is to be used when a driver and one or more kernel modules,
who share a common header file with the driver, need a mechanism to connect and
provide access to a shared object allocated by the ancillary_device's
registering driver.  The registering driver for the ancillary_device(s) and the
kernel module(s) registering ancillary_drivers can be from the same subsystem,
or from multiple subsystems.

The emphasis here is on a common generic interface that keeps subsystem
customization out of the bus infrastructure.

One example could be a multi-port PCI network device that is rdma-capable and
needs to export this functionality and attach to an rdma driver in another
subsystem.  The PCI driver will allocate and register an ancillary_device for
each physical function on the NIC.  The rdma driver will register an
ancillary_driver that will be matched with and probed for each of these
ancillary_devices.  This will give the rdma driver access to the shared data/ops
in the PCI drivers shared object to establish a connection with the PCI driver.

Another use case is for the a PCI device to be split out into multiple sub
functions.  For each sub function an ancillary_device will be created.  A PCI
sub function driver will bind to such devices that will create its own one or
more class devices.  A PCI sub function ancillary device will likely be
contained in a struct with additional attributes such as user defined sub
function number and optional attributes such as resources and a link to the
parent device.  These attributes could be used by systemd/udev; and hence should
be initialized before a driver binds to an ancillary_device.

Ancillary Device
================

An ancillary_device is created and registered to represent a part of its parent
device's functionality. It is given a name that, combined with the registering
drivers KBUILD_MODNAME, creates a match_name that is used for driver binding,
and an id that combined with the match_name provide a unique name to register
with the bus subsystem.

Registering an ancillary_device is a two-step process.  First you must call
ancillary_device_initialize(), which will check several aspects of the
ancillary_device struct and perform a device_initialize().  After this step
completes, any error state must have a call to put_device() in its resolution
path.  The second step in registering an ancillary_device is to perform a call
to ancillary_device_add(), which will set the name of the device and add the
device to the bus.

To unregister an ancillary_device, just a call to ancillary_device_unregister()
is used.  This will perform both a device_del() and a put_device().

.. code-block:: c

	struct ancillary_device {
		struct device dev;
                const char *name;
		u32 id;
	};

If two ancillary_devices both with a match_name "mod.foo" are registered onto
the bus, they must have unique id values (e.g. "x" and "y") so that the
registered devices names will be "mod.foo.x" and "mod.foo.y".  If match_name +
id are not unique, then the device_add will fail and generate an error message.

The ancillary_device.dev.type.release or ancillary_device.dev.release must be
populated with a non-NULL pointer to successfully register the ancillary_device.

The ancillary_device.dev.parent must also be populated.

Ancillary Device Memory Model and Lifespan
------------------------------------------

When a kernel driver registers an ancillary_device on the ancillary bus, we will
use the nomenclature to refer to this kernel driver as a registering driver.  It
is the entity that will allocate memory for the ancillary_device and register it
on the ancillary bus.  It is important to note that, as opposed to the platform
bus, the registering driver is wholly responsible for the management for the
memory used for the driver object.

A parent object, defined in the shared header file, will contain the
ancillary_device.  It will also contain a pointer to the shared object(s), which
will also be defined in the shared header.  Both the parent object and the
shared object(s) will be allocated by the registering driver.  This layout
allows the ancillary_driver's registering module to perform a container_of()
call to go from the pointer to the ancillary_device, that is passed during the
call to the ancillary_driver's probe function, up to the parent object, and then
have access to the shared object(s).

The memory for the ancillary_device will be freed only in its release()
callback flow as defined by its registering driver.

The memory for the shared object(s) must have a lifespan equal to, or greater
than, the lifespan of the memory for the ancillary_device.  The ancillary_driver
should only consider that this shared object is valid as long as the
ancillary_device is still registered on the ancillary bus.  It is up to the
registering driver to manage (e.g. free or keep available) the memory for the
shared object beyond the life of the ancillary_device.

Registering driver must unregister all ancillary devices before its registering
parent device's remove() is completed.

Ancillary Drivers
=================

Ancillary drivers follow the standard driver model convention, where
discovery/enumeration is handled by the core, and drivers
provide probe() and remove() methods. They support power management
and shutdown notifications using the standard conventions.

.. code-block:: c

	struct ancillary_driver {
		int (*probe)(struct ancillary_device *,
                             const struct ancillary_device_id *id);
		int (*remove)(struct ancillary_device *);
		void (*shutdown)(struct ancillary_device *);
		int (*suspend)(struct ancillary_device *, pm_message_t);
		int (*resume)(struct ancillary_device *);
		struct device_driver driver;
		const struct ancillary_device_id *id_table;
	};

Ancillary drivers register themselves with the bus by calling
ancillary_driver_register(). The id_table contains the match_names of ancillary
devices that a driver can bind with.

Example Usage
=============

Ancillary devices are created and registered by a subsystem-level core device
that needs to break up its functionality into smaller fragments. One way to
extend the scope of an ancillary_device would be to encapsulate it within a
domain-specific structure defined by the parent device. This structure contains
the ancillary_device and any associated shared data/callbacks needed to
establish the connection with the parent.

An example would be:

.. code-block:: c

        struct foo {
		struct ancillary_device ancildev;
		void (*connect)(struct ancillary_device *ancildev);
		void (*disconnect)(struct ancillary_device *ancildev);
		void *data;
        };

The parent device would then register the ancillary_device by calling
ancillary_device_initialize(), and then ancillary_device_add(), with the pointer
to the ancildev member of the above structure. The parent would provide a name
for the ancillary_device that, combined with the parent's KBUILD_MODNAME, will
create a match_name that will be used for matching and binding with a driver.

Whenever an ancillary_driver is registered, based on the match_name, the
ancillary_driver's probe() is invoked for the matching devices.  The
ancillary_driver can also be encapsulated inside custom drivers that make the
core device's functionality extensible by adding additional domain-specific ops
as follows:

.. code-block:: c

	struct my_ops {
		void (*send)(struct ancillary_device *ancildev);
		void (*receive)(struct ancillary_device *ancildev);
	};


	struct my_driver {
		struct ancillary_driver ancillary_drv;
		const struct my_ops ops;
	};

An example of this type of usage would be:

.. code-block:: c

	const struct ancillary_device_id my_ancillary_id_table[] = {
		{ .name = "foo_mod.foo_dev" },
		{ },
	};

	const struct my_ops my_custom_ops = {
		.send = my_tx,
		.receive = my_rx,
	};

	const struct my_driver my_drv = {
		.ancillary_drv = {
			.driver = {
				.name = "myancillarydrv",
			},
			.id_table = my_ancillary_id_table,
			.probe = my_probe,
			.remove = my_remove,
			.shutdown = my_shutdown,
		},
		.ops = my_custom_ops,
	};
