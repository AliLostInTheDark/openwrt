// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015 Cortina-Access Limited
 * Copyright (C) BRULE Herman <alpha_one_x86@first-world.info>
 *
 * Minimal poll-mode I2C master for bus 0 of the Cortina-Access "BIW" (bus
 * interface wrapper) I2C controller on the Realtek RTL9607DQ, used to reach the
 * BOSA laser driver and the transceiver DDM.
 *
 * The controller is an OpenCores-style single-byte engine: one data byte per
 * TXR/RXR command, a CTRL register with start/stop/read/write/ack strobes and a
 * done flag, and an ACK register with arbitration-lost, bus-busy and RxACK
 * status. SCL prescaler: prer = pclk / (5 * SCL) - 1, with the 125 MHz APB
 * clock giving 249 for 100 kHz.
 *
 * It is not registered with the I2C subsystem: the BOSA must be programmed from
 * the GPON probe before ranging starts, and a byte at 100 kHz takes ~90 us, so
 * polling is cheap.
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mutex.h>

#include "cortina-i2c.h"

/*
 * BIW0_CFG of bus 0. The peripheral block is at 0x4_fxxx_xxxx in the 40-bit
 * physical map; reading the 32-bit alias aborts.
 */
#define CGI2C_PHYS		0x4f4329160ULL	/* PER 0x4f4329000 + 0x160 */
#define CGI2C_SIZE		0x28

#define CGI2C_PCLK_HZ		125000000	/* g3_apb_pclk */
#define CGI2C_BUS_HZ		100000		/* standard mode */

/* BIW register block (all 32-bit) */
#define BIW_CFG			0x00
#define   BIW_CFG_CORE_EN	BIT(0)
#define   BIW_CFG_SOFT_RESET	BIT(1)
#define   BIW_CFG_PRER_SHIFT	16	/* prer[31:16] = pclk/(5*SCL) - 1 */
#define BIW_CTRL		0x04
#define   BIW_CTRL_DONE		BIT(0)	/* transfer done, write-back to clear */
#define   BIW_CTRL_ACK_IN	BIT(3)	/* master drives NAK on the read byte */
#define   BIW_CTRL_WRITE	BIT(4)
#define   BIW_CTRL_READ		BIT(5)
#define   BIW_CTRL_STOP		BIT(6)
#define   BIW_CTRL_START	BIT(7)
#define BIW_TXR			0x08
#define BIW_RXR			0x0c
#define BIW_ACK			0x10
#define   BIW_ACK_AL		BIT(0)	/* arbitration lost */
#define   BIW_ACK_BUSY		BIT(1)	/* bus busy */
#define   BIW_ACK_NAK		BIT(2)	/* ack_out: 1 = slave NAKed */
#define BIW_IE0			0x14	/* left 0: poll mode, no IRQ routed */
#define BIW_INT0		0x18
#define BIW_IE1			0x1c
#define BIW_INT1		0x20
#define BIW_STAT		0x24

#define CGI2C_BYTE_TIMEOUT_US	10000	/* one byte at 100 kHz is ~90 us */
#define CGI2C_BUSY_TIMEOUT_US	100000

static void __iomem *cgi2c_base;
static DEFINE_MUTEX(cgi2c_lock);

/* Issue one command strobe and wait for the engine's done flag. */
static int cgi2c_cmd(u32 ctrl)
{
	u32 v;
	int ret;

	writel(ctrl, cgi2c_base + BIW_CTRL);
	ret = readl_poll_timeout(cgi2c_base + BIW_CTRL, v, v & BIW_CTRL_DONE,
				 10, CGI2C_BYTE_TIMEOUT_US);
	if (ret)
		return ret;
	writel(v, cgi2c_base + BIW_CTRL);	/* write-back clears done */

	v = readl(cgi2c_base + BIW_ACK);
	if (v & BIW_ACK_AL)
		return -EAGAIN;
	/* the slave ACK only matters on address/write phases */
	if (!(ctrl & BIW_CTRL_READ) && (v & BIW_ACK_NAK))
		return -ENXIO;
	return 0;
}

/* Best-effort bus release after a failed phase, so the next xfer can start. */
static void cgi2c_stop(void)
{
	cgi2c_cmd(BIW_CTRL_STOP);
}

