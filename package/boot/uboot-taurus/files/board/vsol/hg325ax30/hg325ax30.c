// SPDX-License-Identifier: GPL-2.0+
/*
 * VSOL HG325AX30 (Realtek RTL9607DQ "Taurus")
 *
 * Runs either chainloaded or as BL33: DRAM, clocks and the secure monitor
 * (BL31, PSCI) are already set up by the earlier boot stages when it starts.
 */

#include <asm/armv8/mmu.h>
#include <asm/global_data.h>
#include <asm/psci.h>
#include <config.h>
#include <dm.h>
#include <init.h>
#include <wdt.h>
#include <asm/io.h>
#include <linux/psci.h>
#include <linux/sizes.h>

DECLARE_GLOBAL_DATA_PTR;

/*
 * BL2 sets this word to the "warm" value and arms a 23 s watchdog before it
 * starts BL33. If BL33 never clears both, the reset boots the standby FIP.
 */
#define GLOBAL_SCRATCH		0xf432019c

int print_cpuinfo(void)
{
	printf("CPU:   Realtek RTL9607DQ\n");
	return 0;
}

static struct mm_region hg325ax30_mem_map[] = {
	{
		.virt = 0x00000000UL,
		.phys = 0x00000000UL,
		.size = SZ_512M,
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL) |
			 PTE_BLOCK_INNER_SHARE
	}, {
		/* peripherals, 32-bit alias of the 0x4_f4xx_xxxx window */
		.virt = 0xe0000000UL,
		.phys = 0xe0000000UL,
		.size = SZ_512M,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE
	}, {
		/* list terminator */
		0,
	}
};

struct mm_region *mem_map = hg325ax30_mem_map;

int dram_init(void)
{
	/* 512 MiB DDR3; the top 16 MiB of the first 256 MiB belong to BL31 */
	gd->ram_size = SZ_512M;
	return 0;
}

int dram_init_banksize(void)
{
	gd->dram[0].start = 0;
	gd->dram[0].size = SZ_256M - SZ_16M;
	gd->dram[1].start = SZ_256M;
	gd->dram[1].size = SZ_256M;
	return 0;
}

int board_init(void)
{
	return 0;
}

int board_late_init(void)
{
	struct udevice *wdt;

	writel(0, GLOBAL_SCRATCH);
	if (!uclass_get_device(UCLASS_WDT, 0, &wdt))
		wdt_stop(wdt);

	return 0;
}
