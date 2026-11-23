// SPDX-License-Identifier: BSD-2-Clause
/*
 * DUSLIC-XS PCM highway
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 *
 * The part is a PCM slave on a 2.048 MHz bit clock: 32 timeslots of eight
 * bits, a 16-bit linear sample taking two. It samples the received data on
 * the falling edge and drives its transmit data from the rising edge, so the
 * transmit slot has to start two bit clocks before the receive slot for both
 * directions to line up with the host, as the vendor programs it. Measured
 * with a full-scale tone through the line hybrid: any other placement either
 * halves the samples or wraps them.
 */
#include "dxs.h"

#define DXS_MOD_PCM		0
#define DXS_ECMD_PCM_IF		0
#define DXS_ECMD_PCM_CH		1

#define DXS_PCM_IF_EN		(1u << 31)
#define DXS_PCM_IF_DSEN		(1u << 30)	/* double-buffered data */
#define DXS_PCM_IF_XOFF(x)	((uint32_t)((x) & 7) << 24)
#define DXS_PCM_IF_ROFF(x)	((uint32_t)((x) & 7) << 16)

#define DXS_PCM_CH_EN		(1u << 31)
#define DXS_PCM_CH_LINEAR	(0u << 28)
#define DXS_PCM_CH_XTS(x)	((uint32_t)((x) & 0x7f) << 8)
#define DXS_PCM_CH_RTS(x)	((uint32_t)((x) & 0x7f))

#define DXS_PCM_SLOTS		32
#define DXS_PCM_SLOT_BITS	8
#define DXS_PCM_FRAME_BITS	(DXS_PCM_SLOTS * DXS_PCM_SLOT_BITS)
#define DXS_PCM_SLOTS_PER_LINE	2
#define DXS_PCM_TX_LEAD		2	/* bit clocks the transmit slot leads */
#define DXS_PCM_OFFSET_DEFAULT	1	/* DSP_A: data one bit after the sync */

static uint32_t dxs_pcm_hdr(unsigned int chan, unsigned int ecmd)
{
	return DXS_HDR_CMD(DXS_CMD_EOP) | DXS_HDR_CHAN(chan) |
	       DXS_HDR_MOD(DXS_MOD_PCM) | DXS_HDR_ECMD(ecmd) | 4;
}

/* First receive and transmit bit of a line, in bit clocks from the sync. */
static void dxs_pcm_bits(struct dxs *d, unsigned int chan, unsigned int *rx,
			 unsigned int *tx)
{
	unsigned int offset = d->host->pcm_offset_bits;

	if (!offset || offset >= DXS_PCM_SLOT_BITS)
		offset = DXS_PCM_OFFSET_DEFAULT;

	*rx = chan * DXS_PCM_SLOTS_PER_LINE * DXS_PCM_SLOT_BITS + offset;
	*tx = (*rx + DXS_PCM_FRAME_BITS - DXS_PCM_TX_LEAD) % DXS_PCM_FRAME_BITS;
}

/* The bit offsets are shared by all lines, so every line must use the same. */
static int dxs_pcm_if_set(struct dxs *d)
{
	uint32_t cmd[2] = { dxs_pcm_hdr(0, DXS_ECMD_PCM_IF), 0 };
	unsigned int rx, tx;
	int ret;

	/* the settings only take effect on a disabled interface */
	ret = dxs_cmd_write(d, cmd);
	if (ret)
		return ret;

	dxs_pcm_bits(d, 0, &rx, &tx);
	cmd[1] = DXS_PCM_IF_EN | DXS_PCM_IF_DSEN |
		 DXS_PCM_IF_XOFF(tx % DXS_PCM_SLOT_BITS) |
		 DXS_PCM_IF_ROFF(rx % DXS_PCM_SLOT_BITS);

	return dxs_cmd_write(d, cmd);
}

int dxs_pcm_ch_set(struct dxs *d, unsigned int chan, bool on)
{
	uint32_t cmd[2] = { dxs_pcm_hdr(chan, DXS_ECMD_PCM_CH), 0 };
	unsigned int rx, tx;

	dxs_pcm_bits(d, chan, &rx, &tx);
	if (on)
		cmd[1] = DXS_PCM_CH_EN | DXS_PCM_CH_LINEAR |
			 DXS_PCM_CH_XTS(tx / DXS_PCM_SLOT_BITS) |
			 DXS_PCM_CH_RTS(rx / DXS_PCM_SLOT_BITS);

	return dxs_cmd_write(d, cmd);
}

int dxs_pcm_start(struct dxs *d, unsigned int chan)
{
	int ret;

	if (!d->pcm_on) {
		ret = dxs_pcm_if_set(d);
		if (ret)
			return ret;
		d->pcm_on = true;
	}

	return dxs_pcm_ch_set(d, chan, true);
}
