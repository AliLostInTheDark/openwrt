// SPDX-License-Identifier: GPL-2.0-only
/*
 * CPU clock driver for the Realtek RTL9607DQ (Cortina Access) SoC.
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 *
 * The Cortex-A55 cluster runs from the CPLL in the GLB syscon block. On reset
 * the PLL tracks the bootstrap speed strap in GLB_STRAP; software scaling
 * switches the CPU mux onto the FPLL-derived backup clock (div_f2c), reprograms
 * the CPLL feedback/pre-divider and post-divider, waits for PLL lock, and
 * switches the glitch-free mux back to CPLL.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/build_bug.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/limits.h>
#include <linux/math.h>
#include <linux/mfd/syscon.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define GLB_STRAP			0x0ac
#define  GLB_STRAP_SPEED		GENMASK(3, 1)

#define GLB_CPLLDIV			0x200
#define  GLB_CPLLDIV_CORTEX_DIVSEL	GENMASK(5, 0)
#define  GLB_CPLLDIV_DIV_OVERRIDE	BIT(6)
#define  GLB_CPLLDIV_CF_SEL		BIT(11)
#define  GLB_CPLLDIV_MODE_OVERRIDE	BIT(31)

#define GLB_CPLL0			0x230
#define  GLB_CPLL0_DIVN			GENMASK(7, 0)
#define  GLB_CPLL0_RSTB			GENMASK(11, 10)
#define  GLB_CPLL0_PREDIV		GENMASK(13, 12)

#define GLB_CPLL0_DIVN_OFFSET		3
#define GLB_CPLL0_PREDIV_OFFSET		2
#define GLB_CPLL_LOCK_US_MIN		200
#define GLB_CPLL_LOCK_US_MAX		250

struct rtl9607dq_cpu_opp {
	u8 mult;
	u8 div;
	u8 prediv;
	u8 divn;
};

/*
 * Indexed by GLB_STRAP_SPEED (0..7) and ordered by ascending frequency.
 * Each entry satisfies: rate = parent_rate * divn / (prediv * div)
 *                            = parent_rate * mult / div.
 */
static const struct rtl9607dq_cpu_opp rtl9607dq_cpu_opps[] = {
	{ .mult = 48, .div = 3, .prediv = 5, .divn = 240 }, /*  400 MHz */
	{ .mult = 50, .div = 2, .prediv = 5, .divn = 250 }, /*  625 MHz */
	{ .mult = 56, .div = 2, .prediv = 3, .divn = 168 }, /*  700 MHz */
	{ .mult = 64, .div = 2, .prediv = 3, .divn = 192 }, /*  800 MHz */
	{ .mult = 72, .div = 2, .prediv = 3, .divn = 216 }, /*  900 MHz */
	{ .mult = 40, .div = 1, .prediv = 5, .divn = 200 }, /* 1000 MHz */
	{ .mult = 44, .div = 1, .prediv = 5, .divn = 220 }, /* 1100 MHz */
	{ .mult = 48, .div = 1, .prediv = 5, .divn = 240 }, /* 1200 MHz */
};

static_assert(ARRAY_SIZE(rtl9607dq_cpu_opps) == 8);

struct rtl9607dq_cpu_clk {
	struct clk_hw hw;
	struct regmap *glb;
};

static struct rtl9607dq_cpu_clk *to_rtl9607dq_cpu_clk(struct clk_hw *hw)
{
	return container_of(hw, struct rtl9607dq_cpu_clk, hw);
}

static unsigned long rtl9607dq_opp_rate(unsigned long parent_rate,
					const struct rtl9607dq_cpu_opp *opp)
{
	return DIV_ROUND_CLOSEST_ULL((u64)parent_rate * opp->mult, opp->div);
}

static const struct rtl9607dq_cpu_opp *
rtl9607dq_find_opp(unsigned long parent_rate, unsigned long target_rate,
		   unsigned long min_rate, unsigned long max_rate)
{
	const struct rtl9607dq_cpu_opp *best = NULL;
	unsigned long best_diff = ULONG_MAX;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(rtl9607dq_cpu_opps); i++) {
		const struct rtl9607dq_cpu_opp *opp = &rtl9607dq_cpu_opps[i];
		unsigned long rate = rtl9607dq_opp_rate(parent_rate, opp);
		unsigned long diff;

		if (rate < min_rate || rate > max_rate)
			continue;

		diff = abs_diff(rate, target_rate);
		if (diff < best_diff) {
			best = opp;
			best_diff = diff;
		}
	}

	return best;
}

