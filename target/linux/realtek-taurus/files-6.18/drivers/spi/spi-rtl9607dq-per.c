// SPDX-License-Identifier: GPL-2.0-only
/*
 * Peripheral SPI controller of the Realtek RTL9607DQ
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 *
 * The controller runs one "access" at a time: it asserts the selected chip
 * select, shifts out up to 12 command bytes from CA0..CA2 while it shifts the
 * same number of bytes into RDAT1/RDAT0, and deasserts the chip select. The
 * driver moves one byte per access, so the chip select toggles between bytes;
 * devices on this bus (the DUSLIC-XS SLIC on the HG325AX30) accept that. The
 * engine is polled: an access takes a few microseconds at the bus rates used.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>

#define PER_SPI_CLK		0x00
#define  PER_SPI_CLK_DIV	GENMASK(15, 0)	/* rate / (2 * (DIV + 1)) */
#define  PER_SPI_CLK_IGAP	GENMASK(23, 16)
#define PER_SPI_CFG		0x04
#define  PER_SPI_CFG_CMD_ONLY	BIT(0)
#define  PER_SPI_CFG_READ	BIT(1)
#define  PER_SPI_CFG_MW_CS	GENMASK(6, 2)
#define  PER_SPI_CFG_CS		GENMASK(12, 8)
#define  PER_SPI_CFG_CMD_BITS	GENMASK(22, 16)	/* bits per access - 1 */
#define  PER_SPI_CFG_PRE_DATA	GENMASK(29, 24)
#define PER_SPI_MODE		0x08
#define  PER_SPI_MODE_CPOL	BIT(0)
#define  PER_SPI_MODE_CPHA	BIT(1)
/* always set by the vendor driver; bit 3 is set for the SLIC, purpose unknown */
#define  PER_SPI_MODE_FIXED	(BIT(2) | BIT(3))
#define PER_SPI_CTRL		0x0c
#define  PER_SPI_CTRL_DONE	BIT(0)		/* write 1 to clear */
#define  PER_SPI_CTRL_START	BIT(7)
#define PER_SPI_CA0		0x10
#define PER_SPI_RDAT0		0x28

#define PER_SPI_NUM_CS		5
#define PER_SPI_MAX_DIV		0x20000
#define PER_SPI_TIMEOUT_US	1000

struct rtl9607dq_per_spi {
	void __iomem *base;
	unsigned long rate;
};

static void per_spi_setup_xfer(struct rtl9607dq_per_spi *ps,
			       struct spi_device *spi, u32 speed_hz)
{
	u32 div, clk, cfg, mode;

	div = DIV_ROUND_UP(ps->rate, speed_hz);
	div = clamp(round_up(div, 2), 2U, PER_SPI_MAX_DIV);

	clk = readl(ps->base + PER_SPI_CLK);
	clk &= ~(PER_SPI_CLK_DIV | PER_SPI_CLK_IGAP);
	clk |= FIELD_PREP(PER_SPI_CLK_DIV, div / 2 - 1);
	writel(clk, ps->base + PER_SPI_CLK);

	mode = PER_SPI_MODE_FIXED;
	if (spi->mode & SPI_CPOL)
		mode |= PER_SPI_MODE_CPOL;
	if (spi->mode & SPI_CPHA)
		mode |= PER_SPI_MODE_CPHA;
	writel(mode, ps->base + PER_SPI_MODE);

	cfg = readl(ps->base + PER_SPI_CFG);
	cfg &= ~(PER_SPI_CFG_MW_CS | PER_SPI_CFG_CS | PER_SPI_CFG_CMD_BITS |
		 PER_SPI_CFG_PRE_DATA);
	cfg |= PER_SPI_CFG_CMD_ONLY | PER_SPI_CFG_READ |
	       FIELD_PREP(PER_SPI_CFG_CS, BIT(spi_get_chipselect(spi, 0))) |
	       FIELD_PREP(PER_SPI_CFG_CMD_BITS, 7);
	writel(cfg, ps->base + PER_SPI_CFG);
}

static int per_spi_xfer_byte(struct rtl9607dq_per_spi *ps, u8 tx, u8 *rx)
{
	u32 ctrl;
	int ret;

	writel((u32)tx << 24, ps->base + PER_SPI_CA0);

	ctrl = readl(ps->base + PER_SPI_CTRL);
	writel(ctrl | PER_SPI_CTRL_START, ps->base + PER_SPI_CTRL);
	ret = readl_poll_timeout_atomic(ps->base + PER_SPI_CTRL, ctrl,
					ctrl & PER_SPI_CTRL_DONE, 0,
					PER_SPI_TIMEOUT_US);
	if (ret)
		return ret;
	writel(ctrl, ps->base + PER_SPI_CTRL);

	*rx = readl(ps->base + PER_SPI_RDAT0);
	return 0;
}

static int per_spi_transfer_one(struct spi_controller *ctlr,
				struct spi_device *spi,
				struct spi_transfer *xfer)
{
	struct rtl9607dq_per_spi *ps = spi_controller_get_devdata(ctlr);
	const u8 *tx = xfer->tx_buf;
	u8 *rx = xfer->rx_buf;
	unsigned int i;
	u8 in;
	int ret;

	per_spi_setup_xfer(ps, spi, xfer->speed_hz);

	for (i = 0; i < xfer->len; i++) {
		ret = per_spi_xfer_byte(ps, tx ? tx[i] : 0, &in);
		if (ret) {
			dev_err(&ctlr->dev, "access timed out\n");
			return ret;
		}
		if (rx)
			rx[i] = in;
	}

	return 0;
}

static int rtl9607dq_per_spi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rtl9607dq_per_spi *ps;
	struct spi_controller *ctlr;
	struct clk *clk;

	ctlr = devm_spi_alloc_host(dev, sizeof(*ps));
	if (!ctlr)
		return -ENOMEM;

	ps = spi_controller_get_devdata(ctlr);
	ps->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ps->base))
		return PTR_ERR(ps->base);

	clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk), "failed to get clock\n");
	ps->rate = clk_get_rate(clk);
	if (!ps->rate)
		return dev_err_probe(dev, -EINVAL, "clock has no rate\n");

	ctlr->dev.of_node = dev->of_node;
	ctlr->num_chipselect = PER_SPI_NUM_CS;
	ctlr->mode_bits = SPI_CPOL | SPI_CPHA;
	ctlr->bits_per_word_mask = SPI_BPW_MASK(8);
	ctlr->max_speed_hz = ps->rate / 2;
	ctlr->min_speed_hz = DIV_ROUND_UP(ps->rate, PER_SPI_MAX_DIV);
	ctlr->transfer_one = per_spi_transfer_one;

	return devm_spi_register_controller(dev, ctlr);
}

static const struct of_device_id rtl9607dq_per_spi_of_match[] = {
	{ .compatible = "realtek,rtl9607dq-per-spi" },
	{ }
};
MODULE_DEVICE_TABLE(of, rtl9607dq_per_spi_of_match);

static struct platform_driver rtl9607dq_per_spi_driver = {
	.probe = rtl9607dq_per_spi_probe,
	.driver = {
		.name = "rtl9607dq-per-spi",
		.of_match_table = rtl9607dq_per_spi_of_match,
	},
};
module_platform_driver(rtl9607dq_per_spi_driver);

MODULE_DESCRIPTION("Realtek RTL9607DQ peripheral SPI controller");
MODULE_LICENSE("GPL");
