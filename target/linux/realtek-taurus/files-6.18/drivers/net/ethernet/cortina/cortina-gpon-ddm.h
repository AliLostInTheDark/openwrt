/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2015 Cortina-Access Limited
 * Copyright (C) BRULE Herman <alpha_one_x86@first-world.info>
 *
 * SFF-8472 A2h digital diagnostic monitoring (DDM) decode and integer
 * scaling for the GPON BOSA on the RTL9607DQ. Pure functions without
 * floating point; the bus access is in cortina-gpon-bosa.c.
 *
 * The BOSA answers at I2C address 0x51 (A2h) and follows the standard A2h
 * layout: 0x00-0x4F are the alarm/warning thresholds and calibration
 * constants, 0x6E bit 6 is Soft TX Disable and 0x7F selects the page for
 * 0x80-0xFF. Registers 0x60-0x69 (bytes 96-105) are the live, internally
 * calibrated real-time diagnostics; they are un-paged, so reading them never
 * touches 0x7F. No host-side calibration is applied.
 *
 * Scaling (SFF-8472 table 9-11), with example readings from this module:
 *
 *   0x60-61  temperature   s16, 1/256 degC     raw 0x265b -> 38.36 degC
 *   0x62-63  Vcc           u16, 100 uV         raw 0x7e24 -> 3.229 V
 *   0x64-65  TX bias       u16, 2 uA           raw 0x1d7e -> 15.10 mA
 *   0x66-67  TX power      u16, 0.1 uW         raw 0x432d -> +2.35 dBm
 *   0x68-69  RX power      u16, 0.1 uW         raw 0x1652 -> -2.43 dBm
 *
 * The raw words are exported as well, so readings can be re-scaled without
 * a firmware change.
 */

#ifndef _CORTINA_GPON_DDM_H_
#define _CORTINA_GPON_DDM_H_

#include <linux/types.h>

/* SFF-8472 A2h real-time diagnostics: byte 96, ten bytes, read-only. */
#define CG_DDM_BASE		0x60
#define CG_DDM_LEN		10

/*
 * Why a sample is not usable. Anything but CG_DDM_OK means no value may be
 * printed or reported.
 */
enum cg_ddm_status {
	CG_DDM_OK = 0,
	CG_DDM_ERR_IO,		/* an i2c transfer failed */
	CG_DDM_ERR_ALL_ZERO,	/* every byte 0x00: unrouted pinmux fake-ACK */
	CG_DDM_ERR_ALL_ONES,	/* every byte 0xFF: floating bus / no device */
};

/* One decoded sample. The raw 16-bit words are kept as well. */
struct cg_bosa_ddm {
	u16	temp;		/* 0x60-61 */
	u16	vcc;		/* 0x62-63 */
	u16	bias;		/* 0x64-65 */
	u16	tx_pwr;		/* 0x66-67 */
	u16	rx_pwr;		/* 0x68-69 */
	u8	raw[CG_DDM_LEN];
	u8	status;		/* enum cg_ddm_status */
};

/*
 * An optical power no real reading can produce (raw = 1 is -40.00 dBm), used
 * for "the optic reports zero light".
 */
#define CG_DDM_CDBM_NONE	(-100000)

/*
 * Clamp for the OMCI conversion: +-60 dBm covers every physical reading and
 * keeps the G.988 0.002 dB result inside s16.
 */
#define CG_DDM_CDBM_MIN		(-6000)
#define CG_DDM_CDBM_MAX		(6000)

/*
 * Decode ten raw A2h bytes. @io_err is nonzero if any I2C transfer failed, in
 * which case @raw is ignored. Also rejects the two dead-bus signatures,
 * all-0x00 (unrouted pinmux acknowledging every transfer) and all-0xFF (no
 * device), which can only be told apart from real values over the whole block.
 *
 * Returns an enum cg_ddm_status, also stored in @d->status. On failure the
 * value fields are zero.
 */
static inline int cg_ddm_decode(const u8 *raw, int io_err, struct cg_bosa_ddm *d)
{
	unsigned int i;
	u8 and_all = 0xff, or_all = 0x00;

	d->temp = 0;
	d->vcc = 0;
	d->bias = 0;
	d->tx_pwr = 0;
	d->rx_pwr = 0;
	for (i = 0; i < CG_DDM_LEN; i++)
		d->raw[i] = 0;

	if (io_err) {
		d->status = CG_DDM_ERR_IO;
		return d->status;
	}

	for (i = 0; i < CG_DDM_LEN; i++) {
		d->raw[i] = raw[i];
		and_all &= raw[i];
		or_all |= raw[i];
	}
	if (or_all == 0x00) {
		d->status = CG_DDM_ERR_ALL_ZERO;
		return d->status;
	}
	if (and_all == 0xff) {
		d->status = CG_DDM_ERR_ALL_ONES;
		return d->status;
	}

	/* explicit byte math, so it is endian independent */
	d->temp   = ((u16)raw[0] << 8) | raw[1];
	d->vcc    = ((u16)raw[2] << 8) | raw[3];
	d->bias   = ((u16)raw[4] << 8) | raw[5];
	d->tx_pwr = ((u16)raw[6] << 8) | raw[7];
	d->rx_pwr = ((u16)raw[8] << 8) | raw[9];
	d->status = CG_DDM_OK;
	return d->status;
}

