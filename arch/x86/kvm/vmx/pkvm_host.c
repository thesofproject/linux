// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) "pkvm_host: " fmt

#include <linux/kvm_host.h>
#include <asm/kvm_pkvm.h>
#include "pkvm_constants.h"
#include "posted_intr.h"
#include "x86_ops.h"
#include "vmx.h"

static void pkvm_free_loaded_vmcs(struct loaded_vmcs *loaded_vmcs)
{
	if (!loaded_vmcs->vmcs)
		return;
	free_vmcs(loaded_vmcs->vmcs);
	loaded_vmcs->vmcs = NULL;
	if (loaded_vmcs->msr_bitmap)
		free_page((unsigned long)loaded_vmcs->msr_bitmap);
	WARN_ON(loaded_vmcs->shadow_vmcs != NULL);
}

static int pkvm_alloc_loaded_vmcs(struct loaded_vmcs *loaded_vmcs)
{
	loaded_vmcs->vmcs = alloc_vmcs(false);
	if (!loaded_vmcs->vmcs)
		return -ENOMEM;

	loaded_vmcs->shadow_vmcs = NULL;
	loaded_vmcs->cpu = -1;

	if (cpu_has_vmx_msr_bitmap()) {
		loaded_vmcs->msr_bitmap = (unsigned long *)
				__get_free_page(GFP_KERNEL_ACCOUNT);
		if (!loaded_vmcs->msr_bitmap)
			goto out_vmcs;
	}

	return 0;

out_vmcs:
	pkvm_free_loaded_vmcs(loaded_vmcs);
	return -ENOMEM;
}

static void __pkvm_vcpu_unload(void *arg)
{
	struct kvm_vcpu *vcpu = arg;
	struct vcpu_vmx *vmx;

	if (pkvm_hypercall(vcpu_put, vcpu->kvm->arch.pkvm.handle,
			   vcpu->arch.pkvm.handle))
		return;

	vmx = to_vmx(vcpu);
	vmx->loaded_vmcs->cpu = -1;
}

static void pkvm_vcpu_unload(struct kvm_vcpu *vcpu)
{
	int cpu = to_vmx(vcpu)->loaded_vmcs->cpu;

	if (cpu != -1)
		smp_call_function_single(cpu, __pkvm_vcpu_unload, vcpu, 1);
}

static int pkvm_check_processor_compat(void)
{
	return pkvm_hypercall(check_processor_compatibility);
}

static int pkvm_enable_virtualization_cpu(void)
{
	return pkvm_hypercall(enable_virtualization_cpu);
}

static void pkvm_disable_virtualization_cpu(void)
{
	/*
	 * The pKVM hypervisor doesn't support disabling VMX for security
	 * reasons. This means that the CPU will remain in VMX non-root mode
	 * during rebooting if there was no hardware level reset. But pKVM
	 * does not support such warm reboots anyway.
	 */
}

static int pkvm_vm_init(struct kvm *kvm)
{
	void *pkvm_vm;
	int ret;

	/*
	 * Some of struct kvm elements are initialized by the vmx_vm_init()
	 * which can be leveraged by the pKVM host as this initialization is
	 * simple and no VMX hardware involved.
	 */
	ret = vmx_vm_init(kvm);
	if (ret)
		return ret;

	pkvm_vm = alloc_pages_exact(PKVM_VMX_VM_SIZE, GFP_KERNEL_ACCOUNT);
	if (!pkvm_vm)
		return -ENOMEM;

	ret = pkvm_hypercall(vm_init, __pa(kvm), __pa(pkvm_vm));
	if (ret < 0)
		goto free_page;

	kvm->arch.pkvm.handle = ret;

	if (pkvm_is_protected_vm(kvm))
		kvm->arch.has_protected_state = true;

	return 0;

free_page:
	free_pages_exact(pkvm_vm, PKVM_VMX_VM_SIZE);
	return ret;
}

static void pkvm_vm_destroy(struct kvm *kvm)
{
	int vm_handle = kvm->arch.pkvm.handle;
	union pkvm_hc_data out;
	int ret;

	ret = pkvm_hypercall_out(vm_destroy, &out, vm_handle);
	if (ret) {
		pr_err("failed to destroy VM%d: %d\n", vm_handle, ret);
		return;
	}

	kvm_free_pkvm_memcache(&out.vm_destroy.memcache);

	vmx_vm_destroy(kvm);
}

