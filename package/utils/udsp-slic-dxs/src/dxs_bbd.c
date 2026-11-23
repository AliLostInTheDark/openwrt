// SPDX-License-Identifier: BSD-2-Clause
/*
 * DUSLIC-XS line coefficients from a BBD file
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 *
 * A BBD file is a list of tagged blocks: tag and version (16 bits each), a
 * 32-bit length and the data, all big endian, closed by an end block. The
 * DUSLIC-XS blocks carry the AC coefficients (CRAM), the ringer, the DC feed,
 * the DC/DC converter and the message waiting lamp settings of one line.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dxs.h"

#define BBD_MAX			4096
#define BBD_MASTER_MAGIC	0x21626264	/* "!bbd" */
#define BBD_DXS_MAGIC		0x44585321	/* "DXS!" */

#define BBD_END			0x0000
#define BBD_MASTER		0x000a
#define BBD_DXS_CRAM		0x1001
#define BBD_DXS_RING		0x1003
#define BBD_DXS_BASIC		0x1005
#define BBD_DXS_MWL		0x1008
#define BBD_DXS_DCDC		0x1009
#define BBD_DXS_CRAM2		0x1020

#define DXS_COEFF_ACSW		0	/* AC coefficients */
#define DXS_COEFF_ACDC		1	/* second coefficient set */
#define DXS_COEFF_MAX		28	/* payload bytes per command */

static uint16_t be16(const uint8_t *p)
{
	return p[0] << 8 | p[1];
}

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

/* Pack big-endian bytes into payload words; a short tail is zero padded. */
static void dxs_pack(uint32_t *words, const uint8_t *data, size_t len)
{
	size_t i;

	memset(words, 0, (len + 3) / 4 * 4);
	for (i = 0; i < len; i++)
		words[i / 4] |= (uint32_t)data[i] << (24 - 8 * (i % 4));
}

/*
 * A CRAM block is a start offset, the 16-bit coefficients and a CRC. Each
 * command opens with one 16-bit instruction word, destination, offset and
 * count, followed by up to 13 coefficients.
 */
static int dxs_bbd_cram(struct dxs *d, unsigned int chan, unsigned int dest,
			const uint8_t *data, size_t len)
{
	uint32_t cmd[1 + DXS_COEFF_MAX / 4];
	uint8_t buf[DXS_COEFF_MAX];
	unsigned int offset, pos = 0, count, n, bytes;
	int ret;

	if (len < 6)
		return -EINVAL;
	offset = be16(data);
	count = (len - 4) / 2;
	data += 2;

	while (count) {
		bytes = count > DXS_COEFF_MAX / 2 - 1 ? DXS_COEFF_MAX : (count + 1) * 2;
		bytes -= bytes % 4;
		n = bytes / 2 - 1;

		buf[0] = dest << 7 | (offset + pos) >> 3;
		buf[1] = ((offset + pos) & 7) << 5 | n;
		memcpy(&buf[2], &data[pos * 2], n * 2);

		cmd[0] = dxs_sdd_hdr(chan, DXS_ECMD_SDD_COEFF, bytes);
		dxs_pack(&cmd[1], buf, bytes);
		ret = dxs_cmd_write(d, cmd);
		if (ret)
			return ret;

		pos += n;
		count -= n;
	}

	return 0;
}

static int dxs_bbd_ring(struct dxs *d, unsigned int chan, const uint8_t *data,
			size_t len)
{
	uint32_t cmd[1 + 5];
	uint8_t buf[20];

	if (len < sizeof(buf))
		return -EINVAL;

	/* trip type, waveform and crest factor, then the 16-bit settings */
	memcpy(buf, data, sizeof(buf));
	buf[0] &= 0x3f;
	buf[1] = 0;
	buf[16] = buf[17] = 0;

	cmd[0] = dxs_sdd_hdr(chan, DXS_ECMD_SDD_RING, sizeof(buf));
	dxs_pack(&cmd[1], buf, sizeof(buf));

	return dxs_cmd_write(d, cmd);
}

