// SPDX-License-Identifier: BSD-2-Clause
/*
 * udsp SLIC plug-in for the MaxLinear DUSLIC-XS (PEF3200x)
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dxs.h"

#ifndef DXS_FW_PATH
#define DXS_FW_PATH	"/lib/firmware/dxs/fw_dxs.bin"
#endif
#ifndef DXS_BBD_PATH
#define DXS_BBD_PATH	"/lib/firmware/dxs/bbd.bin"
#endif

struct udsp_slic {
	struct dxs dxs;
	struct udsp_slic_safe safe;
};

/* Every line disabled: the part opens tip and ring and stops ringing. */
static void dxs_safe_build(struct udsp_slic *s)
{
	uint32_t cmd[DXS_CHANNELS_MAX * 2];
	unsigned int ch;

	for (ch = 0; ch < s->dxs.channels; ch++)
		cmd[ch * 2] = dxs_opmode_cmd(ch, DXS_OPMODE_DISABLED, &cmd[ch * 2 + 1]);

	s->safe.len = dxs_frame_words(s->safe.bytes, cmd, s->dxs.channels * 2);
}

static struct udsp_slic *dxs_attach(const struct udsp_slic_host *host,
				    unsigned *channels)
{
	struct udsp_slic *s;
	struct dxs *d;

	if (!host)
		return NULL;

	s = calloc(1, sizeof(*s));
	if (!s)
		return NULL;
	d = &s->dxs;
	d->host = host;

	if (dxs_link_check(d) || dxs_version_read(d)) {
		free(s);
		return NULL;
	}

	snprintf(d->name, sizeof(d->name), "pef3200%u", d->channels);
	dxs_log(d, UDSP_SLIC_LOG_INFO, "%s rev %u, %s firmware %u.%u.%u.%u",
		d->name, d->ver.dev & 0x1f, d->ver.major & 0x80 ? "test" : "rom",
		d->ver.major & 0x7f, d->ver.minor, d->ver.hotfix, d->ver.patch);

	dxs_safe_build(s);
	*channels = d->channels;

	return s;
}

static void dxs_detach(struct udsp_slic *s)
{
	free(s);
}

static int dxs_init(struct udsp_slic *s, unsigned chan)
{
	struct dxs *d = &s->dxs;
	int ret;

	if (chan >= d->channels)
		return -EINVAL;

	if (!d->fw_loaded) {
		ret = dxs_fw_load(d, DXS_FW_PATH);
		if (ret)
			return ret;

		dxs_log(d, UDSP_SLIC_LOG_INFO, "running firmware %u.%u.%u.%u",
			d->ver.major & 0x7f, d->ver.minor, d->ver.hotfix,
			d->ver.patch);

		ret = dxs_reg_write(d, DXS_HOST_CFG, DXS_CFG_SCON | DXS_CFG_8BIT);
		if (ret)
			return ret;
	}

	/* coefficients and calibration leave the line disabled, i.e. open */
	ret = dxs_bbd_load(d, chan, DXS_BBD_PATH);
	if (ret)
		return ret;

	return dxs_pcm_start(d, chan);
}

static int dxs_line_check(struct udsp_slic *s, unsigned chan)
{
	if (chan >= s->dxs.channels)
		return -EINVAL;
	if (!s->dxs.line[chan].bbd_loaded)
		return -EAGAIN;

	return 0;
}

static int dxs_linefeed_set(struct udsp_slic *s, unsigned chan,
			    enum udsp_linefeed lf)
{
	static const uint8_t opmode[] = {
		[UDSP_LF_OPEN] = DXS_OPMODE_DISABLED,
		[UDSP_LF_FWD_ACTIVE] = DXS_OPMODE_ACTIVE,
		[UDSP_LF_FWD_OHT] = DXS_OPMODE_ACTIVE,
		[UDSP_LF_RINGING] = DXS_OPMODE_RING,
		[UDSP_LF_REV_ACTIVE] = DXS_OPMODE_ACTIVE_REV,
		[UDSP_LF_REV_OHT] = DXS_OPMODE_ACTIVE_REV,
	};
	int ret;

	ret = dxs_line_check(s, chan);
	if (ret)
		return ret;
	if ((unsigned int)lf >= sizeof(opmode))
		return -EINVAL;

	return dxs_opmode_set(&s->dxs, chan, opmode[lf]);
}

