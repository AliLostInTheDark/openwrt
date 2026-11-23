// SPDX-License-Identifier: BSD-2-Clause
/*
 * DUSLIC-XS register and mailbox access
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 *
 * The part sits on a "CSI" bus: SPI mode 3 with chip select dropped after
 * every byte, which the udsp host does for us. A frame starts with a two-byte
 * header, the access type and the register offset. A register write repeats
 * its 16-bit value; a mailbox write carries one 32-bit word. A read is
 * answered with ten bytes, of which the first is a dummy, so one read frame
 * returns at most four data bytes.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "dxs.h"

#define DXS_CSI_WRITE		0x7e
#define DXS_CSI_READ		0xbe
#define DXS_CSI_READ_LEN	12	/* header, dummy, data */
#define DXS_CSI_WORD_LEN	6	/* header and one 32-bit word */

#define DXS_OBX_POLL_MS		2
#define DXS_OBX_TIMEOUT_MS	3000

void dxs_log(struct dxs *d, enum udsp_slic_log_level level, const char *fmt, ...)
{
	char msg[160];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);

	if (d->host && d->host->log)
		d->host->log(d->host->ctx, level, msg);
	else if (level <= UDSP_SLIC_LOG_INFO)
		fprintf(stderr, "dxs: %s\n", msg);
}

static int dxs_xfer(struct dxs *d, const uint8_t *tx, uint8_t *rx, size_t len)
{
	return d->host->spi_xfer(d->host->ctx, tx, rx, len);
}

static int dxs_read_raw(struct dxs *d, uint8_t reg, uint8_t data[4])
{
	uint8_t tx[DXS_CSI_READ_LEN] = { DXS_CSI_READ, reg };
	uint8_t rx[DXS_CSI_READ_LEN];
	int ret;

	ret = dxs_xfer(d, tx, rx, sizeof(tx));
	if (ret)
		return ret;

	memcpy(data, &rx[3], 4);

	return 0;
}

int dxs_reg_read(struct dxs *d, uint8_t reg, uint16_t *val)
{
	uint8_t data[4];
	int ret;

	ret = dxs_read_raw(d, reg, data);
	if (!ret)
		*val = data[0] << 8 | data[1];

	return ret;
}

int dxs_reg_write(struct dxs *d, uint8_t reg, uint16_t val)
{
	const uint8_t tx[] = {
		DXS_CSI_WRITE, reg, val >> 8, val & 0xff, val >> 8, val & 0xff,
	};

	return dxs_xfer(d, tx, NULL, sizeof(tx));
}

/*
 * After a reset the part answers 0xffff until its SPI interface is up; write
 * the host configuration until it reads back, as the vendor driver does.
 */
int dxs_link_check(struct dxs *d)
{
	const uint16_t cfg = DXS_CFG_SCON | DXS_CFG_8BIT;
	unsigned int i;
	uint16_t val;
	int ret;

	for (i = 0; i <= 400; i++) {
		ret = dxs_reg_write(d, DXS_HOST_CFG, cfg);
		if (!ret)
			ret = dxs_reg_read(d, DXS_HOST_CFG, &val);
		if (ret)
			return ret;
		if (val == cfg)
			return 0;
		d->host->delay_ms(d->host->ctx, 5);
	}

	dxs_log(d, UDSP_SLIC_LOG_ERR, "no answer on the bus (cfg reads %#06x)", val);

	return -ENODEV;
}

/* Frame mailbox words for one spi_xfer; returns the number of bytes. */
size_t dxs_frame_words(uint8_t *buf, const uint32_t *words, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++, buf += DXS_CSI_WORD_LEN) {
		buf[0] = DXS_CSI_WRITE;
		buf[1] = DXS_HOST_DATA;
		buf[2] = words[i] >> 24;
		buf[3] = words[i] >> 16;
		buf[4] = words[i] >> 8;
		buf[5] = words[i];
	}

	return n * DXS_CSI_WORD_LEN;
}

