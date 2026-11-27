/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 */

#include <arch.h>
#include <asm_macros.S>
#include <platform_def.h>

	.globl	plat_my_core_pos
	.globl	plat_core_pos_by_mpidr
	.globl	plat_crash_console_init
	.globl	plat_crash_console_putc
	.globl	plat_crash_console_flush
	.globl	plat_secondary_cold_boot_setup
	.globl	platform_mem_init
	.globl	plat_get_syscnt_freq2

func plat_my_core_pos
	mrs	x0, mpidr_el1
	b	plat_core_pos_by_mpidr
endfunc plat_my_core_pos

/* int plat_core_pos_by_mpidr(u_register_t mpidr) */
func plat_core_pos_by_mpidr
	tst	x0, #MPIDR_CPU_MASK
	b.ne	2f
	ubfx	x0, x0, #MPIDR_AFF1_SHIFT, #MPIDR_AFFINITY_BITS
	cmp	x0, #PLATFORM_CORE_COUNT
	b.lo	1f
2:
	mov	x0, #-1
1:
	ret
endfunc plat_core_pos_by_mpidr

func plat_crash_console_init
	mov_imm	x0, TAURUS_UART0_BASE
	mov_imm	x1, TAURUS_UART0_CLK_IN_HZ
	mov_imm	x2, TAURUS_UART0_BAUDRATE
	b	console_taurus_core_init
endfunc plat_crash_console_init

func plat_crash_console_putc
	mov_imm	x1, TAURUS_UART0_BASE
	b	console_taurus_core_putc
endfunc plat_crash_console_putc

func plat_crash_console_flush
	mov_imm	x0, TAURUS_UART0_BASE
	b	console_taurus_core_flush
endfunc plat_crash_console_flush

func plat_secondary_cold_boot_setup
	mov	x0, #0
	ret
endfunc plat_secondary_cold_boot_setup

func platform_mem_init
	ret
endfunc platform_mem_init

func plat_get_syscnt_freq2
	mov_imm	x0, SYS_COUNTER_FREQ_IN_TICKS
	ret
endfunc plat_get_syscnt_freq2
