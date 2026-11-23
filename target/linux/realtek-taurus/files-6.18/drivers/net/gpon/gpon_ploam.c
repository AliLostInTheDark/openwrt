// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) BRULE Herman <alpha_one_x86@first-world.info> (github.com/alphaonex86)
 *
 * Taken from https://github.com/alphaonex86/openwrt.
 *
 * G.984.3 PLOAM activation state machine (O1..O7): downstream PLOAM
 * dispatch, activation transitions, upstream message builders, burst
 * overhead and equalization delay computation, and the periodic poll
 * decisions. See gpon_ploam.h for the interface.
 */

#include "gpon_ploam.h"

/* Poll cadences and thresholds, in FSM ticks or milliseconds. */
#define GPON_PLOAM_AVC_DELAY_TICKS	2500	/* hold O5 this long first */
#define GPON_PLOAM_AVC_PERIOD_TICKS	150	/* then re-report this often */
#define GPON_PLOAM_AVC_MAX		3	/* reports per O5 entry */
#define GPON_PLOAM_SN_REOFFER_TICKS	50	/* ~twice a second at O3 */
#define GPON_PLOAM_HEALTHY_O5_TICKS	500	/* ~5 s = a healthy provision */
#define GPON_PLOAM_RERANGE_LOG_MS	2000	/* flap damping on the summary */

/* Preamble and delimiter defaults until the OLT sends its own. */
#define GPON_PLOAM_BOH_PTN_DEFAULT	0xaa
#define GPON_PLOAM_BOH_DELIM_DEFAULT	{ 0xab, 0x59, 0x83 }

/* Internal helpers */

/*
 * Wrap-safe "a is strictly after b" on the 32-bit millisecond clock, like
 * time_after() but in unsigned arithmetic.
 */
static bool ms_after(u32 a, u32 b)
{
	u32 diff = a - b;

	return diff != 0u && diff < 0x80000000u;
}

static void ev(const struct gpon_ploam *o, enum gpon_ploam_ev e, u32 a, u32 b)
{
	if (o->ops->trace)
		o->ops->trace(o->sh, e, a, b);
}

/*
 * Every upstream PLOAM goes out here. The urgent queue (0x1) pre-empts the
 * auto-SN burst (0x6) so an Acknowledge reaches the OLT in time; No_message
 * uses the hardware auto slot (0x7).
 */
static void ploam_tx(struct gpon_ploam *o, u8 queue,
		     const u8 m[GPON_PLOAM_US_LEN])
{
	o->ops->ploam_tx(o->sh, queue, m);
	o->tx_total++;
}

/* Upstream message builders. The hardware appends the CRC. */

/* Serial_Number_ONU (US type 0x01) */
static void send_sn(struct gpon_ploam *o)
{
	u8 m[GPON_PLOAM_US_LEN];

	m[0] = 0xff;			/* ONU-ID (unassigned) */
	m[1] = PLM_US_SERIAL_NUMBER;	/* 0x01 */
	memcpy(&m[2], o->sn, 8);	/* ONU-SN: ID(4) + serial(4) */
	m[10] = 0x00;			/* random delay (HW may fill) */
	m[11] = 0x04;			/* G-bit set, power level 0 */

	ploam_tx(o, PLM_US_QUEUE_SN, m);
	o->sn_tx++;
}

/*
 * Password (US type 0x02). Without it the OLT stalls activation and eventually
 * deactivates the ONU with LOAi (loss of acknowledge), even when it
 * authenticates by serial number only and ignores the value. Sent three times
 * on the urgent queue, as G.984.3 repeats it for reliability.
 */
static int send_password(struct gpon_ploam *o)
{
	u8 p[GPON_PLOAM_US_LEN] = { 0 };
	int i;

	p[0] = o->onu_id;		/* our assigned ONU-ID */
	p[1] = PLM_US_PASSWORD;		/* 0x02 */
	/* p[2..11] = the 10-octet password, all zero = empty */
	for (i = 0; i < 3; i++)
		ploam_tx(o, PLM_US_QUEUE_URG, p);
	return 3;
}

/*
 * Acknowledge (US type 0x09). The OLT deactivates the ONU (after ~43 s) if an
 * acknowledge-required message gets no reply. @ds is the full downstream
 * message, which is echoed. Urgent queue, so it pre-empts the SN burst.
 */
static int send_ack(struct gpon_ploam *o, const u8 *ds)
{
	u8 a[GPON_PLOAM_US_LEN] = { 0 };

	a[0] = o->onu_id;		/* our assigned ONU-ID (HW may override) */
	a[1] = PLM_US_ACKNOWLEDGE;	/* 0x09 */
	a[2] = ds[1];			/* acknowledged message type */
	a[3] = ds[0];			/* acknowledged message ONU-ID */
	a[4] = ds[1];			/* acknowledged message type (echo) */
	memcpy(&a[5], &ds[2], 7);	/* first 7 payload octets */
	ploam_tx(o, PLM_US_QUEUE_URG, a);
	return 1;
}

