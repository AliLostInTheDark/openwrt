// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) BRULE Herman <alpha_one_x86@first-world.info> (github.com/alphaonex86)
 *
 * Taken from https://github.com/alphaonex86/openwrt.
 *
 * ITU-T G.988 managed-entity model: the board identity data and table-driven
 * attribute descriptors with one filler shared by Get and MIB-Upload-Next,
 * the static MIB-Upload row table, and the store of OLT-created instances.
 */
#include <linux/string.h>

#include "gpon_omci_me.h"

/* OLT-created ME store */

struct omci_me_inst *omci_store_find(struct omci_onu *o, u16 class_id,
				     u16 inst)
{
	u16 k;

	for (k = 0; k < OMCI_STORE_MAX; k++)
		if (o->store[k].used && o->store[k].class_id == class_id &&
		    o->store[k].inst == inst)
			return &o->store[k];
	return NULL;
}

/*
 * Does the store hold any instance of @class_id? Separates "unknown class"
 * (0x04) from "unknown instance" (0x05).
 */
bool omci_store_has_class(struct omci_onu *o, u16 class_id)
{
	u16 k;

	for (k = 0; k < OMCI_STORE_MAX; k++)
		if (o->store[k].used && o->store[k].class_id == class_id)
			return true;
	return false;
}

/* idx-th used entry in array order, for the MIB-Upload tail rows. */
struct omci_me_inst *omci_store_nth(struct omci_onu *o, u16 idx)
{
	u16 k, n = 0;

	for (k = 0; k < OMCI_STORE_MAX; k++)
		if (o->store[k].used) {
			if (n == idx)
				return &o->store[k];
			n++;
		}
	return NULL;
}

/*
 * Insert one OLT-created instance. Returns false when the store is full; the
 * caller must then NAK so MIB-Data-Sync does not advance and the OLT's audit
 * can detect the mismatch and re-provision.
 */
bool omci_store_put(struct omci_onu *o, u16 class_id, u16 inst,
		    const u8 *body, int blen)
{
	struct omci_me_inst *e = NULL;
	u16 k;

	for (k = 0; k < OMCI_STORE_MAX; k++)
		if (!o->store[k].used) {
			e = &o->store[k];
			break;
		}
	if (!e)
		return false;

	e->used = true;
	e->class_id = class_id;
	e->inst = inst;
	e->blen = 0;
	o->store_n++;
	if (body && blen > 0) {
		if (blen > (int)sizeof(e->body))
			blen = sizeof(e->body);
		memcpy(e->body, body, blen);
		e->blen = (u8)blen;
	}
	return true;
}

/*
 * Apply a Set's attribute values to an OLT-created instance. The body is an
 * opaque blob (there is no attribute-to-offset map for these classes), so
 * the values are written at its head as for Create; a later Get replays it.
 */
void omci_store_merge(struct omci_me_inst *e, const u8 *val, int vlen)
{
	if (vlen <= 0)
		return;
	if (vlen > (int)sizeof(e->body))
		vlen = sizeof(e->body);
	memcpy(e->body, val, vlen);
	if (e->blen < (u8)vlen)
		e->blen = (u8)vlen;
}

void omci_store_del(struct omci_onu *o, u16 class_id, u16 inst)
{
	struct omci_me_inst *e = omci_store_find(o, class_id, inst);

	if (e) {
		e->used = false;
		if (o->store_n)
			o->store_n--;
	}
}

/*
 * ME attribute model
 *
 * Constant attribute bytes live in one pool so a descriptor row can refer to
 * them by a 2-byte offset instead of a pointer.
 */
#define OMCI_B_VENDOR		0	/* "ZYOT"          (4)  */
#define OMCI_B_ONU_G_VER	4	/* "Hardware1.0"   (14) */
#define OMCI_B_SW_BANK0		18	/* "M225-260525"   (14) */
#define OMCI_B_SW_BANK1		32	/* "M225-260515"   (14) */
#define OMCI_B_EQUIP_ID		46	/* "-5221ZY"    (20) */
#define OMCI_B_LOID		66	/* "user"          (24) */
#define OMCI_B_OPER_ID		90	/* "CTC"           (4)  */
#define OMCI_B_ZEROS		94	/* zeros           (16) */

