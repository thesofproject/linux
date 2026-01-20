// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) "pkvm: " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/extable.h>
#include <asm/pkvm_image.h>
#include "pkvm_constants.h"
#include "vmx.h"

static int __init early_pkvm_parse_cmdline(char *buf)
{
	return kstrtobool(buf, &enable_pkvm);
}
early_param("kvm-intel.pkvm", early_pkvm_parse_cmdline);

static DEFINE_PER_CPU(struct vmcs *, pkvm_vmxarea);
static unsigned long data_pages;

struct pkvm_deprivilege_param {
	struct pkvm_hyp *pkvm;
	int ret;
};

/* Only need GDT entries for KERNEL_CS & KERNEL_DS as pKVM only use these two */
static struct gdt_page pkvm_gdt_page = {
	.gdt = {
		[GDT_ENTRY_KERNEL_CS]		= GDT_ENTRY_INIT(0xa09b, 0, 0xfffff),
		[GDT_ENTRY_KERNEL_DS]		= GDT_ENTRY_INIT(0xc093, 0, 0xfffff),
	},
};

static unsigned int intercept_w_msrs[] = {
	MSR_CORE_PERF_GLOBAL_CTRL,
	MSR_IA32_APICBASE,
	X2APIC_MSR(APIC_ID),
};

u64 pkvm_total_reserve_pages(void)
{
	u64 total = pkvm_vmx_data_pages();

	total += pkvm_hyp_pgtable_pages();
	total += pkvm_host_pgtable_pages();
	total += pkvm_vmemmap_pages(PKVM_VMEMMAP_ENTRY_SIZE);

	return total;
}

static __init void pkvm_setup_syms(void)
{
	/*
	 * The pKVM hypervisor has defined the same symbol page_offset_base
	 * and phys_base with the linux kernel. Initialize with the same value
	 * used by the linux kernel before deprivilege. With this, the pkvm
	 * hypervisor code can use __va and __pa to translate between VA and PA.
	 */
	pkvm_sym(page_offset_base) = page_offset_base;
	pkvm_sym(phys_base) = phys_base;

	/*
	 * For the pKVM hypervisor to leverage the boot_cpu_has macro to check
	 * if a specific feature is supported or not.
	 */
	memcpy(&pkvm_sym(boot_cpu_data), &boot_cpu_data, sizeof(struct cpuinfo_x86));

#ifdef CONFIG_DYNAMIC_PHYSICAL_MASK
	/* For pKVM hypervisor to decode the valid physical address bits */
	pkvm_sym(physical_mask) = physical_mask;
#endif
	/* For the pKVM hypervisor to leverage pgprot_val macro */
	pkvm_sym(__default_kernel_pte_mask) = __default_kernel_pte_mask;
#ifdef CONFIG_AMD_MEM_ENCRYPT
	pkvm_sym(sme_me_mask) = sme_me_mask;
#endif
	cpumask_copy(&pkvm_sym(__cpu_possible_mask), cpu_possible_mask);
	pkvm_sym(nr_cpu_ids) = nr_cpu_ids;
}

