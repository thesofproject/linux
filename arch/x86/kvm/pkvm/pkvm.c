// SPDX-License-Identifier: GPL-2.0
#include <linux/types.h>
#include <asm/fpu/xcr.h>
#include <asm/pkvm_spinlock.h>
#include "debug.h"
#include "fpu.h"
#include "init.h"
#include "lapic.h"
#include "mem_protect.h"
#include "memory.h"
#include "pkvm.h"
#include "trace.h"
#include "../x86.h"
#include "../lapic.h"

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

/*
 * Represents the actual, extended kvm_vcpu structure size. It is initialized as
 * the size of struct kvm_vcpu. And if the vendor code extends kvm_vcpu instance
 * via embedding struct kvm_vcpu to its specific structure, this size should also
 * be extended by the vendor code.
 */
size_t kvm_vcpu_sz = sizeof(struct kvm_vcpu);

static int __pkvm_vcpu_free(struct pkvm_vm *pkvm_vm, int vcpu_handle,
			    struct pkvm_memcache *mc);

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

	pkvm_spin_lock_init(&pkvm_vm->lock);

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
	int i;

	if (!pkvm_vm)
		return;

	memset(mc, 0, sizeof(*mc));

	/*
	 * Normally all the created pkvm_vcpus should have been freed already
	 * by the vcpu_free PV interface. In case any pkvm_vcpu is still not
	 * freed, try to free it here.
	 */
	for (i = 0; i < pkvm_vm->kvm.created_vcpus; i++)
		__pkvm_vcpu_free(pkvm_vm, i, mc);

	shared_kvm_pa = __pkvm_pa(pkvm_vm->shared_kvm);

	kvm_x86_call(vm_destroy)(&pkvm_vm->kvm);

	teardown_donated_memory(mc, (void *)pkvm_vm, pkvm_vm->size);

	pkvm_host_unshare_hyp(shared_kvm_pa, kvm_x86_ops.vm_size);
}

static int attach_pkvm_vcpu_to_vm(struct pkvm_vm *pkvm_vm, struct pkvm_vcpu *pkvm_vcpu)
{
	struct kvm *kvm = &pkvm_vm->kvm;
	int vcpu_handle;

	pkvm_spin_lock(&pkvm_vm->lock);

	if (kvm->created_vcpus == KVM_MAX_VCPUS) {
		pkvm_spin_unlock(&pkvm_vm->lock);
		return -EINVAL;
	}
	vcpu_handle = kvm->created_vcpus++;
	pkvm_vcpu->vcpu.arch.pkvm.handle = vcpu_handle;
	pkvm_vcpu->pkvm_vm = pkvm_vm;
	pkvm_vm->vcpus[vcpu_handle] = pkvm_vcpu;

	pkvm_spin_unlock(&pkvm_vm->lock);

	atomic_set(&pkvm_vm->vcpu_refs[vcpu_handle], 1);

	return vcpu_handle;
}

static struct pkvm_vcpu *detach_pkvm_vcpu_from_vm(struct pkvm_vm *pkvm_vm, int vcpu_handle)
{
	int refcount = atomic_cmpxchg(&pkvm_vm->vcpu_refs[vcpu_handle], 1, 0);
	struct pkvm_vcpu *pkvm_vcpu;

	if (refcount > 1) {
		/* The pkvm_vcpu is in use and cannot be detached. */
		pkvm_err("VM%d vcpu%d is busy, refcount %d\n",
			 pkvm_vm->kvm.arch.pkvm.handle,
			 vcpu_handle, refcount);
		return NULL;
	} else if (refcount == 0) {
		/* No pkvm_vcpu is attached. */
		return NULL;
	}

	BUG_ON(refcount != 1);

	pkvm_spin_lock(&pkvm_vm->lock);

	pkvm_vcpu = pkvm_vm->vcpus[vcpu_handle];
	pkvm_vm->vcpus[vcpu_handle] = NULL;

	pkvm_spin_unlock(&pkvm_vm->lock);

	return pkvm_vcpu;
}

