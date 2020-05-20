.. SPDX-License-Identifier: GPL-2.0-only

===========
Ancillary Bus
===========

See <linux/ancillary_bus.h> for the models for ancillary_device and
ancillary_driver.

In some subsystems, the functionality of the core device (PCI/ACPI/other) may
be too complex for a single device to be managed as a monolithic block or
a part of the functionality might need to be exposed to a different subsystem.
Splitting the functionality into smaller orthogonal devices would make it
easier to manage data, power management and domain-specific communication with
the hardware. A key requirement for such a split is that there is no dependency
on a physical bus, device, register accesses or regmap support. These
individual devices split from the core cannot live on the platform bus as they
are not physical devices that are controlled by DT/ACPI. The same argument
applies for not using MFD in this scenario as it relies on individual function
devices being physical devices that are DT enumerated.

An example for this kind of requirement is the audio subsystem where a single
IP handles multiple entities such as HDMI, Soundwire, local devices such as
mics/speakers etc. The split for the core's functionality can be arbitrary or
be defined by the DSP firmware topology and include hooks for test/debug. This
allows for the audio core device to be minimal and tightly coupled with
handling the hardware-specific logic and communication.

The ancillary bus is intended to be minimal, generic and avoid domain-specific
assumptions. Each ancillary_device represents a part of its parent
functionality. The generic behavior can be extended and specialized as needed
by encapsulating an ancillary_device within other domain-specific structures and
the use of .ops callbacks. Devices on the same ancillary bus do not share any
structures and the use of a communication channel with the parent is
domain-specific.

When Should the Ancillary Bus Be Used
=====================================

When a kernel driver registers either an ancillary_device or ancillary_driver on
the ancillary bus, we can use the nomenclature to refer to this kernel driver as
a registering driver.  It is the entity that will allocate memory for and
register something on the ancillary bus.

The ancillary bus is to be used when multiple registering drivers, who share a
common header file, need a mechanism to make connection and provide access to a
shared struct allocated by the registering driver(s) that registered the
ancillary_device(s).  Each of these registering drivers can be from the same
subsystem, or from multiple subsystems.

The emphasis here is on a common generic interface that keeps subsystem
customization out of the bus infrastructure.

When Should the Ancillary Bus Not Be Used
=========================================

If there is a need for a heavier weight bus that provides functionality beyond
just the matching up of multiple registering drivers, and sacrifices common
generality to extend the bus functionality into subsystem specific functions,
then the ancillary bus should not be used.  A custom bus solution, or some other
method should be considered in this use case.

Ancillary Devices
=================

A ancillary_device is created and registered to represent a part of its parent
device's functionality. It is given a match_name that is used for driver
binding and a release callback that is invoked when the device is unregistered.

.. code-block:: c

	struct ancillary_device {
		struct device dev;
		const char *match_name;
		void (*release)(struct ancillary_device *);
		u32 id;
	};

The ancillary device is enumerated when it is attached to the bus. The device
is assigned a unique ID automatically that will be appended to its name. If
two ancillary_devices both named "foo" are registered onto the bus, they will
have the device names, "foo.x" and "foo.y", where x and y are unique integers.

Ancillary Device Memory Model and Lifespan
------------------------------------------

There will be a parent struct, defined in the shared header file, that contains
the ancillary_device as a static element and a pointer to the shared struct,
also defined in the shared header.  Both the parent struct and the shared struct
will be allocated by the ancillary_device's registering driver.  This layout
allows the ancillary_driver's registering driver to perform a container_of()
call to go from the pointer to the ancillary_device, that is passed during the
call to the ancillary_driver's probe function, up to the parent struct, and then
have access to the shared struct.

The memory for the ancillary_device will be freed only in its release()
callback flow as defined by its registering driver.

The memory for the shared struct must have a lifespan equal to, or greater than,
the lifespan of the memory for the ancillary_device.  The ancillary_driver
should only consider that this shared struct is valid as long as the
ancillary_device is still registered on the ancillary bus.  It is up to the
ancillary_device's registering driver to manage (e.g. free or keep available)
the memory for the shared struct beyond the life of the ancillary_device.

Ancillary Drivers
=================

Ancillary drivers follow the standard driver model convention, where
discovery/enumeration is handled by the core, and drivers
provide probe() and remove() methods. They support power management
and shutdown notifications using the standard conventions.

.. code-block:: c

	struct ancillary_driver {
		int (*probe)(struct ancillary_device *);
		int (*remove)(struct ancillary_device *);
		void (*shutdown)(struct ancillary_device *);
		int (*suspend)(struct ancillary_device *, pm_message_t);
		int (*resume)(struct ancillary_device *);
		struct device_driver driver;
		const struct ancillary_device_id *id_table;
	};

Ancillary drivers register themselves with the bus by calling
ancillary_register_driver(). The id_table contains the names of ancillary devices
that a driver can bind with?

Example Usage
=============

Ancillary devices are created and registered by a subsystem-level core device
that needs to break up its functionality into smaller fragments. One way to
extend the scope of a ancillary_device would be to encapsulate it within a
domain-specific structure defined by the parent device. This structure contains
the ancillary bus device and any associated shared data/callbacks needed to
establish the connection with the parent.

An example would be:

.. code-block:: c

        struct foo {
		struct ancillary_device vdev;
		void (*connect)(struct ancillary_device *vdev);
		void (*disconnect)(struct ancillary_device *vdev);
		void *data;
        };

The parent device would then register the ancillary_device by calling
ancillary_register_device() with the pointer to the vdev member of the above
structure. The parent would provide a match_name for the ancillary_device that
will be used for matching and binding with a driver.

For the binding to succeed when a ancillary_device is registered, there needs
to be a ancillary_driver registered with the bus that includes the match_name
provided above in its id_table. The ancillary bus driver can also be
encapsulated inside custom drivers that make the core device's functionality
extensible by adding additional domain-specific ops as follows:

.. code-block:: c

	struct my_ops {
		void (*send)(struct ancillary_device *vdev);
		void (*receive)(struct ancillary_device *vdev);
	};


	struct my_driver {
		struct ancillary_driver ancillary_drv;
		const struct my_ops ops;
	};

An example of this type of usage would be:

.. code-block:: c

	const struct ancillary_device_id my_ancillary_id_table[] = {
		{.name = "foo_dev"},
		{ },
	};

	const struct my_ops my_custom_ops = {
		.send = my_tx,
		.receive = my_rx,
	};

	struct my_driver my_drv = {
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
