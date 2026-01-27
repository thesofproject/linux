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

/*
 * The kvm parameter can be NULL (module initialization, or invocation before
 * VM creation). Be sure to check the kvm parameter before using it.
 */
static bool pkvm_has_emulated_msr(struct kvm *kvm, u32 index)
{
	/* SMM mode is not supported by the pKVM hypervisor. */
	if (index == MSR_IA32_SMBASE)
		return false;

	if (!kvm)
		return vmx_has_emulated_msr(NULL, index);

	return pkvm_host_has_emulated_msr(kvm, index);
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

static void pkvm_vcpu_reset(struct kvm_vcpu *vcpu, bool init_event)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);

	/*
	 * TODO: The vcpu_reset PV interface will be disallowed for the pVM
	 * once its INIT event is handled inside the pKVM hypervisor. So should
	 * check `pkvm_is_protected_vcpu(vcpu)` rather than
	 * `vcpu->arch.guest_state_protected` once it is ready. See comments for
	 * `__pkvm__vcpu_reset` in pkvm_vcpu_handle_host_hypercall.
	 */
	if (!vcpu->arch.guest_state_protected && init_event)
		KVM_BUG_ON(pkvm_hypercall(vcpu_reset), vcpu->kvm);

	/*
	 * The host is responsible for injecting interrupts to the guest. The
	 * pi_desc is the key structure for the host to inject interrupts via
	 * the posted interrupt mechanism. Its physical address is used for the
	 * POSTED_INTR_DESC_ADDR in the VMCS by the pKVM hypervisor. Initialize
	 * the pi_desc when reset vcpu.
	 */
	vmx->vt.pi_desc.nv = POSTED_INTR_VECTOR;
	__pi_set_sn(&vmx->vt.pi_desc);

	/*
	 * The guest CR0/CR4 are managed by the pKVM hypervisor. When the host
	 * reads the guest CR0/CR4, it should get the up-to-date value from the
	 * pKVM. So make all bits in the CR0/CR4 as owned by the guest to
	 * indicate no bit is owned by the host.
	 */
	vcpu->arch.cr0_guest_owned_bits = ~0;
	vcpu->arch.cr4_guest_owned_bits = ~0;
	vcpu->arch.cr4_guest_rsvd_bits = 0;

	kvm_set_cr8(vcpu, 0);

	if (pkvm_is_protected_vcpu(vcpu)) {
		/*
		 * Emulating xapic mode will require the host to decode MMIO
		 * instruction which is not supported if the guest is a pVM as
		 * the pVM's CPU and memory state will be isolated. To avoid
		 * using xapic mode for a pVM, enable x2apic mode by default so
		 * that pVM will use MSR instructions to access lapic, which
		 * doesn't require decoding.
		 */
		u64 data = APIC_DEFAULT_PHYS_BASE | LAPIC_MODE_X2APIC |
			   (kvm_vcpu_is_reset_bsp(vcpu) ? MSR_IA32_APICBASE_BSP : 0);

		guest_cpu_cap_set(vcpu, X86_FEATURE_X2APIC);
		kvm_apic_set_base(vcpu, data, true);
	}
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

static void pkvm_update_exception_bitmap(struct kvm_vcpu *vcpu)
{
	if (!pkvm_is_protected_vcpu(vcpu))
		KVM_BUG_ON(pkvm_hypercall(update_exception_bitmap), vcpu->kvm);
}

static int pkvm_get_feature_msr(u32 msr, u64 *data)
{
	switch (msr) {
	case KVM_FIRST_EMULATED_VMX_MSR ... KVM_LAST_EMULATED_VMX_MSR:
		return 1;
	default:
		return KVM_MSR_RET_UNSUPPORTED;
	}
}

static int pkvm_get_msr(struct kvm_vcpu *vcpu, struct msr_data *msr_info)
{
	if (pkvm_host_has_emulated_msr(vcpu->kvm, msr_info->index))
		return kvm_get_msr_common(vcpu, msr_info);

	if (!vcpu->arch.guest_state_protected) {
		union pkvm_hc_data out;
		int ret;

		ret = pkvm_hypercall_out(get_msr, &out, msr_info->index);
		if (!ret)
			msr_info->data = out.get_msr.data;

		return ret;
	}

	return -EPERM;
}

