// SPDX-License-Identifier: BSD-2-Clause
/*
 * DUSLIC-XS line state: operating modes and SDD events
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 *
 * The firmware runs the line ("SDD") itself. The host asks for an operating
 * mode and learns the outcome, hook changes included, from events in the
 * out-box. Every op runs on the udsp slic thread, so the cached state needs no
 * lock.
 */
#include <errno.h>
#include <math.h>
#include <string.h>

#include "dxs.h"

#define DXS_CALIBRATE_TIMEOUT_MS	10000
#define DXS_EVENT_POLL_MS		10

void dxs_event(struct dxs *d, const uint32_t *msg)
{
	unsigned int chan = DXS_HDR_CHAN_GET(msg[0]);
	struct dxs_line *l;
	unsigned int i;

	/* a clock or frame sync failure stops every line of the device */
	if (DXS_HDR_MOD_GET(msg[0]) == DXS_MOD_SYS &&
	    DXS_HDR_ECMD_GET(msg[0]) == DXS_ECMD_SYS_INTERR) {
		dxs_log(d, UDSP_SLIC_LOG_ERR, "internal error %u",
			DXS_INTERR_CODE(msg[1]));
		if (DXS_INTERR_CODE(msg[1]) == DXS_INTERR_CLK_FAIL ||
		    DXS_INTERR_CODE(msg[1]) == DXS_INTERR_SYNC_FAIL)
			for (i = 0; i < d->channels; i++)
				d->line[i].faults |= UDSP_FAULT_SLIC_CLOCK;
		return;
	}

	if (DXS_HDR_MOD_GET(msg[0]) != DXS_MOD_ALI ||
	    DXS_HDR_ECMD_GET(msg[0]) != DXS_ECMD_EVT_SDD ||
	    DXS_HDR_LEN(msg[0]) != 4 || chan >= d->channels) {
		dxs_log(d, UDSP_SLIC_LOG_DEBUG, "event %#010x", msg[0]);
		return;
	}
	l = &d->line[chan];

	switch (DXS_EVT_SDD_CODE(msg[1])) {
	case DXS_EVT_OPC:
		l->opmode = DXS_EVT_SDD_OPMODE(msg[1]);
		dxs_log(d, UDSP_SLIC_LOG_DEBUG, "line %u opmode %u", chan, l->opmode);
		break;
	case DXS_EVT_OTEMP:
		dxs_log(d, UDSP_SLIC_LOG_ERR, "line %u overheated, disabled", chan);
		l->faults |= UDSP_FAULT_SLIC_THERMAL;
		break;
	case DXS_EVT_OTEMP_FIN:
		dxs_log(d, UDSP_SLIC_LOG_INFO, "line %u cooled down", chan);
		break;
	case DXS_EVT_LT_FIN:
		l->lt_done = 1;
		break;
	case DXS_EVT_LT_ABORT:
		l->lt_done = -1;
		break;
	case DXS_EVT_GF:
		dxs_log(d, UDSP_SLIC_LOG_ERR, "line %u ground fault", chan);
		l->faults |= UDSP_FAULT_SLIC_DC;
		break;
	case DXS_EVT_GF_FIN:
		dxs_log(d, UDSP_SLIC_LOG_INFO, "line %u ground fault cleared", chan);
		break;
	case DXS_EVT_ONH:
		l->offhook = false;
		break;
	case DXS_EVT_OFFH:
		l->offhook = true;
		break;
	default:
		dxs_log(d, UDSP_SLIC_LOG_DEBUG, "line %u sdd event %u", chan,
			DXS_EVT_SDD_CODE(msg[1]));
		break;
	}
}

/*
 * Relative levels of the analog line, in 0.5 dB steps. The firmware takes a
 * linear factor per direction: receive (to the handset) is unity at 0x7800,
 * transmit (from the handset) is unity at 0x4f00. The vendor tables index the
 * transmit factor by attenuation; measured on the line, a larger factor is
 * louder, so both are scaled the same way here. The receive range is the
 * vendor's, the transmit one its mirror.
 */
#define DXS_GAIN_RX_0DB		0x7800
#define DXS_GAIN_TX_0DB		0x4f00
#define DXS_GAIN_RX_MIN		(-24 * 2)
#define DXS_GAIN_RX_MAX		(1 * 2)
#define DXS_GAIN_TX_MIN		(-24 * 2)
#define DXS_GAIN_TX_MAX		(4 * 2)

static uint16_t dxs_gain_factor(unsigned int unity, int half_db)
{
	return lround(unity * pow(10.0, half_db / 40.0));
}

int dxs_level_set(struct dxs *d, unsigned int chan, int tx_half_db,
		  int rx_half_db)
{
	uint32_t cmd[2];

	if (tx_half_db < DXS_GAIN_TX_MIN || tx_half_db > DXS_GAIN_TX_MAX ||
	    rx_half_db < DXS_GAIN_RX_MIN || rx_half_db > DXS_GAIN_RX_MAX)
		return -ERANGE;

	cmd[0] = dxs_sdd_hdr(chan, DXS_ECMD_SDD_GAIN, 4);
	cmd[1] = (uint32_t)dxs_gain_factor(DXS_GAIN_TX_0DB, tx_half_db) << 16 |
		 dxs_gain_factor(DXS_GAIN_RX_0DB, rx_half_db);

	return dxs_cmd_write(d, cmd);
}

uint32_t dxs_faults_take(struct dxs *d, unsigned int chan)
{
	uint32_t bits = d->line[chan].faults;

	d->line[chan].faults = 0;

	return bits;
}

