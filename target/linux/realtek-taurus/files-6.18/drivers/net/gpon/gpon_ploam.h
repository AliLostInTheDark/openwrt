/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) BRULE Herman <alpha_one_x86@first-world.info> (github.com/alphaonex86)
 *
 * Taken from https://github.com/alphaonex86/openwrt.
 *
 * G.984.3 PLOAM activation state machine (O1..O7) for ONUs whose MAC does
 * not run PLOAM in hardware: downstream message dispatch, the activation
 * transitions, upstream message builders, burst-overhead and equalization
 * delay computation, and the periodic poll decisions. All side effects go
 * through struct gpon_ploam_ops, time is passed in by the caller, and wire
 * fields are extracted with explicit byte math.
 */
#ifndef GPON_PLOAM_H
#define GPON_PLOAM_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/string.h>
#else
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
/* Host builds: the UAPI <linux/types.h> lacks the kernel's u8/u16/u32. */
#ifndef GPON_HOST_TYPES_DEFINED
#define GPON_HOST_TYPES_DEFINED
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
#endif
#endif /* !__KERNEL__ */

#include "gpon_common.h"

/* G.984.3 PLOAM message types and upstream queue selectors */
#define PLM_DS_UPSTREAM_OVERHEAD	0x01
#define PLM_DS_ASSIGN_ONU_ID		0x03
#define PLM_DS_RANGING_TIME		0x04
#define PLM_DS_DEACTIVATE_ONU		0x05
#define PLM_DS_DISABLE_SN		0x06
#define PLM_DS_EXT_BURST_LENGTH		0x14
#define PLM_DS_ENCRYPT_PORT		0x08	/* Encrypted_Port-ID (ACK) */
#define PLM_DS_REQUEST_KEY		0x0d	/* Request_key (-> Encryption_Key) */
#define PLM_DS_KEY_SWITCH		0x13	/* Key_Switching_Time (-> arm HW key switch) */
#define PLM_DS_ASSIGN_ALLOC_ID		0x0a	/* Assign_Alloc-ID (ACK) */
#define PLM_DS_REQUEST_PASSWORD		0x09	/* Request_Password (-> US Password 0x02) */
#define PLM_DS_CONFIG_PORT		0x0e	/* Configure_Port-ID (ACK) */
#define PLM_DS_CFG_VPVC			0x07	/* Configure_VP/VC (ATM, unsupported; ACK) */
#define PLM_DS_BER_INTERVAL		0x12	/* BER interval (ACK; arms BER reporting) */
#define PLM_US_ENCRYPT_KEY		0x05	/* US Encryption_Key response */
#define PLM_US_PASSWORD			0x02	/* US Password response (to Request_Password) */
#define PLM_US_SERIAL_NUMBER		0x01
#define PLM_US_ACKNOWLEDGE		0x09	/* US Acknowledge message type */
#define PLM_US_NO_MESSAGE		0x04
#define PLM_US_QUEUE_SN			0x6	/* US_PLOAM_IND[10:8] auto-SN queue */
#define PLM_US_QUEUE_URG		0x1	/* US_PLOAM_IND[10:8] urgent queue (ACKs) */
#define PLM_US_QUEUE_NOMSG		0x7	/* US_PLOAM_IND[10:8] HW auto-No_message slot */

/*
 * A downstream PLOAM is 13 bytes: [0] ONU-ID, [1] type, [2..11] data,
 * [12] CRC-8. An upstream PLOAM is 12 bytes; hardware appends the CRC.
 * GPON_PLOAM_US_LEN is also defined by gpon_common.h.
 */
#ifndef GPON_PLOAM_DS_LEN
#define GPON_PLOAM_DS_LEN		13u
#endif
#ifndef GPON_PLOAM_US_LEN
#define GPON_PLOAM_US_LEN		12u
#endif

/*
 * Burst-overhead geometry: 12 stored BOH_DATA bytes (96 overhead bits),
 * which the hardware extends to BOH_LENGTH via the repeat pointer; 252 is
 * the BOH_LENGTH field limit.
 */
#define GPON_PLOAM_BOH_LEN		12
#define GPON_PLOAM_BOH_MAX_LEN		252

/* One upstream GTC frame in bits (19440 octets at 1.24416 Gbit/s). */
#define GPON_PLOAM_EQD_FRAME_LEN	(19440 * 8)

/*
 * Diagnostic events passed to ->trace(). The core never formats strings;
 * the shell renders events and may read the context for more detail.
 * A NULL ->trace never changes a decision.
 */
