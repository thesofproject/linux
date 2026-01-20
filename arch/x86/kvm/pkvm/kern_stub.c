// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include "kern_stub.h"

void warn_thunk_thunk(void) {}

#ifndef CONFIG_PREEMPTION
int __cond_resched(void) {}
#endif

#ifdef CONFIG_DEBUG_ATOMIC_SLEEP
void __might_resched(const char *file, int line, unsigned int offsets) {}
#endif
