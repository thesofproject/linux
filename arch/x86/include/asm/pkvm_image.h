/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ASM_X86_PKVM_IMAGE_H
#define _ASM_X86_PKVM_IMAGE_H

#if defined(__PKVM_HYP__)
/*
 * For the pKVM hypervisor code to use the pKVM hypervisor symbols.
 * There is no need to manually add the suffix as the Makefile will
 * automatically do this for symbols in the pKVM hypervisor
 * text.
 */
#define PKVM_DECLARE(type, f, params)	type f params
#define pkvm_sym(sym)			sym
#else
/*
 * For the host code to use the pKVM hypervisor symbols which has
 * the __pkvm suffix added by the Makefile. It is necessary to manually
 * add the suffix as the Makefile will not do this for symbols in the
 * host text.
 */
#define PKVM_DECLARE(type, f, params)	type f##__pkvm params
#define pkvm_sym(sym)			sym##__pkvm
#endif

#ifdef LINKER_SCRIPT

#define PKVM_SECTION_NAME(NAME)	.pkvm##NAME

#define BEGIN_PKVM_SECTION(NAME)			\
	PKVM_SECTION_NAME(NAME) : {

#define END_PKVM_SECTION				\
	}

#define PKVM_SECTION(NAME)			\
	BEGIN_PKVM_SECTION(NAME)		\
		*(NAME NAME##.*)		\
	END_PKVM_SECTION

#endif /* LINKER_SCRIPT */

#endif /* _ASM_X86_PKVM_IMAGE_H */
