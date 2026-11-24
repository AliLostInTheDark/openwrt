// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) Cortina Access, Inc.
 * Copyright (C) BRULE Herman <alpha_one_x86@first-world.info>
 *
 * QSPI flash controller of Cortina Access and Realtek RTL9607DQ SoCs.
 *
 * A small PIO engine with one register window, no interrupt and no clock
 * gate.  A flash command is an "access code" (the hardware sequence,
 * ACCESS[11:8]) plus the opcode; three shapes cover the SPI-NAND op set:
 *
 *   0x0  opcode only                      (RESET, WRITE ENABLE, ...)
 *   0x5  opcode + address, no data        (PAGE READ, PROGRAM EXEC, ERASE)
 *   0xF  extended: opcode + optional address/dummy + data, with the
 *        geometry in EXT_ACCESS {opcode, dummy-1, addr-1, count-1}
 *
 * Data moves 32 bits at a time through DATA (little-endian byte order); each
 * word is clocked by writing the start bit (plus the write bit for output) to
 * BUSY and polling the busy bits clear.  On this SoC the busy poll waits for
 * bits 16 and 1, EXT_ACCESS takes the full transfer length and the TIMING
 * register is left at its reset value.
 */

#include <dm.h>
#include <errno.h>
#include <spi.h>
#include <spi-mem.h>
#include <asm/io.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>

/* register window */
#define CA_QSPI_TYPE		0x0c	/* transfer geometry */
#define CA_QSPI_BUSY		0x10	/* kick + completion status */
#define CA_QSPI_ACCESS		0x30	/* opcode / access-code / IO width */
#define CA_QSPI_EXT_ACCESS	0x34	/* extended-op geometry */
#define CA_QSPI_ADDR		0x38	/* flash address */
#define CA_QSPI_DATA		0x3c	/* 32-bit PIO data */

/* TYPE */
#define CA_QSPI_TYPE_SIZE_MASK	GENMASK(10, 9)	/* address size code: 0 = 3 bytes, 2 = 4 bytes */
#define CA_QSPI_TYPE_SIZE_4B	(2 << 9)

/* BUSY */
#define CA_QSPI_BUSY_START	BIT(1)
#define CA_QSPI_BUSY_WR		BIT(9)	/* this kick moves data OUT */
/* completion = both bits clear */
#define CA_QSPI_BUSY_READY_MASK	(BIT(16) | CA_QSPI_BUSY_START)

/* ACCESS */
#define CA_QSPI_ACC_OPCODE(op)	((op) & GENMASK(7, 0))
#define CA_QSPI_ACC_CODE(ac)	(((ac) << 8) & GENMASK(11, 8))
#define CA_QSPI_ACC_FORCE_TERM	BIT(12)
#define CA_QSPI_ACC_FORCE_BURST	BIT(13)
#define CA_QSPI_ACC_MIO_DATA	BIT(24)	/* multi-IO applies to the data phase */
#define CA_QSPI_ACC_MIO_ADDR	BIT(25)	/* ... to the address phase */
#define CA_QSPI_ACC_MIO_CMD	BIT(26)	/* ... to the command phase */
/* 0/1/2 = single/dual/quad */
#define CA_QSPI_ACC_MIO_WIDTH(w)	(((w) << 30) & GENMASK(31, 30))

/* access codes (hardware sequence selectors) */
#define CA_QSPI_AC_OP		0x0	/* bare opcode */
#define CA_QSPI_AC_OP_ADDR	0x5	/* opcode + address */
#define CA_QSPI_AC_EXTENDED	0xf	/* geometry from EXT_ACCESS */

/*
 * EXT_ACCESS: all three count fields are programmed as COUNT-1 and simply
 * wrap into their mask when the phase is absent (a 0-dummy op programs the
 * dummy field as 0x3f, a no-address op programs the address field as 7).
 * That wrap is the vendor-programmed, silicon-proven encoding for "phase
 * absent" on this hardware — do not "fix" it to 0.
 */
#define CA_QSPI_EXT_OPCODE(op)	((op) & GENMASK(7, 0))
#define CA_QSPI_EXT_DATA(n)	(((n) << 8) & GENMASK(20, 8))
#define CA_QSPI_EXT_ADDR(n)	(((n) << 21) & GENMASK(23, 21))
#define CA_QSPI_EXT_DUMMY(n)	(((n) << 24) & GENMASK(29, 24))
#define CA_QSPI_EXT_ENABLE	BIT(31)

/* largest data phase of one op; spi-mem splits anything bigger */
#define CA_QSPI_MAX_DATA	2048

