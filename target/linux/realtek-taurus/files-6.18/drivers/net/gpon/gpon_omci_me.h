/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) BRULE Herman <alpha_one_x86@first-world.info> (github.com/alphaonex86)
 *
 * Taken from https://github.com/alphaonex86/openwrt.
 *
 * ITU-T G.988 managed-entity model: table-driven attribute descriptors and
 * the filler that walks them, the static MIB-Upload row table, and the store
 * of instances created by the OLT. Message handling is in gpon_omci_core.c.
 * Attribute values are emitted big-endian with explicit byte math.
 */
#ifndef GPON_OMCI_ME_H
#define GPON_OMCI_ME_H

#include <linux/types.h>

#include "gpon_omci_core.h"

/*
 * An ME instance created by the OLT. Kept so Get and MIB-Upload reflect the
 * configured MIB; otherwise the OLT's audit fails and it eventually
 * deactivates the ONU.
 */
#define OMCI_STORE_MAX		64
struct omci_me_inst {
	u16	class_id;
	u16	inst;
	u8	body[26];	/* set-by-create attribute bytes */
	u8	blen;
	bool	used;
};

/*
 * One MIB-Upload-Next row: (class, instance, attribute mask) whose selected
 * attributes fit the 26-byte Upload-Next value area.
 */
struct omci_mib_row {
	u16	class_id;
	u16	inst;
	u16	mask;
};

#define OMCI_MIB_ROWS_MAX	72

struct omci_onu {
	u8	sn[8];			/* PLOAM serial number (vendor+VSSN) */
	u8	mds;			/* ME 2 attr 1: MIB-Data-Sync */
	u16	nrows;			/* static MIB row count */
	u16	store_n;		/* provisioned-ME count */
	struct omci_mib_row	rows[OMCI_MIB_ROWS_MAX];
	struct omci_me_inst	store[OMCI_STORE_MAX];
	/*
	 * G.988 11.2.2.1 retained last response. The OMCC is stop-and-wait,
	 * so one entry covers every retransmission.
	 */
	u8	last_req[40];		/* bytes 0..39 (trailer+MIC derived) */
	u8	last_resp[OMCI_LEN];
	bool	have_last;
	/* statistics */
	u32	unhandled;		/* DS message types with no ONU action */
	u32	dup_replay;		/* retransmissions served from the cache */
	u32	rx_extended;		/* devid 0x0b frames seen (not served) */
	u32	no_ack;			/* AR clear: applied, not answered */
	u32	avc_count;		/* autonomous AVC frames emitted */
	bool	avc_veip_up_sent;
	/*
	 * ME 263 ANI-G attribute 10 (RX) and 14 (TX) optical levels in G.988
	 * wire form (2's complement, 0.002 dB units referred to 1 mW). Start
	 * at the fallback values below and are updated from the transceiver
	 * DDM; @anig_live is set once a real measurement was stored.
	 */
	u16	anig_rx_level;
	u16	anig_tx_level;
	bool	anig_live;
};

/*
 * ANI-G optical levels served until a DDM read succeeds: 0xeedc = -8.77 dBm
 * received, 0x04d7 = +2.47 dBm launched. Not measured values.
 */
#define OMCI_ANIG_RX_FALLBACK	0xeedc
#define OMCI_ANIG_TX_FALLBACK	0x04d7

/*
 * Publish a measured optical level into ME 263 attributes 10/14. The caller
 * does the (sleeping) DDM read outside the responder lock and passes values
 * already in wire form.
 */
static inline void omci_onu_set_optical(struct omci_onu *o, u16 rx_level,
					u16 tx_level)
{
	o->anig_rx_level = rx_level;
	o->anig_tx_level = tx_level;
	o->anig_live = true;
}

/*
 * ME class IDs presented in the MIB upload. ONU_DATA and VEIP are also
 * defined, identically, in gpon_omci_core.h.
 */
#define OMCI_ME_ONU_DATA	2
#define OMCI_ME_CARDHOLDER	5
#define OMCI_ME_CIRCUIT_PACK	6
#define OMCI_ME_SW_IMAGE	7
#define OMCI_ME_PPTP_ETH_UNI	11	/* OLTs count these for HGU mode */
#define OMCI_ME_OLT_G		131
#define OMCI_ME_ONU_G		256
#define OMCI_ME_ONU2_G		257
#define OMCI_ME_TCONT		262
#define OMCI_ME_ANI_G		263
#define OMCI_ME_UNI_G		264
#define OMCI_ME_PRIORITY_QUEUE	277
#define OMCI_ME_TRAFFIC_SCHED	278
#define OMCI_ME_VEIP		329
#define OMCI_ME_CTC_LOID_AUTH	65530	/* 0xFFFA, CTC extension */

/*
 * Reset the responder and rebuild the static MIB rows. @mds_seed is the boot
 * MIB-Data-Sync value; it must not match the OLT's stored copy, so that the
 * OLT's audit fails and it re-provisions from MIB-Reset.
 */
void omci_onu_init(struct omci_onu *o, const u8 sn[8], u8 mds_seed);

/* ME model API used by gpon_omci_core.c; documented at the definitions. */

/* dynamic (OLT-provisioned) ME store */
struct omci_me_inst *omci_store_find(struct omci_onu *o, u16 class_id, u16 inst);
bool omci_store_has_class(struct omci_onu *o, u16 class_id);
struct omci_me_inst *omci_store_nth(struct omci_onu *o, u16 idx);
bool omci_store_put(struct omci_onu *o, u16 class_id, u16 inst,
		    const u8 *body, int blen);
void omci_store_merge(struct omci_me_inst *e, const u8 *val, int vlen);
void omci_store_del(struct omci_onu *o, u16 class_id, u16 inst);

/*
 * Emit the attributes in @mask into [v..end) in descriptor order, reporting
 * what was emitted and what the ME models.
 */
u8 omci_me_fill(struct omci_onu *o, u16 class_id, u16 inst, u16 mask,
		u8 *v, const u8 *end, u16 *rmask_out, u16 *known_out);

/* does the model carry this class at all (descriptor row or vendor range)? */
bool omci_class_modelled(u16 class_id);

/* is (class, inst) a MIB instance this ONU holds? */
bool omci_inst_exists(struct omci_onu *o, u16 class_id, u16 inst);

#endif /* GPON_OMCI_ME_H */
