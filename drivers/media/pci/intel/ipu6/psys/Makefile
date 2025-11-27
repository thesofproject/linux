# SPDX-License-Identifier: GPL-2.0-only

intel-ipu6-psys-objs			+= ipu-psys.o \
					   ipu-fw-psys.o \
                                           ipu-fw-resources.o \
					   ipu-resources.o \
					   ipu6-psys.o \
					   ipu6-ppg.o \
					   ipu6-l-scheduler.o \
					   ipu6-fw-resources.o \
					   ipu6se-fw-resources.o \
					   ipu6ep-fw-resources.o

obj-$(CONFIG_VIDEO_INTEL_IPU6)		+= intel-ipu6-psys.o

ccflags-y += -I$(srctree)/drivers/media/pci/intel/ipu6
