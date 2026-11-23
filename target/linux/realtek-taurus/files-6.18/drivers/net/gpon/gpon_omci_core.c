// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) BRULE Herman <alpha_one_x86@first-world.info> (github.com/alphaonex86)
 *
 * Taken from https://github.com/alphaonex86/openwrt.
 *
 * ITU-T G.988 OMCI baseline message layer: parse a downstream PDU, dispatch
 * it by message type and build the upstream response. Message contents start
 * at octet 8; only responses that carry a result code use that octet for it.
 * ME storage is reached through gpon_omci_me.h. The caller serialises calls.
 */
#include <linux/crc32.h>
#include <linux/string.h>

#include "gpon_omci_core.h"
#include "gpon_omci_me.h"	/* struct omci_onu + the ME model / dynamic store */

static inline void omci_put_be16(u8 *p, u16 v)
{
	p[0] = (u8)(v >> 8);
	p[1] = (u8)v;
}

/*
 * MIC (bytes 44..47): the I.363.5 / AAL5 CRC-32 over bytes 0..43 (G.984.4),
 * i.e. the non-reflected crc32_be with all-ones init and final complement,
 * stored big-endian. The MAC's own OMCI CRC insertion stays enabled; it
 * writes the same bytes. A wrong MIC makes the OLT silently drop every
 * response.
 */
static void omci_set_mic(u8 *msg)
{
	u32 c = ~crc32_be(~0u, msg, 44);

	msg[44] = (u8)(c >> 24);
	msg[45] = (u8)(c >> 16);
	msg[46] = (u8)(c >> 8);
	msg[47] = (u8)c;
}

/* Stamp the baseline trailer (40..43 = 00 00 00 28) and MIC. Call last. */
static void omci_finalize(u8 *msg)
{
	msg[40] = 0x00;
	msg[41] = 0x00;
	msg[42] = 0x00;
	msg[43] = 0x28;
	omci_set_mic(msg);
}

/*
 * Get response: result (8), attribute mask (9..10), values (11..35), and the
 * optional-attribute and attribute-execution masks G.988 always reserves at
 * 36..39, so the value area is 25 octets.
 *
 *   unsupported = requested & ~known            -> octets 36..37
 *   failed      = requested & known & ~returned -> octets 38..39
 *   result      = 0x09 when either is set, else 0x00
 *
 * Reporting unsupported/failed attributes stops the OLT from re-reading them
 * forever. OLT-created MEs are answered from the dynamic store; answering
 * UNKNOWN_ME for those aborts the OLT's config load.
 */
static u8 omci_get_fill(struct omci_onu *o, u16 class_id, u16 inst, u16 mask,
			u8 *resp)
{
	u16 rmask = 0, known = 0, unsup, failed;
	u8 rc = omci_me_fill(o, class_id, inst, mask, resp + 11, resp + 36,
			     &rmask, &known);

	if (rc == OMCI_RC_UNKNOWN_ME) {
		struct omci_me_inst *e = omci_store_find(o, class_id, inst);

		if (!e) {
			/*
			 * If the OLT created other instances of this class,
			 * only the instance is unknown (0x05), otherwise the
			 * class is (0x04).
			 */
			omci_put_be16(resp + 9, 0);
			return omci_store_has_class(o, class_id) ?
					OMCI_RC_UNKNOWN_INST :
					OMCI_RC_UNKNOWN_ME;
		}
		/*
		 * No descriptor exists for an OLT-created class: replay the
		 * stored body as-is and echo the requested mask. Reporting the
		 * attributes unsupported would make the OLT abandon the ME.
		 */
		memcpy(resp + 11, e->body, e->blen > 25 ? 25 : e->blen);
		rmask = mask;
		known = mask;
	}

	omci_put_be16(resp + 9, rmask);
	unsup = (u16)(mask & ~known);
	failed = (u16)(mask & known & ~rmask);
	omci_put_be16(resp + 36, unsup);
	omci_put_be16(resp + 38, failed);
	return (unsup | failed) ? OMCI_RC_ATTR_FAILED : OMCI_RC_OK;
}

/*
 * Create / Set / Delete: apply or NAK, and bump MIB-Data-Sync only when the
 * MIB actually changed, so an OLT MIB audit can detect any divergence.
 *   Create: duplicate instance -> 0x07, full store -> 0x09, else store it.
 *   Delete: absent instance -> 0x05.
 *   Set:    unknown class -> 0x04, known class + absent instance -> 0x05.
 * Set attributes are not validated individually: OLTs set ME 131 (OLT-G)
 * attributes the ONU does not model and expect OK.
 */