/*
 * The firmware measures the line all the time: the regulated tip-ring
 * voltage and the loop current, in steps of 144 V and 100 mA over 2^15.
 * It has no separate tip and ring wire readings.
 */
int dxs_measure(struct dxs *d, unsigned int chan, struct udsp_slic_monitor *out)
{
	uint32_t data[3];
	int16_t vline, itrans;
	int ret;

	ret = dxs_cmd_read(d, dxs_sdd_hdr(chan, DXS_ECMD_SDD_CONTMEAS, 12), data, 3);
	if (ret)
		return ret;

	itrans = data[0] & 0xffff;
	vline = data[0] >> 16;

	memset(out, 0, sizeof(*out));
	out->vtr_mv = (int32_t)(((int64_t)vline * 144000) >> 15);
	out->itr_ua = (int32_t)(((int64_t)itrans * 100000) >> 15);

	return 0;
}

#define DXS_LT_TIMEOUT_MS	10000

/*
 * The line test limits, as the vendor programs them for its default
 * 1.5 MOhm measurement range. The capacitance measurement needs them loaded
 * though it checks none of them.
 */
static const uint32_t dxs_lt_cfg[7] = {
	7070,			/* hazardous AC wire to ground, 50 Vrms */
	7070 << 16 | 13500,	/* hazardous AC wire to wire, DC wire to ground */
	13500 << 16 | 1414,	/* hazardous DC wire to wire, foreign AC wire to ground */
	1414 << 16 | 600,	/* foreign AC wire to wire, DC wire to ground */
	600 << 16 | 8523,	/* foreign DC wire to wire, resistive fault limit */
	0x000f << 16 | 0x0057,	/* off-hook linearity, ringer low limit */
	0x09ab << 16,		/* ringer high limit */
};

/*
 * Measure the capacitance tip to ring, tip to ground and ring to ground, in
 * nF. The firmware runs the measurement from the disabled mode and reports
 * its end with a line test event. It leaves the line disabled, and the host
 * still believes in its earlier feed, so the earlier mode is restored here.
 */
static int dxs_capacitance_run(struct dxs *d, unsigned int chan,
			       uint32_t nf[3]);

int dxs_capacitance(struct dxs *d, unsigned int chan, uint32_t nf[3])
{
	struct dxs_line *l = &d->line[chan];
	uint8_t opmode = l->opmode;
	int ret;

	ret = dxs_capacitance_run(d, chan, nf);
	if (dxs_opmode_set(d, chan, opmode) && !ret)
		ret = -EIO;

	return ret;
}

static int dxs_capacitance_run(struct dxs *d, unsigned int chan, uint32_t nf[3])
{
	struct dxs_line *l = &d->line[chan];
	uint32_t cmd[8], data[2];
	unsigned int waited;
	int ret;

	ret = dxs_opmode_set(d, chan, DXS_OPMODE_DISABLED);
	if (ret)
		return ret;

	cmd[0] = dxs_sdd_hdr(chan, DXS_ECMD_SDD_LT_CFG, sizeof(dxs_lt_cfg));
	memcpy(&cmd[1], dxs_lt_cfg, sizeof(dxs_lt_cfg));
	ret = dxs_cmd_write(d, cmd);
	if (ret)
		return ret;

	l->lt_done = 0;
	ret = dxs_opmode_set(d, chan, DXS_OPMODE_CAPMEAS);
	if (ret)
		return ret;

	for (waited = 0; !l->lt_done; waited += DXS_EVENT_POLL_MS) {
		if (waited >= DXS_LT_TIMEOUT_MS)
			return -ETIMEDOUT;
		d->host->delay_ms(d->host->ctx, DXS_EVENT_POLL_MS);
		ret = dxs_events_poll(d);
		if (ret < 0)
			return ret;
	}
	if (l->lt_done < 0)
		return -ECANCELED;

	ret = dxs_cmd_read(d, dxs_sdd_hdr(chan, DXS_ECMD_SDD_CAPMEAS, 8), data, 2);
	if (ret)
		return ret;

	nf[0] = data[1] >> 16;		/* tip to ring */
	nf[1] = data[0] & 0xffff;	/* tip to ground */
	nf[2] = data[0] >> 16;		/* ring to ground */

	return nf[0] ? 0 : -EIO;
}

int dxs_opmode_set(struct dxs *d, unsigned int chan, uint8_t opmode)
{
	uint32_t cmd[2];

	cmd[0] = dxs_opmode_cmd(chan, opmode, &cmd[1]);

	return dxs_cmd_write(d, cmd);
}

/*
 * The calibration measures the line with tip and ring open and returns the
 * line to disabled when done.
 */
int dxs_calibrate(struct dxs *d, unsigned int chan)
{
	struct dxs_line *l = &d->line[chan];
	unsigned int waited;
	int ret;

	l->opmode = DXS_OPMODE_CALIBRATE;
	ret = dxs_opmode_set(d, chan, DXS_OPMODE_CALIBRATE);
	if (ret)
		return ret;

	for (waited = 0; waited < DXS_CALIBRATE_TIMEOUT_MS;
	     waited += DXS_EVENT_POLL_MS) {
		d->host->delay_ms(d->host->ctx, DXS_EVENT_POLL_MS);
		ret = dxs_events_poll(d);
		if (ret < 0)
			return ret;
		if (l->opmode != DXS_OPMODE_CALIBRATE) {
			dxs_log(d, UDSP_SLIC_LOG_INFO, "line %u calibrated in %u ms",
				chan, waited);
			return 0;
		}
	}

	dxs_log(d, UDSP_SLIC_LOG_ERR, "line %u calibration timed out", chan);

	return -ETIMEDOUT;
}
