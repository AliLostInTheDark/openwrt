// SPDX-License-Identifier: BSD-2-Clause
/*
 * DUSLIC-XS controller boot and firmware patch download
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 *
 * The controller boots a firmware from ROM. A newer firmware comes as a patch
 * that the host streams into the mailbox after a reset into SPI boot mode;
 * the controller then starts it on its own. The patch file is a container
 * with one image per silicon variant, picked by the version the ROM reports.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dxs.h"

#define DXS_FW_MAGIC		0x44585346	/* "DXSF" */
#define DXS_FW_TYPE		0xd0000001
#define DXS_FW_HDR_WORDS	11
#define DXS_FW_MAX		(64 * 1024)
#define DXS_FW_TRAILER		24		/* not sent, on V1.2 images */
#define DXS_FW_BATCH		128		/* words per spi_xfer */

enum {
	DXS_FW_V11,
	DXS_FW_DXS1_V12,	/* one channel */
	DXS_FW_DXS2_V12,	/* two channels */
	DXS_FW_IMAGES,
};

static int dxs_wait_boot(struct dxs *d, unsigned int loops)
{
	uint16_t binf = 0;
	int ret;

	while (loops--) {
		ret = dxs_reg_read(d, DXS_HOST_BINF, &binf);
		if (ret)
			return ret;
		if ((binf & DXS_BINF_STATE) >= DXS_BINF_RUNNING)
			return 0;
		d->host->delay_ms(d->host->ctx, 2);
	}

	dxs_log(d, UDSP_SLIC_LOG_ERR, "controller did not boot (state %#x)",
		binf & DXS_BINF_STATE);

	return -ETIMEDOUT;
}

int dxs_version_read(struct dxs *d)
{
	const uint32_t hdr = DXS_HDR_CMD(DXS_CMD_EOP) | DXS_HDR_MOD(DXS_MOD_SYS) |
			     DXS_HDR_ECMD(DXS_ECMD_SYS_VERS) | 12;
	uint32_t data[3];
	int ret;

	ret = dxs_cmd_read(d, hdr, data, 3);
	if (ret)
		return ret;

	d->ver.dev = data[0] & 0xff;
	d->ver.channels = (data[0] >> 8) & 1 ? 1 : 2;
	d->ver.major = data[1] >> 24;
	d->ver.minor = data[1] >> 16;
	d->ver.hotfix = data[1] >> 8;
	d->ver.patch = data[1];
	d->channels = d->ver.channels;

	return 0;
}

/* Disable every line, then reset the controller core into the given boot mode. */
static int dxs_reset(struct dxs *d, uint16_t bcfg)
{
	uint32_t cmd[2];
	unsigned int ch;
	int ret;

	for (ch = 0; ch < d->channels; ch++) {
		cmd[0] = dxs_opmode_cmd(ch, DXS_OPMODE_DISABLED, &cmd[1]);
		ret = dxs_cmd_write(d, cmd);
		if (ret)
			return ret;
	}
	d->host->delay_ms(d->host->ctx, 100);

	ret = dxs_reg_write(d, DXS_HOST_BCFG, bcfg);
	if (!ret)
		ret = dxs_reg_write(d, DXS_HOST_CFG, DXS_CFG_RSTCORE | DXS_CFG_8BIT);

	return ret;
}

int dxs_boot_rom(struct dxs *d)
{
	int ret;

	ret = dxs_reset(d, DXS_BCFG_ROM);
	if (!ret)
		ret = dxs_wait_boot(d, 500);
	if (!ret)
		ret = dxs_version_read(d);

	return ret;
}

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

static int dxs_fw_read(struct dxs *d, const char *path, uint8_t **buf, size_t *len)
{
	FILE *f;
	long size;
	int ret = 0;

	f = fopen(path, "rb");
	if (!f) {
		ret = -errno;
		dxs_log(d, UDSP_SLIC_LOG_ERR, "%s: %s", path, strerror(-ret));
		return ret;
	}

	size = fseek(f, 0, SEEK_END) ? -1 : ftell(f);
	if (size <= 0 || size > DXS_FW_MAX || fseek(f, 0, SEEK_SET)) {
		ret = -EINVAL;
		goto out;
	}

	*buf = malloc(size);
	if (!*buf) {
		ret = -ENOMEM;
		goto out;
	}
	if (fread(*buf, 1, size, f) != (size_t)size) {
		free(*buf);
		ret = -EIO;
		goto out;
	}
	*len = size;
out:
	fclose(f);

	return ret;
}

static int dxs_fw_select(struct dxs *d, const uint8_t *fw, size_t len,
			 const uint8_t **img, size_t *img_len)
{
	const size_t hdr = DXS_FW_HDR_WORDS * 4;
	uint32_t off, size;
	unsigned int i;

	if (len < hdr || be32(fw) != DXS_FW_TYPE || be32(fw + 8) != DXS_FW_MAGIC ||
	    be32(fw + 4) + hdr != len)
		return -EINVAL;

	switch (d->ver.major & 0x7f) {
	case 1:
		i = DXS_FW_V11;
		break;
	case 2:
		i = d->ver.channels == 1 ? DXS_FW_DXS1_V12 : DXS_FW_DXS2_V12;
		break;
	default:
		return -ENODEV;
	}

	off = be32(fw + 20 + i * 8);
	size = be32(fw + 24 + i * 8);
	if (i != DXS_FW_V11)
		size -= DXS_FW_TRAILER;
	if (!size || size % 4 || hdr + off + size > len)
		return -EINVAL;

	*img = fw + hdr + off;
	*img_len = size;

	dxs_log(d, UDSP_SLIC_LOG_INFO, "firmware %u.%u.%u.%u, image %u, %u bytes",
		fw[12], fw[13], fw[14], fw[15], i, size);

	return 0;
}

/*
 * The controller drains the mailbox faster than the bus fills it, so the
 * patch is streamed without polling the in-box space.
 */
static int dxs_fw_stream(struct dxs *d, const uint8_t *img, size_t len)
{
	uint8_t buf[DXS_FW_BATCH * 6];
	uint32_t words[DXS_FW_BATCH];
	size_t pos = 0, n, i;
	int ret;

	while (pos < len) {
		n = (len - pos) / 4;
		if (n > DXS_FW_BATCH)
			n = DXS_FW_BATCH;
		for (i = 0; i < n; i++)
			words[i] = be32(img + pos + i * 4);

		ret = d->host->spi_xfer(d->host->ctx, buf, NULL,
					dxs_frame_words(buf, words, n));
		if (ret)
			return ret;
		pos += n * 4;
	}

	return 0;
}

int dxs_fw_load(struct dxs *d, const char *path)
{
	const uint8_t *img;
	size_t len = 0, img_len;
	uint8_t *fw = NULL;
	int ret;

	ret = dxs_fw_read(d, path, &fw, &len);
	if (ret)
		return ret;

	ret = dxs_fw_select(d, fw, len, &img, &img_len);
	if (ret) {
		dxs_log(d, UDSP_SLIC_LOG_ERR, "%s: no image for this part", path);
		goto out;
	}

	ret = dxs_reset(d, DXS_BCFG_SPI);
	if (!ret)
		ret = dxs_fw_stream(d, img, img_len);
	if (!ret)
		ret = dxs_wait_boot(d, 10);
	if (!ret)
		ret = dxs_reg_write(d, DXS_HOST_BCFG, DXS_BCFG_ROM);
	if (!ret)
		ret = dxs_version_read(d);
	if (!ret)
		d->fw_loaded = true;
out:
	free(fw);

	return ret;
}
