// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <linux/module.h>
#include "vmx.h"

static int __init early_pkvm_parse_cmdline(char *buf)
{
	return kstrtobool(buf, &enable_pkvm);
}
early_param("kvm-intel.pkvm", early_pkvm_parse_cmdline);

u64 pkvm_total_reserve_pages(void)
{
	return pkvm_data_pages();
}

int __init vmx_pkvm_init(void)
{
	return 0;
}

MODULE_LICENSE("GPL");