/*
 * Encryption_Key (US type 0x05). Generate a 128-bit AES key and send it in two
 * fragments (m[2] = key index, m[3] = row, m[4..11] = 8 key bytes), three times
 * over, since the upstream PLOAM channel is lossy. Then load the same key into
 * the hardware staged bank; the fragments must go out first.
 */
static int send_key(struct gpon_ploam *o)
{
	u8 m[GPON_PLOAM_US_LEN];
	int row, rep;

	o->ops->rng(o->sh, o->aes_key, sizeof(o->aes_key));
	o->key_index++;
	for (rep = 0; rep < 3; rep++) {
		for (row = 0; row < 2; row++) {
			memset(m, 0, sizeof(m));
			m[0] = o->onu_id;	/* ONU-ID */
			m[1] = PLM_US_ENCRYPT_KEY;
			m[2] = o->key_index;
			m[3] = (u8)row;		/* fragment row 0/1 */
			memcpy(&m[4], o->aes_key + row * 8, 8);
			ploam_tx(o, PLM_US_QUEUE_URG, m);
		}
	}
	o->ops->aes_stage_key(o->sh, o->aes_key);
	o->key_staged = true;
	ev(o, GPON_PLOAM_EV_KEY_SENT, o->key_index, 0);
	return 6;
}

/* No_message (US type 0x04) for the hardware auto slot. */
static void build_nomsg(u8 m[GPON_PLOAM_US_LEN])
{
	memset(m, 0xaa, GPON_PLOAM_US_LEN);
	m[0] = 0xff;			/* ONU-ID (HW overrides via ONUID_OVRD) */
	m[1] = PLM_US_NO_MESSAGE;	/* 0x04 */
}

/* Burst overhead and equalization delay, as dictated by the OLT */

/*
 * Build the burst overhead.
 *
 * Extended_Burst_Length sets the Type-3 preamble lengths, t3pre for the
 * pre-ranged burst and t3ranged for the ranged burst; the total length is rep +
 * t3 + 3. Without it (t3 == 0) the 12-byte default is used.
 *
 * The delimiter goes at the real end of the stored bytes (oh[size-3..size-1]):
 * the ranged burst is often shorter than 12 bytes and the hardware only sends
 * BOH_LENGTH bytes, so a delimiter at a fixed oh[9..11] would be cut off and
 * the OLT could not frame the burst.
 *
 * BOH_REPEAT is the index of the last preamble byte before the delimiter (size
 * - 4); the hardware repeats from there to extend the stored bytes to
 * BOH_LENGTH. With size = 12 that gives BOH_CFG = 0x083f (REPEAT 8, LENGTH 63),
 * the vendor firmware's O5 value.
 */
static void apply_boh(struct gpon_ploam *o, bool ranged)
{
	u8 oh[GPON_PLOAM_BOH_LEN];
	u8 guard = o->boh_guard;
	u8 t3 = ranged ? o->boh_t3ranged : o->boh_t3pre;
	u8 rep, i, boh_len, size;
	u32 cfg_word;

	if (guard > 32)
		guard = 32;
	rep = guard / 8;			/* boh_repeat = whole guard bytes */

	/*
	 * (rep + t3 + 3) is truncated to u8 before the clamp below; only
	 * reachable with t3 >= 249.
	 */
	boh_len = (u8)(t3 ? (rep + t3 + 3) : GPON_PLOAM_BOH_LEN);
	if (boh_len > GPON_PLOAM_BOH_MAX_LEN)
		boh_len = GPON_PLOAM_BOH_MAX_LEN;
	/* stored bytes (<= 12); the hardware synthesizes the rest */
	size = (boh_len > GPON_PLOAM_BOH_LEN) ? GPON_PLOAM_BOH_LEN : boh_len;
	if (size < 4)				/* need >= 1 fill + 3 delimiter */
		size = 4;

	memset(oh, 0xaa, sizeof(oh));
	for (i = 0; i < rep && i < (u8)(size - 3); i++)
		oh[i] = 0xaa;			/* guard bytes */
	for (; i < (u8)(size - 3); i++)
		oh[i] = o->boh_ptn;		/* Type-3 preamble fill */
	oh[size - 3] = o->boh_delim[0];		/* delimiter at the TRUE end */
	oh[size - 2] = o->boh_delim[1];
	oh[size - 1] = o->boh_delim[2];

	cfg_word = (u32)(((size - 4) & 0xf) << 8) | (boh_len & 0xffu);
	o->ops->boh_write(o->sh, cfg_word, oh, size);
	ev(o, GPON_PLOAM_EV_BOH, ranged ? 1 : 0, cfg_word);
}