enum gpon_ploam_ev {
	GPON_PLOAM_EV_STATE,		/* a = previous O-state, b = new O-state */
	GPON_PLOAM_EV_RERANGE_DONE,	/* a = re-range count, b = outage ms */
	GPON_PLOAM_EV_BOH,		/* a = ranged?, b = BOH_CFG word */
	GPON_PLOAM_EV_DS,		/* a = DS ONU-ID, b = DS type */
	GPON_PLOAM_EV_O3_FEED_RESET,	/* - */
	GPON_PLOAM_EV_ONU_ID,		/* a = assigned ONU-ID, b = T-CONT16 alloc */
	GPON_PLOAM_EV_CAM_READBACK,	/* a = T-CONT, b = wanted alloc */
	GPON_PLOAM_EV_RANGING_TIME,	/* a = EqD */
	GPON_PLOAM_EV_DISABLE_SN,	/* a = disable code d[0] */
	GPON_PLOAM_EV_DEACT,		/* a = our ONU-ID at the time */
	GPON_PLOAM_EV_DEACT_KEEP_LOCK,	/* a = ticks held at O5 (reseat skipped) */
	GPON_PLOAM_EV_EXT_BURST,	/* a = t3pre, b = t3ranged */
	GPON_PLOAM_EV_ACK,		/* a = acknowledged DS type */
	GPON_PLOAM_EV_ASSIGN_ALLOC,	/* a = alloc, b = op code d[2] */
	GPON_PLOAM_EV_DATA_TCONT,	/* a = alloc, b = data T-CONT */
	GPON_PLOAM_EV_REQ_KEY,		/* - */
	GPON_PLOAM_EV_KEY_SENT,		/* a = key index */
	GPON_PLOAM_EV_REQ_PW,		/* - */
	GPON_PLOAM_EV_KEY_SWITCH_ARM,	/* a = superframe count armed */
	GPON_PLOAM_EV_KEY_SWITCH,	/* a = key staged?, b = armed superframe */
	GPON_PLOAM_EV_UNHANDLED,	/* a = DS type, b = DS ONU-ID */
	GPON_PLOAM_EV_SN_REPROVISIONED,	/* - */
	GPON_PLOAM_EV_O5_WATCHDOG,	/* a = ticks held at O5 with no WAN RX */
	GPON_PLOAM_EV_LOS_RERANGE,	/* a = consecutive LOS ticks */
};

/*
 * Side effects the state machine asks the shell to perform. The call order
 * matters: on the O3 edge burst overhead is written before EqD, and on the
 * O4 edge the upstream PLOAM flush follows the ranged BOH switch.
 */
struct gpon_ploam_ops {
	/* Upstream PLOAM transmit */

	/*
	 * Enqueue one 12-byte upstream PLOAM on @queue (PLM_US_QUEUE_*). The
	 * urgent queue pre-empts the auto-SN burst, which gets an Acknowledge
	 * to the OLT in time.
	 */
	void (*ploam_tx)(void *sh, u8 queue, const u8 m[GPON_PLOAM_US_LEN]);

	/* Burst overhead and ranging */

	/* Write BOH_CFG = @cfg_word and the first @size BOH_DATA bytes. */
	void (*boh_write)(void *sh, u32 cfg_word, const u8 *oh, u8 size);
	/* Read US_MIN_DELAY[15:7], needed for the EqD computation. */
	u32  (*get_min_delay)(void *sh);
	/* Write the equalization delay from its two computed halves. */
	void (*set_eqd)(void *sh, u32 multiframe, u32 intraframe);
	/*
	 * Flush the shared CPU upstream PLOAM buffer so no pre-ranged burst
	 * is latched when the first ranged grant arrives.
	 */
	void (*us_ploam_flush)(void *sh);

	/* State reflection */

	/* Reflect the O-state into hardware and the PON LED. */
	void (*set_hw_state)(void *sh, enum gpon_ostate st);
	/* Write the ONU-ID into both hardware fields (upstream and downstream). */
	void (*set_hw_onu_id)(void *sh, u8 onu_id);
	/* The FSM dropped below O5; re-arm whatever the shell gates on O5. */
	void (*on_below_o5)(void *sh);
	/*
	 * The FSM entered O5 with burst re-arm enabled: re-apply the upstream
	 * burst registers. The core sends the following No_message.
	 */
	void (*o5_rearm_burst)(void *sh);

	/* Analog / serializer */

	/* Relock the TX CMU/PLL at O3 entry (needed for cold start). */
	void (*analog_relock)(void *sh);
	/* Datapath reset edge after the relock that restarts the GEM US feed. */
	void (*o3_feed_reset)(void *sh);
	/* Re-seat the US serializer on re-range: reset edge plus CDR pulse. */
	void (*cdr_reseat)(void *sh);