/* Temperature in milli-degrees C (s16, 1/256 degC).  Truncates toward zero. */
static inline s32 cg_ddm_temp_mdegc(u16 raw)
{
	return ((s32)(s16)raw * 1000) / 256;
}

/* Temperature in deci-degrees C, the unit /proc/gpon reports ("temp_dc="). */
static inline s32 cg_ddm_temp_dc(u16 raw)
{
	return ((s32)(s16)raw * 10) / 256;
}

/* Supply voltage in mV (100 uV units). */
static inline u32 cg_ddm_vcc_mv(u16 raw)
{
	return (u32)raw / 10;
}

/* Laser bias current in uA (2 uA units). */
static inline u32 cg_ddm_bias_ua(u16 raw)
{
	return (u32)raw * 2;
}

/*
 * 1000 * log10(x) for 1 <= x <= 65535, integer only.
 *
 * x = 2^msb * (1 + f), f in [0,1).  log10(x) = msb*log10(2) + log10(1+f); the
 * first term is msb * 0.30103 and the second comes from a 17-entry table of
 * 1000*log10(1 + k/16) with linear interpolation on 4 extra fractional bits.
 * Worst-case error over 1..65535 is 0.03 dB, well inside the +-3 dB SFF-8472
 * allows for RX power.
 */
static inline s32 cg_ddm_log10_milli(u32 x)
{
	static const u16 mant[17] = {
		0,  26,  51,  75,  97, 118, 138, 158, 176,
		194, 211, 227, 243, 258, 273, 287, 301,
	};
	u32 r, frac, k, rem;
	int msb = 0;

	if (!x)
		return 0;		/* caller must special-case zero */
	while (x >> (msb + 1))
		msb++;

	r = x - ((u32)1 << msb);		/* 0 <= r < 2^msb */
	frac = msb ? ((r << 8) >> msb) : 0;	/* mantissa fraction, /256 */
	k = frac >> 4;				/* table index 0..15 */
	rem = frac & 0xf;			/* interpolation weight /16 */

	return ((s32)msb * 30103 + 50) / 100 + mant[k] +
	       (s32)((mant[k + 1] - mant[k]) * rem) / 16;
}

/*
 * Optical power in centi-dBm from an SFF-8472 0.1 uW word.
 *
 * P_mW = raw / 10000, so dBm = 10*log10(raw) - 40 and
 * centi-dBm = 1000*log10(raw) - 4000.
 *
 * raw == 0 means the optic reports no light at all (dark fiber or TX gated);
 * that returns the CG_DDM_CDBM_NONE sentinel.
 */
static inline s32 cg_ddm_uw10_to_cdbm(u16 raw)
{
	if (!raw)
		return CG_DDM_CDBM_NONE;
	return cg_ddm_log10_milli(raw) - 4000;
}

/*
 * centi-dBm -> the G.988 ANI-G (ME 263) #10/#14 wire form: 2's complement in
 * 0.002 dB units referred to 1 mW, so dBm * 500 = centi-dBm * 5. Clamped to
 * +-60 dBm, which also maps the "no light" sentinel to the floor.
 */
static inline u16 cg_ddm_cdbm_to_omci(s32 cdbm)
{
	if (cdbm < CG_DDM_CDBM_MIN)
		cdbm = CG_DDM_CDBM_MIN;
	else if (cdbm > CG_DDM_CDBM_MAX)
		cdbm = CG_DDM_CDBM_MAX;
	return (u16)(s16)(cdbm * 5);
}

/* Human-readable reason a sample is unusable.  Never returns NULL. */
static inline const char *cg_ddm_status_str(u8 status)
{
	switch (status) {
	case CG_DDM_OK:
		return "live";
	case CG_DDM_ERR_IO:
		return "unavailable (i2c read error)";
	case CG_DDM_ERR_ALL_ZERO:
		return "unavailable (no response from the module)";
	default:
		return "unavailable (no module detected)";
	}
}

#endif /* _CORTINA_GPON_DDM_H_ */