/*
 * Upstream equalization delay. The burst time is @value plus the local
 * MIN_DELAY1 scaled to bits (x128), split over the 19440x8-bit upstream frame
 * into a multiframe count and an in-frame offset. Before ranging (@value 0)
 * this gives 290 * 128 = 37120 (0x9100). The shell packs the two halves into
 * the register.
 */
static void set_eqd(struct gpon_ploam *o, u32 value)
{
	u32 min_delay1 = o->ops->get_min_delay(o->sh);
	u32 eqd1  = value + min_delay1 * 128;
	u32 multi = eqd1 / GPON_PLOAM_EQD_FRAME_LEN;
	u32 intra = eqd1 - multi * GPON_PLOAM_EQD_FRAME_LEN;

	o->ops->set_eqd(o->sh, multi, intra);
}

/* State transitions */
static void set_state(struct gpon_ploam *o, enum gpon_ostate st, u32 now_ms)
{
	enum gpon_ostate prev = o->state;

	/*
	 * hold: never advance past O1, so the ONU never ranges and the shared
	 * switch datapath stays quiet.
	 */
	if (o->cfg->hold && st > GPON_O1_INITIAL)
		return;
	if (o->state != st)
		ev(o, GPON_PLOAM_EV_STATE, o->state, st);
	if (prev != st)
		o->los_run = 0;		/* fresh LOS debounce on every transition */
	o->state = st;

	if (st == GPON_O5_OPERATION && prev != GPON_O5_OPERATION) {
		/*
		 * Re-entering O5 after an outage (LOS, Deactivate, watchdog):
		 * count it, record its duration and report a rate-limited
		 * summary. The data GEM is reinstalled shortly after from the
		 * poll.
		 */
		if (o->rerange_start_ms) {
			o->rerange_cnt++;
			o->last_outage_ms = now_ms - o->rerange_start_ms;
			o->rerange_start_ms = 0;
			if (!o->rerange_last_log_ms ||
			    ms_after(now_ms, o->rerange_last_log_ms +
					     GPON_PLOAM_RERANGE_LOG_MS)) {
				ev(o, GPON_PLOAM_EV_RERANGE_DONE,
				   o->rerange_cnt, o->last_outage_ms);
				/*
				 * A clock value of 0 reads as "never logged";
				 * harmless, at most one extra summary.
				 */
				o->rerange_last_log_ms = now_ms;
			}
		}
		o->o5_entry_tick = o->ticks ? o->ticks : 1;
		o->avc_sent = 0;	/* re-report oper-up on each online */

		/*
		 * A re-range resets the GMAC/SerDes, which can clear the
		 * upstream burst registers, so re-apply them and re-arm the
		 * hardware No_message keepalive on every O5 entry.
		 */
		if (o->cfg->o5_rearm_burst_gate) {
			u8 nomsg[GPON_PLOAM_US_LEN];

			o->ops->o5_rearm_burst(o->sh);
			build_nomsg(nomsg);
			ploam_tx(o, PLM_US_QUEUE_NOMSG, nomsg);
		}
	} else if (st < GPON_O5_OPERATION && prev >= GPON_O5_OPERATION) {
		o->rerange_start_ms = now_ms ? now_ms : 1;  /* outage timer starts */
		o->o5_entry_tick = 0;
		/* Let the shell re-arm whatever it gates on O5. */
		o->ops->on_below_o5(o->sh);
	}

	/* Reflect the state into the hardware state field and the PON LED. */
	o->ops->set_hw_state(o->sh, st);
}

/*
 * Activation teardowns. The four paths (Deactivate/Disable_SN, SN change,
 * watchdog, LOS) are written out separately because they intentionally differ
 * in what they clear.
 */

