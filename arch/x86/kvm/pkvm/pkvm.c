// SPDX-License-Identifier: GPL-2.0
#include <linux/types.h>
#include <asm/fpu/xcr.h>
#include <asm/pkvm_spinlock.h>
#include "debug.h"
#include "init.h"
#include "lapic.h"
#include "mem_protect.h"
#include "memory.h"
#include "pkvm.h"
#include "trace.h"
#include "../x86.h"

/*
 * Needed by kvm_spurious_fault() which is a generic fault function for the
 * vendor operations, e.g., vmx ops or svm ops. The pKVM hypervisor doesn't
 * have the knowledge about the platform reboot or shutdown, so kvm_rebooting
 * is always false in the pKVM hypervisor.
 */
__visible bool kvm_rebooting;

/*
 * Needed by code sharing with the KVM. As the pKVM hypervisor requires to have
 * a second level page table to translate GPA to HPA, set tdp_enabled as true.
 */
bool tdp_enabled = true;

struct pkvm_hyp *pkvm_hyp;
DEFINE_PER_CPU(struct pkvm_pcpu *, phys_cpu);
DEFINE_PER_CPU(struct kvm_vcpu *, host_vcpu);

/* The maximum number of VMs under pkvm. */
#define MAX_PKVM_VMS		64

static DECLARE_BITMAP(pkvm_vms_bitmap, MAX_PKVM_VMS);
static DEFINE_PKVM_SPINLOCK(pkvm_vms_lock);
static struct pkvm_vm_ref {
	/* Reference counter to indicate if pkvm_vm is in use */
	atomic_t refcount;
	/* Point to pkvm_vm in pkvm */
	struct pkvm_vm *pkvm_vm;
} pkvm_vms_ref[MAX_PKVM_VMS];

static int pkvm_enable_virtualization_cpu(void)
{
	kvm_user_return_msr_cpu_online();

	return kvm_x86_call(enable_virtualization_cpu)();
}

static int allocate_pkvm_vm_handle(struct pkvm_vm *pkvm_vm)
{
	struct pkvm_vm_ref *pkvm_vm_ref;
	int idx;

	pkvm_spin_lock(&pkvm_vms_lock);

	idx = find_first_zero_bit(pkvm_vms_bitmap, MAX_PKVM_VMS);
	if (idx == MAX_PKVM_VMS) {
		pkvm_spin_unlock(&pkvm_vms_lock);
		return -ENOMEM;
	}
	__set_bit(idx, pkvm_vms_bitmap);

	pkvm_vm_ref = &pkvm_vms_ref[idx];
	pkvm_vm_ref->pkvm_vm = pkvm_vm;
	atomic_set(&pkvm_vm_ref->refcount, 1);

	pkvm_spin_unlock(&pkvm_vms_lock);

	return idx;
}

static struct pkvm_vm *free_pkvm_vm_handle(int handle)
{
	struct pkvm_vm_ref *pkvm_vm_ref;
	struct pkvm_vm *pkvm_vm = NULL;
	int idx = handle;

	if (idx < 0 || idx >= MAX_PKVM_VMS)
		return NULL;

	pkvm_spin_lock(&pkvm_vms_lock);

	idx = array_index_nospec(idx, MAX_PKVM_VMS);
	pkvm_vm_ref = &pkvm_vms_ref[idx];
	if (atomic_cmpxchg(&pkvm_vm_ref->refcount, 1, 0) != 1) {
		pkvm_err("VM%d is busy, refcount %d\n", handle,
			 atomic_read(&pkvm_vm_ref->refcount));
		goto out;
	}

	pkvm_vm = pkvm_vm_ref->pkvm_vm;
	pkvm_vm_ref->pkvm_vm = NULL;

	__clear_bit(idx, pkvm_vms_bitmap);
out:
	pkvm_spin_unlock(&pkvm_vms_lock);
	return pkvm_vm;
}