static int setup_vcpu_lapic(struct kvm_vcpu *vcpu)
{
	struct kvm_lapic *apic = vcpu->arch.apic, *shared_apic;
	size_t apic_size = sizeof(struct kvm_lapic);
	void *apic_regs = NULL;
	int ret;

	if (!apic)
		return 0;

	shared_apic = kern_pkvm_va(to_pkvm_vcpu(vcpu)->shared_vcpu->arch.apic);
	/*
	 * Temporary sharing host's apic structure to access its elements for
	 * setting up pKVM's apic structure. It will be unshared after that.
	 */
	ret = pkvm_host_share_hyp(__pkvm_pa(shared_apic), apic_size);
	if (ret)
		return ret;

	apic_regs = kern_pkvm_va(shared_apic->regs);
	if (!apic_regs) {
		ret = -EINVAL;
		goto unshare_apic;
	}

	ret = pkvm_host_share_hyp(__pkvm_pa(apic_regs), PAGE_SIZE);
	if (ret)
		goto unshare_apic;

	apic->regs = apic_regs;
	apic->apicv_active = shared_apic->apicv_active;
	apic->nr_lvt_entries = kvm_apic_calc_nr_lvt_entries(vcpu);
	apic->vcpu = vcpu;

unshare_apic:
	pkvm_host_unshare_hyp(__pkvm_pa(shared_apic), apic_size);
	return ret;
}

static void unsetup_vcpu_lapic(struct kvm_vcpu *vcpu)
{
	struct kvm_lapic *apic = vcpu->arch.apic;

	if (!apic)
		return;

	pkvm_host_unshare_hyp(__pkvm_pa(apic->regs), PAGE_SIZE);
}

static int share_vcpu_mce_banks(struct kvm_vcpu *vcpu)
{
	int ret;

	if (pkvm_is_protected_vcpu(vcpu))
		return -EINVAL;

	ret = pkvm_host_share_hyp(__pkvm_pa(vcpu->arch.mce_banks), KVM_MCE_SIZE);
	if (ret)
		return ret;

	ret = pkvm_host_share_hyp(__pkvm_pa(vcpu->arch.mci_ctl2_banks), KVM_MCI_CTL2_SIZE);
	if (ret)
		pkvm_host_unshare_hyp(__pkvm_pa(vcpu->arch.mce_banks), KVM_MCE_SIZE);

	return ret;
}

static void unshare_vcpu_mce_banks(struct kvm_vcpu *vcpu)
{
	if (pkvm_is_protected_vcpu(vcpu))
		return;

	pkvm_host_unshare_hyp(__pkvm_pa(vcpu->arch.mce_banks), KVM_MCE_SIZE);
	pkvm_host_unshare_hyp(__pkvm_pa(vcpu->arch.mci_ctl2_banks), KVM_MCI_CTL2_SIZE);
}

static int __vcpu_create(struct kvm *kvm, struct kvm_vcpu *vcpu, struct fpstate *fps)
{
	struct pkvm_vcpu *pkvm_vcpu = to_pkvm_vcpu(vcpu);
	int ret = kvm_x86_call(vcpu_precreate)(kvm);
	void *unused = (void *)pkvm_vcpu +
		       PKVM_VCPU_BASE_SIZE +
		       kvm_vcpu_sz;

	if (ret)
		return ret;

	vcpu->kvm = kvm;
	/* Set cpu to -1 to indicate it is not loaded on any CPU */
	vcpu->cpu = -1;

	vcpu->vcpu_id = pkvm_vcpu->shared_vcpu->vcpu_id;
	vcpu->arch.last_vmentry_cpu = -1;
	vcpu->arch.regs_avail = ~0;
	vcpu->arch.regs_dirty = ~0;
	vcpu->arch.pat = MSR_IA32_CR_PAT_DEFAULT;

	if (!pkvm_is_protected_vcpu(vcpu)) {
		vcpu->arch.mce_banks = kern_pkvm_va(pkvm_vcpu->shared_vcpu->arch.mce_banks);
		vcpu->arch.mci_ctl2_banks =
			kern_pkvm_va(pkvm_vcpu->shared_vcpu->arch.mci_ctl2_banks);
		ret = share_vcpu_mce_banks(vcpu);
		if (ret)
			return ret;
	} else {
		vcpu->arch.mce_banks = unused;
		unused += KVM_MCE_SIZE;
		vcpu->arch.mci_ctl2_banks = unused;
		unused += KVM_MCI_CTL2_SIZE;
	}
	vcpu->arch.mcg_cap = KVM_MAX_MCE_BANKS;

	vcpu->arch.apic_base = pkvm_vcpu->shared_vcpu->arch.apic_base;
	if (lapic_in_kernel(pkvm_vcpu->shared_vcpu))
		vcpu->arch.apic = unused;

	ret = setup_vcpu_lapic(vcpu);
	if (ret)
		goto unshare_mce;

	vcpu->arch.guest_fpu.fpstate = fps;
	pkvm_init_guest_fpu(&vcpu->arch.guest_fpu);
	if (pkvm_is_protected_vcpu(vcpu))
		fpstate_set_confidential(&vcpu->arch.guest_fpu);

	ret = kvm_x86_call(vcpu_create)(vcpu);
	if (ret)
		goto unsetup_lapic;

	return 0;

unsetup_lapic:
	unsetup_vcpu_lapic(vcpu);
unshare_mce:
	unshare_vcpu_mce_banks(vcpu);
	return ret;
}