static int dxs_mbx_write(struct dxs *d, const uint32_t *words, size_t n)
{
	uint8_t buf[DXS_MBX_WORDS * DXS_CSI_WORD_LEN];
	uint16_t len;
	unsigned int i;
	int ret;

	if (n > DXS_MBX_WORDS)
		return -EINVAL;

	/* wait for room in the command in-box */
	for (i = 0; i < DXS_OBX_TIMEOUT_MS / DXS_OBX_POLL_MS; i++) {
		ret = dxs_reg_read(d, DXS_HOST_LEN, &len);
		if (ret)
			return ret;
		if ((len & 0xff) * 2 >= n * 4)
			break;
		d->host->delay_ms(d->host->ctx, DXS_OBX_POLL_MS);
	}
	if (i == DXS_OBX_TIMEOUT_MS / DXS_OBX_POLL_MS)
		return -ETIMEDOUT;

	return dxs_xfer(d, buf, NULL, dxs_frame_words(buf, words, n));
}

static int dxs_obx_read(struct dxs *d, uint32_t *word)
{
	uint8_t data[4];
	int ret;

	ret = dxs_read_raw(d, DXS_HOST_DATA, data);
	if (!ret)
		*word = (uint32_t)data[0] << 24 | data[1] << 16 | data[2] << 8 |
			data[3];

	return ret;
}

/* Write a command: header word plus LEN bytes of payload. */
int dxs_cmd_write(struct dxs *d, const uint32_t *cmd)
{
	return dxs_mbx_write(d, cmd, 1 + DXS_HDR_LEN(cmd[0]) / 4);
}

/* Read one out-box message; returns its word count, 0 when the box is empty. */
static int dxs_obx_msg(struct dxs *d, uint32_t *msg)
{
	uint16_t len;
	size_t i, n;
	int ret;

	ret = dxs_reg_read(d, DXS_HOST_LEN, &len);
	if (ret)
		return ret;
	if (!(len >> 8))
		return 0;

	ret = dxs_obx_read(d, &msg[0]);
	if (ret)
		return ret;
	n = DXS_HDR_LEN(msg[0]) / 4;
	if (n >= DXS_MBX_WORDS)
		return -EPROTO;
	for (i = 1; i <= n; i++) {
		ret = dxs_obx_read(d, &msg[i]);
		if (ret)
			return ret;
	}

	return n + 1;
}

/* Drain the out-box, handing every event to the line state. */
int dxs_events_poll(struct dxs *d)
{
	uint32_t msg[DXS_MBX_WORDS];
	int n;

	while ((n = dxs_obx_msg(d, msg)) > 0) {
		if (DXS_HDR_CMD_GET(msg[0]) == DXS_CMD_EVT)
			dxs_event(d, msg);
		else
			dxs_log(d, UDSP_SLIC_LOG_WARN, "stray reply %#010x", msg[0]);
	}

	return n;
}

/*
 * Read a command: send the header with RW set and collect the answer from the
 * out-box, handling the events that arrive before it.
 */
int dxs_cmd_read(struct dxs *d, uint32_t hdr, uint32_t *out, size_t words)
{
	const uint32_t req = hdr | DXS_HDR_RW;
	uint32_t msg[DXS_MBX_WORDS];
	unsigned int waited;
	int n;

	n = dxs_mbx_write(d, &req, 1);
	if (n)
		return n;

	for (waited = 0; waited < DXS_OBX_TIMEOUT_MS; waited += DXS_OBX_POLL_MS) {
		n = dxs_obx_msg(d, msg);
		if (n < 0)
			return n;
		if (!n) {
			d->host->delay_ms(d->host->ctx, DXS_OBX_POLL_MS);
			continue;
		}

		if (DXS_HDR_CMD_GET(msg[0]) == DXS_CMD_EVT) {
			dxs_event(d, msg);
			continue;
		}
		if ((msg[0] & ~DXS_HDR_LEN(~0u)) != (req & ~DXS_HDR_LEN(~0u)) ||
		    (size_t)n - 1 < words)
			return -EPROTO;

		memcpy(out, &msg[1], words * sizeof(*out));

		return 0;
	}

	return -ETIMEDOUT;
}

uint32_t dxs_sdd_hdr(unsigned int chan, unsigned int ecmd, unsigned int len)
{
	return DXS_HDR_CMD(DXS_CMD_SDD) | DXS_HDR_CHAN(chan) |
	       DXS_HDR_MOD(DXS_MOD_SDD) | DXS_HDR_ECMD(ecmd) | len;
}

uint32_t dxs_opmode_cmd(unsigned int chan, uint8_t opmode, uint32_t *payload)
{
	*payload = (uint32_t)opmode << 16;

	return dxs_sdd_hdr(chan, DXS_ECMD_SDD_OPMODE, 4);
}
