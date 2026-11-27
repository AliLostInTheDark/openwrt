/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 *
 * Cortina UART console driver for TF-A MULTI_CONSOLE_API.
 */

#include <asm_macros.S>
#include <console_macros.S>

#define UTX_DATA		0x10
#define UINFO			0x18
#define  UINFO_TX_EMPTY		(1 << 3)
#define  UINFO_TX_FULL		(1 << 2)

	.globl console_taurus_register
	.globl console_taurus_core_init
	.globl console_taurus_core_putc
	.globl console_taurus_core_flush

/*
 * int console_taurus_core_init(uintptr_t base_addr, unsigned int clock,
 *                              unsigned int baud)
 * Assumes BL1/BL2 already configured UART0 at 115200 8N1.
 */
func console_taurus_core_init
	mov	w0, #1
	ret
endfunc console_taurus_core_init

/* int console_taurus_core_putc(int c, uintptr_t base_addr) */
func console_taurus_core_putc
	cbz	x1, putc_err
	cmp	w0, #'\n'
	bne	2f
1:
	ldr	w2, [x1, #UINFO]
	tbnz	w2, #2, 1b		/* wait while TX_FULL */
	mov	w2, #'\r'
	str	w2, [x1, #UTX_DATA]
2:
	ldr	w2, [x1, #UINFO]
	tbnz	w2, #2, 2b		/* wait while TX_FULL */
	str	w0, [x1, #UTX_DATA]
	ret
putc_err:
	mov	w0, #-1
	ret
endfunc console_taurus_core_putc

/* void console_taurus_core_flush(uintptr_t base_addr) */
func console_taurus_core_flush
	cbz	x0, flush_done
1:
	ldr	w1, [x0, #UINFO]
	tbz	w1, #3, 1b		/* wait until TX_EMPTY */
flush_done:
	mov	w0, #0
	ret
endfunc console_taurus_core_flush

	.globl console_taurus_putc
func console_taurus_putc
	ldr	x1, [x1, #CONSOLE_T_BASE]
	b	console_taurus_core_putc
endfunc console_taurus_putc

	.globl console_taurus_flush
func console_taurus_flush
	ldr	x0, [x0, #CONSOLE_T_BASE]
	b	console_taurus_core_flush
endfunc console_taurus_flush

/*
 * int console_taurus_register(uintptr_t base_addr, uint32_t clock,
 *                             uint32_t baud, console_t *console)
 */
func console_taurus_register
	stp	x4, x30, [sp, #-16]!
	mov	x4, x3
	str	x0, [x4, #CONSOLE_T_BASE]
	bl	console_taurus_core_init
	cbz	w0, register_fail
	mov	x0, x4
	ldp	x4, x30, [sp], #16
	finish_console_register taurus putc=1, flush=1
register_fail:
	ldp	x4, x30, [sp], #16
	ret
endfunc console_taurus_register
