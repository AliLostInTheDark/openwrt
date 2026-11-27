/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 *
 * Cortina/Realtek SiP runtime service (0x820011xx).
 */

#include <stdint.h>

#include <common/runtime_svc.h>
#include <drivers/delay_timer.h>
#include <lib/mmio.h>
#include <smccc_helpers.h>

#include <platform_def.h>

#define TAURUS_SIP_TREF_TUNING		0x82001108U

static uint32_t base_drr_refi;
static bool tms_initialized;

/*
 * Scale the DDR3 refresh interval (DRR[23:8]) by die temperature, as the
 * stock BL31 does: below 75 C 1x, 75-84 C 2x, from 85 C 4x.
 */
static void taurus_tref_tuning(uint32_t already_init)
{
	int temp_c;
	uint32_t div, cur, target, drr;

	if (!tms_initialized) {
		if (already_init == 0U) {
			mmio_write_32(TAURUS_GLB_TMS_REG_A, 0x07f80000U);
			mmio_write_32(TAURUS_GLB_TMS_REG_B, 0x00374000U);
			mmio_write_32(TAURUS_GLB_TMS_REG_C, 0x2102011eU);
			udelay(1000);
			mmio_write_32(TAURUS_GLB_TMS_REG_C, 0x2102011fU);
			udelay(1000);
		}
		base_drr_refi = (mmio_read_32(TAURUS_DDR_DRR_REG) >> 8) &
				0xffffU;
		tms_initialized = true;
	}

	temp_c = (int)((mmio_read_32(TAURUS_GLB_TMS_TEMP_DATA) >> 10) & 0xffU) - 5;
	div = (temp_c >= 85) ? 4U : (temp_c >= 75) ? 2U : 1U;
	target = base_drr_refi / div;

	drr = mmio_read_32(TAURUS_DDR_DRR_REG);
	cur = (drr >> 8) & 0xffffU;
	if (target != cur) {
		drr = (drr & ~0x00ffff00U) | ((target & 0xffffU) << 8);
		mmio_write_32(TAURUS_DDR_DRR_REG, drr);
	}
}

static uintptr_t taurus_sip_handler(uint32_t smc_fid,
				    u_register_t x1,
				    u_register_t x2,
				    u_register_t x3,
				    u_register_t x4,
				    void *cookie,
				    void *handle,
				    u_register_t flags)
{
	switch (smc_fid) {
	case TAURUS_SIP_TREF_TUNING:
		taurus_tref_tuning((uint32_t)x1);
		SMC_RET1(handle, 0);
	default:
		SMC_RET1(handle, SMC_UNK);
	}
}

DECLARE_RT_SVC(
	taurus_sip_svc,
	OEN_SIP_START,
	OEN_SIP_END,
	SMC_TYPE_FAST,
	NULL,
	taurus_sip_handler
);
