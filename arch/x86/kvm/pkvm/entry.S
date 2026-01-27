/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/linkage.h>
#include <asm/asm.h>
#include <asm/cache.h>
#include <asm/nospec-branch.h>
#include <asm/segment.h>
#include <asm/trapnr.h>
#include <asm/unwind_hints.h>

.macro save_frame
	push %_ASM_DI
	push %_ASM_SI
	push %_ASM_DX
	push %_ASM_CX
	push %_ASM_AX
	push %r8
	push %r9
	push %r10
	push %r11
	push %_ASM_BX
	push %_ASM_BP
	push %r12
	push %r13
	push %r14
	push %r15
.endm

.macro restore_frame
	pop %r15
	pop %r14
	pop %r13
	pop %r12
	pop %_ASM_BP
	pop %_ASM_BX
	pop %r11
	pop %r10
	pop %r9
	pop %r8
	pop %_ASM_AX
	pop %_ASM_CX
	pop %_ASM_DX
	pop %_ASM_SI
	pop %_ASM_DI
.endm

.macro exception_entry name vector has_error_code
SYM_CODE_START(handle_exception_\name)
	UNWIND_HINT_UNDEFINED
	ENDBR

	.if \has_error_code == 0
		pushq   $-1
	.endif

	save_frame

	movq %rsp, %rdi
	movq $\vector, %rsi
	movq $\has_error_code, %rdx

	call handle_exception

	restore_frame

	/* Pop error code unconditionally as iretq won't do that */
	addq    $8, %rsp

	iretq
SYM_CODE_END(handle_exception_\name)
.endm

#define GEN(v, e)	exception_entry #v vector=v has_error_code=e
#include <asm/GEN-for-each-exc.h>
#undef GEN

.pushsection .pkvm.text, "ax"

.align L1_CACHE_BYTES, 0xcc
SYM_CODE_START_NOALIGN(x86_verw_sel)
	UNWIND_HINT_UNDEFINED
	ANNOTATE_NOENDBR
	.word __KERNEL_DS
.align L1_CACHE_BYTES, 0xcc
SYM_CODE_END(x86_verw_sel);

.popsection

/* Clobbers AX, CX, DX */
SYM_FUNC_START(write_ibpb)
	ANNOTATE_NOENDBR
	movl	$MSR_IA32_PRED_CMD, %ecx
	movl	_ASM_RIP(x86_pred_cmd), %eax
	xorl	%edx, %edx
	wrmsr

	/* Make sure IBPB clears return stack preductions too. */
	FILL_RETURN_BUFFER %rax, RSB_CLEAR_LOOPS, X86_BUG_IBPB_NO_RET
	RET
SYM_FUNC_END(write_ibpb)
