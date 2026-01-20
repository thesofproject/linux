// SPDX-License-Identifier: GPL-2.0
#include <linux/percpu-defs.h>
#include <asm/apic.h>
#include <asm/msr.h>
#include "debug.h"
#include "lapic.h"

#define LAPIC_MODE_X2APIC (X2APIC_ENABLE | XAPIC_ENABLE)

struct pkvm_lapic {
	bool ready;
	u32 apic_id;
};

static DEFINE_PER_CPU(struct pkvm_lapic, pkvm_lapic);

static int setup_lapic(struct pkvm_lapic *lapic, u64 apicbase)
{
	if ((apicbase & LAPIC_MODE_X2APIC) != LAPIC_MODE_X2APIC) {
		pkvm_err("Lapic is not in X2APIC mode.\n");
		return -EOPNOTSUPP;
	}

	lapic->apic_id = native_apic_msr_read(APIC_ID);
	/*
	 * Make sure ready is updated after the apic_id, and pairs with the
	 * smp_load_acquire() in the pkvm_lapic_send_init().
	 */
	smp_store_release(&lapic->ready, true);
	return 0;
}

int pkvm_lapic_init(void)
{
	struct pkvm_lapic *lapic = this_cpu_ptr(&pkvm_lapic);
	u64 apicbase;

	if (lapic->ready)
		return -EBUSY;

	rdmsrq(MSR_IA32_APICBASE, apicbase);

	return setup_lapic(lapic, apicbase);
}

void pkvm_lapic_send_init(int cpu)
{
	u32 icrlow = APIC_INT_ASSERT | APIC_DM_INIT;
	struct pkvm_lapic *local, *remote;

	/*
	 * Check the CPU number to make sure only sending the INIT to other CPU
	 * but not self.
	 */
	if (unlikely(cpu == raw_smp_processor_id()))
		return;

	local = this_cpu_ptr(&pkvm_lapic);
	remote = per_cpu_ptr(&pkvm_lapic, cpu);

	/*
	 * Pairs with the smp_store_release() in the setup_lapic().
	 * If remote lapic is not ready, it means the remote CPU is not
	 * initialized yet(by pkvm_init()). In this case, it is not necessary to
	 * send INIT to kick as this remote CPU will handle all the pending
	 * requests before being initialized.
	 */
	if (unlikely(!smp_load_acquire(&remote->ready)))
		return;

	/*
	 * If remote lapic is ready, it means the remote CPU has already been
	 * initialized and may run in non-root mode. It does need to be kicked
	 * in this case. To do so, the local lapic must be initialized. If it is
	 * not, it means a bug somewhere.
	 */
	BUG_ON(!local->ready);

	native_x2apic_icr_write(icrlow, remote->apic_id);
}
