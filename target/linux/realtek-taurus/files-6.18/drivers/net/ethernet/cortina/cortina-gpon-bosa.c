// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015 Cortina-Access Limited
 * Copyright (C) BRULE Herman <alpha_one_x86@first-world.info>
 *
 * Semtech GN25L95 BOSA (burst-mode laser driver + APD/TIA) initialization for
 * the VSOL HG325AX30 (Realtek RTL9607DQ).
 *
 * The BOSA sits on the SoC per_i2c bus 0 at slave 0x51.  Out of power-on reset
 * the bias/modulation/APD tables are empty and TX is gated.  This file runs
 * the vendor rtkbosa tool's GN25L95 register sequence from the same 1 KB
 * per-unit calibration image, read from the board's "calibration" nvmem cell.
 *
 * Registers 0x00-0x7F are un-paged, 0x80-0xFF are selected by the table
 * register 0x7F.  The laser gate at the end of the sequence is TX_CTL (0x6E)
 * bit6 = 0 after a 1->0 reset pulse, with PON_CTL (0x6F) = 0.
 */

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/seq_file.h>

#include "cortina-i2c.h"
#include "cortina-gpon-bosa.h"
#include "cortina-gpon-ddm.h"

#define BOSA_I2C_ADDR		0x51	/* SFF-8472 A2h address */

/* GN25L95 registers (un-paged, 0x00-0x7F window) */
#define BOSA_REG_TX_CTL		0x6e	/* bit6 = soft TX-disable */
#define BOSA_REG_PON_CTL	0x6f	/* 0x00 = burst TX enabled */
#define BOSA_REG_PON_CTL2	0x72
#define BOSA_REG_PON_CTL3	0x78
#define BOSA_REG_PON_CTL4	0x79
#define BOSA_REG_PASSWD		0x7b	/* 0x7b-0x7e, unlock = all 0xFF */
#define BOSA_REG_PAGE		0x7f	/* table select for 0x80-0xFF */

#define BOSA_TX_SOFT_DIS	0x40	/* TX_CTL bit6 */

/* paged registers (table.reg) */
#define BOSA_T1_ALARM_EN	0xf8	/* alarm enables (0xf8/0xf9) */
#define BOSA_T1_WARN_EN		0xfc	/* warning enables (0xfc/0xfd) */
#define BOSA_T2_SAFE_MODE	0xa0	/* safe-mode start-up */
#define BOSA_T2_PASSWD_LVL	0xbb	/* bit0 = password level */
#define BOSA_SAFE_MODE_START	0x6a

/* pages (tables) of the 0x80-0xFF window */
#define BOSA_PAGE_ALARM		0x01	/* table 1: alarm/warning enables */
#define BOSA_PAGE_DEVICE	0x02	/* table 2: device settings, slopes/offsets */
#define BOSA_PAGE_BIAS_LUT	0x04	/* table 4: bias-DAC LUT */
#define BOSA_PAGE_MOD_LUT	0x05	/* table 5: modulation-DAC LUT */
#define BOSA_PAGE_APD_LUT	0x06	/* table 6: APD LUT */

#define BOSA_PEER_ADDR		0x50	/* SFF-8472 A0h address */

/*
 * Calibration-image layout: un-paged and alarm registers sit at cal[reg + 0x100],
 * table 2 at cal[reg + 0x180], tables 4/5/6 at cal[reg + 0x200/0x280/0x300].
 */
#define BOSA_CAL_A2		0x100
#define BOSA_CAL_T2		0x180
#define BOSA_CAL_T4		0x200
#define BOSA_CAL_T5		0x280
#define BOSA_CAL_T6		0x300

static int bosa_wr(struct device *dev, u8 reg, u8 val)
{
	int ret = cg_i2c_write_byte(BOSA_I2C_ADDR, reg, val);

	if (ret)
		dev_err(dev, "BOSA: write reg 0x%02x=0x%02x failed (%d)\n",
			reg, val, ret);
	return ret;
}

static int bosa_rd(struct device *dev, u8 reg, u8 *val)
{
	int ret = cg_i2c_read_byte(BOSA_I2C_ADDR, reg, val);

	if (ret)
		dev_err(dev, "BOSA: read reg 0x%02x failed (%d)\n", reg, ret);
	return ret;
}