static int pkvm_vcpu_create(struct kvm_vcpu *vcpu)
{
	size_t vcpu_size, fps_size;
	void *pkvm_vcpu, *fps;
	struct vcpu_vmx *vmx;
	int ret;

	vmx = to_vmx(vcpu);

	INIT_LIST_HEAD(&vmx->vt.pi_wakeup_list);

	ret = pkvm_alloc_loaded_vmcs(&vmx->vmcs01);
	if (ret < 0)
		return ret;

	vmx->loaded_vmcs = &vmx->vmcs01;
	vmx->loaded_vmcs->cpu = -1;

	vcpu_size = PKVM_VMX_VCPU_SIZE;
	if (pkvm_is_protected_vcpu(vcpu))
		vcpu_size += KVM_MCE_SIZE + KVM_MCI_CTL2_SIZE;
	if (lapic_in_kernel(vcpu))
		vcpu_size += sizeof(struct kvm_lapic);

	ret = -ENOMEM;
	pkvm_vcpu = alloc_pages_exact(vcpu_size, GFP_KERNEL_ACCOUNT);
	if (!pkvm_vcpu)
		goto free_vmcs;

	fps_size = pkvm_guest_initial_fpstate_size(vcpu->kvm);
	fps = alloc_pages_exact(fps_size, GFP_KERNEL_ACCOUNT);
	if (!fps)
		goto free_vcpu;

	ret = pkvm_hypercall(vcpu_create, vcpu->kvm->arch.pkvm.handle,
			     __pa(vcpu), __pa(pkvm_vcpu), __pa(fps));
	if (ret < 0)
		goto free_fpu;

	vcpu->arch.pkvm.handle = ret;

	return 0;

free_fpu:
	free_pages_exact(fps, fps_size);
free_vcpu:
	free_pages_exact(pkvm_vcpu, vcpu_size);
free_vmcs:
	pkvm_free_loaded_vmcs(vmx->loaded_vmcs);
	return ret;
}

static void pkvm_vcpu_free(struct kvm_vcpu *vcpu)
{
	int vm_handle = vcpu->kvm->arch.pkvm.handle;
	int vcpu_handle = vcpu->arch.pkvm.handle;
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	union pkvm_hc_data out;
	int ret;

	pkvm_vcpu_unload(vcpu);

	ret = pkvm_hypercall_out(vcpu_free, &out, vm_handle, vcpu_handle);
	if (ret) {
		pr_err("failed to free VM%d vcpu%d: %d\n", vm_handle, vcpu_handle, ret);
		return;
	}

	kvm_free_pkvm_memcache(&out.vcpu_free.memcache);

	pkvm_free_loaded_vmcs(vmx->loaded_vmcs);
}

static void pkvm_vcpu_load(struct kvm_vcpu *vcpu, int cpu)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	bool already_loaded;

	already_loaded = vmx->loaded_vmcs->cpu == cpu;
	if (!already_loaded)
		pkvm_vcpu_unload(vcpu);

	if (KVM_BUG_ON(pkvm_hypercall(vcpu_load, vcpu->kvm->arch.pkvm.handle,
				      vcpu->arch.pkvm.handle), vcpu->kvm))
		return;

	if (!already_loaded)
		vmx->loaded_vmcs->cpu = cpu;

	vmx_vcpu_pi_load(vcpu, cpu);
}

static void pkvm_vcpu_put(struct kvm_vcpu *vcpu)
{
	vmx_vcpu_pi_put(vcpu);
}

struct kvm_x86_ops pkvm_host_vt_x86_ops __initdata = {
	.name = KBUILD_MODNAME,

	.check_processor_compatibility = pkvm_check_processor_compat,

	.enable_virtualization_cpu = pkvm_enable_virtualization_cpu,
	.disable_virtualization_cpu = pkvm_disable_virtualization_cpu,
	.emergency_disable_virtualization_cpu = pkvm_disable_virtualization_cpu,

	.vm_size = sizeof(struct kvm_vmx),
	.vm_init = pkvm_vm_init,
	.vm_destroy = pkvm_vm_destroy,

	.vcpu_precreate = vmx_vcpu_precreate,
	.vcpu_create = pkvm_vcpu_create,
	.vcpu_free = pkvm_vcpu_free,

	.vcpu_load = pkvm_vcpu_load,
	.vcpu_put = pkvm_vcpu_put,
};

bool pkvm_interrupt_blocked(struct kvm_vcpu *vcpu)
{
	/* TODO: Check the interrupt state with the pKVM hypervisor. */
	return false;
}