	/* AES key exchange */

	/* Random bytes (injected for reproducible tests). */
	void (*rng)(void *sh, u8 *out, unsigned int len);
	/* Load the key into the hardware staged bank. */
	void (*aes_stage_key)(void *sh, const u8 key[16]);
	/* Arm the key-switch superframe comparator. */
	void (*aes_arm_switch)(void *sh, u32 superframe);

	/*
	 * GEM / T-CONT provisioning. Table operations may time out; on a
	 * non-zero return the matching installed flag stays clear so the next
	 * provisioning event retries.
	 */
	int  (*install_omcc)(void *sh, u16 gem);
	int  (*install_tcont)(void *sh, u8 tcont, u16 alloc);
	void (*install_data_gem)(void *sh);

	/* OMCI */

	/* Report the VEIP operational state up to the OLT. */
	void (*omci_report_oper_up)(void *sh);

	/* Diagnostics; NULL is always allowed */
	void (*trace)(void *sh, enum gpon_ploam_ev ev, u32 a, u32 b);
};

/*
 * Per-board configuration, owned by the shell and read by the core on every
 * use, so the shell can expose the fields as writable module parameters.
 */
struct gpon_ploam_cfg {
	/* Keep the FSM at O1 so the ONU never ranges. */
	bool hold;
	/* Per-PLOAM tracing; only gates ->trace() calls. */
	bool trace;
	/* Also bind the OMCC Alloc-ID to the alternate T-CONT (non-standard). */
	bool omcc_alt_bind;
	/* Re-seat the US serializer on a re-range. */
	bool cdr_reseat_on_reactivate;
	/* Install the data GEM once the OLT has created it. */
	bool data_gem_en;
	/* Re-apply the O5 burst registers on every O5 entry. */
	bool o5_rearm_burst_gate;
	/* Take the datapath reset edge after the O3 relock. */
	bool o3_feed_reset;

	/* OMCC Alloc-ID override; 0 = use the ONU-ID (G.984.3 default). */
	u16 omcc_alloc_override;

	/*
	 * Consecutive (optic_los && sds_dark) poll ticks before a downstream
	 * LOS re-range. 0 disables.
	 */
	unsigned int los_rerange_ticks;
	/* Re-range after this many ticks at O5 with no WAN RX. 0 disables. */
	unsigned int o5_provision_watchdog_ticks;
	/* Send a No_message upstream every N ticks at O5. 0 disables. */
	unsigned int o5_ploam_keepalive_ticks;

	/* T-CONT indices used by this silicon */
	u8 omcc_tcont;
	u8 omcc_tcont_alt;	/* used only with omcc_alt_bind */
	u8 data_tcont;
};

/*
 * FSM context. All state lives here, none at file scope. The shell may
 * read any field; only the core writes.
 */
struct gpon_ploam {
	const struct gpon_ploam_ops *ops;
	const struct gpon_ploam_cfg *cfg;
	void *sh;			/* opaque shell handle passed to every op */

	/* identity */
	u8 sn[8];			/* G.984.3 ONU-SN: 4-byte ID + 4-byte serial */
	u8 onu_id;			/* 0xff = unassigned */
	bool sn_changed;		/* SN was (re)provisioned -> must re-range */

	/* activation state */
	enum gpon_ostate state;
	u32 ticks;			/* FSM poll ticks (~10 ms each) */
	u32 o5_entry_tick;		/* tick of the last O5 entry (0 = not at O5) */
	int avc_sent;			/* oper-state AVCs emitted this O5 */
	u32 los_run;			/* consecutive real-LOS ticks */

	/* burst overhead learned from the OLT */
	u8 boh_guard;			/* Upstream_Overhead d[0] = guard bits */
	u8 boh_ptn;			/* Upstream_Overhead d[3] = Type-3 pattern */
	u8 boh_delim[3];		/* Upstream_Overhead d[4..6] */
	u8 boh_t3pre;			/* Ext_Burst_Length d[0] = pre-ranged Type-3 len */
	u8 boh_t3ranged;		/* Ext_Burst_Length d[1] = ranged Type-3 len */

	/*
	 * Provisioning flags set and cleared by PLOAM events and poll
	 * teardowns. The table work is done by the install ops.
	 */
	bool omcc_installed;		/* OMCC GEM datapath installed (one-shot) */
	/* Never set to true; only cleared and reported. */
	bool tcont_installed;		/* OMCC T-CONT/alloc-id bound (one-shot) */
	/*
	 * Set by the shell from inside its data install, through
	 * gpon_ploam_set_data_installed(), before the upstream NIC mode change
	 * that reads it.
	 */
	bool data_installed;		/* WAN data GEM datapath installed */
	bool data_gem_solicited;	/* OLT sent the OMCI ME268 Create -> may install */
	bool data_tcont_installed;	/* the OLT's data Alloc-ID bound to the data T-CONT */
	u16  data_alloc;		/* the OLT's data Alloc-ID */

