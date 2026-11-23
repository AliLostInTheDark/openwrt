/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2015 Cortina-Access Limited
 * Copyright (C) BRULE Herman <alpha_one_x86@first-world.info>
 *
 * Semtech GN25L95 BOSA initialization and optical diagnostics on the
 * RTL9607DQ.
 */

#ifndef _CORTINA_GPON_BOSA_H_
#define _CORTINA_GPON_BOSA_H_

#include <linux/types.h>

struct device;
struct seq_file;
struct cg_bosa_ddm;

#define CG_BOSA_CAL_SIZE	1024	/* per-unit calibration image */

int cg_bosa_init(struct device *dev, const u8 *cal);
int cg_bosa_dump(struct device *dev);
void cg_bosa_proc_show(struct device *dev, struct seq_file *m);

/*
 * Sample the SFF-8472 A2h optical diagnostics. Process context only (the I2C
 * master sleeps). Returns an enum cg_ddm_status; anything but CG_DDM_OK (0)
 * means no value may be reported.
 */
int cg_bosa_ddm_read(struct device *dev, struct cg_bosa_ddm *d);
int cg_bosa_eeprom_read(struct device *dev, u8 i2c_addr, u8 page,
			u16 offset, u16 len, u8 *data);

#endif /* _CORTINA_GPON_BOSA_H_ */
