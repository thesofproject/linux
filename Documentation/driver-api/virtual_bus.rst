.. SPDX-License-Identifier: GPL-2.0-only

===========
Virtual Bus
===========

See <linux/virtual_bus.h> for the models for virtbus_device and virtbus_driver.

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

The virtual bus is intended to be minimal, generic and avoid domain-specific
assumptions. Each virtual bus device represents a part of its parent
functionality. The generic behavior can be extended and specialized as needed
by encapsulating a virtual bus device within other domain-specific structures
and the use of .ops callbacks. Devices on the same virtual bus do not share any
structures and the use of a communication channel with the parent is
domain-specific.

Virtbus Devices
~~~~~~~~~~~~~~~

A virtbus_device is created and registered to represent a part of its parent
device's functionality. It is given a match_name that is used for driver
binding and a release callback that is invoked when the device is unregistered.

.. code-block:: c

	struct virtbus_device {
		struct device dev;
		const char *match_name;
		void (*release)(struct virtbus_device *);
		u32 id;
	};

The virtbus device is enumerated when it is attached to the bus. The device
is assigned a unique ID automatically that will be appended to its name. If
two virtbus_devices both named "foo" are registered onto the bus, they will
have the device names, "foo.x" and "foo.y", where x and y are unique integers.

Virtbus Drivers
~~~~~~~~~~~~~~~

Virtbus drivers follow the standard driver model convention, where
discovery/enumeration is handled by the core, and drivers
provide probe() and remove() methods. They support power management
and shutdown notifications using the standard conventions.

.. code-block:: c

	struct virtbus_driver {
		int (*probe)(struct virtbus_device *);
		int (*remove)(struct virtbus_device *);
		void (*shutdown)(struct virtbus_device *);
		int (*suspend)(struct virtbus_device *, pm_message_t);
		int (*resume)(struct virtbus_device *);
		struct device_driver driver;
		const struct virtbus_device_id *id_table;
	};

Virtbus drivers register themselves with the bus by calling
virtbus_register_driver(). The id_table contains the names of virtbus devices
that a driver can bind with?

Example Usage
~~~~~~~~~~~~~

Virtbus devices are created and registered by a subsystem-level core device
that needs to break up its functionality into smaller fragments. One way to
extend the scope of a virtbus_device would be to encapsulate it within a
domain-specific structure defined by the parent device. This structure contains
the virtual bus device and any associated shared data/callbacks needed to
establish the connection with the parent.

An example would be:

.. code-block:: c

        struct foo {
		struct virtbus_device vdev;
		void (*connect)(struct virtbus_device *vdev);
		void (*disconnect)(struct virtbus_device *vdev);
		void *data;
        };

The parent device would then register the virtbus_device by calling
virtbus_register_device() with the pointer to the vdev member of the above
structure. The parent would provide a match_name for the virtbus_device that
will be used for matching and binding with a driver.

For the binding to succeed when a virtbus_device is registered, there needs
to be a virtbus_driver registered with the bus that includes the match_name
provided above in its id_table. The virtual bus driver can also be
encapsulated inside custom drivers that make the core device's functionality
extensible by adding additional domain-specific ops as follows:

.. code-block:: c

	struct my_ops {
		void (*send)(struct virtbus_device *vdev);
		void (*receive)(struct virtbus_device *vdev);
	};


	struct my_driver {
		struct virtbus_driver virtbus_drv;
		const struct my_ops ops;
	};

An example of this type of usage would be:

.. code-block:: c

	const struct virtbus_device_id my_virtbus_id_table[] = {
		{.name = "foo_dev"},
		{ },
	};

	const struct my_ops my_custom_ops = {
		.send = my_tx,
		.receive = my_rx,
	};

	struct my_driver my_drv = {
		.virtbus_drv = {
			.driver = {
				.name = "myvirtbusdrv",
			},
			.id_table = my_virtbus_id_table,
			.probe = my_probe,
			.remove = my_remove,
			.shutdown = my_shutdown,
		},
		.ops = my_custom_ops,
	};
