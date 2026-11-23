// SPDX-License-Identifier: GPL-2.0-only
/*
 * Watchdog of the Realtek RTL9607DQ (Cortina Access) SoC.
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 *
 * The register layout follows the vendor cortina_per_wdt driver. A prescaler
 * turns the peripheral clock into 1 ms ticks and a divider of 1000 into
 * 1 s ticks, so the load value is the timeout in seconds. U-Boot may leave the
 * watchdog running; the core then keeps it fed until userspace takes over.
 */

#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/watchdog.h>

#define RTL9607DQ_WDT_CTRL		0x00
#define  RTL9607DQ_WDT_CTRL_EN		BIT(0)
#define  RTL9607DQ_WDT_CTRL_RSTEN	BIT(1)
#define RTL9607DQ_WDT_PS		0x04	/* prescaler load value */
#define RTL9607DQ_WDT_DIV		0x08	/* prescaler divider */
#define RTL9607DQ_WDT_LD		0x0c	/* counter load value */
#define RTL9607DQ_WDT_LOADE		0x10	/* load enable */
#define  RTL9607DQ_WDT_LOADE_WDT	BIT(0)
#define  RTL9607DQ_WDT_LOADE_PRE	BIT(1)

#define RTL9607DQ_WDT_TICK_DIV		1000	/* 1 ms ticks -> 1 s ticks */
#define RTL9607DQ_WDT_DEFAULT_TIMEOUT	60
#define RTL9607DQ_WDT_MAX_TIMEOUT	U16_MAX

struct rtl9607dq_wdt {
	struct watchdog_device	wdd;
	void __iomem		*base;
	u32			prescaler;
};

static int rtl9607dq_wdt_ping(struct watchdog_device *wdd)
{
	struct rtl9607dq_wdt *wdt = watchdog_get_drvdata(wdd);

	writel(RTL9607DQ_WDT_LOADE_WDT | RTL9607DQ_WDT_LOADE_PRE,
	       wdt->base + RTL9607DQ_WDT_LOADE);
	return 0;
}

static int rtl9607dq_wdt_start(struct watchdog_device *wdd)
{
	struct rtl9607dq_wdt *wdt = watchdog_get_drvdata(wdd);

	writel(wdt->prescaler, wdt->base + RTL9607DQ_WDT_PS);
	writel(RTL9607DQ_WDT_TICK_DIV, wdt->base + RTL9607DQ_WDT_DIV);
	writel(wdd->timeout, wdt->base + RTL9607DQ_WDT_LD);
	rtl9607dq_wdt_ping(wdd);
	writel(RTL9607DQ_WDT_CTRL_EN | RTL9607DQ_WDT_CTRL_RSTEN,
	       wdt->base + RTL9607DQ_WDT_CTRL);
	return 0;
}

static int rtl9607dq_wdt_stop(struct watchdog_device *wdd)
{
	struct rtl9607dq_wdt *wdt = watchdog_get_drvdata(wdd);

	writel(0, wdt->base + RTL9607DQ_WDT_CTRL);
	return 0;
}

static int rtl9607dq_wdt_set_timeout(struct watchdog_device *wdd,
				     unsigned int timeout)
{
	wdd->timeout = timeout;
	if (watchdog_active(wdd) || watchdog_hw_running(wdd))
		return rtl9607dq_wdt_start(wdd);
	return 0;
}

static const struct watchdog_info rtl9607dq_wdt_info = {
	.identity	= "RTL9607DQ watchdog",
	.options	= WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING |
			  WDIOF_MAGICCLOSE,
};

static const struct watchdog_ops rtl9607dq_wdt_ops = {
	.owner		= THIS_MODULE,
	.start		= rtl9607dq_wdt_start,
	.stop		= rtl9607dq_wdt_stop,
	.ping		= rtl9607dq_wdt_ping,
	.set_timeout	= rtl9607dq_wdt_set_timeout,
};

static int rtl9607dq_wdt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rtl9607dq_wdt *wdt;
	unsigned long rate;
	struct clk *clk;
	int ret;

	wdt = devm_kzalloc(dev, sizeof(*wdt), GFP_KERNEL);
	if (!wdt)
		return -ENOMEM;

	wdt->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(wdt->base))
		return PTR_ERR(wdt->base);

	clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk), "cannot get clock\n");
	rate = clk_get_rate(clk);
	if (rate < 1000)
		return dev_err_probe(dev, -EINVAL, "clock rate %lu too low\n",
				     rate);
	wdt->prescaler = rate / 1000 - 1;

	wdt->wdd.info = &rtl9607dq_wdt_info;
	wdt->wdd.ops = &rtl9607dq_wdt_ops;
	wdt->wdd.parent = dev;
	wdt->wdd.min_timeout = 1;
	wdt->wdd.max_timeout = RTL9607DQ_WDT_MAX_TIMEOUT;
	wdt->wdd.timeout = RTL9607DQ_WDT_DEFAULT_TIMEOUT;
	watchdog_init_timeout(&wdt->wdd, 0, dev);
	watchdog_set_drvdata(&wdt->wdd, wdt);
	watchdog_stop_on_reboot(&wdt->wdd);

	/*
	 * Left running by the boot loader: restart it with our timeout and
	 * let the core feed it until userspace opens the device.
	 */
	if (readl(wdt->base + RTL9607DQ_WDT_CTRL) & RTL9607DQ_WDT_CTRL_EN) {
		rtl9607dq_wdt_start(&wdt->wdd);
		set_bit(WDOG_HW_RUNNING, &wdt->wdd.status);
	}

	ret = devm_watchdog_register_device(dev, &wdt->wdd);
	if (ret)
		return ret;

	dev_dbg(dev, "timeout %u s%s\n", wdt->wdd.timeout,
		 watchdog_hw_running(&wdt->wdd) ? ", running" : "");
	return 0;
}

static const struct of_device_id rtl9607dq_wdt_of_match[] = {
	{ .compatible = "realtek,rtl9607dq-wdt" },
	{ }
};
MODULE_DEVICE_TABLE(of, rtl9607dq_wdt_of_match);

static struct platform_driver rtl9607dq_wdt_driver = {
	.probe	= rtl9607dq_wdt_probe,
	.driver	= {
		.name		= "rtl9607dq-wdt",
		.of_match_table	= rtl9607dq_wdt_of_match,
	},
};
module_platform_driver(rtl9607dq_wdt_driver);

MODULE_AUTHOR("Ifran Ali <khanu.i.i.k.2000@gmail.com>");
MODULE_DESCRIPTION("Realtek RTL9607DQ watchdog driver");
MODULE_LICENSE("GPL");