static int dxs_ring_start(struct udsp_slic *s, unsigned chan)
{
	return dxs_linefeed_set(s, chan, UDSP_LF_RINGING);
}

static int dxs_ring_stop(struct udsp_slic *s, unsigned chan)
{
	return dxs_linefeed_set(s, chan, UDSP_LF_FWD_ACTIVE);
}

static int dxs_hook_read(struct udsp_slic *s, unsigned chan, bool *offhook)
{
	int ret;

	ret = dxs_line_check(s, chan);
	if (ret)
		return ret;

	ret = dxs_events_poll(&s->dxs);
	if (ret < 0)
		return ret;

	*offhook = s->dxs.line[chan].offhook;

	return 0;
}

static int dxs_fault_read(struct udsp_slic *s, unsigned chan, uint32_t *bits)
{
	int ret;

	ret = dxs_line_check(s, chan);
	if (ret)
		return ret;

	ret = dxs_events_poll(&s->dxs);
	if (ret < 0)
		return ret;

	*bits = dxs_faults_take(&s->dxs, chan);

	return 0;
}

static int dxs_monitor_read(struct udsp_slic *s, unsigned chan,
			    struct udsp_slic_monitor *out)
{
	int ret;

	ret = dxs_line_check(s, chan);
	if (ret)
		return ret;

	return dxs_measure(&s->dxs, chan, out);
}

/*
 * Line test records in the layout uvoice decodes (version 1): version, test,
 * status, a valid mask, then the values. Only the capacitance test runs here;
 * the others need the full GR-909 sequence of the firmware.
 */
#define DXS_MLT_VERSION		1
#define DXS_MLT_CAPACITANCE	2
#define DXS_MLT_VALID_C		(7u << 12)	/* tip-ring, tip-ground, ring-ground */
#define DXS_MLT_C_SIZE		20

static void dxs_put32(uint8_t *p, uint32_t v)
{
	p[0] = v;
	p[1] = v >> 8;
	p[2] = v >> 16;
	p[3] = v >> 24;
}

static int dxs_mlt_run(struct udsp_slic *s, unsigned chan, uint8_t test_id,
		       struct udsp_slic_mlt_result *out)
{
	uint32_t nf[3];
	unsigned int i;
	int ret;

	ret = dxs_line_check(s, chan);
	if (ret)
		return ret;
	if (test_id != DXS_MLT_CAPACITANCE)
		return -EOPNOTSUPP;

	ret = dxs_capacitance(&s->dxs, chan, nf);
	if (ret)
		return ret;

	memset(out, 0, sizeof(*out));
	out->test_id = test_id;
	out->len = DXS_MLT_C_SIZE;
	out->data[0] = DXS_MLT_VERSION;
	out->data[1] = test_id;
	dxs_put32(&out->data[4], DXS_MLT_VALID_C);
	for (i = 0; i < 3; i++)
		dxs_put32(&out->data[8 + i * 4], nf[i]);

	return 0;
}

static int dxs_gain_set(struct udsp_slic *s, unsigned chan, int tx_half_db,
			int rx_half_db)
{
	int ret;

	ret = dxs_line_check(s, chan);
	if (ret)
		return ret;

	return dxs_level_set(&s->dxs, chan, tx_half_db, rx_half_db);
}

static void dxs_safe_xfer(struct udsp_slic *s, struct udsp_slic_safe *out)
{
	*out = s->safe;
}

static const char *dxs_part_name(struct udsp_slic *s)
{
	return s->dxs.name;
}

static const struct udsp_slic_ops dxs_ops = {
	.name = "dxs",
	.attach = dxs_attach,
	.detach = dxs_detach,
	.init = dxs_init,
	.linefeed_set = dxs_linefeed_set,
	.ring_start = dxs_ring_start,
	.ring_stop = dxs_ring_stop,
	.hook_read = dxs_hook_read,
	.fault_read = dxs_fault_read,
	.gain_set = dxs_gain_set,
	.safe_xfer = dxs_safe_xfer,
	.part_name = dxs_part_name,
	.monitor_read = dxs_monitor_read,
	.mlt_run = dxs_mlt_run,
};

const struct udsp_slic_descriptor udsp_slic_descriptor = {
	.abi_version = UDSP_SLIC_ABI_VERSION,
	.struct_size = sizeof(struct udsp_slic_ops),
	.ops = &dxs_ops,
};