static int bosa_rmw(struct device *dev, u8 reg, u8 clr, u8 set)
{
	u8 v;
	int ret = bosa_rd(dev, reg, &v);

	return ret ?: bosa_wr(dev, reg, (v & ~clr) | set);
}

/* Select @page and write regs 0x80-0xFF from @img (indexed by register). */
static int bosa_load_table(struct device *dev, u8 page, const u8 *img)
{
	unsigned int reg;
	int ret = bosa_wr(dev, BOSA_REG_PAGE, page);

	for (reg = 0x80; !ret && reg <= 0xff; reg++)
		ret = bosa_wr(dev, reg, img[reg]);
	return ret;
}

static int bosa_load_device(struct device *dev, const u8 *cal)
{
	static const u8 gn25l95_fixed[] = { 0x01, 0x4f, 0xd1, 0x42 };
	unsigned int reg;
	int ret = bosa_load_table(dev, BOSA_PAGE_DEVICE, cal + BOSA_CAL_T2);

	/* slopes and offsets, then the GN25L95 fixed values over d2..d5 */
	for (reg = 0xd2; !ret && reg < 0xe8; reg++)
		ret = bosa_wr(dev, reg, cal[reg + BOSA_CAL_T2]);
	for (reg = 0; !ret && reg < ARRAY_SIZE(gn25l95_fixed); reg++)
		ret = bosa_wr(dev, 0xd2 + reg, gn25l95_fixed[reg]);
	return ret;
}

static int bosa_load_alarm_levels(struct device *dev, const u8 *cal)
{
	unsigned int reg;
	int ret = 0;

	for (reg = 0; !ret && reg < 0x50; reg++)
		ret = bosa_wr(dev, reg, cal[reg + BOSA_CAL_A2]);
	return ret;
}

static int bosa_set_pon_control(struct device *dev, const u8 *cal)
{
	return bosa_wr(dev, BOSA_REG_PON_CTL, cal[BOSA_CAL_A2 + BOSA_REG_PON_CTL]) ?:
	       bosa_wr(dev, BOSA_REG_PON_CTL2, cal[BOSA_CAL_A2 + BOSA_REG_PON_CTL2]) ?:
	       bosa_wr(dev, BOSA_REG_PON_CTL3, cal[BOSA_CAL_A2 + BOSA_REG_PON_CTL3]) ?:
	       bosa_wr(dev, BOSA_REG_PON_CTL4, cal[BOSA_CAL_A2 + BOSA_REG_PON_CTL4]);
}

static int bosa_enable_alarms(struct device *dev, const u8 *cal)
{
	return bosa_wr(dev, BOSA_REG_PAGE, BOSA_PAGE_ALARM) ?:
	       bosa_wr(dev, BOSA_T1_ALARM_EN, cal[BOSA_CAL_A2 + BOSA_T1_ALARM_EN]) ?:
	       bosa_rmw(dev, BOSA_T1_ALARM_EN + 1, 0, 0xc0) ?:
	       bosa_wr(dev, BOSA_T1_WARN_EN, cal[BOSA_CAL_A2 + BOSA_T1_WARN_EN]) ?:
	       bosa_rmw(dev, BOSA_T1_WARN_EN + 1, 0, 0xc0);
}

/* unlock the password level and clear bit0 of the level register */
static int bosa_unlock(struct device *dev, const u8 *cal)
{
	unsigned int reg;
	u8 v;
	int ret = 0;

	for (reg = BOSA_REG_PASSWD; !ret && reg < BOSA_REG_PASSWD + 4; reg++)
		ret = bosa_wr(dev, reg, 0xff);

	ret = ret ?: bosa_wr(dev, BOSA_REG_PAGE, BOSA_PAGE_DEVICE) ?:
	      bosa_rmw(dev, BOSA_T2_PASSWD_LVL, 0x01, 0) ?:
	      bosa_rd(dev, BOSA_T2_PASSWD_LVL, &v);
	if (!ret && (v & 0x01)) {
		dev_err(dev, "BOSA: password level did not latch (0xbb=0x%02x)\n", v);
		return -EIO;
	}

	/* GN25L95 tail: enable register, and the peer page register on 0x50 */
	return ret ?: bosa_wr(dev, BOSA_REG_PAGE, BOSA_PAGE_DEVICE) ?:
	       bosa_wr(dev, 0xc4, 0x7f) ?:
	       cg_i2c_write_byte(BOSA_PEER_ADDR, BOSA_REG_PAGE, cal[BOSA_REG_PAGE]);
}

