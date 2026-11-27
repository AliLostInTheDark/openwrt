# SPDX-License-Identifier: BSD-3-Clause
#
# Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
#

PLAT_INCLUDES		:=	-Iplat/realtek/rtl9607dq/include

include lib/xlat_tables_v2/xlat_tables.mk
GICV3_SUPPORT_GIC600	:=	1
include drivers/arm/gic/v3/gicv3.mk

PLAT_BL_COMMON_SOURCES	:=	${XLAT_TABLES_LIB_SRCS}				\
				plat/realtek/rtl9607dq/taurus_console.S		\
				plat/realtek/rtl9607dq/taurus_helpers.S

BL31_SOURCES		+=	lib/cpus/aarch64/cortex_a55.S			\
				drivers/arm/cci/cci.c				\
				drivers/delay_timer/delay_timer.c		\
				drivers/delay_timer/generic_delay_timer.c	\
				plat/common/plat_psci_common.c			\
				plat/common/plat_gicv3.c			\
				${GICV3_SOURCES}				\
				plat/realtek/rtl9607dq/taurus_bl31_setup.c	\
				plat/realtek/rtl9607dq/taurus_pm.c		\
				plat/realtek/rtl9607dq/taurus_sip_svc.c		\
				plat/realtek/rtl9607dq/taurus_spinand.c

USE_COHERENT_MEM	:=	0
HW_ASSISTED_COHERENCY	:=	1
CTX_INCLUDE_AARCH32_REGS :=	0
COLD_BOOT_SINGLE_CPU	:=	1
PROGRAMMABLE_RESET_ADDRESS :=	1
RESET_TO_BL31		:=	0
ENABLE_PIE		:=	0
MULTI_CONSOLE_API	:=	1