static int pkvm_vm_init(phys_addr_t host_kvm_pa, phys_addr_t pkvm_vm_pa)
{
	struct pkvm_vm *pkvm_vm;
	struct kvm *kvm;
	size_t size;
	u8 vm_type;
	int ret;

	ret = pkvm_host_share_hyp(host_kvm_pa, kvm_x86_ops.vm_size);
	if (ret)
		return ret;

	size = PAGE_ALIGN(PKVM_VM_BASE_SIZE + kvm_x86_ops.vm_size);
	ret = pkvm_host_donate_hyp(pkvm_vm_pa, size, true);
	if (ret)
		goto unshare;

	pkvm_vm = __pkvm_va(pkvm_vm_pa);
	pkvm_vm->size = size;
	pkvm_vm->shared_kvm = __pkvm_va(host_kvm_pa);
	kvm = &pkvm_vm->kvm;

	vm_type = pkvm_vm->shared_kvm->arch.vm_type;
	if (!kvm_is_vm_type_supported(vm_type)) {
		ret = -EOPNOTSUPP;
		goto undonate;
	}

	kvm->arch.vm_type = vm_type;
	if (pkvm_is_protected_vm(kvm))
		kvm->arch.disabled_quirks = kvm_caps.inapplicable_quirks &
					    kvm_caps.supported_quirks;
	else
		kvm->arch.disabled_quirks = (kvm_caps.inapplicable_quirks |
					     pkvm_vm->shared_kvm->arch.disabled_quirks) &
					    kvm_caps.supported_quirks;

	ret = allocate_pkvm_vm_handle(pkvm_vm);
	if (ret < 0)
		goto undonate;

	kvm->arch.pkvm.handle = ret;

	ret = kvm_x86_call(vm_init)(kvm);
	if (ret)
		goto free_handle;

	return kvm->arch.pkvm.handle;

free_handle:
	free_pkvm_vm_handle(kvm->arch.pkvm.handle);
undonate:
	pkvm_hyp_donate_host(__pkvm_pa(pkvm_vm), size, false);
unshare:
	pkvm_host_unshare_hyp(host_kvm_pa, kvm_x86_ops.vm_size);
	return ret;
}

static void teardown_donated_memory(struct pkvm_memcache *mc, void *addr, size_t size)
{
	BUG_ON(!PAGE_ALIGNED(addr) || !PAGE_ALIGNED(size));

	pkvm_clear_memory(addr, size);

	push_pkvm_memcache(mc, addr, size, pkvm_virt_to_host_gpa);

	/*
	 * Sensitive data in this memory range has been already cleared
	 * by pkvm_clear_memory(). Now this memory is used to store the
	 * information about the memory pages for the host to free by
	 * push_pkvm_memcache(), so undonate without clearing.
	 */
	pkvm_hyp_donate_host(__pkvm_pa(addr), size, false);
}

static void pkvm_vm_destroy(int vm_handle, struct pkvm_memcache *mc)
{
	struct pkvm_vm *pkvm_vm = free_pkvm_vm_handle(vm_handle);
	unsigned long shared_kvm_pa;

	if (!pkvm_vm)
		return;

	memset(mc, 0, sizeof(*mc));

	shared_kvm_pa = __pkvm_pa(pkvm_vm->shared_kvm);

	kvm_x86_call(vm_destroy)(&pkvm_vm->kvm);

	teardown_donated_memory(mc, (void *)pkvm_vm, pkvm_vm->size);

	pkvm_host_unshare_hyp(shared_kvm_pa, kvm_x86_ops.vm_size);
}