static u8 omci_config_apply(struct omci_onu *o, u8 mt, u16 class_id, u16 inst,
			    const u8 *msg, unsigned int len)
{
	struct omci_me_inst *e = omci_store_find(o, class_id, inst);
	u16 mask;

	switch (mt) {
	case OMCI_MT_CREATE:
		/*
		 * Only OLT-created instances count as existing: OLTs create
		 * ME 262/268 instances that the static model also describes.
		 */
		if (e)
			return OMCI_RC_INST_EXISTS;
		if (!omci_store_put(o, class_id, inst, msg + 8,
				    len > 8 ? (int)(len - 8) : 0))
			return OMCI_RC_ATTR_FAILED;
		break;
	case OMCI_MT_DELETE:
		if (!e)
			return OMCI_RC_UNKNOWN_INST;
		omci_store_del(o, class_id, inst);
		break;
	default:					/* OMCI_MT_SET */
		if (len < 10)		/* no attribute mask on the wire */
			return OMCI_RC_PARAM_ERROR;
		if (!omci_inst_exists(o, class_id, inst))
			return (omci_class_modelled(class_id) ||
				omci_store_has_class(o, class_id)) ?
					OMCI_RC_UNKNOWN_INST :
					OMCI_RC_UNKNOWN_ME;
		mask = ((u16)msg[8] << 8) | msg[9];
		if (e)
			omci_store_merge(e, msg + 10, (int)(len - 10));
		/*
		 * A Set of ME 2 attribute 1 writes MIB-Data-Sync; the +1 for
		 * this Set still applies afterwards.
		 */
		if (class_id == OMCI_ME_ONU_DATA && len >= 11 &&
		    (mask & 0x8000))
			o->mds = msg[10];
		break;
	}

	/*
	 * MIB-Data-Sync: +1 per applied config message, wrapping 255 -> 1
	 * (0 means just reset).
	 */
	if (++o->mds == 0)
		o->mds = 1;
	return OMCI_RC_OK;
}

