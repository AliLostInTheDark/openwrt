/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2015 Cortina-Access Limited
 * Copyright (C) BRULE Herman <alpha_one_x86@first-world.info>
 *
 * Minimal poll-mode I2C master for the Cortina-Access "BIW" controller on
 * the Realtek RTL9607DQ.
 */

#ifndef _CORTINA_I2C_H_
#define _CORTINA_I2C_H_

#include <linux/types.h>

struct device;

int cg_i2c_init(struct device *dev);
int cg_i2c_write_byte(u8 addr, u8 reg, u8 val);
int cg_i2c_read_byte(u8 addr, u8 reg, u8 *val);

#endif /* _CORTINA_I2C_H_ */
