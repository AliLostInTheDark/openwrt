/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 *
 * Minimal Cortina QSPI PIO SPI-NAND + FIP loader for BL33 when BL31 is
 * launched by the vendor BL2 (which loads only BL31 and hands off the active
 * vs standby partition flag in SRAM at PLAT_TAURUS_FIP_FLAG_ADDR).
 */

#include <stdint.h>
#include <string.h>

#include <common/debug.h>
#include <drivers/delay_timer.h>
#include <lib/mmio.h>

#include <platform_def.h>

/* Cortina QSPI register offsets at TAURUS_QSPI_BASE (0xf4324000) */
#define CA_QSPI_TYPE		0x0c
#define CA_QSPI_BUSY		0x10
#define CA_QSPI_ACCESS		0x30
#define CA_QSPI_EXT_ACCESS	0x34
#define CA_QSPI_ADDR		0x38
#define CA_QSPI_DATA		0x3c

#define CA_QSPI_BUSY_START	BIT_32(1)
#define CA_QSPI_BUSY_READY_MASK	(BIT_32(16) | CA_QSPI_BUSY_START)

#define CA_QSPI_AC_OP_ADDR	0x5U
#define CA_QSPI_AC_EXTENDED	0xfU

#define CA_QSPI_ACC_FORCE_TERM	BIT_32(12)
#define CA_QSPI_ACC_FORCE_BURST	BIT_32(13)

#define CA_QSPI_EXT_OPCODE(op)	((uint32_t)(op) & 0xffU)
#define CA_QSPI_EXT_DATA(n)	(((uint32_t)(n) & 0x1fffU) << 8)
#define CA_QSPI_EXT_ADDR(n)	(((uint32_t)(n) & 0x7U) << 21)
#define CA_QSPI_EXT_DUMMY(n)	(((uint32_t)(n) & 0x3fU) << 24)
#define CA_QSPI_EXT_ENABLE	BIT_32(31)

#define SPINAND_PAGE_SIZE	2048U
#define FIP_TOC_HEADER_MAGIC	0xAA640001U

/* UUID a7eed0d6-eafc-4bd5-9782-9934f234b6e4 (NT_FW_CONFIG / BL33) */
static const uint8_t bl33_uuid[16] = {
	0xd6, 0xd0, 0xee, 0xa7, 0xfc, 0xea, 0xd5, 0x4b,
	0x97, 0x82, 0x99, 0x34, 0xf2, 0x34, 0xb6, 0xe4
};

/* Kept in .bss because PLATFORM_STACK_SIZE is 4 KiB */
static uint8_t spinand_page_buf[SPINAND_PAGE_SIZE];
static uint8_t fip_hdr_buf[SPINAND_PAGE_SIZE];

static int qspi_wait_ready(void)
{
	unsigned int timeout = 200000;

	while (mmio_read_32(TAURUS_QSPI_BASE + CA_QSPI_BUSY) &
	       CA_QSPI_BUSY_READY_MASK) {
		if (--timeout == 0U)
			return -1;
		udelay(1);
	}
	return 0;
}

static int spinand_page_read(uint32_t page)
{
	uint32_t status;
	unsigned int tries = 20000;

	/* PAGE READ TO CACHE (0x13), 3-byte row address */
	mmio_write_32(TAURUS_QSPI_BASE + CA_QSPI_ADDR, page);
	mmio_write_32(TAURUS_QSPI_BASE + CA_QSPI_ACCESS,
		      (CA_QSPI_AC_OP_ADDR << 8) | 0x13U);
	mmio_write_32(TAURUS_QSPI_BASE + CA_QSPI_EXT_ACCESS, 0U);
	mmio_write_32(TAURUS_QSPI_BASE + CA_QSPI_BUSY, CA_QSPI_BUSY_START);
	if (qspi_wait_ready() != 0)
		return -1;

	/* Poll GET FEATURE (0x0F, addr 0xC0) until OIP (bit 0) clears */
	do {
		mmio_write_32(TAURUS_QSPI_BASE + CA_QSPI_ADDR, 0xc0U);
		mmio_write_32(TAURUS_QSPI_BASE + CA_QSPI_ACCESS,
			      (CA_QSPI_AC_EXTENDED << 8) | 0x0fU);
		mmio_write_32(TAURUS_QSPI_BASE + CA_QSPI_EXT_ACCESS,
			      CA_QSPI_EXT_ENABLE |
			      CA_QSPI_EXT_DUMMY(0x3fU) |
			      CA_QSPI_EXT_ADDR(0U) |
			      CA_QSPI_EXT_DATA(0U) |
			      CA_QSPI_EXT_OPCODE(0x0fU));
		mmio_write_32(TAURUS_QSPI_BASE + CA_QSPI_BUSY,
			      CA_QSPI_BUSY_START);
		if (qspi_wait_ready() != 0)
			return -1;
		status = mmio_read_32(TAURUS_QSPI_BASE + CA_QSPI_DATA) & 0xffU;
		if ((status & 1U) == 0U)
			return 0;
		udelay(2);
	} while (--tries > 0U);

	return -1;
}