static void __vcpu_free(struct kvm_vcpu *vcpu)
{
	kvm_x86_call(vcpu_free)(vcpu);

	unsetup_vcpu_lapic(vcpu);
	unshare_vcpu_mce_banks(vcpu);
}

static int pkvm_vcpu_create(int vm_handle, phys_addr_t host_vcpu_pa,
			    phys_addr_t pkvm_vcpu_pa, phys_addr_t fpu_pa)
{
	struct kvm_vcpu *shared_vcpu;
	struct pkvm_vcpu *pkvm_vcpu;
	size_t vcpu_size, fps_size;
	struct pkvm_vm *pkvm_vm;
	struct fpstate *fps;
	int ret;

	pkvm_vm = pkvm_get_vm(vm_handle);
	if (!pkvm_vm)
		return -EINVAL;

	ret = pkvm_host_share_hyp(host_vcpu_pa, kvm_vcpu_sz);
	if (ret)
		goto put_vm;

	shared_vcpu = __pkvm_va(host_vcpu_pa);
	vcpu_size = PKVM_VCPU_BASE_SIZE + kvm_vcpu_sz;
	if (pkvm_is_protected_vm(&pkvm_vm->kvm))
		vcpu_size += KVM_MCE_SIZE + KVM_MCI_CTL2_SIZE;
	if (lapic_in_kernel(shared_vcpu))
		vcpu_size += sizeof(struct kvm_lapic);
	vcpu_size = PAGE_ALIGN(vcpu_size);

	ret = pkvm_host_donate_hyp(pkvm_vcpu_pa, vcpu_size, true);
	if (ret)
		goto unshare_vcpu;

	pkvm_vcpu = __pkvm_va(pkvm_vcpu_pa);
	pkvm_vcpu->shared_vcpu = shared_vcpu;
	pkvm_vcpu->size = vcpu_size;

	fps_size = pkvm_guest_initial_fpstate_size(&pkvm_vm->kvm);
	ret = pkvm_host_donate_hyp(fpu_pa, fps_size, true);
	if (ret)
		goto undonate_vcpu;

	fps = __pkvm_va(fpu_pa);
	fps->size = fps_size;

	ret = __vcpu_create(&pkvm_vm->kvm, &pkvm_vcpu->vcpu, fps);
	if (ret)
		goto undonate_fps;

	ret = attach_pkvm_vcpu_to_vm(pkvm_vm, pkvm_vcpu);
	if (ret < 0)
		goto destroy_vcpu;

	pkvm_put_vm(pkvm_vm);

	return pkvm_vcpu->vcpu.arch.pkvm.handle;

destroy_vcpu:
	__vcpu_free(&pkvm_vcpu->vcpu);
undonate_fps:
	pkvm_hyp_donate_host(__pkvm_pa(fps), fps_size, false);
undonate_vcpu:
	pkvm_hyp_donate_host(__pkvm_pa(pkvm_vcpu), vcpu_size, false);
unshare_vcpu:
	pkvm_host_unshare_hyp(host_vcpu_pa, kvm_vcpu_sz);
put_vm:
	pkvm_put_vm(pkvm_vm);
	return ret;
}

