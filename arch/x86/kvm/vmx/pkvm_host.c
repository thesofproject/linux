// SPDX-License-Identifier: GPL-2.0
#include <linux/kvm_host.h>

struct kvm_x86_ops pkvm_host_vt_x86_ops __initdata = {
	.name = KBUILD_MODNAME,
};