static int spinand_read_cache(uint32_t col, uint8_t *dst, size_t len)
{
	uint32_t word;
	size_t i = 0;

	mmio_write_32(TAURUS_QSPI_BASE + CA_QSPI_ACCESS,
		      (CA_QSPI_AC_EXTENDED << 8) |
		      ((len > 4U) ? CA_QSPI_ACC_FORCE_BURST : 0U));
	mmio_write_32(TAURUS_QSPI_BASE + CA_QSPI_EXT_ACCESS, 0U);
	mmio_write_32(TAURUS_QSPI_BASE + CA_QSPI_EXT_ACCESS,
		      CA_QSPI_EXT_ENABLE |
		      CA_QSPI_EXT_DUMMY(7U) |		/* 8 dummy clocks */
		      CA_QSPI_EXT_ADDR(1U) |		/* 2-byte col addr */
		      CA_QSPI_EXT_DATA(len - 1U) |
		      CA_QSPI_EXT_OPCODE(0x03U));
	mmio_write_32(TAURUS_QSPI_BASE + CA_QSPI_ADDR, col);

	while (i < len) {
		size_t chunk = (len - i > 4U) ? 4U : (len - i);

		mmio_write_32(TAURUS_QSPI_BASE + CA_QSPI_BUSY,
			      CA_QSPI_BUSY_START);
		if (qspi_wait_ready() != 0)
			return -1;
		word = mmio_read_32(TAURUS_QSPI_BASE + CA_QSPI_DATA);
		memcpy(dst + i, &word, chunk);
		i += chunk;
	}

	if (len > 4U) {
		mmio_write_32(TAURUS_QSPI_BASE + CA_QSPI_ACCESS,
			      (CA_QSPI_AC_EXTENDED << 8) |
			      CA_QSPI_ACC_FORCE_TERM);
	}
	mmio_write_32(TAURUS_QSPI_BASE + CA_QSPI_ACCESS, 0U);
	return 0;
}

static int spinand_read(uint32_t flash_off, uint8_t *dst, size_t len)
{
	uint32_t cur_page = UINT32_MAX;

	while (len > 0U) {
		uint32_t page = flash_off / SPINAND_PAGE_SIZE;
		uint32_t col = flash_off % SPINAND_PAGE_SIZE;
		size_t chunk = SPINAND_PAGE_SIZE - col;

		if (chunk > len)
			chunk = len;
		if (page != cur_page) {
			if (spinand_page_read(page) != 0 ||
			    spinand_read_cache(0U, spinand_page_buf,
					       SPINAND_PAGE_SIZE) != 0)
				return -1;
			cur_page = page;
		}
		memcpy(dst, spinand_page_buf + col, chunk);
		dst += chunk;
		flash_off += chunk;
		len -= chunk;
	}
	return 0;
}

static int load_from_fip(uint32_t fip_base, uintptr_t dst, size_t max_size,
			 size_t *out_size)
{
	uint32_t magic;
	size_t pos = 16;

	if (spinand_read(fip_base, fip_hdr_buf, sizeof(fip_hdr_buf)) != 0)
		return -1;

	memcpy(&magic, fip_hdr_buf, sizeof(magic));
	if (magic != FIP_TOC_HEADER_MAGIC)
		return -1;

	while (pos + 40U <= sizeof(fip_hdr_buf)) {
		uint64_t off, sz;
		bool zero = true;

		for (size_t k = 0; k < 16U; k++) {
			if (fip_hdr_buf[pos + k] != 0U) {
				zero = false;
				break;
			}
		}
		if (zero)
			break;

		memcpy(&off, fip_hdr_buf + pos + 16U, sizeof(off));
		memcpy(&sz, fip_hdr_buf + pos + 24U, sizeof(sz));
		if (memcmp(fip_hdr_buf + pos, bl33_uuid, 16) == 0) {
			if (sz == 0U || sz > max_size)
				return -1;
			if (spinand_read(fip_base + (uint32_t)off,
					 (uint8_t *)dst, (size_t)sz) != 0)
				return -1;
			*out_size = (size_t)sz;
			return 0;
		}
		pos += 40U;
	}

	return -1;
}

int taurus_load_bl33(uintptr_t dst, size_t max_size, size_t *out_size)
{
	uint32_t flag = mmio_read_32(PLAT_TAURUS_FIP_FLAG_ADDR);
	bool standby = (flag != 1U);
	uint32_t primary = standby ? TAURUS_FIP1_OFFSET : TAURUS_FIP0_OFFSET;
	uint32_t fallback = standby ? TAURUS_FIP0_OFFSET : TAURUS_FIP1_OFFSET;

	NOTICE("BL31: loading BL33 from %s (0x%x, flag=0x%x)\n",
	       standby ? "fip1" : "fip0", primary, flag);
	if (load_from_fip(primary, dst, max_size, out_size) == 0)
		return 0;

	WARN("BL31: primary FIP failed, trying %s (0x%x)\n",
	     standby ? "fip0" : "fip1", fallback);
	return load_from_fip(fallback, dst, max_size, out_size);
}