static unsigned long rtl9607dq_cpu_recalc_rate(struct clk_hw *hw,
					       unsigned long parent_rate)
{
	struct rtl9607dq_cpu_clk *cpu_clk = to_rtl9607dq_cpu_clk(hw);
	unsigned int divn, prediv, div;
	u32 cplldiv, cpll0, strap;

	if (regmap_read(cpu_clk->glb, GLB_CPLLDIV, &cplldiv))
		return 0;

	if (!(cplldiv & GLB_CPLLDIV_MODE_OVERRIDE)) {
		unsigned int idx;

		if (regmap_read(cpu_clk->glb, GLB_STRAP, &strap))
			return 0;

		idx = FIELD_GET(GLB_STRAP_SPEED, strap);
		return rtl9607dq_opp_rate(parent_rate, &rtl9607dq_cpu_opps[idx]);
	}

	if (regmap_read(cpu_clk->glb, GLB_CPLL0, &cpll0))
		return 0;

	divn = FIELD_GET(GLB_CPLL0_DIVN, cpll0) + GLB_CPLL0_DIVN_OFFSET;
	prediv = FIELD_GET(GLB_CPLL0_PREDIV, cpll0) + GLB_CPLL0_PREDIV_OFFSET;

	if (cplldiv & GLB_CPLLDIV_DIV_OVERRIDE) {
		div = FIELD_GET(GLB_CPLLDIV_CORTEX_DIVSEL, cplldiv) >> 1;
		if (!div)
			div = 1;
	} else {
		div = 1;
	}

	return DIV_ROUND_CLOSEST_ULL((u64)parent_rate * divn,
				     (u64)prediv * div);
}

static int rtl9607dq_cpu_determine_rate(struct clk_hw *hw,
					struct clk_rate_request *req)
{
	const struct rtl9607dq_cpu_opp *opp;

	if (!req->best_parent_rate)
		return -EINVAL;

	opp = rtl9607dq_find_opp(req->best_parent_rate, req->rate,
				 req->min_rate, req->max_rate);
	if (!opp)
		return -EINVAL;

	req->rate = rtl9607dq_opp_rate(req->best_parent_rate, opp);

	return 0;
}