static int dxs_bbd_basic(struct dxs *d, unsigned int chan, const uint8_t *data,
			 size_t len)
{
	uint32_t cmd[1 + 5];
	int ret;

	if (len < 20)
		return -EINVAL;

	cmd[0] = dxs_sdd_hdr(chan, DXS_ECMD_SDD_BASIC, 20);
	ret = dxs_cmd_read(d, cmd[0], &cmd[1], 5);
	if (ret)
		return ret;

	/* duplication counts, auto bias */
	cmd[1] &= 0xf00f00df;
	cmd[1] |= (uint32_t)(data[0] & 0x0f) << 24 | (uint32_t)(data[1] >> 4) << 20 |
		  (uint32_t)(data[2] >> 4) << 12 | (data[2] & 0x0f) << 8 |
		  (data[3] & 0x20);
	/* clamping, DC/DC overhead, standby voltage, TTX burst length */
	cmd[2] &= 0x00010000;
	cmd[2] |= (uint32_t)(data[4] >> 4) << 28 | (uint32_t)(data[4] & 0x0f) << 24 |
		  (uint32_t)((data[5] & 0xf1) >> 3) << 19 |
		  (uint32_t)((data[5] & 0x06) >> 1) << 17 | be16(&data[6]);
	cmd[3] = (uint32_t)be16(&data[8]) << 16 | be16(&data[10]);
	cmd[4] = (cmd[4] & 0xffff) | (uint32_t)be16(&data[12]) << 16;
	cmd[5] = be32(&data[16]);

	return dxs_cmd_write(d, cmd);
}

static int dxs_bbd_copy(struct dxs *d, unsigned int chan, unsigned int ecmd,
			const uint8_t *data, size_t len, size_t need)
{
	uint32_t cmd[1 + 2];

	if (len < need)
		return -EINVAL;

	cmd[0] = dxs_sdd_hdr(chan, ecmd, need);
	dxs_pack(&cmd[1], data, need);

	return dxs_cmd_write(d, cmd);
}

static int dxs_bbd_block(struct dxs *d, unsigned int chan, uint16_t tag,
			 const uint8_t *data, size_t len)
{
	switch (tag) {
	case BBD_DXS_CRAM:
		return dxs_bbd_cram(d, chan, DXS_COEFF_ACSW, data, len);
	case BBD_DXS_CRAM2:
		return dxs_bbd_cram(d, chan, DXS_COEFF_ACDC, data, len);
	case BBD_DXS_RING:
		return dxs_bbd_ring(d, chan, data, len);
	case BBD_DXS_BASIC:
		return dxs_bbd_basic(d, chan, data, len);
	case BBD_DXS_DCDC:
		/* an IBB converter: no charge pump to start */
		if (len >= 2 && data[1] >> 4)
			return -EOPNOTSUPP;
		return dxs_bbd_copy(d, chan, DXS_ECMD_SDD_DCDC, data, len, 8);
	case BBD_DXS_MWL:
		return dxs_bbd_copy(d, chan, DXS_ECMD_SDD_MWL, data, len, 8);
	default:
		dxs_log(d, UDSP_SLIC_LOG_DEBUG, "bbd block %#06x skipped", tag);
		return 0;
	}
}

static int dxs_bbd_parse(struct dxs *d, unsigned int chan, const uint8_t *bbd,
			 size_t size)
{
	size_t pos = 0, len;
	uint16_t tag;
	int ret;

	if (size < 16 || be16(bbd) != BBD_MASTER ||
	    be32(bbd + 8) != BBD_MASTER_MAGIC || be32(bbd + 12) != BBD_DXS_MAGIC)
		return -EINVAL;

	while (pos + 8 <= size) {
		tag = be16(bbd + pos);
		len = be32(bbd + pos + 4);
		pos += 8;
		if (tag == BBD_END)
			return 0;
		if (len > size - pos)
			return -EINVAL;

		ret = dxs_bbd_block(d, chan, tag, bbd + pos, len);
		if (ret) {
			dxs_log(d, UDSP_SLIC_LOG_ERR, "bbd block %#06x: %d", tag, ret);
			return ret;
		}
		pos += len;
	}

	return -EINVAL;
}

int dxs_bbd_load(struct dxs *d, unsigned int chan, const char *path)
{
	uint8_t *bbd;
	size_t size;
	FILE *f;
	int ret;

	f = fopen(path, "rb");
	if (!f) {
		ret = -errno;
		dxs_log(d, UDSP_SLIC_LOG_ERR, "%s: %s", path, strerror(errno));
		return ret;
	}

	bbd = malloc(BBD_MAX);
	if (!bbd) {
		fclose(f);
		return -ENOMEM;
	}
	size = fread(bbd, 1, BBD_MAX, f);
	fclose(f);

	ret = dxs_bbd_parse(d, chan, bbd, size);
	free(bbd);
	if (ret)
		return ret;

	d->line[chan].bbd_loaded = true;

	return dxs_calibrate(d, chan);
}