/* Downstream PLOAM dispatch */
int gpon_ploam_ds(struct gpon_ploam *o, const u8 *m, unsigned int len, u32 now_ms)
{
	u32 tx0 = o->tx_total;
	u8 onu_id, type;
	const u8 *d;

	/* Drop short frames without any state change. */
	if (!m || len < GPON_PLOAM_DS_LEN)
		return 0;

	onu_id = m[0];
	type   = m[1];
	d      = &m[2];			/* 10 data octets */

	/*
	 * Trace downstream PLOAMs other than the repetitive broadcast
	 * acquisition traffic.
	 */
	o->last_ds_type = type;
	if (o->cfg->trace && type != PLM_DS_UPSTREAM_OVERHEAD &&
	    type != PLM_DS_EXT_BURST_LENGTH)
		ev(o, GPON_PLOAM_EV_DS, onu_id, type);

	switch (type) {
	case PLM_DS_UPSTREAM_OVERHEAD:
		/*
		 * Broadcast continuously while the OLT acquires ONUs. On the
		 * O1/O2 -> O3 edge, program the burst overhead and pre-ranging
		 * EqD before the first Serial_Number, then go to O3; the SN is
		 * then re-sent, throttled, from the poll.
		 * Payload: d[0] guard bits, d[3] Type-3 preamble pattern,
		 * d[4..6] delimiter, d[7] bit 5 = pre-EqD present with value
		 * d[8:9] (x32 x8 bits).
		 */
		if (o->state < GPON_O3_SERIAL) {
			u32 pre_eqd = ((d[7] >> 5) & 1) ?
				(((u32)d[8] << 8) | d[9]) * 32 * 8 : 0;

			o->boh_guard    = d[0];
			o->boh_ptn      = d[3];
			o->boh_delim[0] = d[4];
			o->boh_delim[1] = d[5];
			o->boh_delim[2] = d[6];
			apply_boh(o, false);	/* uses any earlier Ext_Burst t3pre */
			set_eqd(o, pre_eqd);
			set_state(o, GPON_O2_STANDBY, now_ms);
			set_state(o, GPON_O3_SERIAL, now_ms);
			/*
			 * Relock the TX CMU PLL now that the downstream is
			 * stable: on a cold power-on under strong light, signal
			 * detect can assert before the CMU settles and the TX
			 * PLL locks to the wrong rate.
			 */
			o->ops->analog_relock(o->sh);
			if (o->cfg->o3_feed_reset) {
				/*
				 * The relock stops the GEM upstream feed;
				 * restart it with the full datapath reset edge
				 * while the downstream is not yet locked.
				 */
				o->ops->o3_feed_reset(o->sh);
				ev(o, GPON_PLOAM_EV_O3_FEED_RESET, 0, 0);
			}
			send_sn(o);		/* first SN immediately */
		}
		break;

	case PLM_DS_ASSIGN_ONU_ID:
		/* d[0] = assigned ONU-ID, d[1..8] = serial number to match. */
		if (!memcmp(&d[1], o->sn, 8)) {
			u16 tcont16_alloc;

			o->onu_id = d[0];
			o->ops->set_hw_onu_id(o->sh, o->onu_id);
			/*
			 * Bind the OMCC T-CONT to its Alloc-ID, which is the
			 * ONU-ID (G.984.3 default) unless overridden.
			 */
			tcont16_alloc = o->cfg->omcc_alloc_override ?
					o->cfg->omcc_alloc_override : o->onu_id;
			(void)o->ops->install_tcont(o->sh, o->cfg->omcc_tcont,
						    tcont16_alloc);
			ev(o, GPON_PLOAM_EV_CAM_READBACK, o->cfg->omcc_tcont,
			   tcont16_alloc);
			/*
			 * Optional (default off) bind of the same Alloc-ID to
			 * the alternate T-CONT. It makes grants resolve to an
			 * empty T-CONT, so DBRu reports zero occupancy and the
			 * OLT stops granting.
			 */
			if (o->cfg->omcc_alt_bind && tcont16_alloc != o->onu_id)
				(void)o->ops->install_tcont(o->sh,
						o->cfg->omcc_tcont_alt,
						tcont16_alloc);
			ev(o, GPON_PLOAM_EV_ONU_ID, o->onu_id, tcont16_alloc);
			set_state(o, GPON_O4_RANGING, now_ms);
		}
		break;

	case PLM_DS_RANGING_TIME:
		/*
		 * Only the main-path EqD (d[0] bit 0 clear) is used. EqD is
		 * d[1..4], big-endian.
		 */
		if (onu_id == o->onu_id && !(d[0] & 0x01)) {
			u32 eqd = ((u32)d[1] << 24) | ((u32)d[2] << 16) |
				  ((u32)d[3] << 8) | d[4];

			set_eqd(o, eqd);
			apply_boh(o, true);	/* switch to the ranged burst */
			/*
			 * Flush any pre-ranged upstream PLOAM still latched in
			 * the shared CPU buffer before the first ranged grant,
			 * as the OLT's ranged receive window is narrow.
			 */
			o->ops->us_ploam_flush(o->sh);
			ev(o, GPON_PLOAM_EV_RANGING_TIME, eqd, 0);
			set_state(o, GPON_O5_OPERATION, now_ms);
		}
		break;

	case PLM_DS_DISABLE_SN:
		/*
		 * Disable_Serial_Number: d[0] is the disable/enable code,
		 * d[1..8] the target SN. Only a real disable resets the ONU:
		 * 0xff for our SN, or 0x0f (disable all). An enable (0x00) must
		 * not reset, or the ONU fights the OLT re-allowing it.
		 */
		if (!((d[0] == 0xff && !memcmp(&d[1], o->sn, 8)) || d[0] == 0x0f))
			break;		/* enable / not our SN -> keep activating */
		ev(o, GPON_PLOAM_EV_DISABLE_SN, d[0], 0);
		/* A real Disable_SN tears down like a Deactivate. */
		fallthrough;

	case PLM_DS_DEACTIVATE_ONU:
		if (onu_id == o->onu_id || onu_id == 0xff) {
			ev(o, GPON_PLOAM_EV_DEACT, o->onu_id, 0);
			/*
			 * Teardown 1/4, OLT Deactivate or Disable_SN: full
			 * reset to O1, including the install flags and the
			 * hardware ONU-ID, so the next ranging rebuilds the
			 * OMCI datapath. Only this path clears
			 * data_gem_solicited: the OLT re-sends ME 268 after a
			 * real deprovision.
			 */
			o->onu_id = 0xff;
			o->omcc_installed = false;
			o->tcont_installed = false;
			o->data_installed = false;
			o->data_gem_solicited = false;
			o->aes_switch_time = 0xffffffff;
			o->key_staged = false;
			o->ops->set_hw_onu_id(o->sh, 0xff);
			/*
			 * Re-seat the serializer only if the last O5 was short
			 * (under ~5 s); a deactivate after a healthy O5 is not
			 * a serializer fault and a re-seat only forces another
			 * re-range.
			 */
			if (o->cfg->cdr_reseat_on_reactivate &&
			    !(o->state == GPON_O5_OPERATION && o->o5_entry_tick &&
			      (o->ticks - o->o5_entry_tick) > GPON_PLOAM_HEALTHY_O5_TICKS)) {
				o->ops->cdr_reseat(o->sh);
			} else if (o->cfg->cdr_reseat_on_reactivate) {
				ev(o, GPON_PLOAM_EV_DEACT_KEEP_LOCK,
				   o->o5_entry_tick ? o->ticks - o->o5_entry_tick : 0, 0);
			}
			set_state(o, GPON_O1_INITIAL, now_ms);
		}
		break;

	case PLM_DS_EXT_BURST_LENGTH:
		/*
		 * Extended_Burst_Length: d[0] is the Type-3 preamble length for
		 * the pre-ranged burst, d[1] for the ranged burst. The longer
		 * SN-burst preamble lets the OLT lock and range us. Only
		 * re-applied while still unassigned.
		 */
		o->boh_t3ranged = d[1];		/* applied at the O5 transition */
		if (o->onu_id == 0xff && o->boh_t3pre != d[0]) {
			o->boh_t3pre = d[0];
			apply_boh(o, false);
			ev(o, GPON_PLOAM_EV_EXT_BURST, o->boh_t3pre,
			   o->boh_t3ranged);
		}
		break;

	case PLM_DS_CONFIG_PORT:
		/*
		 * Configure_Port-ID: the OLT assigns the OMCC GEM port (d[0]
		 * bit 0 = enable, gem = (d[1] << 4) | (d[2] >> 4)). Install the
		 * OMCC datapath once, then acknowledge so the ONU is ready to
		 * receive before the OLT continues. The data GEM is installed
		 * later from the poll, once the OLT has created it via OMCI.
		 */
		if (onu_id == o->onu_id) {
			u16 gem = ((u16)d[1] << 4) | (d[2] >> 4);

			if ((d[0] & 0x1) && !o->omcc_installed) {
				if (!o->ops->install_omcc(o->sh, gem))
					o->omcc_installed = true;
			}
			send_ack(o, m);
			if (o->cfg->trace)
				ev(o, GPON_PLOAM_EV_ACK, type, 0);
		}
		break;

	case PLM_DS_ASSIGN_ALLOC_ID:
		/*
		 * Assign_Alloc-ID: alloc = (d[0] << 4) | (d[1] >> 4); d[2] 0x01
		 * = allocate, 0xff = deallocate. Then acknowledge.
		 *
		 * This is a data Alloc-ID: bind it to the data T-CONT. The OMCC
		 * T-CONT stays bound to the ONU-ID; rebinding it here makes the
		 * OMCC unreachable.
		 */
		if (onu_id == o->onu_id) {
			u16 alloc = ((u16)d[0] << 4) | (d[1] >> 4);

			ev(o, GPON_PLOAM_EV_ASSIGN_ALLOC, alloc, d[2]);
			if (d[2] == 0x01) {
				if (alloc != o->onu_id && !o->data_tcont_installed) {
					if (!o->ops->install_tcont(o->sh, o->cfg->data_tcont,
								   alloc)) {
						o->data_tcont_installed = true;
						o->data_alloc = alloc;
						ev(o, GPON_PLOAM_EV_DATA_TCONT,
						   alloc, o->cfg->data_tcont);
					}
				}
			} else if (d[2] == 0xff && o->data_tcont_installed &&
				   alloc == o->data_alloc) {
				/*
				 * Deallocated: allow a re-bind on the next
				 * allocate. The CAM binding is idempotent, so
				 * no teardown is needed.
				 */
				o->data_tcont_installed = false;
			}
			send_ack(o, m);
			if (o->cfg->trace)
				ev(o, GPON_PLOAM_EV_ACK, type, 0);
		}
		break;

	case PLM_DS_REQUEST_KEY:
		/*
		 * The OLT requests a downstream AES key; reply with
		 * Encryption_Key and load the same key into the staged bank.
		 */
		if (onu_id == o->onu_id) {
			send_key(o);
			ev(o, GPON_PLOAM_EV_REQ_KEY, 0, 0);
		}
		break;

	case PLM_DS_REQUEST_PASSWORD:
		/*
		 * Required to advance activation even though the value is
		 * ignored. Broadcast is accepted.
		 */
		if (onu_id == o->onu_id || onu_id == 0xff) {
			send_password(o);
			ev(o, GPON_PLOAM_EV_REQ_PW, 0, 0);
		}
		break;

	case PLM_DS_KEY_SWITCH:
		/*
		 * Key_Switching_Time: the 30-bit superframe count at which the
		 * hardware promotes the staged key. Arm the comparator and
		 * acknowledge; the OLT does not proceed to OMCI without this.
		 * The OLT repeats it, so arm once per value.
		 */
		if (onu_id == o->onu_id || onu_id == 0xff) {
			u32 fc = ((u32)(d[0] & 0x3f) << 24) | ((u32)d[1] << 16) |
				 ((u32)d[2] << 8) | d[3];

			/*
			 * Arm only once a key is staged, or a garbage key would
			 * be promoted. Acknowledge either way.
			 */
			if (o->key_staged && fc != o->aes_switch_time) {
				o->aes_switch_time = fc;
				o->ops->aes_arm_switch(o->sh, fc);
				ev(o, GPON_PLOAM_EV_KEY_SWITCH_ARM, fc, 0);
			}
			send_ack(o, m);
			ev(o, GPON_PLOAM_EV_KEY_SWITCH, o->key_staged ? 1 : 0,
			   o->aes_switch_time);
		}
		break;

	case PLM_DS_ENCRYPT_PORT:
		/* Encrypted_Port-ID requires an Acknowledge. */
		if (onu_id == o->onu_id) {
			send_ack(o, m);
			if (o->cfg->trace)
				ev(o, GPON_PLOAM_EV_ACK, type, 0);
		}
		break;

	case PLM_DS_CFG_VPVC:
		/*
		 * Configure_VP/VC: ATM, unsupported, but must still be
		 * acknowledged.
		 */
		if (onu_id == o->onu_id) {
			send_ack(o, m);
			ev(o, GPON_PLOAM_EV_ACK, type, 0);
		}
		break;

	case PLM_DS_BER_INTERVAL:
		/*
		 * BER_interval requires an Acknowledge. Remote_Error_Indication
		 * reporting is not implemented. Broadcast is accepted.
		 */
		if (onu_id == o->onu_id || onu_id == 0xff) {
			send_ack(o, m);
			ev(o, GPON_PLOAM_EV_ACK, type, 0);
		}
		break;

	default:
		/*
		 * Report unmodelled messages addressed to us. PEE, Power_Level,
		 * PST and Ranging_Adjustment need no acknowledge.
		 */
		if (onu_id == o->onu_id || onu_id == 0xff)
			ev(o, GPON_PLOAM_EV_UNHANDLED, type, onu_id);
		break;
	}

	o->ds_rx++;			/* downstream-lock liveness */
	return (int)(o->tx_total - tx0);
}