static int __pkvm_vcpu_free(struct pkvm_vm *pkvm_vm, int vcpu_handle,
			    struct pkvm_memcache *mc)
{
	struct pkvm_vcpu *pkvm_vcpu = detach_pkvm_vcpu_from_vm(pkvm_vm, vcpu_handle);
	unsigned long shared_vcpu_pa;
	struct fpstate *fps;

	if (!pkvm_vcpu)
		return -EINVAL;

	shared_vcpu_pa = __pkvm_pa(pkvm_vcpu->shared_vcpu);

	__vcpu_free(&pkvm_vcpu->vcpu);

	fps = pkvm_vcpu->vcpu.arch.guest_fpu.fpstate;
	teardown_donated_memory(mc, fps, fps->size);
	teardown_donated_memory(mc, pkvm_vcpu, pkvm_vcpu->size);

	pkvm_host_unshare_hyp(shared_vcpu_pa, kvm_vcpu_sz);

	return 0;
}

static int pkvm_vcpu_free(int vm_handle, int vcpu_handle, struct pkvm_memcache *mc)
{
	struct pkvm_vm *pkvm_vm;
	int ret;

	if (vcpu_handle < 0 || vcpu_handle >= KVM_MAX_VCPUS)
		return -EINVAL;

	pkvm_vm = pkvm_get_vm(vm_handle);
	if (!pkvm_vm)
		return -EINVAL;

	memset(mc, 0, sizeof(*mc));

	ret = __pkvm_vcpu_free(pkvm_vm, array_index_nospec(vcpu_handle, KVM_MAX_VCPUS), mc);

	pkvm_put_vm(pkvm_vm);
	return ret;
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
	case __pkvm__vcpu_create:
		ret = pkvm_vcpu_create(pkvm_hc_input1(vcpu),
				       pkvm_host_gpa_to_phys(pkvm_hc_input2(vcpu)),
				       pkvm_host_gpa_to_phys(pkvm_hc_input3(vcpu)),
				       pkvm_host_gpa_to_phys(pkvm_hc_input4(vcpu)));
		break;
	case __pkvm__vcpu_free:
		ret = pkvm_vcpu_free(pkvm_hc_input1(vcpu), pkvm_hc_input2(vcpu),
				     &out.vcpu_free.memcache);
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

struct pkvm_vm *pkvm_get_vm(int vm_handle)
{
	struct pkvm_vm_ref *pkvm_vm_ref;
	int idx = vm_handle;

	if (idx < 0 || idx >= MAX_PKVM_VMS)
		return NULL;

	idx = array_index_nospec(idx, MAX_PKVM_VMS);
	pkvm_vm_ref = &pkvm_vms_ref[idx];

	return atomic_inc_not_zero(&pkvm_vm_ref->refcount) ? pkvm_vm_ref->pkvm_vm : NULL;
}

void pkvm_put_vm(struct pkvm_vm *pkvm_vm)
{
	int idx = pkvm_vm->kvm.arch.pkvm.handle;
	struct pkvm_vm_ref *pkvm_vm_ref;

	if (idx < 0 || idx >= MAX_PKVM_VMS)
		return;

	pkvm_vm_ref = &pkvm_vms_ref[idx];

	WARN_ON(atomic_dec_if_positive(&pkvm_vm_ref->refcount) <= 0);
}

struct pkvm_vcpu *pkvm_get_vcpu(int vm_handle, int vcpu_handle)
{
	struct pkvm_vm *pkvm_vm;

	if (vcpu_handle < 0 || vcpu_handle >= KVM_MAX_VCPUS)
		return NULL;

	pkvm_vm = pkvm_get_vm(vm_handle);
	if (!pkvm_vm)
		return NULL;

	vcpu_handle = array_index_nospec(vcpu_handle, KVM_MAX_VCPUS);
	if (atomic_inc_not_zero(&pkvm_vm->vcpu_refs[vcpu_handle]))
		return pkvm_vm->vcpus[vcpu_handle];

	pkvm_put_vm(pkvm_vm);
	return NULL;
}

void pkvm_put_vcpu(struct pkvm_vcpu *pkvm_vcpu)
{
	int vcpu_handle = pkvm_vcpu->vcpu.arch.pkvm.handle;

	WARN_ON(atomic_dec_if_positive(&pkvm_vcpu->pkvm_vm->vcpu_refs[vcpu_handle]) <= 0);

	pkvm_put_vm(pkvm_vcpu->pkvm_vm);
}