static __init int pkvm_setup_host_vmcs_config(void)
{
	struct vmcs_config *vmcs_config = &pkvm_sym(host_vmcs_config);
	struct vmx_capability *vmx_cap = &pkvm_sym(vmx_capability);
	struct vmcs_config_setting setting = {
		.cpu_based_vm_exec_ctrl_req =
			CPU_BASED_INTR_WINDOW_EXITING |
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
		.pin_based_vm_exec_ctrl_req =
			PIN_BASED_VMX_PREEMPTION_TIMER,
		.pin_based_vm_exec_ctrl_opt = 0,
		.vmexit_ctrl_req =
			VM_EXIT_HOST_ADDR_SPACE_SIZE |
			VM_EXIT_LOAD_IA32_PAT |
			VM_EXIT_LOAD_IA32_EFER |
			VM_EXIT_SAVE_IA32_PAT |
			VM_EXIT_SAVE_IA32_EFER |
			VM_EXIT_SAVE_DEBUG_CONTROLS |
			VM_EXIT_LOAD_IA32_PERF_GLOBAL_CTRL,
		.vmexit_ctrl_opt = 0,
		.vmentry_ctrl_req =
			VM_ENTRY_LOAD_DEBUG_CONTROLS |
			VM_ENTRY_IA32E_MODE |
			VM_ENTRY_LOAD_IA32_EFER |
			VM_ENTRY_LOAD_IA32_PAT |
			VM_ENTRY_LOAD_IA32_PERF_GLOBAL_CTRL,
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
	struct kvm_vmx *kvmx = pkvm_sym(pkvm_early_alloc_contig)(PKVM_HOST_KVM_VMX_PAGES);

	if (!kvmx) {
		pr_err("no kvm_vmx memory\n");
		return -ENOMEM;
	}

	/*
	 * Only a few fields in the kvm structure will be used, e.g.,
	 * hlt_in_guest for exception injection code to clear hlt state.
	 * As HLT instruction will be passthrough to the host VM, set
	 * hlt_in_guest as true. As the mwait/pause/cstate will also be
	 * passthrough, initialized them as well to reflect the fact.
	 */
	kvm_disable_exits(&kvmx->kvm, KVM_X86_DISABLE_EXITS_MWAIT |
				      KVM_X86_DISABLE_EXITS_HLT   |
				      KVM_X86_DISABLE_EXITS_PAUSE |
				      KVM_X86_DISABLE_EXITS_CSTATE);
	pkvm->host_kvm = &kvmx->kvm;

	return 0;
}

static struct vmcs *pkvm_alloc_vmcs(void)
{
	struct vmcs *vmcs = pkvm_sym(pkvm_early_alloc_page)();

	if (!vmcs)
		return NULL;

	if (!PAGE_ALIGNED(__pa(vmcs)))
		return NULL;

	vmcs->hdr.revision_id = vmx_basic_vmcs_revision_id(pkvm_sym(host_vmcs_config).basic);

	return vmcs;
}

static __init int pkvm_alloc_vmxarea(int cpu)
{
	struct vmcs *vmcs = pkvm_alloc_vmcs();

	if (!vmcs)
		return -ENOMEM;

	per_cpu(pkvm_vmxarea, cpu) = vmcs;
	return 0;
}

static __init void init_gdt(struct pkvm_pcpu *pcpu)
{
	pcpu->gdt_page = pkvm_gdt_page;
}

static __init void init_idt(struct pkvm_pcpu *pcpu)
{
	void (*pkvm_exception_handlers[X86_TRAP_IRET])(void) = {
#define GEN(x, ...)	\
		[x] = pkvm_sym(handle_exception_##x),
#include <asm/GEN-for-each-exc.h>
#undef GEN
	};
	gate_desc *idt = pcpu->idt_page.idt;
	struct idt_data d = {
		.segment = __KERNEL_CS,
		.bits.ist = 0,
		.bits.zero = 0,
		.bits.type = GATE_INTERRUPT,
		.bits.dpl = 0,
		.bits.p = 1,
	};
	gate_desc desc;
	int i;

	for (i = 0; i < X86_TRAP_IRET; i++) {
		d.vector = i;
		d.bits.ist = 0;
		d.addr = (const void *)pkvm_exception_handlers[i];
		idt_init_desc(&desc, &d);
		write_idt_entry(idt, i, &desc);
	}
}

static __init void init_tss(struct pkvm_pcpu *pcpu)
{
	struct desc_struct *d = pcpu->gdt_page.gdt;
	tss_desc tss;

	set_tssldt_descriptor(&tss, (unsigned long)&pcpu->tss, DESC_TSS,
			      __KERNEL_TSS_LIMIT);

	write_gdt_entry(d, GDT_ENTRY_TSS, &tss, DESC_TSS);
}

static __init int pkvm_setup_pcpu(struct pkvm_hyp *pkvm, int cpu)
{
	struct pkvm_pcpu *pcpu;
	int ret;

	if (cpu >= CONFIG_NR_CPUS) {
		pr_err("setup_pcpu: invalid CPU number %d\n", cpu);
		return -EINVAL;
	}

	pcpu = pkvm_sym(pkvm_early_alloc_contig)(PKVM_PCPU_PAGES);
	if (!pcpu) {
		pr_err("no pcpu memory for CPU%d\n", cpu);
		return -ENOMEM;
	}

	init_gdt(pcpu);
	init_idt(pcpu);
	init_tss(pcpu);

	pkvm->pcpus[cpu] = pcpu;

	ret = pkvm_alloc_vmxarea(cpu);
	if (ret)
		pr_err("alloc vmxarea for CPU%d failed with ret %d\n", cpu, ret);

	return 0;
}

static __init int pkvm_setup_host_vcpu(struct pkvm_hyp *pkvm, int cpu)
{
	struct kvm *kvm = pkvm->host_kvm;
	struct vcpu_vmx *vmx;

	if (cpu >= CONFIG_NR_CPUS) {
		pr_err("setup_host_vcpu: invalid CPU number %d\n", cpu);
		return -EINVAL;
	}

	vmx = pkvm_sym(pkvm_early_alloc_contig)(PKVM_HOST_VCPU_VMX_PAGES);
	if (!vmx) {
		pr_err("no host vcpu memory for CPU%d\n", cpu);
		return -ENOMEM;
	}

	vmx->vmcs01.vmcs = pkvm_alloc_vmcs();
	if (!vmx->vmcs01.vmcs) {
		pr_err("no vmcs page for CPU%d\n", cpu);
		return -ENOMEM;
	}

	vmx->vmcs01.msr_bitmap = pkvm_sym(pkvm_early_alloc_page)();
	if (!vmx->vmcs01.msr_bitmap) {
		pr_err("no msr_bitmap page for CPU%d\n", cpu);
		return -ENOMEM;
	}

	/* Set msr bitmap to intercept some MSR writing */
	for (int i = 0; i < ARRAY_SIZE(intercept_w_msrs); i++)
		vmx_set_msr_bitmap_write(vmx->vmcs01.msr_bitmap, intercept_w_msrs[i]);

	vmx->vcpu.cpu = cpu;
	vmx->vcpu.vcpu_id = kvm->created_vcpus;
	vmx->vcpu.kvm = kvm;
	kvm->created_vcpus++;
	pkvm->host_vcpus[cpu] = &vmx->vcpu;

	return 0;
}

static __init int pkvm_setup_per_cpu(struct pkvm_hyp *pkvm, int cpu)
{
#ifndef CONFIG_PKVM_X86_DEBUG
	unsigned int nr_pages;
	void *per_cpu_base;
#endif

	if (cpu >= CONFIG_NR_CPUS) {
		pr_err("setup_percpu: invalid CPU number %d\n", cpu);
		return -EINVAL;
	}

#ifndef CONFIG_PKVM_X86_DEBUG
	nr_pages = pkvm_sym(pkvm_per_cpu_nr_pages)();
	if (!nr_pages)
		return 0;

	per_cpu_base = pkvm_sym(pkvm_early_alloc_contig)(nr_pages);
	if (!per_cpu_base || pkvm_sym(pkvm_setup_per_cpu)(cpu, __pa(per_cpu_base))) {
		pr_err("no percpu page for CPU%d\n", cpu);
		return -ENOMEM;
	}
#else
	/*
	 * Overwrite the pkvm's percpu setup symbols with the host percpu value
	 * as the same percpu base will be used by the pKVM and the host in the
	 * debug build.
	 */
	if (pkvm_sym(pkvm_setup_per_cpu)(cpu, __pa(__per_cpu_offset[cpu]))) {
		pr_err("no percpu page for CPU%d\n", cpu);
		return -ENOMEM;
	}
#endif

	return 0;
}

static inline u32 get_ar(u16 sel)
{
	u32 access_rights;

	if (sel == 0) {
		access_rights = 0x10000;
	} else {
		asm ("lar %%ax, %%rax\n"
				: "=a"(access_rights) : "a"(sel));
		access_rights = access_rights >> 8;
		access_rights = access_rights & 0xf0ff;
	}

	return access_rights;
}

#define init_guestsegment(seg, SEG, base, limit)		\
	do  {							\
		u16 sel;					\
		u32 ar;						\
								\
		savesegment(seg, sel);				\
		ar = get_ar(sel);				\
		vmcs_write16(GUEST_##SEG##_SELECTOR, sel);	\
		vmcs_write32(GUEST_##SEG##_AR_BYTES, ar);	\
		vmcs_writel(GUEST_##SEG##_BASE, base);		\
		vmcs_write32(GUEST_##SEG##_LIMIT, limit);	\
	} while (0)

static __init void init_guest_state_area_from_native(struct vcpu_vmx *vmx)
{
	int cpu = smp_processor_id();
	struct desc_ptr dt;
	u64 msrq;
	u16 ldtr;

	/* Initialize CR registers */
	vmcs_writel(GUEST_CR0, read_cr0() & ~X86_CR0_TS);
	vmcs_writel(GUEST_CR3, __read_cr3());
	vmcs_writel(GUEST_CR4, __read_cr4());

	/* Initialize cs/ss/ds/es */
	init_guestsegment(cs, CS, 0x0, 0xffffffff);
	init_guestsegment(ss, SS, 0x0, 0xffffffff);
	init_guestsegment(ds, DS, 0x0, 0xffffffff);
	init_guestsegment(es, ES, 0x0, 0xffffffff);

	/* Initialize fs/gs */
	rdmsrq(MSR_FS_BASE, msrq);
	init_guestsegment(fs, FS, msrq, 0xffffffff);
	rdmsrq(MSR_GS_BASE, msrq);
	init_guestsegment(gs, GS, msrq, 0xffffffff);

	/* Initialize GDTR */
	native_store_gdt(&dt);
	vmcs_writel(GUEST_GDTR_BASE, dt.address);
	vmcs_write32(GUEST_GDTR_LIMIT, dt.size);

	/* Initialize TR */
	vmcs_write16(GUEST_TR_SELECTOR, GDT_ENTRY_TSS*8);
	vmcs_write32(GUEST_TR_AR_BYTES, get_ar(GDT_ENTRY_TSS*8));
	vmcs_writel(GUEST_TR_BASE, (unsigned long)&get_cpu_entry_area(cpu)->tss.x86_tss);
	vmcs_write32(GUEST_TR_LIMIT, __KERNEL_TSS_LIMIT);

	/* Initialize LDTR */
	store_ldt(ldtr);
	vmcs_write16(GUEST_LDTR_SELECTOR, ldtr);
	vmcs_write32(GUEST_LDTR_AR_BYTES, 0x10000);
	vmcs_writel(GUEST_LDTR_BASE, 0x0);
	vmcs_write32(GUEST_LDTR_LIMIT, 0xffffffff);

	/* Initialize IDTR */
	store_idt(&dt);
	vmcs_writel(GUEST_IDTR_BASE, dt.address);
	vmcs_write32(GUEST_IDTR_LIMIT, dt.size);

	/* Set MSRs */
	vmcs_write64(GUEST_IA32_DEBUGCTL, get_debugctlmsr());

	rdmsrq(MSR_IA32_SYSENTER_CS, msrq);
	vmcs_write32(GUEST_SYSENTER_CS, (u32)msrq);

	rdmsrq(MSR_IA32_SYSENTER_ESP, msrq);
	vmcs_writel(GUEST_SYSENTER_ESP, msrq);

	rdmsrq(MSR_IA32_SYSENTER_EIP, msrq);
	vmcs_writel(GUEST_SYSENTER_EIP, msrq);

	rdmsrq(MSR_EFER, msrq);
	vmcs_write64(GUEST_IA32_EFER, msrq);

	rdmsrq(MSR_IA32_CR_PAT, msrq);
	vmcs_write64(GUEST_IA32_PAT, msrq);

	if (!rdmsrq_safe(MSR_CORE_PERF_GLOBAL_CTRL, &msrq)) {
		struct kvm_pmu *pmu = vcpu_to_pmu(&vmx->vcpu);
		union cpuid10_eax eax = {
			.full = native_cpuid_eax(10),
		};

		pmu->version = eax.split.version_id;
		pmu->global_ctrl = msrq;
		vmcs_write64(GUEST_IA32_PERF_GLOBAL_CTRL, msrq);
	}
}

static __init void init_guest_state_area(struct vcpu_vmx *vmx)
{
	init_guest_state_area_from_native(vmx);

	/*Guest non register state*/
	vmcs_write32(GUEST_ACTIVITY_STATE, GUEST_ACTIVITY_ACTIVE);
	vmcs_write32(GUEST_INTERRUPTIBILITY_INFO, 0);
	vmcs_writel(GUEST_PENDING_DBG_EXCEPTIONS, 0);
	vmcs_write64(VMCS_LINK_POINTER, -1ull);
}

static __init void init_host_state_area(struct vcpu_vmx *vmx, struct pkvm_hyp *pkvm)
{
	int cpu = smp_processor_id();
	unsigned long host_rsp;
	struct pkvm_pcpu *pcpu;
#ifdef CONFIG_PKVM_X86_DEBUG
	struct desc_ptr dt;
	u16 selector;
#endif
	u64 msrq;

	vmcs_writel(HOST_CR0, read_cr0() & ~X86_CR0_TS);
	/* Use host cr3 until the pKVM hypervisor created its own MMU */
	vmcs_writel(HOST_CR3, __read_cr3());
	/*
	 * Disable FRED for the pKVM hypervisor if it is enabled by the host.
	 * There is no too much benifit for the pKVM hypervisor to use the FRED
	 * event delivery as the NMI is the only event expected to be received
	 * by the pKVM hypervisor. The exceptions are not expected to be
	 * happened in the pKVM hypervisor and all hardware interrupts will
	 * directly go to the host. Meanwhile, enabling the FRED in the pkvm
	 * hypervisor will result in additional FRED MSRs switching overhead. So
	 * keep the FRED being disabled in the pKVM hypervisor.
	 */
	vmcs_writel(HOST_CR4, __read_cr4() & ~X86_CR4_FRED);

#ifdef CONFIG_PKVM_X86_DEBUG
	savesegment(cs, selector);
	vmcs_write16(HOST_CS_SELECTOR, selector);
	savesegment(ss, selector);
	vmcs_write16(HOST_SS_SELECTOR, selector);
	savesegment(ds, selector);
	vmcs_write16(HOST_DS_SELECTOR, selector);
	savesegment(es, selector);
	vmcs_write16(HOST_ES_SELECTOR, selector);
	savesegment(fs, selector);
	vmcs_write16(HOST_FS_SELECTOR, selector);
	rdmsrq(MSR_FS_BASE, msrq);
	vmcs_writel(HOST_FS_BASE, msrq);
	savesegment(gs, selector);
	vmcs_write16(HOST_GS_SELECTOR, selector);
	rdmsrq(MSR_GS_BASE, msrq);
	vmcs_writel(HOST_GS_BASE, msrq);

	vmcs_write16(HOST_TR_SELECTOR, GDT_ENTRY_TSS*8);
	vmcs_writel(HOST_TR_BASE, (unsigned long)&get_cpu_entry_area(cpu)->tss.x86_tss);

	native_store_gdt(&dt);
	vmcs_writel(HOST_GDTR_BASE, dt.address);

	/*
	 * Use pKVM's exception handlers, to minimize differences from
	 * non-debug mode.
	 */
	pcpu = pkvm->pcpus[cpu];
	vmcs_writel(HOST_IDTR_BASE, (unsigned long)(&pcpu->idt_page));

	rdmsrq(MSR_IA32_SYSENTER_CS, msrq);
	vmcs_write32(HOST_IA32_SYSENTER_CS, (u32)msrq);

	rdmsrq(MSR_IA32_SYSENTER_ESP, msrq);
	vmcs_writel(HOST_IA32_SYSENTER_ESP, msrq);

	rdmsrq(MSR_IA32_SYSENTER_EIP, msrq);
	vmcs_writel(HOST_IA32_SYSENTER_EIP, msrq);
#else
	vmcs_write16(HOST_CS_SELECTOR, __KERNEL_CS);
	vmcs_write16(HOST_SS_SELECTOR, __KERNEL_DS);
	vmcs_write16(HOST_DS_SELECTOR, __KERNEL_DS);
	vmcs_write16(HOST_ES_SELECTOR, 0);
	vmcs_write16(HOST_TR_SELECTOR, GDT_ENTRY_TSS*8);
	vmcs_write16(HOST_FS_SELECTOR, 0);
	vmcs_write16(HOST_GS_SELECTOR, 0);
	vmcs_writel(HOST_FS_BASE, 0);
	vmcs_writel(HOST_GS_BASE, pkvm_sym(pkvm_per_cpu_offset)(cpu));

	pcpu = pkvm->pcpus[cpu];
	vmcs_writel(HOST_TR_BASE, (unsigned long)&pcpu->tss);
	vmcs_writel(HOST_GDTR_BASE, (unsigned long)(&pcpu->gdt_page));
	vmcs_writel(HOST_IDTR_BASE, (unsigned long)(&pcpu->idt_page));
#endif

	rdmsrq(MSR_EFER, msrq);
	vmcs_write64(HOST_IA32_EFER, msrq);

	rdmsrq(MSR_IA32_CR_PAT, msrq);
	vmcs_write64(HOST_IA32_PAT, msrq);

	vmcs_write64(HOST_IA32_PERF_GLOBAL_CTRL, 0);

	/*
	 * [pcpu->stack, pcpu->stack + PKVM_STACK_SIZE) is per cpu stack.
	 * It is used as stack when the pcpu enters pKVM, i.e. HOST stack from
	 * VMX point of view.
	 *
	 * Within the top of stack, a small region starting from stack_resv
	 * is reserved  to store private paremeters,
	 *
	 * ------------ Stack layout ----------
	 * stack_top:
	 * stack_resv + 8:	struct vcpu_vmx *vmx
	 * stack_resv + 0:	pointer to vcpu->arch.regs
	 * stack_resv:		(stack_top - PKVM_STACK_TOP_RESV) = VMCS.HOST_RSP for PCPU
	 *			.........
	 *			.........
	 * stack_bottom:
	 */
	host_rsp = get_host_stack_top(pcpu) - PKVM_STACK_TOP_RESV;

	vmcs_writel(HOST_RSP, host_rsp);
	*((struct vcpu_vmx **) (host_rsp + 8)) = vmx;
	*((unsigned long **) host_rsp) = vmx->vcpu.arch.regs;

	vmcs_writel(HOST_RIP, (unsigned long)pkvm_sym(pkvm_host_vmexit_entry));
}

static __init void init_execution_control(struct vcpu_vmx *vmx)
{
	/* Preemption timer is toggled dynamically */
	pin_controls_set(vmx, pkvm_sym(host_vmcs_config).pin_based_exec_ctrl &
			      ~PIN_BASED_VMX_PREEMPTION_TIMER);

	/*
	 * CR3 LOAD/STORE EXITING are always read as 1 from the
	 * MSR_IA32_VMX_PROCBASED_CTLS. Clear these two bits as the CR3 will be
	 * passthrough to the host VM.
	 * INTR WINDOW EXITING is toggled dynamically.
	 */
	exec_controls_set(vmx, pkvm_sym(host_vmcs_config).cpu_based_exec_ctrl &
			       ~(CPU_BASED_CR3_LOAD_EXITING |
				 CPU_BASED_CR3_STORE_EXITING |
				 CPU_BASED_INTR_WINDOW_EXITING));

	/* Disable EPT/VPID first, enable after EPT pgtable created */
	secondary_exec_controls_set(vmx, pkvm_sym(host_vmcs_config).cpu_based_2nd_exec_ctrl &
					 ~(SECONDARY_EXEC_ENABLE_EPT |
					   SECONDARY_EXEC_ENABLE_VPID));
	/*
	 * Shadow VMCS will not be used as the VMCS will be exposed via PV-based
	 * method.
	 */
	vmcs_write64(VMCS_LINK_POINTER, INVALID_GPA);

	/* Host VM owns cr3 */
	vmcs_write32(CR3_TARGET_COUNT, 0);

	/* Host VM handles exceptions directly */
	vmcs_write32(EXCEPTION_BITMAP, 0);

	vmcs_write64(MSR_BITMAP, __pa(vmx->vmcs01.msr_bitmap));

	/*
	 * Host VM owns cr0 and cr4 except VMXE bit.
	 * Does not care about IA32_VMX_CRx_FIXED0/1 setting, so if host VM
	 * modifies cr0/cr4 conflicting with FIXED0/1, just let #GP happen.
	 * For example, as pKVM does not enable unrestricted guest feature,
	 * cr0.PE/PG must keep as 1 in host VM.
	 */
	vmcs_writel(CR0_GUEST_HOST_MASK, 0);
	vmcs_writel(CR4_GUEST_HOST_MASK, X86_CR4_VMXE);

	/*
	 * Set the VMXE bit in CR4_READ_SHADOW so that the host VM will see the
	 * consistent values between "native" cr4 and its cached cpu_tlbstate.cr4
	 * (which is set when turns on VMX via kvm_cpu_vmxon).
	 */
	vmcs_writel(CR4_READ_SHADOW, X86_CR4_VMXE);
}

static __init void init_vmexit_control(struct vcpu_vmx *vmx)
{
	u32 vmexit_ctrl = pkvm_sym(host_vmcs_config).vmexit_ctrl;
	struct kvm_pmu *pmu = vcpu_to_pmu(&vmx->vcpu);

	/* No need to switch if PMU is not enabled */
	if (!pmu->global_ctrl)
		vmexit_ctrl &= ~VM_EXIT_LOAD_IA32_PERF_GLOBAL_CTRL;

	vm_exit_controls_set(vmx, vmexit_ctrl);
	vmcs_write32(VM_EXIT_MSR_STORE_COUNT, 0);
}

static __init void init_vmentry_control(struct vcpu_vmx *vmx)
{
	u32 vmentry_ctrl = pkvm_sym(host_vmcs_config).vmentry_ctrl;
	struct kvm_pmu *pmu = vcpu_to_pmu(&vmx->vcpu);

	/* No need to switch if PMU is not enabled */
	if (!pmu->global_ctrl)
		vmentry_ctrl &= ~VM_ENTRY_LOAD_IA32_PERF_GLOBAL_CTRL;

	vm_entry_controls_set(vmx, vmentry_ctrl);
	vmcs_write32(VM_ENTRY_INTR_INFO_FIELD, 0);
	vmcs_write32(VM_ENTRY_MSR_LOAD_COUNT, 0);
	vmcs_write32(VM_ENTRY_INTR_INFO_FIELD, 0);
}

static __init int pkvm_host_init_vmx(struct vcpu_vmx *vmx, struct pkvm_hyp *pkvm)
{
	vmx->loaded_vmcs = &vmx->vmcs01;
	vmcs_load(vmx->loaded_vmcs->vmcs);
	vmx->loaded_vmcs->cpu = smp_processor_id();

	init_guest_state_area(vmx);
	init_host_state_area(vmx, pkvm);
	init_execution_control(vmx);
	init_vmexit_control(vmx);
	init_vmentry_control(vmx);

	return 0;
}

static noinline int local_deprivilege_cpu(void)
{
	int ret;

	asm volatile(
		"pushfq\n"
		"popq %%rax\n"
		"movq %3, %%rdx\n"
		"vmwrite %%rax, %%rdx\n"
		"movq %%rsp, %%rax\n"
		"movq %4, %%rdx\n"
		"vmwrite %%rax, %%rdx\n"
		"movq $host_vm_entry_point, %%rax\n"
		"movq %1, %%rdx\n"
		"vmwrite %%rax, %%rdx\n"
		"movl $0, %0\n"
		"vmlaunch\n"
		/* vmlaunch failed */
		"movl %2, %0\n"
		/* successfully deprivileged */
		"host_vm_entry_point: nop\n"
		: "=m"(ret)
		: "i"(GUEST_RIP), "i"(-EINVAL), "i"(GUEST_RFLAGS), "i"(GUEST_RSP)
		: "rax", "rdx", "memory");

	return ret;
}

static DEFINE_PER_CPU(bool, deprivileged);
static __init void pkvm_host_reprivilege_cpu(void *data)
{
	unsigned long flags;
	int cpu = get_cpu();
	int ret;

	if (!this_cpu_read(deprivileged)) {
		put_cpu();
		return;
	}

	local_irq_save(flags);

	/*
	 * Load the RW GDT page for reprivilege code
	 * to reload TR.
	 */
	load_direct_gdt(cpu);

	/*
	 * Intel CET requires indirect jmp/call to return to
	 * endbr64 instruction. So we can't use kvm_hypercall
	 * here.
	 */
	asm volatile(
		"vmcall\n"
		"endbr64\n"
		: "=a"(ret)
		: "a"(__pkvm__reprivilege_cpu)
		: "memory");

	/* Switch back to RO GDT page */
	load_fixmap_gdt(cpu);

	if (!ret) {
		this_cpu_write(deprivileged, false);
		kvm_cpu_vmxoff();
		pr_info("%s: CPU%d back in host mode\n", __func__, cpu);
	} else {
		pr_warn("%s: CPU%d failed to reprivilege(err=%d)\n", __func__, cpu, ret);
	}

	local_irq_restore(flags);
	put_cpu();
}

static __init void pkvm_host_reprivilege_cpus(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		if (!per_cpu(deprivileged, cpu))
			continue;

		smp_call_function_single(cpu, pkvm_host_reprivilege_cpu,
					 NULL, true);
	}
}

static __init void pkvm_host_deprivilege_cpu(void *data)
{
	struct pkvm_deprivilege_param *p = data;
	int cpu = smp_processor_id(), ret;
	struct kvm_vcpu *vcpu;

	if (!p || !p->pkvm)
		return;

	vcpu = p->pkvm->host_vcpus[cpu];

	ret = kvm_cpu_vmxon(__pa(this_cpu_read(pkvm_vmxarea)));
	if (ret) {
		pr_err("CPU%d vmxon failed, ret %d\n", cpu, ret);
		goto done;
	}

	ret = pkvm_host_init_vmx(to_vmx(vcpu), p->pkvm);
	if (ret) {
		pr_err("CPU%d init vmx failed, ret %d\n", cpu, ret);
		goto vmxoff;
	}

	ret = local_deprivilege_cpu();
	if (ret) {
		pr_err("CPU%d deprivilege failed, ret %d\n", cpu, ret);
		goto vmxoff;
	}

	vcpu->mode = IN_GUEST_MODE;
	this_cpu_write(deprivileged, true);
	pr_info("CPU%d in guest mode\n", cpu);
	return;
vmxoff:
	kvm_cpu_vmxoff();
done:
	p->ret = ret;
}

/*
 * Used in root mode to deprivilege CPUs
 */
static __init int pkvm_host_deprivilege_cpus(struct pkvm_hyp *pkvm)
{
	struct pkvm_deprivilege_param p = {
		.pkvm = pkvm,
		.ret = 0,
	};
	int cpu, ret = 0;

	pkvm_sym(pkvm_vmx_register_excp_handlers)();

	/*
	 * The pKVM hypervisor's IDT will be programmed into VMCS before
	 * deprivileging the CPU. Once deprivileging is done and the CPU
	 * enters to the root mode, the pKVM's exception handlers should be
	 * functional. So before that, sort pKVM's exception table to make
	 * sure the exception fixup working as expected.
	 */
	if (&pkvm_sym(__stop___ex_table) > &pkvm_sym(__start___ex_table))
		sort_extable(pkvm_sym(__start___ex_table), pkvm_sym(__stop___ex_table));

	for_each_possible_cpu(cpu) {
		ret = smp_call_function_single(cpu, pkvm_host_deprivilege_cpu, &p, 1);
		if (ret || p.ret) {
			pr_err("Failed to deprivilege CPU%d: smp_call %d, deprivilege: %d\n",
			       cpu, ret, p.ret);
			break;
		}
	}

	return ret ? ret : p.ret;
}

static void do_pkvm_hyp_init(void *data)
{
	unsigned long data_size = data_pages << PAGE_SHIFT;
	struct pkvm_mem_info infos[] = {
		{
			.type	= PKVM_RESERVED_USED_MEMORY,
			.va	= (unsigned long)__va(pkvm_mem_base),
			.pa	= pkvm_mem_base,
			.size	= data_size,
			.prot	= pgprot_val(PAGE_KERNEL),
		},
		{
			.type	= PKVM_RESERVED_UNUSED_MEMORY,
			.va	= (unsigned long)__va(pkvm_mem_base + data_size),
			.pa	= pkvm_mem_base + data_size,
			.size	= pkvm_mem_size - data_size,
			.prot	= pgprot_val(PAGE_KERNEL),
		},
		{
			.type	= PKVM_TEXT_DATA,
			.va	= (unsigned long)pkvm_sym(text_start),
			.pa	= __pa_symbol(pkvm_sym(text_start)),
			.size	= pkvm_sym(text_end) - pkvm_sym(text_start),
			.prot	= pgprot_val(PAGE_KERNEL_EXEC),
		},
		{
			.type	= PKVM_TEXT_DATA,
			.va	= (unsigned long)pkvm_sym(rodata_start),
			.pa	= __pa_symbol(pkvm_sym(rodata_start)),
			.size	= pkvm_sym(rodata_end) - pkvm_sym(rodata_start),
			.prot	= pgprot_val(PAGE_KERNEL_RO),
		},
		{
			.type	= PKVM_TEXT_DATA,
			.va	= (unsigned long)pkvm_sym(data_start),
			.pa	= __pa_symbol(pkvm_sym(data_start)),
			.size	= pkvm_sym(data_end) - pkvm_sym(data_start),
			.prot	= pgprot_val(PAGE_KERNEL),
		},
		{
			.type	= PKVM_TEXT_DATA,
			.va	= (unsigned long)pkvm_sym(bss_start),
			.pa	= __pa_symbol(pkvm_sym(bss_start)),
			.size	= pkvm_sym(bss_end) - pkvm_sym(bss_start),
			.prot	= pgprot_val(PAGE_KERNEL),
		},
	};
	int ret = pkvm_hypercall(init, (unsigned long)infos, ARRAY_SIZE(infos));

	if (data)
		*(int *)data = ret;
}

static __init int pkvm_hyp_init(void)
{
	int ret, cpu, init_ret;

	for_each_possible_cpu(cpu) {
		ret = smp_call_function_single(cpu, do_pkvm_hyp_init,
					       &init_ret, 1);
		if (ret || init_ret) {
			pr_err("Failed to initialize pKVM hyp on CPU%d: smp_call %d, init: %d\n",
			       cpu, ret, init_ret);
			break;
		}
	}

	/*
	 * XXX: Revert
	 * Temporarily fail pkvm initialization until pVMCS is fully merged.
	 * pKVM doesn't serve any real purpose until we have pVMCS ready and
	 * this failure helps us test reprivilege logic. This also enables
	 * host to boot normally with KVM enabled and thereby not breaking
	 * any virtualization functionality.
	 */
	if (!ret || !init_ret) {
		pr_err("Explicitly triggering pkvm initialization failure!\n");
		ret = -EFAULT;
	}
	return ret ? ret : init_ret;
}

int __init vmx_pkvm_init(void)
{
	struct pkvm_hyp *pkvm;
	int ret, cpu;

	if (!enable_pkvm)
		return 0;

	if (!pkvm_mem_base) {
		pr_err("required memory not reserved\n");
		ret = -ENOMEM;
		goto out;
	}

	data_pages = pkvm_vmx_data_pages();
	pkvm_sym(pkvm_early_alloc_init)(__va(pkvm_mem_base), data_pages << PAGE_SHIFT);

	pkvm = pkvm_sym(pkvm_hyp) = pkvm_sym(pkvm_early_alloc_contig)(PKVM_HYP_PAGES);
	if (!pkvm) {
		pr_err("cannot alloc pkvm_hyp\n");
		ret = -ENOMEM;
		goto out;
	}

	pkvm_setup_syms();

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
		ret = pkvm_setup_per_cpu(pkvm, cpu);
		if (ret)
			goto out;
	}

	pkvm_sym(init_ops) = pkvm_sym(pkvm_vmx_init_ops);

	ret = pkvm_host_deprivilege_cpus(pkvm);
	if (ret)
		goto repriv_cpus;

	ret = pkvm_hyp_init();
	if (ret)
		goto repriv_cpus;

	pkvm_hypercall(init_finalize);

	pkvm_init_debugfs();

	pr_info("Hypervisor is up and running!\n");
	return 0;

repriv_cpus:
	pkvm_host_reprivilege_cpus();
out:
	/*
	 * As the reserved memory at the pkvm_mem_base will not be
	 * released back to the host, no need to de-initialize or
	 * free for the early_alloc.
	 */
	pkvm_sym(pkvm_hyp) = NULL;
	enable_pkvm = false;
	return ret;
}

MODULE_LICENSE("GPL");
