// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 *
 * Thermal sensor of the Realtek RTL9607DQ / Cortina Access network SoC.
 *
 * The sensor sits in the global register block (TMS_*).  The temperature
 * word is a sign bit, eight integer bits and ten bits of milli-degrees.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/thermal.h>

#include "thermal_hwmon.h"

#define TMS_CTRL_A		0x00
#define TMS_CTRL_B		0x04
#define TMS_CTRL_C		0x08
#define TMS_DATA		0x0c
#define  TMS_DATA_FRAC		GENMASK(9, 0)
#define  TMS_DATA_INT		GENMASK(17, 10)
#define  TMS_DATA_NEG		BIT(18)

/* Start values of the sensor, as programmed by the vendor kernel. */
#define TMS_CTRL_A_INIT		0x07f80000
#define TMS_CTRL_B_INIT		0x00374000
#define TMS_CTRL_C_RESET	0x2102011e
#define TMS_CTRL_C_RUN		0x2102011f

/* The vendor device tree calibrates the readout 5 degrees down. */
#define TMS_DEVIATION_MC	5000
#define TMS_SETTLE_MS		300

struct cortina_thermal {
	void __iomem *base;
};

static int cortina_thermal_get_temp(struct thermal_zone_device *tz, int *temp)
{
	struct cortina_thermal *ct = thermal_zone_device_priv(tz);
	u32 val = readl(ct->base + TMS_DATA);
	int whole = sign_extend32(FIELD_GET(TMS_DATA_INT, val), 7);
	int frac = FIELD_GET(TMS_DATA_FRAC, val);

	if (val & TMS_DATA_NEG)
		frac = -frac;

	*temp = whole * 1000 + frac - TMS_DEVIATION_MC;
	return 0;
}

static const struct thermal_zone_device_ops cortina_thermal_ops = {
	.get_temp = cortina_thermal_get_temp,
};

static int cortina_thermal_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct thermal_zone_device *tz;
	struct cortina_thermal *ct;
	struct resource *res;

	ct = devm_kzalloc(dev, sizeof(*ct), GFP_KERNEL);
	if (!ct)
		return -ENOMEM;

	/* part of the shared global register block: map, do not claim */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;
	ct->base = devm_ioremap(dev, res->start, resource_size(res));
	if (!ct->base)
		return -ENOMEM;

	writel(TMS_CTRL_A_INIT, ct->base + TMS_CTRL_A);
	writel(TMS_CTRL_B_INIT, ct->base + TMS_CTRL_B);
	writel(TMS_CTRL_C_RESET, ct->base + TMS_CTRL_C);
	writel(TMS_CTRL_C_RUN, ct->base + TMS_CTRL_C);
	msleep(TMS_SETTLE_MS);

	tz = devm_thermal_of_zone_register(dev, 0, ct, &cortina_thermal_ops);
	if (IS_ERR(tz))
		return dev_err_probe(dev, PTR_ERR(tz),
				     "failed to register the thermal zone\n");

	/* Zones registered from the device tree do not get hwmon by default. */
	return devm_thermal_add_hwmon_sysfs(dev, tz);
}

static const struct of_device_id cortina_thermal_of_match[] = {
	{ .compatible = "realtek,rtl9607dq-thermal" },
	{ }
};
MODULE_DEVICE_TABLE(of, cortina_thermal_of_match);

static struct platform_driver cortina_thermal_driver = {
	.probe = cortina_thermal_probe,
	.driver = {
		.name = "cortina-thermal",
		.of_match_table = cortina_thermal_of_match,
	},
};
module_platform_driver(cortina_thermal_driver);

MODULE_DESCRIPTION("Realtek RTL9607DQ (Cortina Access) thermal sensor");
MODULE_LICENSE("GPL");