static const u8 omci_blob[] = {
	/* [0] vendor ID: ONU-G #1, Circuit-Pack #5 */
	'Z', 'Y', 'O', 'T',
	/* [4] ONU-G #2 version, zero-padded to 14 */
	'H', 'a', 'r', 'd', 'w', 'a', 'r', 'e', '1', '.', '0', 0, 0, 0,
	/* [18] SW-image bank 0 (active) version, also Circuit-Pack #4 */
	'M', '2', '2', '5', '-', '2', '6', '0', '5', '2', '5', 0, 0, 0,
	/* [32] SW-image bank 1 version */
	'M', '2', '2', '5', '-', '2', '6', '0', '5', '1', '5', 0, 0, 0,
	/* [46] ONU2-G #1 equipment ID, zero-padded to 20 */
	'A', 'O', 'T', '-', '5', '2', '2', '1', 'Z', 'Y',
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	/* [66] logical ONU ID: ONU-G #10, CTC LoID #2, zero-padded to 24 */
	'u', 's', 'e', 'r',
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	/* [90] CTC #1 operation ID, zero-padded to 4 */
	'C', 'T', 'C', 0,
	/*
	 * [94] all-zero source: SW-image #5 hash (16), ONU-G #11 logical
	 * password / CTC #3 password (12)
	 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

_Static_assert(sizeof(omci_blob) == 110, "omci_blob offsets and sizes drifted");

/* Where a descriptor row takes its value bytes from. */
enum omci_attr_src {
	OMCI_SRC_CONST,		/* v = the value, big-endian in `size` bytes */
	OMCI_SRC_BLOB,		/* v = byte offset into omci_blob[] */
	OMCI_SRC_SN,		/* the board serial number (8) */
	OMCI_SRC_MDS,		/* ME 2 #1 = the live MIB-Data-Sync */
	OMCI_SRC_DYN,		/* v = enum omci_attr_dyn */
};

/* The few attributes whose value is derived from the ME INSTANCE. */
enum omci_attr_dyn {
	OMCI_DYN_SW_VER,	/* ME 7 #1: per-bank version blob */
	OMCI_DYN_SW_FLAG,	/* ME 7 #2/#3: bank 0 is the active+committed */
	OMCI_DYN_TCONT_ALLOC,	/* ME 262 #1: alloc-ID of this T-CONT */
	OMCI_DYN_PQ_PORT,	/* ME 277 #6: related port, queue n -> 7 - n */
	OMCI_DYN_TS_TCONT,	/* ME 278 #1: T-CONT pointer == the instance */
	OMCI_DYN_ANIG_RX,	/* ME 263 #10: live RX optical level */
	OMCI_DYN_ANIG_TX,	/* ME 263 #14: live TX optical level */
};

/*
 * One modelled attribute. Rows of a class are contiguous and in ascending
 * attribute order, which is the order G.988 packs them on the wire. A class
 * with no modelled attributes has one marker row with attr 0.
 */
struct omci_attr {
	u16	class_id;
	u16	v;
	u8	attr;
	u8	size;
	u8	src;
};

#define AT(cls, n, sz, s, arg)	{ (cls), (arg), (n), (sz), (s) }
#define A_C(cls, n, sz, val)	AT(cls, n, sz, OMCI_SRC_CONST, val)
#define A_B(cls, n, sz, off)	AT(cls, n, sz, OMCI_SRC_BLOB, off)
#define A_SN(cls, n)		AT(cls, n, 8, OMCI_SRC_SN, 0)
#define A_MDS(cls, n)		AT(cls, n, 1, OMCI_SRC_MDS, 0)
#define A_D(cls, n, sz, dyn)	AT(cls, n, sz, OMCI_SRC_DYN, dyn)
#define A_NO_ATTRS(cls)		AT(cls, 0, 0, OMCI_SRC_CONST, 0)

static const struct omci_attr omci_attrs[] = {
	/* ME 2 ONU-Data (inst 0) */
	A_MDS(2, 1),				/* #1  MIB-Data-Sync */

	/* ME 256 ONU-G (inst 0): all 14 attributes must be served */
	A_B(256,  1,  4, OMCI_B_VENDOR),	/* #1  Vendor ID */
	A_B(256,  2, 14, OMCI_B_ONU_G_VER),	/* #2  Version */
	A_SN(256, 3),				/* #3  Serial number */
	A_C(256,  4,  1, 0x02),			/* #4  Traffic-mgmt option */
	A_C(256,  5,  1, 0x00),			/* #5  ATM CC option */
	A_C(256,  6,  1, 0x00),			/* #6  Battery backup */
	A_C(256,  7,  1, 0x00),			/* #7  Admin state */
	A_C(256,  8,  1, 0x00),			/* #8  Op state */
	A_C(256,  9,  1, 0x00),			/* #9  Survival time */
	A_B(256, 10, 24, OMCI_B_LOID),		/* #10 Logical ONU ID */
	A_B(256, 11, 12, OMCI_B_ZEROS),		/* #11 Logical password */
	A_C(256, 12,  1, 0x00),			/* #12 Credentials status */
	A_C(256, 13,  2, 0x0000),		/* #13 Ext TC-layer options */
	A_C(256, 14,  1, 0x01),			/* #14 ONT state */

	/* ME 257 ONU2-G (inst 0) */
	A_B(257,  1, 20, OMCI_B_EQUIP_ID),	/* #1  Equipment ID */
	A_C(257,  2,  1, 0x80),			/* #2  OMCC version: baseline */
	A_C(257,  3,  2, 0x0000),		/* #3  Vendor product code */
	A_C(257,  4,  1, 0x01),			/* #4  Security capability */
	A_C(257,  5,  1, 0x01),			/* #5  Security mode */
	A_C(257,  6,  2, 0x0060),		/* #6  Total priority queues */
	A_C(257,  7,  1, 0x0c),			/* #7  Total traffic scheds */
	A_C(257,  8,  1, 0x01),			/* #8  Mode */
	A_C(257,  9,  2, 0x0040),		/* #9  Total GEM ports */
	A_C(257, 10,  4, 3600),			/* #10 SysUpTime (4 bytes) */
	A_C(257, 11,  2, 0x007f),		/* #11 Connectivity capability */
	A_C(257, 12,  1, 0x00),			/* #12 Current conn mode */
	A_C(257, 13,  2, 0x003b),		/* #13 QoS config flexibility */
	A_C(257, 14,  2, 0x0001),		/* #14 Priority-queue scale */

	/* ME 5 Cardholder (inst 0x0101) */
	A_C(5, 1, 1, 47),			/* #1  Actual type = Eth UNI */
	A_C(5, 2, 1, 47),			/* #2  Expected type */
	A_C(5, 3, 1, 1),			/* #3  Expected port count */

	/* ME 6 Circuit-Pack (inst 0x0101) */
	A_C(6,  1,  1, 47),			/* #1  Type */
	A_C(6,  2,  1, 1),			/* #2  Number of ports */
	A_SN(6, 3),				/* #3  Serial number */
	A_B(6,  4, 14, OMCI_B_SW_BANK0),	/* #4  Version */
	A_B(6,  5,  4, OMCI_B_VENDOR),		/* #5  Vendor ID */
	A_C(6, 12,  1, 8),			/* #12 Total priority queues */

	/* ME 7 Software-Image, banks 0 (active) and 1 */
	A_D(7, 1, 14, OMCI_DYN_SW_VER),		/* #1  Version */
	A_D(7, 2,  1, OMCI_DYN_SW_FLAG),	/* #2  Is committed */
	A_D(7, 3,  1, OMCI_DYN_SW_FLAG),	/* #3  Is active */
	A_C(7, 4,  1, 1),			/* #4  Is valid */
	A_B(7, 5, 16, OMCI_B_ZEROS),		/* #5  Image hash */

	/* ME 11 PPTP Ethernet UNI (inst 0x0101) */
	A_C(11,  1, 1, 47),			/* #1  Expected type */
	A_C(11,  2, 1, 47),			/* #2  Sensed type */
	A_C(11,  3, 1, 0),			/* #3  Auto-detect config */
	A_C(11,  4, 1, 0),			/* #4  Eth loopback config */
	A_C(11,  5, 1, 0),			/* #5  Admin state (unlocked) */
	/*
	 * Operational state must be enabled (0): with the UNI reported disabled
	 * the OLT does not create the data GEM port.
	 */
	A_C(11,  6, 1, 0),			/* #6  Op state = enabled */
	A_C(11,  7, 1, 0),			/* #7  Config ind */
	A_C(11,  8, 2, 1518),			/* #8  Max frame size */
	A_C(11,  9, 1, 0),			/* #9  DTE/DCE ind */
	A_C(11, 10, 2, 0xffff),			/* #10 Pause time */
	A_C(11, 11, 1, 2),			/* #11 Bridged/IP ind */
	A_C(11, 12, 1, 0),			/* #12 ARC */
	A_C(11, 13, 1, 0),			/* #13 ARC interval */
	A_C(11, 14, 1, 0),			/* #14 PPPoE filter */
	A_C(11, 15, 1, 0),			/* #15 Power control */

	/* ME 131 OLT-G: known, no modelled attributes (the OLT sets it) */
	A_NO_ATTRS(131),

	/* ME 262 T-CONT (inst 0x8000..0x800b) */
	A_D(262, 1, 2, OMCI_DYN_TCONT_ALLOC),	/* #1  Alloc-ID */
	A_C(262, 2, 1, 1),			/* #2  Mode indicator */
	A_C(262, 3, 1, 0),			/* #3  Policy */

	/* ME 263 ANI-G (inst 0x8001) */
	A_C(263,  1, 1, 1),			/* #1  SR indication */
	A_C(263,  2, 2, 12),			/* #2  Total T-CONTs */
	A_C(263,  3, 2, 48),			/* #3  GEM block length */
	A_C(263,  4, 1, 0),			/* #4  Piggyback DBA */
	A_C(263,  5, 1, 0),			/* #5  (deprecated) */
	A_C(263,  6, 1, 5),			/* #6  SF threshold */
	A_C(263,  7, 1, 9),			/* #7  SD threshold */
	A_C(263,  8, 1, 0),			/* #8  ARC */
	A_C(263,  9, 1, 0),			/* #9  ARC interval */
	/*
	 * #10/#14 are the optical levels from the transceiver DDM (see
	 * omci_onu_set_optical()), or OMCI_ANIG_{RX,TX}_FALLBACK until read.
	 */
	A_D(263, 10, 2, OMCI_DYN_ANIG_RX),	/* #10 RX optical level */
	A_C(263, 11, 1, 0xff),			/* #11 Lower optical thresh */
	A_C(263, 12, 1, 0xff),			/* #12 Upper optical thresh */
	A_C(263, 13, 2, 0x0000),		/* #13 ONU response time */
	A_D(263, 14, 2, OMCI_DYN_ANIG_TX),	/* #14 TX optical level */
	A_C(263, 15, 1, 0x81),			/* #15 Lower TX power thresh */
	A_C(263, 16, 1, 0x81),			/* #16 Upper TX power thresh */

	/* ME 264 UNI-G (inst 0x0101) */
	A_C(264, 1, 2, 0x0000),			/* #1  Config-option status */
	A_C(264, 2, 1, 0),			/* #2  Admin state */
	A_C(264, 3, 1, 1),			/* #3  Management capability */
	A_C(264, 4, 2, 0x0000),			/* #4  Non-OMCI mgmt ID */
	A_C(264, 5, 2, 0x0000),			/* #5  Relay-agent options */

	/* ME 277 Priority-Queue (inst 0..7) */
	A_C(277, 1, 1, 1),			/* #1  Queue config option */
	A_C(277, 2, 2, 3276),			/* #2  Max queue size */
	A_C(277, 3, 2, 3276),			/* #3  Allocated queue size */
	A_C(277, 4, 2, 0),			/* #4  Discard reset interval */
	A_C(277, 5, 2, 0),			/* #5  Threshold value */
	A_D(277, 6, 4, OMCI_DYN_PQ_PORT),	/* #6  Related port */
	A_C(277, 7, 2, 0x0000),			/* #7  Traffic-sched pointer */
	A_C(277, 8, 1, 1),			/* #8  Weight */

	/* ME 278 Traffic-Scheduler (inst 0x8000..0x800b) */
	A_D(278, 1, 2, OMCI_DYN_TS_TCONT),	/* #1  T-CONT pointer */
	A_C(278, 2, 2, 0x0000),			/* #2  Traffic-sched pointer */
	A_C(278, 3, 1, 1),			/* #3  Policy */
	A_C(278, 4, 1, 0),			/* #4  Priority/weight */

	/* ME 329 VEIP (inst 0x0601) */
	A_C(329, 1, 1, 0),			/* #1  Admin state */
	A_C(329, 2, 1, 0),			/* #2  Op state */
	A_C(329, 3, 2, 0x0000),			/* #3  Interworking-TP ptr */

	/* ME 65530 CTC LoID authentication (inst 0) */
	A_B(65530, 1,  4, OMCI_B_OPER_ID),	/* #1  Operation ID */
	A_B(65530, 2, 24, OMCI_B_LOID),		/* #2  LoID */
	A_B(65530, 3, 12, OMCI_B_ZEROS),	/* #3  Password (empty) */
	A_C(65530, 4,  1, 0x01),		/* #4  Auth status = success */

	{ 0, 0, 0, 0, 0 },			/* terminator */
};

/*
 * Is @class_id in a G.988 vendor-reserved range (240..255, 350..399,
 * 65280..65535)? These are treated as known MEs without attributes, since
 * answering UNKNOWN_ME aborts some OLTs' config load. They are not part of
 * the MIB upload.
 */
static bool omci_vendor_class(u16 class_id)
{
	return (class_id >= 240 && class_id <= 255) ||
	       (class_id >= 350 && class_id <= 399) ||
	       class_id >= 65280;
}

/* First descriptor row of @class_id, or NULL if the model does not carry it. */
static const struct omci_attr *omci_me_find(u16 class_id)
{
	const struct omci_attr *a;

	for (a = omci_attrs; a->class_id; a++)
		if (a->class_id == class_id)
			return a;
	return NULL;
}

/* Is the class modelled, by a descriptor or as a vendor class? */
bool omci_class_modelled(u16 class_id)
{
	return omci_me_find(class_id) || omci_vendor_class(class_id);
}

/*
 * The bytes of one attribute. Integers are big-endian, right-aligned in
 * @size octets; @scratch must hold 4 bytes.
 */
static const u8 *omci_attr_bytes(const struct omci_onu *o,
				 const struct omci_attr *a, u16 inst,
				 u8 *scratch)
{
	u32 val;

	switch (a->src) {
	case OMCI_SRC_BLOB:
		return omci_blob + a->v;
	case OMCI_SRC_SN:
		return o->sn;
	case OMCI_SRC_MDS:
		val = o->mds;
		break;
	case OMCI_SRC_DYN:
		switch (a->v) {
		case OMCI_DYN_SW_VER:
			return omci_blob + (inst ? OMCI_B_SW_BANK1 :
						   OMCI_B_SW_BANK0);
		case OMCI_DYN_SW_FLAG:
			val = inst ? 0 : 1;
			break;
		case OMCI_DYN_TCONT_ALLOC:
			val = (inst == 0x8000) ? 0x0100 : 0x00ff;
			break;
		case OMCI_DYN_PQ_PORT:
			val = ((u32)0x0101 << 16) | (7u - (inst & 7));
			break;
		case OMCI_DYN_ANIG_RX:
			val = o->anig_rx_level;
			break;
		case OMCI_DYN_ANIG_TX:
			val = o->anig_tx_level;
			break;
		default:	/* OMCI_DYN_TS_TCONT */
			val = inst;
			break;
		}
		break;
	default:		/* OMCI_SRC_CONST */
		val = a->v;
		break;
	}
	scratch[0] = (u8)(val >> 24);
	scratch[1] = (u8)(val >> 16);
	scratch[2] = (u8)(val >> 8);
	scratch[3] = (u8)val;
	return scratch + 4 - a->size;
}

/*
 * Attribute filler shared by Get and MIB-Upload-Next. @mask selects
 * attributes (bit 15 = attribute 1), emitted into [v..end) in descriptor
 * order. An attribute that does not fit is skipped; a later smaller one may
 * still be emitted.
 *   *rmask_out = the attributes actually emitted
 *   *known_out = every attribute this ME models, requested or not
 */
u8 omci_me_fill(struct omci_onu *o, u16 class_id, u16 inst, u16 mask,
		u8 *v, const u8 *end, u16 *rmask_out, u16 *known_out)
{
	const struct omci_attr *a = omci_me_find(class_id);
	u16 rmask = 0, known = 0;
	bool over = false;

	*rmask_out = 0;
	*known_out = 0;
	if (!a)
		return omci_vendor_class(class_id) ? OMCI_RC_OK :
						     OMCI_RC_UNKNOWN_ME;

	for (; a->class_id == class_id; a++) {
		u16 bit;
		u8 scratch[4];

		if (!a->attr)			/* marker row: no attributes */
			continue;
		bit = (u16)OMCI_ATTR_BIT(a->attr);
		known |= bit;
		if (!(mask & bit))
			continue;
		if (v + a->size > end) {
			over = true;
			continue;
		}
		memcpy(v, omci_attr_bytes(o, a, inst, scratch), a->size);
		v += a->size;
		rmask |= bit;
	}

	*rmask_out = rmask;
	*known_out = known;
	return over ? OMCI_RC_ATTR_FAILED : OMCI_RC_OK;
}

/*
 * Build the static MIB-Upload row table: every auto-instantiated ME, split so
 * each row's attributes fit the 26-byte Upload-Next value area. It also
 * defines which instances exist, so a Set of an instance not listed here
 * (and not created by the OLT) is answered 0x05.
 */
static void omci_build_mib(struct omci_onu *o)
{
	u16 n = 0;
	u16 i;

#define ROW(c, ins, m) do {						\
		if (n < OMCI_MIB_ROWS_MAX) {				\
			o->rows[n].class_id = (c);			\
			o->rows[n].inst = (ins);			\
			o->rows[n].mask = (m);				\
			n++;						\
		}							\
	} while (0)

	ROW(OMCI_ME_ONU_DATA, 0x0000, OMCI_ATTR_BIT(1));

	/*
	 * ME 256 ONU-G, split by the 26-byte limit:
	 * A = vid(4)+ver(14)+sn(8) = 26, B = #4..#9 = 6x1, C = LoID(24),
	 * D = #11(12)+#12(1)+#13(2)+#14(1) = 16.
	 */
	ROW(OMCI_ME_ONU_G, 0x0000, OMCI_ATTR_BIT(1) | OMCI_ATTR_BIT(2) |
				   OMCI_ATTR_BIT(3));
	ROW(OMCI_ME_ONU_G, 0x0000, OMCI_ATTR_BIT(4) | OMCI_ATTR_BIT(5) |
				   OMCI_ATTR_BIT(6) | OMCI_ATTR_BIT(7) |
				   OMCI_ATTR_BIT(8) | OMCI_ATTR_BIT(9));
	ROW(OMCI_ME_ONU_G, 0x0000, OMCI_ATTR_BIT(10));
	ROW(OMCI_ME_ONU_G, 0x0000, OMCI_ATTR_BIT(11) | OMCI_ATTR_BIT(12) |
				   OMCI_ATTR_BIT(13) | OMCI_ATTR_BIT(14));

	/* ME 257 ONT2-G: A = EquipmentID(20), B = all scalars (21B). */
	ROW(OMCI_ME_ONU2_G, 0x0000, OMCI_ATTR_BIT(1));
	ROW(OMCI_ME_ONU2_G, 0x0000, OMCI_ATTR_BIT(2) | OMCI_ATTR_BIT(3) |
				    OMCI_ATTR_BIT(4) | OMCI_ATTR_BIT(5) |
				    OMCI_ATTR_BIT(6) | OMCI_ATTR_BIT(7) |
				    OMCI_ATTR_BIT(8) | OMCI_ATTR_BIT(9) |
				    OMCI_ATTR_BIT(10) | OMCI_ATTR_BIT(11) |
				    OMCI_ATTR_BIT(12) | OMCI_ATTR_BIT(13) |
				    OMCI_ATTR_BIT(14));

	ROW(OMCI_ME_CARDHOLDER, 0x0101, OMCI_ATTR_BIT(1) | OMCI_ATTR_BIT(2) |
					OMCI_ATTR_BIT(3));

	/* ME 6 Circuit-Pack: A = #1..#4 = 24B, B = #5(4)+#12(1) = 5B. */
	ROW(OMCI_ME_CIRCUIT_PACK, 0x0101, OMCI_ATTR_BIT(1) | OMCI_ATTR_BIT(2) |
					  OMCI_ATTR_BIT(3) | OMCI_ATTR_BIT(4));
	ROW(OMCI_ME_CIRCUIT_PACK, 0x0101, OMCI_ATTR_BIT(5) | OMCI_ATTR_BIT(12));

	/*
	 * ME 7 Software-Image, two banks: A = ver+committed+active+valid = 17B,
	 * B = hash(16).
	 */
	ROW(OMCI_ME_SW_IMAGE, 0x0000, OMCI_ATTR_BIT(1) | OMCI_ATTR_BIT(2) |
				      OMCI_ATTR_BIT(3) | OMCI_ATTR_BIT(4));
	ROW(OMCI_ME_SW_IMAGE, 0x0000, OMCI_ATTR_BIT(5));
	ROW(OMCI_ME_SW_IMAGE, 0x0001, OMCI_ATTR_BIT(1) | OMCI_ATTR_BIT(2) |
				      OMCI_ATTR_BIT(3) | OMCI_ATTR_BIT(4));
	ROW(OMCI_ME_SW_IMAGE, 0x0001, OMCI_ATTR_BIT(5));

	/* ME 11 PPTP Ethernet UNI: #1..#15 = 17B, one row. */
	ROW(OMCI_ME_PPTP_ETH_UNI, 0x0101, OMCI_ATTR_BIT(1) | OMCI_ATTR_BIT(2) |
					  OMCI_ATTR_BIT(3) | OMCI_ATTR_BIT(4) |
					  OMCI_ATTR_BIT(5) | OMCI_ATTR_BIT(6) |
					  OMCI_ATTR_BIT(7) | OMCI_ATTR_BIT(8) |
					  OMCI_ATTR_BIT(9) | OMCI_ATTR_BIT(10) |
					  OMCI_ATTR_BIT(11) | OMCI_ATTR_BIT(12) |
					  OMCI_ATTR_BIT(13) | OMCI_ATTR_BIT(14) |
					  OMCI_ATTR_BIT(15));

	/*
	 * OLT-G is not uploaded: it holds the OLT's own identity, which the OLT
	 * sets, and a row with an empty attribute mask makes some OLTs abandon
	 * the MIB upload.
	 */

	/* ME 263 ANI-G: A = #1..#9 = 11B, B = #10..#16 = 10B. */
	ROW(OMCI_ME_ANI_G, 0x8001, OMCI_ATTR_BIT(1) | OMCI_ATTR_BIT(2) |
				   OMCI_ATTR_BIT(3) | OMCI_ATTR_BIT(4) |
				   OMCI_ATTR_BIT(5) | OMCI_ATTR_BIT(6) |
				   OMCI_ATTR_BIT(7) | OMCI_ATTR_BIT(8) |
				   OMCI_ATTR_BIT(9));
	ROW(OMCI_ME_ANI_G, 0x8001, OMCI_ATTR_BIT(10) | OMCI_ATTR_BIT(11) |
				   OMCI_ATTR_BIT(12) | OMCI_ATTR_BIT(13) |
				   OMCI_ATTR_BIT(14) | OMCI_ATTR_BIT(15) |
				   OMCI_ATTR_BIT(16));

	/* ME 262 T-CONT (inst 0x8000..0x800b): 4B each. */
	for (i = 0; i < 12; i++)
		ROW(OMCI_ME_TCONT, 0x8000 + i, OMCI_ATTR_BIT(1) |
					       OMCI_ATTR_BIT(2) |
					       OMCI_ATTR_BIT(3));

	ROW(OMCI_ME_UNI_G, 0x0101, OMCI_ATTR_BIT(1) | OMCI_ATTR_BIT(2) |
				   OMCI_ATTR_BIT(3) | OMCI_ATTR_BIT(4) |
				   OMCI_ATTR_BIT(5));

	/*
	 * ME 277 Priority-Queue: only the single UNI's 8 queues, to keep the
	 * upload short enough for the OLT's authentication timer.
	 */
	for (i = 0; i < 8; i++)
		ROW(OMCI_ME_PRIORITY_QUEUE, i, OMCI_ATTR_BIT(1) |
					       OMCI_ATTR_BIT(2) |
					       OMCI_ATTR_BIT(3) |
					       OMCI_ATTR_BIT(4) |
					       OMCI_ATTR_BIT(5) |
					       OMCI_ATTR_BIT(6) |
					       OMCI_ATTR_BIT(7) |
					       OMCI_ATTR_BIT(8));

	/* ME 278 Traffic-Scheduler (inst 0x8000..0x800b): 6B each. */
	for (i = 0; i < 12; i++)
		ROW(OMCI_ME_TRAFFIC_SCHED, 0x8000 + i, OMCI_ATTR_BIT(1) |
						       OMCI_ATTR_BIT(2) |
						       OMCI_ATTR_BIT(3) |
						       OMCI_ATTR_BIT(4));

	ROW(OMCI_ME_VEIP, 0x0601, OMCI_ATTR_BIT(1) | OMCI_ATTR_BIT(2) |
				  OMCI_ATTR_BIT(3));

	/*
	 * ME 65530 (CTC LoID authentication) is answered on Get but, as on the
	 * vendor firmware, kept out of the MIB upload as a whole class.
	 * omci_inst_exists() accepts it as a vendor class.
	 */

#undef ROW
	o->nrows = n;
}

void omci_onu_init(struct omci_onu *o, const u8 sn[8], u8 mds_seed)
{
	memset(o, 0, sizeof(*o));
	memcpy(o->sn, sn, 8);
	o->mds = mds_seed;
	/*
	 * Serve the fallback optical levels until the first DDM sample is
	 * published; anig_live stays false until then.
	 */
	o->anig_rx_level = OMCI_ANIG_RX_FALLBACK;
	o->anig_tx_level = OMCI_ANIG_TX_FALLBACK;
	omci_build_mib(o);
}

/*
 * Is (class, inst) an instance this ONU holds? True for vendor-reserved
 * classes, OLT-created instances and the static MIB-Upload rows.
 */
bool omci_inst_exists(struct omci_onu *o, u16 class_id, u16 inst)
{
	u16 i;

	if (omci_vendor_class(class_id))
		return true;
	if (omci_store_find(o, class_id, inst))
		return true;
	for (i = 0; i < o->nrows; i++)
		if (o->rows[i].class_id == class_id && o->rows[i].inst == inst)
			return true;
	return false;
}