static int cgi2c_wait_idle(void)
{
	u32 v;

	return readl_poll_timeout(cgi2c_base + BIW_ACK, v, !(v & BIW_ACK_BUSY),
				  10, CGI2C_BUSY_TIMEOUT_US);
}

/* Address phase: START (or repeated START) + slave address byte. */
static int cgi2c_start(u8 addr, bool read)
{
	writel((addr << 1) | (read ? 1 : 0), cgi2c_base + BIW_TXR);
	return cgi2c_cmd(BIW_CTRL_START | BIW_CTRL_WRITE);
}

/* Plain 2-byte {reg, val} write. */
int cg_i2c_write_byte(u8 addr, u8 reg, u8 val)
{
	int ret;

	if (!cgi2c_base)
		return -ENODEV;

	mutex_lock(&cgi2c_lock);
	ret = cgi2c_wait_idle();
	if (ret)
		goto out;
	ret = cgi2c_start(addr, false);
	if (ret)
		goto out_stop;
	writel(reg, cgi2c_base + BIW_TXR);
	ret = cgi2c_cmd(BIW_CTRL_WRITE);
	if (ret)
		goto out_stop;
	writel(val, cgi2c_base + BIW_TXR);
	ret = cgi2c_cmd(BIW_CTRL_WRITE | BIW_CTRL_STOP);
	goto out;

out_stop:
	cgi2c_stop();
out:
	mutex_unlock(&cgi2c_lock);
	return ret;
}

/*
 * Register read: reg-pointer write, then a repeated-START 1-byte read with
 * NAK+STOP on the data byte.
 */
int cg_i2c_read_byte(u8 addr, u8 reg, u8 *val)
{
	int ret;

	if (!cgi2c_base)
		return -ENODEV;

	mutex_lock(&cgi2c_lock);
	ret = cgi2c_wait_idle();
	if (ret)
		goto out;
	ret = cgi2c_start(addr, false);
	if (ret)
		goto out_stop;
	writel(reg, cgi2c_base + BIW_TXR);
	ret = cgi2c_cmd(BIW_CTRL_WRITE);
	if (ret)
		goto out_stop;
	ret = cgi2c_start(addr, true);		/* repeated START, read */
	if (ret)
		goto out_stop;
	ret = cgi2c_cmd(BIW_CTRL_READ | BIW_CTRL_ACK_IN | BIW_CTRL_STOP);
	if (ret)
		goto out;
	*val = readl(cgi2c_base + BIW_RXR) & 0xff;
	goto out;

out_stop:
	cgi2c_stop();
out:
	mutex_unlock(&cgi2c_lock);
	return ret;
}

/*
 * Map + initialize the controller: soft-reset if it was left enabled (warm
 * boot / bootloader use), program the SCL prescaler, enable the core.
 * Idempotent; interrupts stay off (poll mode).
 */
int cg_i2c_init(struct device *dev)
{
	u32 prer = CGI2C_PCLK_HZ / (5 * CGI2C_BUS_HZ) - 1;
	u32 v;

	if (cgi2c_base)
		return 0;

	cgi2c_base = devm_ioremap(dev, CGI2C_PHYS, CGI2C_SIZE);
	if (!cgi2c_base) {
		dev_err(dev, "cortina-i2c: cannot map BIW @ 0x%llx\n", CGI2C_PHYS);
		return -ENOMEM;
	}

	v = readl(cgi2c_base + BIW_CFG);
	if (v & BIW_CFG_CORE_EN) {
		writel(v | BIW_CFG_SOFT_RESET, cgi2c_base + BIW_CFG);
		msleep(50);
		writel(v & ~BIW_CFG_SOFT_RESET, cgi2c_base + BIW_CFG);
	}
	writel((prer << BIW_CFG_PRER_SHIFT) | BIW_CFG_CORE_EN,
	       cgi2c_base + BIW_CFG);
	msleep(50);
	writel(0, cgi2c_base + BIW_IE0);
	writel(0, cgi2c_base + BIW_IE1);

	dev_dbg(dev, "cortina-i2c: BIW @ 0x%llx up, %d kHz (prer=%u)\n",
		CGI2C_PHYS, CGI2C_BUS_HZ / 1000, prer);
	return 0;
}