/*
 * Program the GN25L95 from the per-unit calibration image, in the vendor tool's
 * order. Runs once per boot. Process context only (the I2C master sleeps).
 */
int cg_bosa_init(struct device *dev, const u8 *cal)
{
	static bool programmed;
	u8 id, tx, pon;
	int ret;

	if (programmed)
		return 0;

	ret = cg_i2c_init(dev);
	if (ret)
		return ret;

	if (!cal) {
		dev_err(dev, "BOSA: no calibration data - laser stays unprogrammed\n");
		return -ENOENT;
	}

	/* the GN25L95 reports 0xAx in the high nibble of table 2 register 0xd1 */
	ret = bosa_wr(dev, BOSA_REG_PAGE, BOSA_PAGE_DEVICE) ?:
	      bosa_rd(dev, 0xd1, &id);
	if (ret)
		return ret;
	if ((id & 0xf0) != 0xa0) {
		dev_err(dev, "BOSA: device id 0x%02x is not a GN25L95\n", id);
		return -ENODEV;
	}

	ret = bosa_load_table(dev, BOSA_PAGE_BIAS_LUT, cal + BOSA_CAL_T4) ?:
	      bosa_load_table(dev, BOSA_PAGE_MOD_LUT, cal + BOSA_CAL_T5) ?:
	      bosa_load_table(dev, BOSA_PAGE_APD_LUT, cal + BOSA_CAL_T6) ?:
	      bosa_load_device(dev, cal) ?:
	      bosa_load_alarm_levels(dev, cal) ?:
	      bosa_set_pon_control(dev, cal) ?:
	      bosa_enable_alarms(dev, cal) ?:
	      /* soft TX-disable reset pulse */
	      bosa_rmw(dev, BOSA_REG_TX_CTL, 0, BOSA_TX_SOFT_DIS) ?:
	      bosa_rmw(dev, BOSA_REG_TX_CTL, BOSA_TX_SOFT_DIS, 0) ?:
	      bosa_wr(dev, BOSA_REG_PAGE, BOSA_PAGE_DEVICE) ?:
	      bosa_wr(dev, BOSA_T2_SAFE_MODE, BOSA_SAFE_MODE_START) ?:
	      bosa_unlock(dev, cal);
	if (ret)
		return ret;

	tx = 0xff;
	pon = 0xff;
	bosa_rd(dev, BOSA_REG_TX_CTL, &tx);
	bosa_rd(dev, BOSA_REG_PON_CTL, &pon);
	dev_dbg(dev, "BOSA: GN25L95 programmed, tx_ctl=0x%02x pon_ctl=0x%02x\n",
		tx, pon);
	if ((tx & BOSA_TX_SOFT_DIS) || pon)
		dev_warn(dev, "BOSA: laser gate NOT open after init\n");
	programmed = true;
	return 0;
}

/*
 * Dump all GN25L95 registers to the kernel log (`echo 'bosa dump' >
 * /proc/gpon`). Reads 0x00-0xFF of tables 0-6 and 0x80/0x81/0x86/0x87/0xFF;
 * only the table-select register 0x7F is written, and it is restored. About 3k
 * reads, ~1.5 s.
 */
int cg_bosa_dump(struct device *dev)
{
	static const u8 pages[] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
				    0x80, 0x81, 0x86, 0x87, 0xff };
	char line[3 * 16 + 1];
	u8 curpage = 0xee, v;
	unsigned int p, r, i, rd_fail = 0;
	int ret;

	ret = cg_i2c_init(dev);
	if (ret)
		return ret;

	ret = bosa_rd(dev, BOSA_REG_PAGE, &curpage);
	if (ret) {
		dev_err(dev, "BOSA dump: no ACK from 0x%02x (%d)\n",
			BOSA_I2C_ADDR, ret);
		return ret;
	}
	dev_dbg(dev, "BOSADUMP start, curpage=0x%02x\n", curpage);

	for (p = 0; p < ARRAY_SIZE(pages); p++) {
		ret = bosa_wr(dev, BOSA_REG_PAGE, pages[p]);
		if (ret)
			break;
		for (r = 0; r < 0x100; r += 16) {
			for (i = 0; i < 16; i++) {
				if (cg_i2c_read_byte(BOSA_I2C_ADDR, r + i, &v)) {
					rd_fail++;
					sprintf(line + 3 * i, " ??");
				} else {
					sprintf(line + 3 * i, " %02x", v);
				}
			}
			dev_dbg(dev, "BOSADUMP p%02x %02x:%s\n",
				pages[p], r, line);
		}
	}

	bosa_wr(dev, BOSA_REG_PAGE, curpage);
	dev_dbg(dev, "BOSADUMP %s, rd_fail=%u (page restored to 0x%02x)\n",
		ret ? "FAILED" : "done", rd_fail, curpage);
	return ret;
}

