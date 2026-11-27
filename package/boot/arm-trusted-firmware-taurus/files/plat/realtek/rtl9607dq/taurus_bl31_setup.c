/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 */

#include <assert.h>
#include <stdint.h>

#include <arch_helpers.h>
#include <common/bl_common.h>
#include <common/debug.h>
#include <drivers/arm/cci.h>
#include <drivers/arm/gicv3.h>
#include <drivers/console.h>
#include <drivers/generic_delay_timer.h>
#include <lib/mmio.h>
#include <lib/xlat_tables/xlat_tables_v2.h>
#include <plat/common/platform.h>

#include <platform_def.h>

extern int console_taurus_register(uintptr_t base_addr, uint32_t clock,
				   uint32_t baud, console_t *console);
extern int taurus_load_bl33(uintptr_t dst, size_t max_size, size_t *out_size);

static console_t taurus_console;
static entry_point_info_t bl33_image_ep_info;

static const interrupt_prop_t taurus_interrupt_props[] = {
	INTR_PROP_DESC(8, GIC_HIGHEST_SEC_PRIORITY, INTR_GROUP1S,
		       GIC_INTR_CFG_LEVEL),
	INTR_PROP_DESC(9, GIC_HIGHEST_SEC_PRIORITY, INTR_GROUP0,
		       GIC_INTR_CFG_LEVEL),
	INTR_PROP_DESC(10, GIC_HIGHEST_SEC_PRIORITY, INTR_GROUP0,
		       GIC_INTR_CFG_LEVEL),
	INTR_PROP_DESC(11, GIC_HIGHEST_SEC_PRIORITY, INTR_GROUP1S,
		       GIC_INTR_CFG_LEVEL),
	INTR_PROP_DESC(12, GIC_HIGHEST_SEC_PRIORITY, INTR_GROUP1S,
		       GIC_INTR_CFG_LEVEL),
	INTR_PROP_DESC(13, GIC_HIGHEST_SEC_PRIORITY, INTR_GROUP1S,
		       GIC_INTR_CFG_LEVEL),
	INTR_PROP_DESC(14, GIC_HIGHEST_SEC_PRIORITY, INTR_GROUP1S,
		       GIC_INTR_CFG_LEVEL),
	INTR_PROP_DESC(15, GIC_HIGHEST_SEC_PRIORITY, INTR_GROUP1S,
		       GIC_INTR_CFG_LEVEL),
};

static const int taurus_cci_map[] = { TAURUS_CCI_CLUSTER_SLAVE };

static uintptr_t rdistif_base_addrs[PLATFORM_CORE_COUNT];

static unsigned int taurus_mpidr_to_core_pos(u_register_t mpidr)
{
	return (unsigned int)plat_core_pos_by_mpidr(mpidr);
}

static const gicv3_driver_data_t taurus_gic_data = {
	.gicd_base = TAURUS_GICD_BASE,
	.gicr_base = TAURUS_GICR_BASE,
	.interrupt_props = taurus_interrupt_props,
	.interrupt_props_num = ARRAY_SIZE(taurus_interrupt_props),
	.rdistif_num = PLATFORM_CORE_COUNT,
	.rdistif_base_addrs = rdistif_base_addrs,
	.mpidr_to_core_pos = taurus_mpidr_to_core_pos,
};

void bl31_early_platform_setup2(u_register_t arg0, u_register_t arg1,
				u_register_t arg2, u_register_t arg3)
{
	console_taurus_register(TAURUS_UART0_BASE, TAURUS_UART0_CLK_IN_HZ,
				TAURUS_UART0_BAUDRATE, &taurus_console);
	console_set_scope(&taurus_console, CONSOLE_FLAG_BOOT |
			  CONSOLE_FLAG_RUNTIME);

	/* Let the non-secure world reach the peripherals and their DMA */
	mmio_write_32(TAURUS_GLB_SECURITY_OVERRIDE, 0x007b73ffU);

	/* One DSU cluster on CCI-400 slave port 2; let non-secure access it */
	cci_init(TAURUS_CCI_BASE, taurus_cci_map, ARRAY_SIZE(taurus_cci_map));
	cci_enable_snoop_dvm_reqs(0);
	mmio_write_32(TAURUS_CCI_BASE + 0x8, 1U);

	generic_delay_timer_init();
}

void bl31_plat_arch_setup(void)
{
	const mmap_region_t bl_regions[] = {
		MAP_REGION_FLAT(BL_CODE_BASE, BL_CODE_END - BL_CODE_BASE,
				MT_CODE | MT_SECURE),
		MAP_REGION_FLAT(BL_CODE_END, BL31_LIMIT - BL_CODE_END,
				MT_MEMORY | MT_RW | MT_SECURE),
		{0}
	};
	const mmap_region_t plat_regions[] = {
		MAP_REGION_FLAT(PLAT_TAURUS_BL33_BASE, PLAT_TAURUS_BL33_MAX_SIZE,
				MT_MEMORY | MT_RW | MT_NS),
		MAP_REGION_FLAT(PLAT_TAURUS_SRAM_BASE, PLAT_TAURUS_SRAM_SIZE,
				MT_DEVICE | MT_RW | MT_SECURE),
		MAP_REGION_FLAT(PLAT_TAURUS_PERIPH_BASE, PLAT_TAURUS_PERIPH_SIZE,
				MT_DEVICE | MT_RW | MT_SECURE),
		MAP_REGION_FLAT(TAURUS_GIC_BASE, TAURUS_GIC_SIZE,
				MT_DEVICE | MT_RW | MT_SECURE),
		{0}
	};

	setup_page_tables(bl_regions, plat_regions);
	enable_mmu_el3(0);
}

void bl31_platform_setup(void)
{
	size_t bl33_size = 0;

	gicv3_driver_init(&taurus_gic_data);
	gicv3_distif_init();
	gicv3_rdistif_init(plat_my_core_pos());
	gicv3_cpuif_enable(plat_my_core_pos());

	if (taurus_load_bl33(PLAT_TAURUS_BL33_BASE, PLAT_TAURUS_BL33_MAX_SIZE,
			     &bl33_size) != 0) {
		ERROR("Failed to load BL33 from FIP\n");
		panic();
	}

	flush_dcache_range(PLAT_TAURUS_BL33_BASE, bl33_size);

	SET_PARAM_HEAD(&bl33_image_ep_info, PARAM_EP, VERSION_1, 0);
	bl33_image_ep_info.pc = PLAT_TAURUS_BL33_BASE;
	bl33_image_ep_info.spsr = SPSR_64(MODE_EL2, MODE_SP_ELX,
					  DISABLE_ALL_EXCEPTIONS);
	SET_SECURITY_STATE(bl33_image_ep_info.h.attr, NON_SECURE);

	NOTICE("BL31: BL33 loaded (%u bytes) -> 0x%lx (EL2h)\n",
	       (unsigned int)bl33_size, (unsigned long)bl33_image_ep_info.pc);
}

entry_point_info_t *bl31_plat_get_next_image_ep_info(uint32_t type)
{
	if (type == NON_SECURE)
		return &bl33_image_ep_info;
	return NULL;
}
