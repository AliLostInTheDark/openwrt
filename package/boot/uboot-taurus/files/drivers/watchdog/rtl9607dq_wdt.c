// SPDX-License-Identifier: GPL-2.0-only
/*
 * Watchdog of the Realtek RTL9607DQ (Cortina Access) SoC.
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 *
 * A prescaler turns the peripheral clock into 1 ms ticks and a divider of
 * 1000 into 1 s ticks, so the load value is the timeout in seconds.
 */

#include <dm.h>
#include <errno.h>
#include <wdt.h>
#include <asm/io.h>
#include <linux/bitops.h>

#define WDT_CTRL		0x00
#define  WDT_CTRL_EN		BIT(0)
#define  WDT_CTRL_RSTEN		BIT(1)
#define WDT_PS			0x04	/* prescaler load value */
#define WDT_DIV			0x08	/* prescaler divider */
#define WDT_LD			0x0c	/* counter load value */
#define WDT_LOADE		0x10	/* load enable */
#define WDT_IE			0x18	/* interrupt enable */
#define  WDT_LOADE_WDT		BIT(0)
#define  WDT_LOADE_PRE		BIT(1)

#define WDT_TICK_DIV		1000	/* 1 ms ticks -> 1 s ticks */

/*
 * Global reset configuration: the reset only reaches the chip with these
 * bits set.
 */
#define WDT_RESET_SUB		BIT(4)
#define WDT_RESET_ALL_BLOCK	BIT(6)
#define WDT_RESET_REMAP		BIT(7)
#define WDT_EXT_RESET		BIT(8)
#define WDT_RESET_ALL		(WDT_RESET_SUB | WDT_RESET_ALL_BLOCK | \
				 WDT_RESET_REMAP | WDT_EXT_RESET)

struct rtl9607dq_wdt {
	void __iomem *base;
	void __iomem *global_config;
	u32 prescaler;
};

static int rtl9607dq_wdt_reset(struct udevice *dev)
{
	struct rtl9607dq_wdt *wdt = dev_get_priv(dev);

	writel(WDT_LOADE_WDT | WDT_LOADE_PRE, wdt->base + WDT_LOADE);

	return 0;
}

static int rtl9607dq_wdt_start(struct udevice *dev, u64 timeout_ms, ulong flags)
{
	struct rtl9607dq_wdt *wdt = dev_get_priv(dev);

	/* route the reset first, then set up the timer and enable it */
	setbits_le32(wdt->global_config, WDT_RESET_ALL);
	writel(wdt->prescaler, wdt->base + WDT_PS);
	writel(WDT_TICK_DIV, wdt->base + WDT_DIV);
	writel(DIV_ROUND_UP(timeout_ms, 1000), wdt->base + WDT_LD);
	writel(1, wdt->base + WDT_IE);
	writel(WDT_CTRL_EN | WDT_CTRL_RSTEN, wdt->base + WDT_CTRL);
	rtl9607dq_wdt_reset(dev);

	return 0;
}

static int rtl9607dq_wdt_stop(struct udevice *dev)
{
	struct rtl9607dq_wdt *wdt = dev_get_priv(dev);

	writel(0, wdt->base + WDT_CTRL);

	return 0;
}

static int rtl9607dq_wdt_of_to_plat(struct udevice *dev)
{
	struct rtl9607dq_wdt *wdt = dev_get_priv(dev);
	u32 rate;

	wdt->base = dev_remap_addr_index(dev, 0);
	wdt->global_config = dev_remap_addr_index(dev, 1);
	if (!wdt->base || !wdt->global_config)
		return -EINVAL;

	rate = dev_read_u32_default(dev, "clock-frequency", 0);
	if (rate < 1000)
		return -EINVAL;
	wdt->prescaler = rate / 1000 - 1;

	return 0;
}

static const struct wdt_ops rtl9607dq_wdt_ops = {
	.start = rtl9607dq_wdt_start,
	.stop = rtl9607dq_wdt_stop,
	.reset = rtl9607dq_wdt_reset,
};

static const struct udevice_id rtl9607dq_wdt_ids[] = {
	{ .compatible = "realtek,rtl9607dq-wdt" },
	{ }
};

U_BOOT_DRIVER(rtl9607dq_wdt) = {
	.name = "rtl9607dq_wdt",
	.id = UCLASS_WDT,
	.of_match = rtl9607dq_wdt_ids,
	.of_to_plat = rtl9607dq_wdt_of_to_plat,
	.priv_auto = sizeof(struct rtl9607dq_wdt),
	.ops = &rtl9607dq_wdt_ops,
};
