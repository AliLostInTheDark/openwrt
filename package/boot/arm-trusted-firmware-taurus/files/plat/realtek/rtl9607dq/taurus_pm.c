/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 */

#include <assert.h>
#include <stdint.h>

#include <arch_helpers.h>
#include <common/debug.h>
#include <drivers/arm/gicv3.h>
#include <drivers/delay_timer.h>
#include <lib/mmio.h>
#include <lib/psci/psci.h>
#include <plat/common/platform.h>

#include <platform_def.h>

static uintptr_t taurus_sec_entrypoint;

static void taurus_pwr_domain_on_finish(const psci_power_state_t *target_state)
{
	assert(target_state->pwr_domain_state[MPIDR_AFFLVL0] ==
	       PLAT_MAX_OFF_STATE);

	gicv3_rdistif_init(plat_my_core_pos());
	gicv3_cpuif_enable(plat_my_core_pos());
}

static int taurus_pwr_domain_on(u_register_t mpidr)
{
	int core = plat_core_pos_by_mpidr(mpidr);
	uint32_t bit, rst;

	if (core < 0)
		return PSCI_E_INVALID_PARAMS;

	/* Bit 0 of GLOBAL_CONFIG marks the dual-core parts */
	if ((mmio_read_32(TAURUS_GLB_GLOBAL_CONFIG) & 1U) && core > 1)
		return PSCI_E_INVALID_PARAMS;

	bit = BIT_32(core);
	rst = (bit << 1) | (bit << 5);

	/* The core starts at RVBAR (in words) when its reset is released */
	mmio_write_32(TAURUS_GLB_ARM_CONFIG_D0,
		      (uint32_t)(taurus_sec_entrypoint >> 2));
	mmio_setbits_32(TAURUS_GLB_ARM_RESET, rst);
	isb();

	mmio_clrbits_32(TAURUS_GLB_ARM_CONFIG_B, bit << 16);
	mmio_clrbits_32(TAURUS_GLB_ARM_CONFIG_B, bit << 20);
	mmio_clrbits_32(TAURUS_GLB_ARM_CONFIG_B, bit << 24);
	isb();
	dsb();

	udelay(1);
	mmio_clrbits_32(TAURUS_GLB_ARM_RESET, rst);
	isb();
	dsb();
	udelay(1);

	mmio_write_32(TAURUS_GLB_ARM_CONFIG_D0, TAURUS_RVBAR_ROM);

	return PSCI_E_SUCCESS;
}

static void taurus_pwr_domain_off(const psci_power_state_t *target_state)
{
	gicv3_cpuif_disable(plat_my_core_pos());
}

static void __dead2 taurus_system_reset(void)
{
	/* The peripheral watchdog resets the whole SoC, boot ROM first */
	mmio_clrbits_32(TAURUS_WDT_BASE + 0x10, 3U);
	mmio_clrbits_32(TAURUS_WDT_BASE + 0x18, 1U);
	mmio_clrbits_32(TAURUS_WDT_BASE + 0x24, 1U);
	mmio_write_32(TAURUS_WDT_BASE + 0x04, 0x1e848U); /* prescale = 125000 */
	mmio_clrsetbits_32(TAURUS_WDT_BASE + 0x08, 0xffffU, 1U); /* div = 1 */
	mmio_clrbits_32(TAURUS_WDT_BASE + 0x00, 7U);

	mmio_write_32(TAURUS_GLB_ARM_CONFIG_D0, TAURUS_RVBAR_ROM);
	mmio_setbits_32(TAURUS_GLB_GLOBAL_CONFIG, 0x1d0U);

	mmio_write_32(TAURUS_WDT_BASE + 0x0c, 1U);       /* load */
	mmio_setbits_32(TAURUS_WDT_BASE + 0x10, 3U);
	mmio_setbits_32(TAURUS_WDT_BASE + 0x00, 7U);     /* enable + reset */

	isb();
	for (;;)
		wfi();
}

static void __dead2 taurus_system_off(void)
{
	gicv3_cpuif_disable(plat_my_core_pos());
	for (;;)
		wfi();
}

static int taurus_validate_power_state(unsigned int power_state,
				       psci_power_state_t *req_state)
{
	unsigned int pstate = psci_get_pstate_type(power_state);
	unsigned int pwr_lvl = psci_get_pstate_pwrlvl(power_state);

	if (pwr_lvl > PLAT_MAX_PWR_LVL)
		return PSCI_E_INVALID_PARAMS;

	if (pstate == PSTATE_TYPE_STANDBY) {
		req_state->pwr_domain_state[MPIDR_AFFLVL0] = PLAT_MAX_RET_STATE;
		return PSCI_E_SUCCESS;
	}

	req_state->pwr_domain_state[MPIDR_AFFLVL0] = PLAT_MAX_OFF_STATE;
	if (pwr_lvl >= MPIDR_AFFLVL1)
		req_state->pwr_domain_state[MPIDR_AFFLVL1] = PLAT_MAX_OFF_STATE;
	if (pwr_lvl >= MPIDR_AFFLVL2)
		req_state->pwr_domain_state[MPIDR_AFFLVL2] = PLAT_MAX_OFF_STATE;
	return PSCI_E_SUCCESS;
}

static const plat_psci_ops_t taurus_psci_ops = {
	.pwr_domain_on = taurus_pwr_domain_on,
	.pwr_domain_off = taurus_pwr_domain_off,
	.pwr_domain_on_finish = taurus_pwr_domain_on_finish,
	.system_off = taurus_system_off,
	.system_reset = taurus_system_reset,
	.validate_power_state = taurus_validate_power_state,
};

int plat_setup_psci_ops(uintptr_t sec_entrypoint,
			const plat_psci_ops_t **psci_ops)
{
	taurus_sec_entrypoint = sec_entrypoint;
	*psci_ops = &taurus_psci_ops;
	return 0;
}

const unsigned char *plat_get_power_domain_tree_desc(void)
{
	static const unsigned char tree[] = {
		PLATFORM_CLUSTER_COUNT,
		PLATFORM_MAX_CPUS_PER_CLUSTER,
		1, 1, 1, 1
	};

	return tree;
}