/* Poll steps, called in the order given in gpon_ploam.h */

/* The single tick source. */
u32 gpon_ploam_tick(struct gpon_ploam *o)
{
	o->ticks++;
	return o->ticks;
}

/*
 * The serial number was changed after ranging started: drop to O1 and offer the
 * new one.
 *
 * Teardown 2/4: keeps data_gem_solicited and does not re-seat the serializer.
 */
int gpon_ploam_sn_changed(struct gpon_ploam *o, u32 now_ms)
{
	u32 tx0 = o->tx_total;

	if (o->sn_changed) {
		o->sn_changed = false;
		if (o->state > GPON_O1_INITIAL) {
			o->onu_id = 0xff;
			o->omcc_installed = false;
			o->tcont_installed = false;
			o->data_installed = false;
			o->aes_switch_time = 0xffffffff;
			o->key_staged = false;
			o->ops->set_hw_onu_id(o->sh, 0xff);
			set_state(o, GPON_O1_INITIAL, now_ms);
			ev(o, GPON_PLOAM_EV_SN_REPROVISIONED, 0, 0);
		}
	}
	return (int)(o->tx_total - tx0);
}

/*
 * Two O5 steps, in this order:
 *
 * 1. Install the data GEM once the OLT has created it (ME 268); never ahead of
 * it. Done from the poll to keep the upstream NIC mode change out of the OMCI
 * receive softirq.
 *
 * 2. Report the VEIP operational state up, ~2500 ticks after O5 entry: OLTs
 * gate downstream user data on it, and reporting during configuration disrupts
 * it. Re-armed on each O5 entry.
 */