	/* AES key exchange */
	u8   aes_key[16];
	u8   key_index;
	u32  aes_switch_time;		/* last armed Key_Switching_Time (de-dup) */
	bool key_staged;		/* a valid key is loaded in the staged bank */

	/* outage / re-range accounting (time in ms, supplied by the shell) */
	u32 rerange_start_ms;		/* 0 = no outage in flight */
	u32 rerange_last_log_ms;	/* 0 = never logged (flap damping) */
	u32 rerange_cnt;
	u32 last_outage_ms;

	/* statistics */
	u32 sn_tx;			/* Serial_Number_ONU messages emitted */
	u32 ds_rx;			/* DS PLOAMs dispatched (DS-lock liveness) */
	u32 tx_total;			/* US PLOAMs emitted (all queues) */
	u8  last_ds_type;		/* last DS PLOAM type seen */
};

/*
 * Entry points. Each takes @now_ms, a free-running millisecond clock from
 * the shell, used only through wrap-safe differences.
 *
 * The poll steps are separate calls because the shell interleaves its own
 * work between them; call them once per tick in this order:
 *
 *	gpon_ploam_tick()
 *	gpon_ploam_sn_changed()
 *	gpon_ploam_ds()			for each downstream message
 *	gpon_ploam_poll_provision()
 *	gpon_ploam_poll_watchdog()
 *	gpon_ploam_poll_los()
 *	gpon_ploam_poll_sn_reoffer()
 *	gpon_ploam_poll_keepalive()
 */

/*
 * Initialise the context. @sn may be NULL. Sets the G.984.3 burst defaults
 * and parks the FSM at O1; calls no op.
 */
void gpon_ploam_init(struct gpon_ploam *o, const struct gpon_ploam_ops *ops,
		     const struct gpon_ploam_cfg *cfg, void *sh, const u8 sn[8]);

/*
 * Parse a serial number in the form "XPON12345678" (4 ASCII vendor-ID
 * characters and 8 hex digits) into 8 wire bytes.
 */
void gpon_ploam_parse_sn(const char *s, u8 sn[8]);

/*
 * Install a new serial number; the re-range happens on the next
 * gpon_ploam_sn_changed() call.
 */
void gpon_ploam_set_sn(struct gpon_ploam *o, const u8 sn[8]);

/*
 * The OLT created the data GEM (OMCI ME 268). The data GEM is never
 * installed before that.
 */
void gpon_ploam_set_data_gem_solicited(struct gpon_ploam *o, bool solicited);

/* The shell installed (or tore down) the data GEM datapath. */
void gpon_ploam_set_data_installed(struct gpon_ploam *o, bool installed);

/*
 * Advance the tick counter and return the new value, so the shell has a
 * single tick source for its own cadences.
 */
u32 gpon_ploam_tick(struct gpon_ploam *o);

/*
 * Dispatch one downstream PLOAM. Frames shorter than GPON_PLOAM_DS_LEN are
 * dropped. Returns the number of upstream PLOAMs sent.
 */
int gpon_ploam_ds(struct gpon_ploam *o, const u8 *m, unsigned int len, u32 now_ms);

/* Poll steps; each returns the number of upstream PLOAMs sent. */
int gpon_ploam_sn_changed(struct gpon_ploam *o, u32 now_ms);
int gpon_ploam_poll_provision(struct gpon_ploam *o, u32 now_ms);
/* @wan_rx_zero: the WAN netdev has received nothing since the last re-range. */
int gpon_ploam_poll_watchdog(struct gpon_ploam *o, bool wan_rx_zero, u32 now_ms);
/*
 * @optic_los: GTC optical loss of signal; @sds_dark: SerDes signal detect
 * lost. Only both together indicate a real fibre pull.
 */
int gpon_ploam_poll_los(struct gpon_ploam *o, bool optic_los, bool sds_dark, u32 now_ms);
int gpon_ploam_poll_sn_reoffer(struct gpon_ploam *o, u32 now_ms);
int gpon_ploam_poll_keepalive(struct gpon_ploam *o, u32 now_ms);

/* G.984.3 O-state name, for logs. */
const char *gpon_ploam_state_name(enum gpon_ostate st);

#endif /* GPON_PLOAM_H */
