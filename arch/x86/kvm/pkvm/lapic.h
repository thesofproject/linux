/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PKVM_X86_LAPIC_H
#define __PKVM_X86_LAPIC_H

int pkvm_lapic_init(void);
void pkvm_lapic_send_init(int cpu);

#endif /* __PKVM_X86_LAPIC_H */