int gpon_ploam_poll_provision(struct gpon_ploam *o, u32 now_ms)
{
	(void)now_ms;

	if (o->state == GPON_O5_OPERATION && o->cfg->data_gem_en &&
	    o->omcc_installed && o->data_gem_solicited && !o->data_installed)
		o->ops->install_data_gem(o->sh);

	if (o->state == GPON_O5_OPERATION && o->omcc_installed &&
	    o->avc_sent < GPON_PLOAM_AVC_MAX && o->o5_entry_tick &&
	    (o->ticks - o->o5_entry_tick) > GPON_PLOAM_AVC_DELAY_TICKS &&
	    ((o->ticks - o->o5_entry_tick) % GPON_PLOAM_AVC_PERIOD_TICKS) == 0) {
		o->ops->omci_report_oper_up(o->sh);
		o->avc_sent++;
	}
	return 0;		/* emits no upstream PLOAM */
}

/*
 * O5 provisioning watchdog: if the ONU sits at O5 with no WAN traffic for too
 * long, the upstream serializer phase is likely bad and the OLT will not
 * deactivate us. Re-range, including a serializer re-seat.
 *
 * Teardown 3/4: keeps data_gem_solicited.
 */
int gpon_ploam_poll_watchdog(struct gpon_ploam *o, bool wan_rx_zero, u32 now_ms)
{
	u32 tx0 = o->tx_total;

	if (o->cfg->o5_provision_watchdog_ticks &&
	    o->state == GPON_O5_OPERATION && o->onu_id != 0xff &&
	    o->o5_entry_tick &&
	    (o->ticks - o->o5_entry_tick) > o->cfg->o5_provision_watchdog_ticks &&
	    wan_rx_zero) {
		ev(o, GPON_PLOAM_EV_O5_WATCHDOG, o->ticks - o->o5_entry_tick, 0);
		o->onu_id = 0xff;
		o->omcc_installed = false;
		o->tcont_installed = false;
		o->data_installed = false;
		o->aes_switch_time = 0xffffffff;
		o->key_staged = false;
		o->ops->set_hw_onu_id(o->sh, 0xff);
		if (o->cfg->cdr_reseat_on_reactivate)
			o->ops->cdr_reseat(o->sh);
		set_state(o, GPON_O1_INITIAL, now_ms);
	}
	return (int)(o->tx_total - tx0);
}

