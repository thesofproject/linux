===============================
Virtual Bus Devices and Drivers
===============================

See <linux/virtual_bus.h> for the models for virtbus_device and virtbus_driver.
This bus is meant to be a lightweight software based bus to attach generic
devices and drivers to so that a chunk of data can be passed between them.

One use case example is an RDMA driver needing to connect with several
different types of PCI LAN devices to be able to request resources from
them (queue sets).  Each LAN driver that supports RDMA will register a
virtbus_device on the virtual bus for each physical function.  The RDMA
driver will register as a virtbus_driver on the virtual bus to be
matched up with multiple virtbus_devices and receive a pointer to a
struct containing the callbacks that the PCI LAN drivers support for
registering with them.

Sections in this document:
        Virtbus devices
        Virtbus drivers
        Device Enumeration
        Device naming and driver binding
        Virtual Bus API entry points

Virtbus devices
~~~~~~~~~~~~~~~
Virtbus_devices support the minimal device functionality.  Devices will
accept a name, and then, when added to the virtual bus, an automatically
generated index is concatenated onto it for the virtbus_device->name.

Virtbus drivers
~~~~~~~~~~~~~~~
Virtbus drivers register with the virtual bus to be matched with virtbus
devices.  They expect to be registered with a probe and remove callback,
and also support shutdown, suspend, and resume callbacks.  They otherwise
follow the standard driver behavior of having discovery and enumeration
handled in the bus infrastructure.

Virtbus drivers register themselves with the API entry point
virtbus_register_driver and unregister with virtbus_unregister_driver.

Device Enumeration
~~~~~~~~~~~~~~~~~~
Enumeration is handled automatically by the bus infrastructure via the
ida_simple methods.

Device naming and driver binding
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
The virtbus_device.dev.name is the canonical name for the device. It is
built from two other parts:

        - virtbus_device.name (also used for matching).
        - virtbus_device.id (generated automatically from ida_simple calls)

Virtbus device IDs are always in "<name>.<instance>" format.  Instances are
automatically selected through an ida_simple_get so are positive integers.
Name is taken from the device name field.

Driver IDs are simple <name>.

Need to extract the name from the Virtual Device compare to name of the
driver.
