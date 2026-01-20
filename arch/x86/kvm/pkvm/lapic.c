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
	lapic->ready = true;
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
