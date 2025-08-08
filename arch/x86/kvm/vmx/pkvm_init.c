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

static struct vmcs_config host_vmcs_config;

u64 pkvm_total_reserve_pages(void)
{
	return pkvm_vmx_data_pages();
}

static __init int pkvm_setup_host_vmcs_config(void)
{
	struct vmcs_config *vmcs_config = &host_vmcs_config;
	struct vmx_capability *vmx_cap = &vmx_capability;
	struct vmcs_config_setting setting = {
		.cpu_based_vm_exec_ctrl_req =
			CPU_BASED_USE_MSR_BITMAPS |
			CPU_BASED_ACTIVATE_SECONDARY_CONTROLS,
		.cpu_based_vm_exec_ctrl_opt = 0,
		.secondary_vm_exec_ctrl_req =
			SECONDARY_EXEC_ENABLE_EPT,
		.secondary_vm_exec_ctrl_opt =
			SECONDARY_EXEC_ENABLE_VPID |
			SECONDARY_EXEC_ENABLE_INVPCID |
			SECONDARY_EXEC_ENABLE_XSAVES |
			SECONDARY_EXEC_ENABLE_RDTSCP |
			SECONDARY_EXEC_ENABLE_USR_WAIT_PAUSE,
		.tertiary_vm_exec_ctrl_opt = 0,
		.pin_based_vm_exec_ctrl_req = 0,
		.pin_based_vm_exec_ctrl_opt = 0,
		.vmexit_ctrl_req =
			VM_EXIT_HOST_ADDR_SPACE_SIZE |
			VM_EXIT_LOAD_IA32_PAT |
			VM_EXIT_LOAD_IA32_EFER |
			VM_EXIT_SAVE_IA32_PAT |
			VM_EXIT_SAVE_IA32_EFER |
			VM_EXIT_SAVE_DEBUG_CONTROLS,
		.vmexit_ctrl_opt = 0,
		.vmentry_ctrl_req =
			VM_ENTRY_LOAD_DEBUG_CONTROLS |
			VM_ENTRY_IA32E_MODE |
			VM_ENTRY_LOAD_IA32_EFER |
			VM_ENTRY_LOAD_IA32_PAT,
		.vmentry_ctrl_opt = 0,
	};

	if (setup_vmcs_config_common(vmcs_config, vmx_cap, &setting))
		return -EINVAL;

	pr_info("pin_based_exec_ctrl 0x%x\n", vmcs_config->pin_based_exec_ctrl);
	pr_info("cpu_based_exec_ctrl 0x%x\n", vmcs_config->cpu_based_exec_ctrl);
	pr_info("cpu_based_2nd_exec_ctrl 0x%x\n", vmcs_config->cpu_based_2nd_exec_ctrl);
	pr_info("vmexit_ctrl 0x%x\n", vmcs_config->vmexit_ctrl);
	pr_info("vmentry_ctrl 0x%x\n", vmcs_config->vmentry_ctrl);

	return 0;
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

	ret = pkvm_setup_host_vmcs_config();
	if (ret) {
		pr_err("setup host vmcs config failed\n");
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