void pkvm_handle_host_hypercall(struct kvm_vcpu *vcpu)
{
	enum pkvm_hc hc = pkvm_hc(vcpu);
	union pkvm_hc_data out;
	int ret = 0;

	switch (hc) {
	case __pkvm__init:
		ret = pkvm_init((struct pkvm_mem_info *)pkvm_hc_input1(vcpu),
				pkvm_hc_input2(vcpu));
		break;
	case __pkvm__init_finalize:
		ret = pkvm_init_finalize();
		break;
	case __pkvm__reprivilege_cpu:
		ret = pkvm_reprivilege_vcpu(vcpu);
		break;
	case __pkvm__enable_vmexit_trace:
		pkvm_enable_vmexit_trace(pkvm_hc_input1(vcpu));
		break;
	case __pkvm__dump_vmexit_trace:
		ret = pkvm_dump_vmexit_trace(pkvm_host_gpa_to_phys(pkvm_hc_input1(vcpu)),
					     pkvm_hc_input2(vcpu));
		break;
	case __pkvm__check_processor_compatibility:
		ret = kvm_x86_call(check_processor_compatibility)();
		break;
	case __pkvm__enable_virtualization_cpu:
		ret = pkvm_enable_virtualization_cpu();
		break;
	case __pkvm__vm_init:
		ret = pkvm_vm_init(pkvm_host_gpa_to_phys(pkvm_hc_input1(vcpu)),
				   pkvm_host_gpa_to_phys(pkvm_hc_input2(vcpu)));
		break;
	case __pkvm__vm_destroy:
		pkvm_vm_destroy(pkvm_hc_input1(vcpu), &out.vm_destroy.memcache);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pkvm_hc_set_output(vcpu, hc, &out);

	pkvm_hc_set_ret(vcpu, ret);
}

void pkvm_kick_vcpu(struct kvm_vcpu *vcpu)
{
	/* No need to kick if a vcpu is already out of guest mode */
	if (kvm_vcpu_exiting_guest_mode(vcpu) != IN_GUEST_MODE)
		return;

	pkvm_lapic_send_init(READ_ONCE(vcpu->cpu));
}

int pkvm_x86_vendor_init(struct kvm_x86_init_ops *ops)
{
	int r;

	memset(&kvm_caps, 0, sizeof(kvm_caps));

	kvm_caps.supported_vm_types = BIT(KVM_X86_DEFAULT_VM) |
				      BIT(KVM_X86_PKVM_PROTECTED_VM);
	if (IS_ENABLED(CONFIG_KVM_SW_PROTECTED_VM))
		kvm_caps.supported_vm_types |= BIT(KVM_X86_SW_PROTECTED_VM);
	kvm_caps.supported_mce_cap = MCG_CTL_P | MCG_SER_P;

	if (boot_cpu_has(X86_FEATURE_XSAVE)) {
		kvm_host.xcr0 = xgetbv(XCR_XFEATURE_ENABLED_MASK);
		kvm_caps.supported_xcr0 = kvm_host.xcr0 & KVM_SUPPORTED_XCR0;
	}

	if (boot_cpu_has(X86_FEATURE_XSAVES)) {
		rdmsrq(MSR_IA32_XSS, kvm_host.xss);
		kvm_caps.supported_xss = kvm_host.xss & KVM_SUPPORTED_XSS;
	}

	kvm_caps.supported_quirks = KVM_X86_VALID_QUIRKS;
	kvm_caps.inapplicable_quirks = KVM_X86_CONDITIONAL_QUIRKS;

	rdmsrq_safe(MSR_EFER, &kvm_host.efer);

	if (boot_cpu_has(X86_FEATURE_ARCH_CAPABILITIES))
		rdmsrq(MSR_IA32_ARCH_CAPABILITIES, kvm_host.arch_capabilities);

	r = ops->hardware_setup();
	if (r)
		return r;

	memcpy(&kvm_x86_ops, ops->runtime_ops, sizeof(kvm_x86_ops));

	if (!kvm_cpu_cap_has(X86_FEATURE_XSAVES))
		kvm_caps.supported_xss = 0;

	if (!kvm_cpu_cap_has(X86_FEATURE_SHSTK) &&
	    !kvm_cpu_cap_has(X86_FEATURE_IBT))
		kvm_caps.supported_xss &= ~XFEATURE_MASK_CET_ALL;

	if ((kvm_caps.supported_xss & XFEATURE_MASK_CET_ALL) != XFEATURE_MASK_CET_ALL) {
		kvm_cpu_cap_clear(X86_FEATURE_SHSTK);
		kvm_cpu_cap_clear(X86_FEATURE_IBT);
		kvm_caps.supported_xss &= ~XFEATURE_MASK_CET_ALL;
	}

	return 0;
}