/*
 * Downstream LOS recovery. Without downstream light the OLT cannot deactivate
 * us, so on sustained LOS tear down to O1 and re-acquire when light returns.
 *
 * A real fibre loss drops both the GTC optical LOS and the SerDes signal
 * detect; either one alone can blip during I2C access or a SerDes re-seat.
 * Requiring both allows a short debounce.
 *
 * Teardown 4/4.
 */
int gpon_ploam_poll_los(struct gpon_ploam *o, bool optic_los, bool sds_dark,
			u32 now_ms)
{
	u32 tx0 = o->tx_total;

	if (o->cfg->los_rerange_ticks && o->state >= GPON_O2_STANDBY) {
		if (optic_los && sds_dark) {
			if (++o->los_run == o->cfg->los_rerange_ticks) {
				ev(o, GPON_PLOAM_EV_LOS_RERANGE, o->los_run, 0);
				o->onu_id = 0xff;
				o->omcc_installed = false;
				o->tcont_installed = false;
				o->data_installed = false;
				/*
				 * Keep data_gem_solicited: the OLT did not
				 * deprovision us, so it keeps the GEM and does
				 * not re-send ME 268 after the outage. The GEM
				 * is reinstalled as soon as O5 is reached
				 * again.
				 */
				o->aes_switch_time = 0xffffffff;
				o->key_staged = false;
				o->ops->set_hw_onu_id(o->sh, 0xff);
				if (o->cfg->cdr_reseat_on_reactivate)
					o->ops->cdr_reseat(o->sh);
				set_state(o, GPON_O1_INITIAL, now_ms);
			}
		} else {
			o->los_run = 0;
		}
	}
	return (int)(o->tx_total - tx0);
}

