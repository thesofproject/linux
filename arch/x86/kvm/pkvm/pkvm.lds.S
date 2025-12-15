/* SPDX-License-Identifier: GPL-2.0 */
#include <asm/pkvm_image.h>
#include <asm-generic/vmlinux.lds.h>
#include <asm/cache.h>
#include <asm/page.h>

SECTIONS {
	PKVM_SECTION(.text)
	PKVM_SECTION(.noinstr.text)
	PKVM_SECTION(__ex_table)
	PKVM_SECTION(.rodata)
	PKVM_SECTION(.data)
	PKVM_SECTION(.bss)
}
