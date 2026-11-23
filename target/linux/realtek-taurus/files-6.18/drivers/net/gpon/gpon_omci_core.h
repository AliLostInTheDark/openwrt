/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) BRULE Herman <alpha_one_x86@first-world.info> (github.com/alphaonex86)
 *
 * Taken from https://github.com/alphaonex86/openwrt.
 *
 * ITU-T G.988 OMCI baseline message layer: message types, result codes,
 * request parsing, dispatch by message type and construction of the
 * upstream response including trailer and MIC. Managed-entity storage lives
 * in the ME model (gpon_omci_me.c). All wire access is explicit byte math, so
 * the same source works on big- and little-endian CPUs.
 */
#ifndef GPON_OMCI_CORE_H
#define GPON_OMCI_CORE_H

#include <linux/types.h>

#define OMCI_LEN		48	/* baseline message, incl. trailer+MIC */

/* Message types (G.988 Table 11.2.2-1). */
#define OMCI_MT_CREATE		0x04
#define OMCI_MT_DELETE		0x06
#define OMCI_MT_SET		0x08
#define OMCI_MT_GET		0x09
#define OMCI_MT_GET_ALL_ALARMS	0x0b
#define OMCI_MT_GET_ALL_ALRM_NX	0x0c
#define OMCI_MT_MIB_UPLOAD	0x0d
#define OMCI_MT_MIB_UPLOAD_NX	0x0e
#define OMCI_MT_MIB_RESET	0x0f
#define OMCI_MT_ALARM		0x10	/* ONU-autonomous alarm */
#define OMCI_MT_AVC		0x11	/* ONU-autonomous notification */
#define OMCI_MT_TEST		0x12
#define OMCI_MT_START_SW_DL	0x13
#define OMCI_MT_DOWNLOAD_SEC	0x14
#define OMCI_MT_END_SW_DL	0x15
#define OMCI_MT_ACTIVATE_SW	0x16
#define OMCI_MT_COMMIT_SW	0x17
#define OMCI_MT_SYNC_TIME	0x18
#define OMCI_MT_REBOOT		0x19
#define OMCI_MT_GET_NEXT	0x1a

/*
 * Result codes (G.988 Table 11.2.2-2): 0..7 and 9 are assigned; 8 and
 * anything above 9 must never be sent.
 */
#define OMCI_RC_OK		0x00
/*
 * Not emitted: an unimplemented action is answered with 0x00 and empty
 * contents, as some OLTs abort their config load on 0x02.
 */
#define OMCI_RC_NOT_SUPPORTED	0x02
#define OMCI_RC_PARAM_ERROR	0x03
#define OMCI_RC_UNKNOWN_ME	0x04
#define OMCI_RC_UNKNOWN_INST	0x05
#define OMCI_RC_INST_EXISTS	0x07
#define OMCI_RC_ATTR_FAILED	0x09

/* Attribute-mask bit: attr #n is bit (16-n), so bit15 = attr 1. */
#define OMCI_ATTR_BIT(n)	(1u << (16 - (n)))

/*
 * ME classes the message layer itself handles: a Set of ONU-DATA attribute 1
 * is a MIB-Data-Sync write, and the VEIP AVC is emitted here. gpon_omci_me.h
 * defines them identically.
 */
#define OMCI_ME_ONU_DATA	2
#define OMCI_ME_VEIP		329

/* Owned by the ME-model layer; the message layer only ever holds a pointer. */
struct omci_onu;

/*
 * Process one downstream baseline PDU and fill @resp (48 bytes, trailer and
 * MIC included). Returns OMCI_LEN, or 0 when no response must be sent (runt,
 * non-baseline, or AR clear). With AR clear the request has still been
 * applied; only the acknowledge is suppressed.
 */
int omci_onu_input(struct omci_onu *o, const u8 *req, unsigned int len, u8 *resp);

/*
 * Autonomous VEIP (ME 329) operational-state-up AVC. Some OLTs gate
 * downstream user data on this report. Fills @out (48 bytes, trailer and MIC
 * included) and returns OMCI_LEN.
 */
int omci_onu_emit_veip_up_avc(struct omci_onu *o, u8 *out);

#endif /* GPON_OMCI_CORE_H */