static int pkvm_set_msr(struct kvm_vcpu *vcpu, struct msr_data *msr_info)
{
	if (pkvm_host_has_emulated_msr(vcpu->kvm, msr_info->index))
		return kvm_set_msr_common(vcpu, msr_info);

	if (!vcpu->arch.guest_state_protected)
		return pkvm_hypercall(set_msr, msr_info->index, msr_info->data);

	return -EPERM;
}

static bool pkvm_is_valid_cr0(struct kvm_vcpu *vcpu, unsigned long cr0)
{
	return true;
}

static void pkvm_set_cr0(struct kvm_vcpu *vcpu, unsigned long cr0)
{
	/*
	 * Segment will updated by the pKVM hypervisor if the vCPU enters the
	 * long mode. Clears the segment cache unconditionally for below
	 * reasons:
	 * 1) the clearing is just one line of code which is simpler comparing
	 * with checking if the vCPU enters the long mode or not.
	 * 2) the overall overhead is smaller than checking if the vCPU enters
	 * the long mode or not. By clearing the segment cache unconditionally,
	 * the host will need to use the get_segment PV interface if the host
	 * wants to read the segment register after setting the CR0. So the
	 * additional overhead is by sending one more PV interface. But if check
	 * whether a vCPU will enter the long mode or not before clearing the
	 * segment cache, there is also one more PV interface overhead which is
	 * to send cache_reg PV interface to read CR0 PG bit first if the CR0 is
	 * not up-to-date. As it is unlikely that the host wants to read the
	 * segment register after setting the CR0 but likely the CR0 is not
	 * up-to-date before setting the CR0, it seems the overall overhead of
	 * clearing the segment cache unconditionally is smaller.
	 */
	vmx_segment_cache_clear(to_vmx(vcpu));

	if (!vcpu->arch.guest_state_protected)
		KVM_BUG_ON(pkvm_hypercall(set_cr0, cr0), vcpu->kvm);

	vcpu->arch.cr0 = cr0;
	kvm_register_mark_available(vcpu, VCPU_EXREG_CR0);
}

static bool pkvm_is_valid_cr4(struct kvm_vcpu *vcpu, unsigned long cr4)
{
	/* The pKVM doesn't support VMX feature. */
	return !(cr4 & X86_CR4_VMXE);
}

static void pkvm_set_cr4(struct kvm_vcpu *vcpu, unsigned long cr4)
{
	if (!vcpu->arch.guest_state_protected)
		KVM_BUG_ON(pkvm_hypercall(set_cr4, cr4), vcpu->kvm);

	vcpu->arch.cr4 = cr4;
	kvm_register_mark_available(vcpu, VCPU_EXREG_CR4);
}

static int pkvm_set_efer(struct kvm_vcpu *vcpu, u64 efer)
{
	int ret = -EINVAL;

	if (!vcpu->arch.guest_state_protected)
		ret = pkvm_hypercall(set_efer, efer);

	vcpu->arch.efer = efer;
	return ret;
}

static void pkvm_set_dr7(struct kvm_vcpu *vcpu, unsigned long val)
{
	if (!pkvm_is_protected_vcpu(vcpu))
		KVM_BUG_ON(pkvm_hypercall(set_dr7, val), vcpu->kvm);
}

