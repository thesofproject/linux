// SPDX-License-Identifier: GPL-2.0
#include <linux/align.h>
#include <linux/array_size.h>
#include <asm/kvm_pkvm.h>
#include <asm/page.h>
#include <asm/percpu.h>
#include <asm/processor.h>
#include <asm/sections.h>
#include "memory.h"

unsigned long __per_cpu_offset[NR_CPUS];
DEFINE_PER_CPU_CACHE_HOT(unsigned long, this_cpu_off);
DEFINE_PER_CPU_CACHE_HOT(int, cpu_number);

unsigned int pkvm_per_cpu_nr_pages(void)
{
	unsigned long per_cpu_size = (unsigned long)__per_cpu_end -
				     (unsigned long)__per_cpu_start;

	return ALIGN(per_cpu_size, PAGE_SIZE) >> PAGE_SHIFT;
}

int pkvm_setup_per_cpu(int cpu, unsigned long base)
{
	if (cpu >= ARRAY_SIZE(__per_cpu_offset))
		return -EINVAL;

	__per_cpu_offset[cpu] = (unsigned long)__pkvm_va(base) -
				(unsigned long)__per_cpu_start;
	per_cpu(this_cpu_off, cpu) = __per_cpu_offset[cpu];
	per_cpu(cpu_number, cpu) = cpu;

	return 0;
}

unsigned long pkvm_per_cpu_offset(int cpu)
{
	if (cpu < 0 || cpu >= ARRAY_SIZE(__per_cpu_offset))
		return 0;

	return __per_cpu_offset[cpu];
}
