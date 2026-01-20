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
	return pkvm_vmx_data_pages();
}

static __init int pkvm_setup_host_vm(struct pkvm_hyp *pkvm)
{
	struct kvm_vmx *kvmx = pkvm_early_alloc_contig(PKVM_HOST_KVM_VMX_PAGES);

	if (!kvmx) {
		pr_err("no kvm_vmx memory\n");
		return -ENOMEM;
	}

	pkvm->host_kvm = &kvmx->kvm;

	return 0;
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

static __init int pkvm_setup_host_vcpu(struct pkvm_hyp *pkvm, int cpu)
{
	struct vcpu_vmx *vmx;

	if (cpu >= CONFIG_NR_CPUS) {
		pr_err("setup_host_vcpu: invalid CPU number %d\n", cpu);
		return -EINVAL;
	}

	vmx = pkvm_early_alloc_contig(PKVM_HOST_VCPU_VMX_PAGES);
	if (!vmx) {
		pr_err("no host vcpu memory for CPU%d\n", cpu);
		return -ENOMEM;
	}

	vmx->vcpu.cpu = cpu;
	vmx->vcpu.kvm = pkvm->host_kvm;
	pkvm->host_vcpus[cpu] = &vmx->vcpu;

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

	nr_pages = pkvm_vmx_data_pages();
	pkvm_early_alloc_init(__va(pkvm_mem_base), nr_pages << PAGE_SHIFT);

	pkvm = pkvm_early_alloc_contig(PKVM_HYP_PAGES);
	if (!pkvm) {
		pr_err("cannot alloc pkvm_hyp\n");
		ret = -ENOMEM;
		goto out;
	}

	ret = pkvm_setup_host_vm(pkvm);
	if (ret)
		goto out;

	pkvm->num_cpus = num_possible_cpus();

	for_each_possible_cpu(cpu) {
		ret = pkvm_setup_pcpu(pkvm, cpu);
		if (ret)
			goto out;
		ret = pkvm_setup_host_vcpu(pkvm, cpu);
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