static void pkvm_cache_reg(struct kvm_vcpu *vcpu, enum kvm_reg reg)
{
	union pkvm_hc_data out;

	if (pkvm_is_protected_vcpu(vcpu))
		return;

	if (KVM_BUG_ON(pkvm_hypercall_out(cache_reg, &out, reg), vcpu->kvm))
		return;

	kvm_register_mark_available(vcpu, reg);

	switch (reg) {
	case VCPU_REGS_RSP:
		vcpu->arch.regs[VCPU_REGS_RSP] = out.cache_reg.rsp;
		break;
	case VCPU_REGS_RIP:
		vcpu->arch.regs[VCPU_REGS_RIP] = out.cache_reg.rip;
		break;
	case VCPU_EXREG_PDPTR: {
		struct kvm_mmu *mmu = vcpu->arch.walk_mmu;

		mmu->pdptrs[0] = out.cache_reg.pdptrs[0];
		mmu->pdptrs[1] = out.cache_reg.pdptrs[1];
		mmu->pdptrs[2] = out.cache_reg.pdptrs[2];
		mmu->pdptrs[3] = out.cache_reg.pdptrs[3];
		break;
	}
	case VCPU_EXREG_CR0:
		vcpu->arch.cr0 = out.cache_reg.cr0;
		break;
	case VCPU_EXREG_CR3:
		vcpu->arch.cr3 = out.cache_reg.cr3;
		break;
	case VCPU_EXREG_CR4:
		vcpu->arch.cr4 = out.cache_reg.cr4;
		break;
	default:
		KVM_BUG_ON(1, vcpu->kvm);
		break;
	}
}

static unsigned long pkvm_get_rflags(struct kvm_vcpu *vcpu)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);

	if (!kvm_register_is_available(vcpu, VCPU_EXREG_RFLAGS)) {
		if (vcpu->arch.guest_state_protected) {
			vmx->rflags = 0;
		} else {
			union pkvm_hc_data out;

			if (KVM_BUG_ON(pkvm_hypercall_out(get_rflags, &out), vcpu->kvm))
				return 0;

			vmx->rflags = out.get_rflags.data;
		}
		kvm_register_mark_available(vcpu, VCPU_EXREG_RFLAGS);
	}

	return vmx->rflags;
}

static void pkvm_set_rflags(struct kvm_vcpu *vcpu, unsigned long rflags)
{
	to_vmx(vcpu)->rflags = rflags;
	if (!vcpu->arch.guest_state_protected)
		KVM_BUG_ON(pkvm_hypercall(set_rflags, rflags), vcpu->kvm);
	kvm_register_mark_available(vcpu, VCPU_EXREG_RFLAGS);
}

static bool pkvm_get_if_flag(struct kvm_vcpu *vcpu)
{
	return pkvm_get_rflags(vcpu) & X86_EFLAGS_IF;
}

struct kvm_x86_ops pkvm_host_vt_x86_ops __initdata = {
	.name = KBUILD_MODNAME,

	.check_processor_compatibility = pkvm_check_processor_compat,

	.enable_virtualization_cpu = pkvm_enable_virtualization_cpu,
	.disable_virtualization_cpu = pkvm_disable_virtualization_cpu,
	.emergency_disable_virtualization_cpu = pkvm_disable_virtualization_cpu,

	.has_emulated_msr = pkvm_has_emulated_msr,

	.vm_size = sizeof(struct kvm_vmx),
	.vm_init = pkvm_vm_init,
	.vm_destroy = pkvm_vm_destroy,

	.vcpu_precreate = vmx_vcpu_precreate,
	.vcpu_create = pkvm_vcpu_create,
	.vcpu_free = pkvm_vcpu_free,
	.vcpu_reset = pkvm_vcpu_reset,

	.vcpu_load = pkvm_vcpu_load,
	.vcpu_put = pkvm_vcpu_put,

	.update_exception_bitmap = pkvm_update_exception_bitmap,
	.get_feature_msr = pkvm_get_feature_msr,
	.get_msr = pkvm_get_msr,
	.set_msr = pkvm_set_msr,
	.is_valid_cr0 = pkvm_is_valid_cr0,
	.set_cr0 = pkvm_set_cr0,
	.is_valid_cr4 = pkvm_is_valid_cr4,
	.set_cr4 = pkvm_set_cr4,
	.set_efer = pkvm_set_efer,
	.set_dr7 = pkvm_set_dr7,
	.cache_reg = pkvm_cache_reg,
	.get_rflags = pkvm_get_rflags,
	.set_rflags = pkvm_set_rflags,
	.get_if_flag = pkvm_get_if_flag,
};

bool pkvm_interrupt_blocked(struct kvm_vcpu *vcpu)
{
	/* TODO: Check the interrupt state with the pKVM hypervisor. */
	return false;
}