/* generous per-word completion cap; a word normally completes in < 1 us */
#define CA_QSPI_POLL_TIMEOUT_US	200000

struct ca_qspi {
	void __iomem *base;
};

/* kick one controller transaction and wait for it to retire */
static int ca_qspi_kick(struct ca_qspi *qspi, bool data_out)
{
	u32 stat;

	writel(data_out ? CA_QSPI_BUSY_WR | CA_QSPI_BUSY_START :
			  CA_QSPI_BUSY_START,
	       qspi->base + CA_QSPI_BUSY);
	return readl_poll_timeout(qspi->base + CA_QSPI_BUSY, stat,
				  !(stat & CA_QSPI_BUSY_READY_MASK),
				  CA_QSPI_POLL_TIMEOUT_US);
}

static int ca_qspi_read_words(struct ca_qspi *qspi, u8 *buf, unsigned int len)
{
	while (len) {
		unsigned int n = min_t(unsigned int, len, 4);
		int ret = ca_qspi_kick(qspi, false);
		u32 data;

		if (ret)
			return ret;
		data = readl(qspi->base + CA_QSPI_DATA);
		len -= n;
		while (n--) {
			*buf++ = data & 0xff;
			data >>= 8;
		}
	}
	return 0;
}

static int ca_qspi_write_words(struct ca_qspi *qspi, const u8 *buf,
			       unsigned int len)
{
	while (len) {
		unsigned int n = min_t(unsigned int, len, 4);
		u32 data = 0;
		int i, ret;

		for (i = 0; i < n; i++)
			data |= (u32)buf[i] << (8 * i);
		buf += n;
		len -= n;
		writel(data, qspi->base + CA_QSPI_DATA);
		ret = ca_qspi_kick(qspi, true);
		if (ret)
			return ret;
	}
	return 0;
}

/* extended op: opcode + optional address/dummy + a data phase */
static int ca_qspi_exec_extended(struct ca_qspi *qspi,
				 const struct spi_mem_op *op)
{
	unsigned int dummy_clks = op->dummy.buswidth ?
		(op->dummy.nbytes * 8) / op->dummy.buswidth :
		op->dummy.nbytes * 8;
	bool burst = op->data.nbytes > 4;
	u32 val, width;
	int ret;

	writel(CA_QSPI_ACC_CODE(CA_QSPI_AC_EXTENDED),
	       qspi->base + CA_QSPI_ACCESS);

	/* like the vendor driver, only ops with more than 6 data bytes
	 * reprogram the multi-IO widths; shorter ones run single-bit
	 */
	if (op->data.nbytes > 6) {
		switch (op->data.buswidth) {
		case 1:
			width = CA_QSPI_ACC_MIO_WIDTH(0);
			break;
		case 2:
			width = CA_QSPI_ACC_MIO_WIDTH(1) | CA_QSPI_ACC_MIO_DATA;
			break;
		case 4:
			width = CA_QSPI_ACC_MIO_WIDTH(2) | CA_QSPI_ACC_MIO_DATA;
			break;
		default:
			return -EOPNOTSUPP;
		}
		if (op->addr.buswidth > 1)
			width |= CA_QSPI_ACC_MIO_ADDR;
		if (op->cmd.buswidth > 1)
			width |= CA_QSPI_ACC_MIO_CMD;
		val = readl(qspi->base + CA_QSPI_ACCESS);
		val &= ~(CA_QSPI_ACC_MIO_WIDTH(3) | CA_QSPI_ACC_MIO_DATA |
			 CA_QSPI_ACC_MIO_ADDR | CA_QSPI_ACC_MIO_CMD);
		writel(val | width, qspi->base + CA_QSPI_ACCESS);
	}

	/* hold CS across the whole multi-word data phase */
	if (burst) {
		val = readl(qspi->base + CA_QSPI_ACCESS);
		val &= ~CA_QSPI_ACC_FORCE_TERM;
		writel(val | CA_QSPI_ACC_FORCE_BURST,
		       qspi->base + CA_QSPI_ACCESS);
	}

	writel(0, qspi->base + CA_QSPI_EXT_ACCESS);
	writel(CA_QSPI_EXT_OPCODE(op->cmd.opcode) |
	       CA_QSPI_EXT_DUMMY(dummy_clks - 1) |
	       CA_QSPI_EXT_ADDR(op->addr.nbytes - 1) |
	       CA_QSPI_EXT_DATA(op->data.nbytes - 1) |	/* TAURUS: full length */
	       CA_QSPI_EXT_ENABLE,
	       qspi->base + CA_QSPI_EXT_ACCESS);

	if (op->addr.nbytes)
		writel((u32)op->addr.val, qspi->base + CA_QSPI_ADDR);

	if (op->data.dir == SPI_MEM_DATA_IN)
		ret = ca_qspi_read_words(qspi, op->data.buf.in,
					 op->data.nbytes);
	else
		ret = ca_qspi_write_words(qspi, op->data.buf.out,
					  op->data.nbytes);

	if (burst) {
		val = readl(qspi->base + CA_QSPI_ACCESS);
		val &= ~CA_QSPI_ACC_FORCE_BURST;
		writel(val | CA_QSPI_ACC_FORCE_TERM,
		       qspi->base + CA_QSPI_ACCESS);
	}
	writel(0, qspi->base + CA_QSPI_ACCESS);
	return ret;
}