int omci_onu_input(struct omci_onu *o, const u8 *msg, unsigned int len, u8 *resp)
{
	u16 class_id, inst;
	u8 mt, devid;

	if (len < 8)
		return 0;
	devid = msg[3];
	mt = msg[2] & 0x1f;
	class_id = ((u16)msg[4] << 8) | msg[5];
	inst = ((u16)msg[6] << 8) | msg[7];

	if (devid != 0x0a) {
		/*
		 * Only the baseline message set is supported (ONU2-G advertises
		 * OMCC version 0x80). Extended requests (devid 0x0b) cannot be
		 * answered in baseline format, so they are counted and dropped.
		 */
		if (devid == 0x0b)
			o->rx_extended++;
		return 0;
	}

	/*
	 * G.988 11.2.2.1: the OMCC is stop-and-wait, so a byte-identical
	 * repeat of the last answered request is a retransmission after our
	 * response was lost. Replay the stored response instead of executing
	 * it again, which would bump MIB-Data-Sync twice for one transaction.
	 * Bytes 40..47 are derived, so 0..39 identify the request.
	 */
	if (o->have_last && len >= 40 && !memcmp(msg, o->last_req, 40)) {
		memcpy(resp, o->last_resp, OMCI_LEN);
		o->dup_replay++;
		return OMCI_LEN;
	}

	memset(resp, 0, OMCI_LEN);
	resp[0] = msg[0];			/* TID echo */
	resp[1] = msg[1];
	resp[2] = (msg[2] & 0x1f) | 0x20;	/* clear AR, set AK */
	resp[3] = 0x0a;
	resp[4] = msg[4];			/* class echo */
	resp[5] = msg[5];
	resp[6] = msg[6];			/* instance echo */
	resp[7] = msg[7];

	switch (mt) {
	case OMCI_MT_MIB_RESET:
		/*
		 * Zero MIB-Data-Sync (the OLT restarts its count from 0) and
		 * drop all OLT-created instances.
		 */
		o->mds = 0;
		memset(o->store, 0, sizeof(o->store));
		o->store_n = 0;
		resp[8] = OMCI_RC_OK;
		break;
	case OMCI_MT_MIB_UPLOAD:
		/* Row count at octets 8..9, no result byte. */
		omci_put_be16(resp + 8, o->nrows + o->store_n);
		break;
	case OMCI_MT_GET:
		/* Without a mask the reply would carry stale bytes. */
		if (len < 10)
			return 0;
		resp[8] = omci_get_fill(o, class_id, inst,
					((u16)msg[8] << 8) | msg[9], resp);
		break;
	case OMCI_MT_SET:
	case OMCI_MT_CREATE:
	case OMCI_MT_DELETE:
		resp[8] = omci_config_apply(o, mt, class_id, inst, msg, len);
		break;
	case OMCI_MT_GET_ALL_ALARMS:
		/* Alarm count at octets 8..9, no result byte. */
		omci_put_be16(resp + 8, 0x0000);	/* no active alarms */
		break;
	case OMCI_MT_MIB_UPLOAD_NX: {
		/*
		 * Sequence number at msg[8..9]; reply is class (8..9),
		 * instance (10..11), attribute mask (12..13) and values
		 * (14..39), no result byte.
		 */
		u16 seq;
		u16 wmask = 0, wknown = 0;

		if (len < 10)
			return 0;
		seq = ((u16)msg[8] << 8) | msg[9];
		if (seq < o->nrows) {
			const struct omci_mib_row *r = &o->rows[seq];

			omci_put_be16(resp + 8, r->class_id);
			omci_put_be16(resp + 10, r->inst);
			omci_me_fill(o, r->class_id, r->inst, r->mask,
				     resp + 14, resp + 40, &wmask, &wknown);
			omci_put_be16(resp + 12, wmask);
		} else if (seq < o->nrows + o->store_n) {
			/*
			 * OLT-created MEs follow the static rows with mask 0;
			 * their values are served via Get.
			 */
			const struct omci_me_inst *e =
				omci_store_nth(o, seq - o->nrows);

			if (e) {
				omci_put_be16(resp + 8, e->class_id);
				omci_put_be16(resp + 10, e->inst);
			}
		}
		/* out-of-range seq -> all-zero row, still well-formed */
		break;
	}
	case OMCI_MT_TEST:
	case OMCI_MT_SYNC_TIME:
	case OMCI_MT_REBOOT:		/* ACK, do NOT actually reboot */
	case OMCI_MT_START_SW_DL:
	case OMCI_MT_DOWNLOAD_SEC:
	case OMCI_MT_END_SW_DL:
	case OMCI_MT_ACTIVATE_SW:
	case OMCI_MT_COMMIT_SW:
		/*
		 * Not performed (no software image handling), but acknowledged
		 * so the OLT's provisioning completes.
		 */
		resp[8] = OMCI_RC_OK;
		break;
	case OMCI_MT_GET_ALL_ALRM_NX:
	case OMCI_MT_GET_NEXT:
		/*
		 * No table attributes or alarms are modelled: result 0x00 with
		 * empty contents means end of table. resp is already zero.
		 */
		break;
	default:
		/*
		 * No ONU-side action: answer 0x00 with empty contents. Silence
		 * can make the OLT deactivate the ONU and 0x02 can abort its
		 * config load.
		 */
		o->unhandled++;
		break;
	}

	omci_finalize(resp);

	/*
	 * The retransmission cache may only hold the response to the most
	 * recent request. If this one is not acknowledged or too short to
	 * identify, drop the old entry: the MIB may have changed since.
	 */
	if ((msg[2] & 0x40) && len >= 40) {
		memcpy(o->last_req, msg, 40);
		memcpy(o->last_resp, resp, OMCI_LEN);
		o->have_last = true;
	} else {
		o->have_last = false;
	}

	/* AR clear: the message was applied but must not be acknowledged. */
	if (!(msg[2] & 0x40)) {
		o->no_ack++;
		return 0;
	}
	return OMCI_LEN;
}

/*
 * Autonomous AVC (MT 0x11, TID 0): report that the attributes in @mask of
 * (class, inst) changed to @val.
 */
static void omci_emit_avc(struct omci_onu *o, u16 class_id, u16 inst, u16 mask,
			  const u8 *val, unsigned int vlen, u8 *out)
{
	memset(out, 0, OMCI_LEN);
	out[2] = OMCI_MT_AVC;
	out[3] = 0x0a;
	omci_put_be16(out + 4, class_id);
	omci_put_be16(out + 6, inst);
	omci_put_be16(out + 8, mask);
	if (val && vlen) {
		if (vlen > 30)
			vlen = 30;
		memcpy(out + 10, val, vlen);
	}
	omci_finalize(out);
	o->avc_count++;
}

int omci_onu_emit_veip_up_avc(struct omci_onu *o, u8 *out)
{
	/* VEIP 0x0601 attribute 2 (operational state), 0 = enabled. */
	static const u8 up;

	omci_emit_avc(o, OMCI_ME_VEIP, 0x0601, OMCI_ATTR_BIT(2), &up, 1, out);
	o->avc_veip_up_sent = true;
	return OMCI_LEN;
}