static int rtl9607dq_cpu_set_rate(struct clk_hw *hw, unsigned long rate,
				  unsigned long parent_rate)
{
	struct rtl9607dq_cpu_clk *cpu_clk = to_rtl9607dq_cpu_clk(hw);
	const struct rtl9607dq_cpu_opp *opp;
	u32 val;
	int ret;

	if (!parent_rate)
		return -EINVAL;

	opp = rtl9607dq_find_opp(parent_rate, rate, 0, ULONG_MAX);
	if (!opp)
		return -EINVAL;

	/* Switch CPU to backup clock (div_f2c) before reconfiguring CPLL. */
	ret = regmap_set_bits(cpu_clk->glb, GLB_CPLLDIV, GLB_CPLLDIV_CF_SEL);
	if (ret)
		return ret;

	/* Assert CPLL reset while updating feedback and pre-divider values. */
	ret = regmap_clear_bits(cpu_clk->glb, GLB_CPLL0, GLB_CPLL0_RSTB);
	if (ret)
		goto restore_mux;

	val = FIELD_PREP(GLB_CPLL0_PREDIV,
			 opp->prediv - GLB_CPLL0_PREDIV_OFFSET) |
	      FIELD_PREP(GLB_CPLL0_DIVN, opp->divn - GLB_CPLL0_DIVN_OFFSET);
	ret = regmap_update_bits(cpu_clk->glb, GLB_CPLL0,
				 GLB_CPLL0_PREDIV | GLB_CPLL0_DIVN, val);
	if (ret)
		goto restore_rstb;

	/* Enable software override of the bootstrap CPLL mode. */
	ret = regmap_set_bits(cpu_clk->glb, GLB_CPLLDIV,
			      GLB_CPLLDIV_MODE_OVERRIDE);
	if (ret)
		goto restore_rstb;

	/* Release CPLL reset and wait for the PLL to lock. */
	ret = regmap_update_bits(cpu_clk->glb, GLB_CPLL0, GLB_CPLL0_RSTB,
				 FIELD_PREP(GLB_CPLL0_RSTB, 1));
	if (ret)
		goto restore_mux;

	usleep_range(GLB_CPLL_LOCK_US_MIN, GLB_CPLL_LOCK_US_MAX);

	/* Program the post-PLL Cortex divider (encoded as div << 1). */
	val = GLB_CPLLDIV_DIV_OVERRIDE |
	      FIELD_PREP(GLB_CPLLDIV_CORTEX_DIVSEL, opp->div << 1);
	ret = regmap_update_bits(cpu_clk->glb, GLB_CPLLDIV,
				 GLB_CPLLDIV_DIV_OVERRIDE |
				 GLB_CPLLDIV_CORTEX_DIVSEL, val);
	if (ret)
		goto restore_mux;

	/* Switch CPU back to the locked CPLL. */
	return regmap_clear_bits(cpu_clk->glb, GLB_CPLLDIV, GLB_CPLLDIV_CF_SEL);

restore_rstb:
	regmap_update_bits(cpu_clk->glb, GLB_CPLL0, GLB_CPLL0_RSTB,
			   FIELD_PREP(GLB_CPLL0_RSTB, 1));
	usleep_range(GLB_CPLL_LOCK_US_MIN, GLB_CPLL_LOCK_US_MAX);
restore_mux:
	regmap_clear_bits(cpu_clk->glb, GLB_CPLLDIV, GLB_CPLLDIV_CF_SEL);
	return ret;
}

static const struct clk_ops rtl9607dq_cpu_clk_ops = {
	.recalc_rate = rtl9607dq_cpu_recalc_rate,
	.determine_rate = rtl9607dq_cpu_determine_rate,
	.set_rate = rtl9607dq_cpu_set_rate,
};

static const struct clk_parent_data rtl9607dq_cpu_parent[] = {
	{ .index = 0 },
};

static int rtl9607dq_clk_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct clk_init_data init = {};
	struct rtl9607dq_cpu_clk *cpu_clk;
	int ret;

	cpu_clk = devm_kzalloc(dev, sizeof(*cpu_clk), GFP_KERNEL);
	if (!cpu_clk)
		return -ENOMEM;

	cpu_clk->glb = syscon_node_to_regmap(dev->of_node);
	if (IS_ERR(cpu_clk->glb))
		return dev_err_probe(dev, PTR_ERR(cpu_clk->glb),
				     "failed to get GLB regmap\n");

	init.name = "cpu";
	init.ops = &rtl9607dq_cpu_clk_ops;
	init.parent_data = rtl9607dq_cpu_parent;
	init.num_parents = ARRAY_SIZE(rtl9607dq_cpu_parent);
	init.flags = CLK_IS_CRITICAL;

	cpu_clk->hw.init = &init;

	ret = devm_clk_hw_register(dev, &cpu_clk->hw);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register CPU clock\n");

	return devm_of_clk_add_hw_provider(dev, of_clk_hw_simple_get,
					   &cpu_clk->hw);
}

static const struct of_device_id rtl9607dq_clk_of_match[] = {
	{ .compatible = "realtek,rtl9607dq-glb" },
	{ }
};
MODULE_DEVICE_TABLE(of, rtl9607dq_clk_of_match);

static struct platform_driver rtl9607dq_clk_driver = {
	.probe = rtl9607dq_clk_probe,
	.driver = {
		.name = "rtl9607dq-clk",
		.of_match_table = rtl9607dq_clk_of_match,
	},
};
module_platform_driver(rtl9607dq_clk_driver);

MODULE_AUTHOR("Ifran Ali <khanu.i.i.k.2000@gmail.com>");
MODULE_DESCRIPTION("Realtek RTL9607DQ CPU clock driver");
MODULE_LICENSE("GPL");