/* dataless op, with or without an address phase */
static int ca_qspi_exec_simple(struct ca_qspi *qspi,
			       const struct spi_mem_op *op)
{
	u8 ac = op->addr.nbytes ? CA_QSPI_AC_OP_ADDR : CA_QSPI_AC_OP;
	int ret;

	writel(CA_QSPI_ACC_CODE(ac) | CA_QSPI_ACC_OPCODE(op->cmd.opcode),
	       qspi->base + CA_QSPI_ACCESS);

	if (op->addr.nbytes) {
		u32 val = readl(qspi->base + CA_QSPI_TYPE);

		val &= ~CA_QSPI_TYPE_SIZE_MASK;
		if (op->addr.nbytes > 3)
			val |= CA_QSPI_TYPE_SIZE_4B;
		writel(val, qspi->base + CA_QSPI_TYPE);
		writel((u32)op->addr.val, qspi->base + CA_QSPI_ADDR);
	}

	/* dataless ops kick as "write" (no data to clock in) */
	ret = ca_qspi_kick(qspi, op->data.dir != SPI_MEM_DATA_IN);

	/* the vendor driver waits after a command with an address */
	if (op->addr.nbytes)
		udelay(10);
	return ret;
}

static int ca_qspi_exec_op(struct spi_slave *slave,
			   const struct spi_mem_op *op)
{
	struct ca_qspi *qspi = dev_get_priv(slave->dev->parent);

	if (op->data.nbytes)
		return ca_qspi_exec_extended(qspi, op);
	return ca_qspi_exec_simple(qspi, op);
}

static int ca_qspi_adjust_op_size(struct spi_slave *slave,
				  struct spi_mem_op *op)
{
	op->data.nbytes = min_t(unsigned int, op->data.nbytes,
				CA_QSPI_MAX_DATA);
	return 0;
}

static const struct spi_controller_mem_ops ca_qspi_mem_ops = {
	.adjust_op_size	= ca_qspi_adjust_op_size,
	.exec_op	= ca_qspi_exec_op,
};

static int ca_qspi_claim_bus(struct udevice *dev)
{
	return 0;
}

static int ca_qspi_release_bus(struct udevice *dev)
{
	return 0;
}

static int ca_qspi_set_speed(struct udevice *dev, uint speed)
{
	return 0;
}

static int ca_qspi_set_mode(struct udevice *dev, uint mode)
{
	return 0;
}

static int ca_qspi_probe(struct udevice *dev)
{
	struct ca_qspi *qspi = dev_get_priv(dev);

	qspi->base = dev_read_addr_ptr(dev);
	if (!qspi->base)
		return -EINVAL;

	/* default to 4-byte address geometry; TIMING keeps its reset value */
	writel(CA_QSPI_TYPE_SIZE_4B, qspi->base + CA_QSPI_TYPE);

	return 0;
}

static const struct dm_spi_ops ca_qspi_ops = {
	.claim_bus	= ca_qspi_claim_bus,
	.release_bus	= ca_qspi_release_bus,
	.set_speed	= ca_qspi_set_speed,
	.set_mode	= ca_qspi_set_mode,
	.mem_ops	= &ca_qspi_mem_ops,
};

static const struct udevice_id ca_qspi_ids[] = {
	{ .compatible = "cortina,ca-qspi" },
	{ }
};

U_BOOT_DRIVER(cortina_qspi) = {
	.name		= "cortina_qspi",
	.id		= UCLASS_SPI,
	.of_match	= ca_qspi_ids,
	.ops		= &ca_qspi_ops,
	.priv_auto	= sizeof(struct ca_qspi),
	.probe		= ca_qspi_probe,
};