/* While unregistered at O3, re-offer the serial number about twice a second. */
int gpon_ploam_poll_sn_reoffer(struct gpon_ploam *o, u32 now_ms)
{
	u32 tx0 = o->tx_total;

	(void)now_ms;
	if (o->state >= GPON_O3_SERIAL && o->onu_id == 0xff &&
	    (o->ticks % GPON_PLOAM_SN_REOFFER_TICKS) == 0)
		send_sn(o);
	return (int)(o->tx_total - tx0);
}

/*
 * Optional periodic No_message at O5 (default off): the FSM otherwise sends no
 * upstream PLOAM once ranged, and the hardware auto No_message template can be
 * overwritten by other sends.
 */
int gpon_ploam_poll_keepalive(struct gpon_ploam *o, u32 now_ms)
{
	u32 tx0 = o->tx_total;

	(void)now_ms;
	if (o->state == GPON_O5_OPERATION && o->onu_id != 0xff &&
	    o->cfg->o5_ploam_keepalive_ticks &&
	    (o->ticks % o->cfg->o5_ploam_keepalive_ticks) == 0) {
		u8 nomsg[GPON_PLOAM_US_LEN];

		build_nomsg(nomsg);
		ploam_tx(o, PLM_US_QUEUE_NOMSG, nomsg);
	}
	return (int)(o->tx_total - tx0);
}

/* Identity and lifecycle */

/*
 * Parse "XPON12345678" -> {'X','P','O','N',0x12,0x34,0x56,0x78}. An invalid hex
 * digit gives 0xf, as the kernel's hex_to_bin() returning -1 did.
 */
static u8 sn_hex_nibble(char c)
{
	if (c >= '0' && c <= '9')
		return (u8)(c - '0');
	if (c >= 'a' && c <= 'f')
		return (u8)(c - 'a' + 10);
	if (c >= 'A' && c <= 'F')
		return (u8)(c - 'A' + 10);
	return 0xf;			/* hex_to_bin() returned -1 */
}

void gpon_ploam_parse_sn(const char *s, u8 sn[8])
{
	int i;

	if (!s || !sn)
		return;
	for (i = 0; i < 4 && s[i]; i++)
		sn[i] = (u8)s[i];
	for (i = 0; i < 4; i++) {
		u8 hi = 0, lo = 0;

		if (s[4 + 2 * i])
			hi = sn_hex_nibble(s[4 + 2 * i]);
		if (s[4 + 2 * i + 1])
			lo = sn_hex_nibble(s[4 + 2 * i + 1]);
		sn[4 + i] = (u8)((hi << 4) | lo);
	}
}

void gpon_ploam_set_sn(struct gpon_ploam *o, const u8 sn[8])
{
	memcpy(o->sn, sn, sizeof(o->sn));
	o->sn_changed = true;
}

void gpon_ploam_set_data_gem_solicited(struct gpon_ploam *o, bool solicited)
{
	o->data_gem_solicited = solicited;
}

void gpon_ploam_set_data_installed(struct gpon_ploam *o, bool installed)
{
	o->data_installed = installed;
}

void gpon_ploam_init(struct gpon_ploam *o, const struct gpon_ploam_ops *ops,
		     const struct gpon_ploam_cfg *cfg, void *sh, const u8 sn[8])
{
	static const u8 delim_default[3] = GPON_PLOAM_BOH_DELIM_DEFAULT;

	memset(o, 0, sizeof(*o));
	o->ops = ops;
	o->cfg = cfg;
	o->sh  = sh;

	if (sn)
		memcpy(o->sn, sn, sizeof(o->sn));
	o->onu_id = 0xff;
	o->state  = GPON_O1_INITIAL;
	o->boh_ptn = GPON_PLOAM_BOH_PTN_DEFAULT;
	memcpy(o->boh_delim, delim_default, sizeof(o->boh_delim));
	o->aes_switch_time = 0xffffffff;
}

const char *gpon_ploam_state_name(enum gpon_ostate st)
{
	switch (st) {
	case GPON_O1_INITIAL:	return "O1-Initial";
	case GPON_O2_STANDBY:	return "O2-Standby";
	case GPON_O3_SERIAL:	return "O3-Serial-Number";
	case GPON_O4_RANGING:	return "O4-Ranging";
	case GPON_O5_OPERATION:	return "O5-Operation";
	case GPON_O6_POPUP:	return "O6-POPUP";
	case GPON_O7_EMERGENCY:	return "O7-Emergency-Stop";
	}
	return "O?-unknown";
}