/*
 * Read the SFF-8472 A2h real-time diagnostics (regs 0x60-0x69): temperature,
 * Vcc, TX bias, TX power and RX power. Scaling is in cortina-gpon-ddm.h.
 *
 * Sleeps: process context only, never from the OMCI RX path, IRQ, softirq or
 * under a spinlock. The registers are un-paged, so the table select is left
 * alone. Ten reads take about 1 ms, so values are sampled on demand.
 *
 * Returns an enum cg_ddm_status (CG_DDM_OK == 0).
 */
int cg_bosa_ddm_read(struct device *dev, struct cg_bosa_ddm *d)
{
	u8 raw[CG_DDM_LEN] = { 0 };
	unsigned int i;
	int io_err = cg_i2c_init(dev) ? 1 : 0;

	for (i = 0; !io_err && i < CG_DDM_LEN; i++) {
		if (cg_i2c_read_byte(BOSA_I2C_ADDR, CG_DDM_BASE + i, &raw[i])) {
			io_err = 1;
			break;
		}
	}

	if (cg_ddm_decode(raw, io_err, d) != CG_DDM_OK)
		dev_dbg_ratelimited(dev, "optical diagnostics %s\n",
				    cg_ddm_status_str(d->status));
	return d->status;
}

/*
 * /proc/gpon: read back the un-paged laser gate registers. The laser bursts
 * when tx_ctl bit 6 is 0 and pon_ctl is 0.
 */
void cg_bosa_proc_show(struct device *dev, struct seq_file *m)
{
	u8 tx = 0xff, pon = 0xff;

	if (cg_i2c_init(dev) ||
	    cg_i2c_read_byte(BOSA_I2C_ADDR, BOSA_REG_TX_CTL, &tx) ||
	    cg_i2c_read_byte(BOSA_I2C_ADDR, BOSA_REG_PON_CTL, &pon)) {
		seq_puts(m, "bosa           = i2c read FAILED\n");
		return;
	}
	seq_printf(m, "bosa           = tx_ctl(6e)=0x%02x pon_ctl(6f)=0x%02x  (laser gate open = bit6:0 / 0x00)\n",
		   tx, pon);
}

/*
 * Standard SFF-8472 EEPROM / DDM page reader for ethtool_ops
 * (.get_module_info / .get_module_eeprom / .get_module_eeprom_by_page).
 */
int cg_bosa_eeprom_read(struct device *dev, u8 i2c_addr, u8 page,
			u16 offset, u16 len, u8 *data)
{
	u8 curpage = 0;
	bool paged;
	u16 i;
	int ret;

	if (i2c_addr != BOSA_PEER_ADDR && i2c_addr != BOSA_I2C_ADDR)
		return -EINVAL;
	if (!len || offset + len > 256)
		return -EINVAL;

	ret = cg_i2c_init(dev);
	if (ret)
		return ret;

	paged = (i2c_addr == BOSA_I2C_ADDR && offset + len > 0x80 && page != 0);
	if (paged) {
		ret = cg_i2c_read_byte(BOSA_I2C_ADDR, BOSA_REG_PAGE, &curpage);
		if (ret)
			return ret;
		if (curpage != page) {
			ret = cg_i2c_write_byte(BOSA_I2C_ADDR, BOSA_REG_PAGE, page);
			if (ret)
				return ret;
		}
	}

	for (i = 0; i < len; i++) {
		ret = cg_i2c_read_byte(i2c_addr, offset + i, &data[i]);
		if (ret)
			break;
	}

	if (paged && curpage != page)
		cg_i2c_write_byte(BOSA_I2C_ADDR, BOSA_REG_PAGE, curpage);

	return ret ?: len;
}
