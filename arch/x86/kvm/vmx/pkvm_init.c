// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) "pkvm: " fmt

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

static __init int pkvm_setup_pcpu(struct pkvm_hyp *pkvm, int cpu)
{
	struct pkvm_pcpu *pcpu;

	if (cpu >= CONFIG_NR_CPUS) {
		pr_err("setup_pcpu: invalid CPU number %d\n", cpu);
		return -EINVAL;
	}

	pcpu = pkvm_early_alloc_contig(PKVM_PCPU_PAGES);
	if (!pcpu) {
		pr_err("no pcpu memory for CPU%d\n", cpu);
		return -ENOMEM;
	}

	pkvm->pcpus[cpu] = pcpu;

	return 0;
}

int __init vmx_pkvm_init(void)
{
	unsigned long nr_pages;
	struct pkvm_hyp *pkvm;
	int ret, cpu;

	if (!enable_pkvm)
		return 0;

	if (!pkvm_mem_base) {
		pr_err("required memory not reserved\n");
		ret = -ENOMEM;
		goto out;
	}

	nr_pages = pkvm_data_pages();
	pkvm_early_alloc_init(__va(pkvm_mem_base), nr_pages << PAGE_SHIFT);

	pkvm = pkvm_early_alloc_contig(PKVM_HYP_PAGES);
	if (!pkvm) {
		pr_err("cannot alloc pkvm_hyp\n");
		ret = -ENOMEM;
		goto out;
	}

	pkvm->num_cpus = num_possible_cpus();

	for_each_possible_cpu(cpu) {
		ret = pkvm_setup_pcpu(pkvm, cpu);
		if (ret)
			goto out;
	}

	return 0;
out:
	/*
	 * As the reserved memory at the pkvm_mem_base will not be
	 * released back to the host, no need to de-initialize or
	 * free for the early_alloc.
	 */
	enable_pkvm = false;
	return ret;
}

MODULE_LICENSE("GPL");
