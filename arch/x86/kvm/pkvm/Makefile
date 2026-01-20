# SPDX-License-Identifier: GPL-2.0

ccflags-y += -I $(srctree)/arch/x86/kvm

pkvm-hyp-y			:= early_alloc.o

pkvm-hyp-$(CONFIG_PKVM_INTEL)	+= vmx/host_vmentry.o vmx/host_vmx.o

obj-$(CONFIG_PKVM_X86)		+= pkvm-hyp.o
