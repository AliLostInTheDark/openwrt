// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2015 Cortina-Access Limited
 * Copyright (C) BRULE Herman <alpha_one_x86@first-world.info>
 *
 * nf_flow_table hardware offload for the L3FE main-hash flow engine of the
 * RTL9607DQ network engine. The flow_block layer follows mtk_ppe_offload.c.
 *
 * Only routed IPv4 TCP/UDP NAT flows between the LAN bridge and the PON WAN
 * (IPoE or PPPoE, optionally 802.1Q tagged) are offloaded. Each direction is
 * one entry: the US (LAN->WAN) leg egresses to the data GEM's T-CONT, the DS
 * (WAN->LAN) leg to the LAN port the client's FDB entry points at.
 *
 * Engine: 64K entries in 8-way buckets at crc16 & ~7; a CRC32 key word and a
 * 32-byte action per entry in DDR; the SWO engine computes the key the same
 * way the lookup does; a 4-bit age per entry makes it live.
 */

#define pr_fmt(fmt) "cortina-ppe: " fmt

#include <linux/kernel.h>
#include <linux/bitfield.h>
#include <linux/bitmap.h>
#include <linux/bitops.h>
#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/if_arp.h>
#include <linux/if_ether.h>
#include <linux/if_vlan.h>
#include <linux/io.h>
#include <linux/ip.h>
#include <linux/jiffies.h>
#include <linux/rhashtable.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>
#include <net/flow_offload.h>
#include <net/pkt_cls.h>

#include "cortina-ni.h"
#include "cortina-ppe.h"
#include "cortina-ppe-tables.h"

#define PPE_POLL_TRIES			1000
#define PPE_SWEEP_MS			5000

/*
 * Age codes: 0 frees a slot, anything else is live. A hardware hit re-arms
 * the slot to 6, so a slot above IDLE has seen traffic since the last sweep.
 */
#define PPE_AGE_FREE			0
#define PPE_AGE_IDLE			1
#define PPE_AGE_START			2
#define PPE_AGE_STATIC			3

/* engine setup values, as the stock firmware programs them */
#define PPE_HASH_INI			0x0003007Du	/* 64K, 8-way, 32 B, def_reg */
#define PPE_DEFAULT_ACTION		0x0E4D0000u
#define PPE_CACHE_INI			0x00050304u
#define PPE_CACHE_MISC			0xA0000000u
#define PPE_CHK_FAIL_CTRL		0x0000D0D0u
#define PPE_AXIM2_CONFIG		0x000002FFu
#define PPE_AQM_TIMER			0x5000A2D0u
#define PPE_RSV0_PATCH			(BIT(31) | BIT(0))
#define PPE_RSV1_PATCH			BIT(0)
#define PPE_PPPOE_CFG			0x00000011u
#define PPE_PPPOE_PROT_CFG		0x00570021u
/*
 * With granularity 0 the ager never runs and a hit is not visible as a
 * re-arm; this slow cadence keeps idle entries far longer than any
 * flowtable timeout.
 */
#define PPE_AGING_GRAN_SLOW		0x08000000u

/* hash profiles: routed frames are stamped with profile 3 */
#define PPE_PROFILE_WAN			0
#define PPE_PROFILE_LAN			1
#define PPE_PROFILE_ROUTED		3
#define PPE_PROFILE_MAX			6

/* logical ports */
#define PPE_LDPID_L3_WAN		0x18
#define PPE_LDPID_L3_LAN		0x19
#define PPE_PDPID_L3_WAN		0x0a
#define PPE_LDPID_PON_US_0		0x20

/* egress encapsulation */
#define PPE_PON_DEEPQ_TCONT_MAX		7
#define PPE_WAN_EGR_MCGID(tcont)	(((PPE_LDPID_PON_US_0 + (tcont)) & 0x3f) << 3)
#define PPE_LAN_EGR_MCGID(ldpid)	((ldpid) & 0xff)
#define PPE_LAN_PORT_LDPID_MAX		6u
/* egress L3-IF entries: SMAC substitution (+ PPPoE header for idx 1) */
#define PPE_PPPOE_L3IF_IDX		1
#define PPE_IPOE_L3IF_IDX		2
#define PPE_LAN_L3IF_IDX		3
/* MAC-DA CAM entries and the an_sel that selects them */
#define PPE_AN_IDX_LAN			0
#define PPE_AN_IDX_WAN			1
#define PPE_LAN_AN_SEL			(PPE_AN_IDX_LAN + 1)
#define PPE_WAN_AN_SEL			(PPE_AN_IDX_WAN + 1)
#define PPE_TPID_SLOTS			4

struct cortina_ppe_act {
	DECLARE_BITMAP(bits, PPE_ACT_BITS);
};

#define ppe_act_set(act, field, val)					\
	bitmap_write((act)->bits, (val), PPE_ACT_##field##_SHIFT,		\
		     PPE_ACT_##field##_BITS)
#define ppe_hdri_set(hdri, field, val)					\
	bitmap_write((hdri), (val), HDRI_##field##_SHIFT, HDRI_##field##_BITS)

/* one device's binding to a flowtable block (the block callback's priv) */
struct cortina_ppe_bind {
	struct cortina_ppe	*ppe;
	struct net_device	*dev;
	struct flow_block	*block;
};

/* WAN encapsulation found by walking the forward path of a WAN device */
struct cortina_ppe_wan_encap {
	int	vid;
	__be16	vproto;
	int	sid;
	u8	ac_mac[ETH_ALEN];
	bool	ac_mac_vld;
	bool	walk_ok;
};

/*
 * The GPON driver is a separate module without a handle to the NI: its
 * calls, its netdev's setup_tc and the per-frame WAN check in the RX path
 * find the PPE here.
 */
static struct cortina_ppe *cortina_ppe_inst;

static const struct rhashtable_params cortina_ppe_ht_params = {
	.head_offset		= offsetof(struct cortina_ppe_flow, node),
	.key_offset		= offsetof(struct cortina_ppe_flow, cookie),
	.key_len		= sizeof(unsigned long),
	.automatic_shrinking	= true,
};

static int ppe_poll(void __iomem *reg, u32 busy)
{
	int i;

	for (i = 0; i < PPE_POLL_TRIES; i++) {
		if (!(readl(reg) & busy))
			return 0;
		cpu_relax();
	}
	return -ETIMEDOUT;
}

static int cortina_ppe_poll(struct cortina_ppe *ppe, u32 reg, u32 busy)
{
	return ppe_poll(ppe->base + reg, busy);
}

static int cortina_ppe_go(struct cortina_ppe *ppe, u32 reg, u32 val, u32 busy)
{
	writel(val, ppe->base + reg);
	return cortina_ppe_poll(ppe, reg, busy);
}

/* ---- engine setup ---- */

static int cortina_ppe_engine_init(struct cortina_ppe *ppe)
{
	dma_addr_t key_pa = ppe->carve_pa;
	dma_addr_t act_pa = ppe->carve_pa + PPE_KEY_TBL_BYTES;
	void __iomem *base = ppe->base;
	int ret, i;

	writel(HS_MEM_INI_REQ, base + REG_HS_MEM_INI);
	ret = cortina_ppe_poll(ppe, REG_HS_MEM_INI, HS_MEM_INI_REQ);
	if (ret)
		return ret;

	memset(ppe->carve, 0, PPE_KEY_TBL_BYTES + PPE_ACT_TBL_BYTES);
	/* the zeroed tables must reach DDR before the engine gets their bases */
	wmb();

	writel(lower_32_bits(key_pa), base + REG_HS_BA_MH0);
	writel(upper_32_bits(key_pa) & 0xff, base + REG_HS_BA_MH1);
	writel(lower_32_bits(act_pa), base + REG_HS_BA_MA0);
	writel(upper_32_bits(act_pa) & 0xff, base + REG_HS_BA_MA1);

	writel(PPE_HASH_INI, base + REG_HS_HASH_INI);
	writel(PPE_CACHE_INI, base + REG_HS_CACHE_INI);
	writel(PPE_CACHE_MISC, base + REG_HS_CACHE_MISC);

	/* vendor patch bits: the engine can stall under DDR load without them */
	writel(readl(base + REG_HS_RSV0) | PPE_RSV0_PATCH, base + REG_HS_RSV0);
	writel(readl(base + REG_HS_RSV1) | PPE_RSV1_PATCH, base + REG_HS_RSV1);
	writel(PPE_AXIM2_CONFIG, base + REG_AXIM2_CONFIG);

	/* a recheck failure or miss punts, never drops */
	writel(PPE_CHK_FAIL_CTRL, base + REG_HS_CHK_FAIL_CTRL);
	for (i = 0; i < 4; i++)
		writel(PPE_DEFAULT_ACTION, base + REG_HS_DEFAULT_ACTION(i));

	writel(0, base + REG_HS_AGING_GRANULARITY);
	writel(PPE_AQM_TIMER, base + REG_AQM_TIMER);
	return 0;
}

static int cortina_ppe_mask_write(struct cortina_ppe *ppe, u32 idx,
				  const u32 lo[4], const u32 hi[4])
{
	u32 acc = HS_ACCESS_GO | HS_ACCESS_WRITE |
		  FIELD_PREP(HS_MASK_ACCESS_IDX, idx);
	int i, ret;

	for (i = 0; i < 4; i++)
		writel(lo[i], ppe->base + REG_HS_MASK_DATA(i));
	ret = cortina_ppe_go(ppe, REG_HS_MASK_ACCESS, acc, HS_ACCESS_GO);
	if (ret)
		return ret;

	for (i = 0; i < 4; i++)
		writel(hi[i], ppe->base + REG_HS_MASK_DATA(i));
	return cortina_ppe_go(ppe, REG_HS_MASK_ACCESS,
			      acc | HS_MASK_ACCESS_UPPER, HS_ACCESS_GO);
}

static int cortina_ppe_classify_setup(struct cortina_ppe *ppe)
{
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(ppe_mask_lo); i++) {
		ret = cortina_ppe_mask_write(ppe, i, ppe_mask_lo[i],
					     ppe_mask_hi[i]);
		if (ret)
			return ret;
	}
	for (i = 0; i < ARRAY_SIZE(ppe_profile_regs); i++)
		writel(ppe_profile_regs[i][1], ppe->base + ppe_profile_regs[i][0]);
	return 0;
}

static int cls_fib_write(void __iomem *base, u16 idx,
			 const u32 w[CLS_FIB_WORDS])
{
	int i;

	for (i = 0; i < CLS_FIB_WORDS; i++)
		writel(w[i], base + REG_CLS_FIB_DATA0 - i * 4);
	writel(HS_ACCESS_GO | HS_ACCESS_WRITE | idx, base + REG_CLS_FIB_ACCESS);
	return ppe_poll(base + REG_CLS_FIB_ACCESS, HS_ACCESS_GO);
}

static int cls_key_write(void __iomem *base, u16 idx,
			 const u32 w[CLS_KEY_WORDS])
{
	int i;

	for (i = 0; i < CLS_KEY_WORDS; i++)
		writel(w[i], base + REG_CLS_KEY_ACCESS + (CLS_KEY_WORDS - i) * 4);
	writel(HS_ACCESS_GO | HS_ACCESS_WRITE |
	       FIELD_PREP(CLS_KEY_ACCESS_IDX, idx), base + REG_CLS_KEY_ACCESS);
	return ppe_poll(base + REG_CLS_KEY_ACCESS, HS_ACCESS_GO);
}

static int pp_cam_write(void __iomem *base, u8 sel, u8 idx,
			const u32 w[PP_CAM_WORDS])
{
	int i;

	for (i = 0; i < PP_CAM_WORDS; i++)
		writel(w[i], base + REG_PP_FIELD_CAM_DATA(i));
	writel(HS_ACCESS_GO | HS_ACCESS_WRITE |
	       FIELD_PREP(PP_FIELD_CAM_SEL, sel) | idx,
	       base + REG_PP_FIELD_CAM_ACCESS);
	return ppe_poll(base + REG_PP_FIELD_CAM_ACCESS, HS_ACCESS_GO);
}

/*
 * Classifier rows and field CAMs of the stock firmware. Runs from the RX
 * classifier init, before the PPE probe, so it takes the register window.
 */
int cortina_ppe_cls_stock_load(void __iomem *base)
{
	static const u32 zero[PP_CAM_WORDS];
	int sel, idx, i, ret;

	/* the MAC-DA CAM holds the router MACs and is left alone */
	for (sel = 0; sel <= PP_CAM_SEL_LAST; sel++) {
		if (sel == PP_CAM_SEL_MAC_DA)
			continue;
		for (idx = 0; idx < PP_CAM_ENTRIES; idx++) {
			ret = pp_cam_write(base, sel, idx, zero);
			if (ret)
				return ret;
		}
	}
	for (i = 0; i < ARRAY_SIZE(cam_stock); i++) {
		ret = pp_cam_write(base, cam_stock[i].sel, cam_stock[i].idx,
				   cam_stock[i].w);
		if (ret)
			return ret;
	}
	for (i = 0; i < ARRAY_SIZE(cls_fib_stock); i++) {
		ret = cls_fib_write(base, cls_fib_stock[i].idx,
				    cls_fib_stock[i].w);
		if (ret)
			return ret;
	}
	for (i = 0; i < ARRAY_SIZE(cls_key_stock); i++) {
		ret = cls_key_write(base, cls_key_stock[i].idx,
				    cls_key_stock[i].w);
		if (ret)
			return ret;
	}
	return 0;
}

static int cortina_ppe_mac_da_cam_set(struct cortina_ppe *ppe, u32 idx,
				      const u8 *mac)
{
	u32 w[PP_CAM_WORDS] = {
		get_unaligned_be32(mac + 2),
		PP_CAM_MAC_DA_VLD | get_unaligned_be16(mac),
	};

	if (idx >= PP_CAM_MAC_DA_ENTRIES)
		return -EINVAL;
	return pp_cam_write(ppe->base, PP_CAM_SEL_MAC_DA, idx, w);
}

static int cortina_ppe_fdb_static_add(struct cortina_ppe *ppe, const u8 *mac,
				      u32 ldpid)
{
	void __iomem *base = ppe->base;
	u32 d3 = (mac[0] >> 5) & 0x7;
	u32 d2 = ((u32)(mac[0] & 0x1f) << 27) | ((u32)mac[1] << 19) |
		 ((u32)mac[2] << 11) | ((u32)mac[3] << 3) | ((mac[4] >> 5) & 0x7);
	u32 d1 = (u32)(((mac[4] & 0x1f) << 8) | mac[5]) << 19;
	u32 d0 = FIELD_PREP(FDB_LPID, ldpid) | FDB_VALID | FDB_STATIC |
		 FDB_DA_PERMIT | FDB_SA_PERMIT;

	writel(0, base + REG_FDB_CMD_RETURN);
	writel(d3, base + REG_FDB_DATA3);
	writel(d2, base + REG_FDB_DATA2);
	writel(d1, base + REG_FDB_DATA1);
	writel(d0, base + REG_FDB_DATA0);
	return cortina_ppe_go(ppe, REG_FDB_ACCESS,
			      HS_ACCESS_GO | FDB_OP_REPLACE, HS_ACCESS_GO);
}

/* the WAN MAC from the last upstream flow, LAN MAC + 1 until then */
static void cortina_ppe_wan_mac(struct cortina_ppe *ppe, const u8 *lan_mac,
				u8 *wan_mac)
{
	if (is_valid_ether_addr(ppe->wan_mac))
		ether_addr_copy(wan_mac, ppe->wan_mac);
	else
		u64_to_ether_addr(ether_addr_to_u64(lan_mac) + 1, wan_mac);
}

/*
 * The upstream egress SMAC is the WAN entry of the MAC-DA CAM. A PPPoE BRAS
 * only accepts the session's own MAC, so use the WAN device's MAC.
 */
static int cortina_ppe_wan_mac_set(struct cortina_ppe *ppe, const u8 *wan_mac)
{
	int ret;

	if (ether_addr_equal(wan_mac, ppe->wan_mac))
		return 0;
	ret = cortina_ppe_mac_da_cam_set(ppe, PPE_AN_IDX_WAN, wan_mac);
	if (!ret)
		ret = cortina_ppe_fdb_static_add(ppe, wan_mac, PPE_LDPID_L3_WAN);
	if (!ret)
		ether_addr_copy(ppe->wan_mac, wan_mac);
	return ret;
}

/* router MACs into the PP MAC-DA CAM; re-applied on link up */
int cortina_ppe_intf_add(struct cortina_ppe *ppe, const u8 *lan_mac)
{
	u8 wan_mac[ETH_ALEN];
	int ret;

	if (!ppe || !lan_mac)
		return -EINVAL;
	cortina_ppe_wan_mac(ppe, lan_mac, wan_mac);
	ret = cortina_ppe_mac_da_cam_set(ppe, PPE_AN_IDX_LAN, lan_mac);
	if (!ret)
		ret = cortina_ppe_mac_da_cam_set(ppe, PPE_AN_IDX_WAN, wan_mac);
	return ret;
}

/*
 * Route LAN and WAN frames through the main hash with a miss punted to the
 * CPU: miss actions, CLS default rows, PE config, WAN ingress admission and
 * the router MACs in the MAC-DA CAM and the FDB.
 */
static int cortina_ppe_l3_forward_enable(struct cortina_ppe *ppe)
{
	void __iomem *base = ppe->base;
	u8 wan_mac[ETH_ALEN];
	int i, ret;
	u32 v;

	for (i = 0; i < ARRAY_SIZE(ppe_def_reg); i++)
		writel(ppe_def_reg[i], base + REG_HS_DEF_REG(i));

	for (i = 0; i < ARRAY_SIZE(ppe_cls_default); i++) {
		ret = cls_fib_write(base, ppe_cls_default[i].idx,
				    ppe_cls_default[i].w);
		if (ret)
			return ret;
	}

	writel(PPE_MASK_5TUPLE, base + REG_HS_PROFILE_TUPLE0(PPE_PROFILE_WAN));
	writel(PPE_MASK_5TUPLE, base + REG_HS_PROFILE_TUPLE0(PPE_PROFILE_LAN));
	writel(PPE_MASK_5TUPLE, base + REG_HS_PROFILE_TUPLE0(PPE_PROFILE_ROUTED));

	/* PON egress LDPIDs start at 0x20 + T-CONT, GEM id from the action */
	v = readl(base + REG_PE_CFG);
	v &= ~(PE_CFG_LDPID_BASE | PE_CFG_MTU_CHK_EN);
	v |= FIELD_PREP(PE_CFG_LDPID_BASE, PPE_LDPID_PON_US_0) |
	     PE_CFG_GEMID_MAP | PE_CFG_PAD_CTRL | PE_CFG_RSVD14;
	writel(v, base + REG_PE_CFG);

	writel(PPE_PPPOE_CFG, base + REG_PE_PPPOE_CFG);
	writel(PPE_PPPOE_PROT_CFG, base + REG_PE_PPPOE_PROT_CFG);
	writel(PPE_AGING_GRAN_SLOW, base + REG_HS_AGING_GRANULARITY);

	/* frames stamped L3_WAN by the PON PDC enter the L3FE WAN port */
	for (i = 0; i < 4; i++) {
		u32 idx = PPE_LDPID_L3_WAN |
			  ((i & 1) ? PDPID_IDX_DBUF : 0) |
			  ((i & 2) ? PDPID_IDX_MYMAC : 0);

		writel(PPE_PDPID_L3_WAN & 0xf, base + REG_PDPID_MAP_DATA);
		ret = cortina_ppe_go(ppe, REG_PDPID_MAP_ACCESS,
				     HS_ACCESS_GO | HS_ACCESS_WRITE | idx,
				     HS_ACCESS_GO);
		if (ret)
			return ret;
	}

	if (!ppe->router_mac_valid)
		return 0;

	/* router MACs: PP MAC-DA CAM, and FDB entries to the L3FE ports */
	ret = cortina_ppe_intf_add(ppe, ppe->router_mac);
	if (ret)
		return ret;
	cortina_ppe_wan_mac(ppe, ppe->router_mac, wan_mac);
	ret = cortina_ppe_fdb_static_add(ppe, wan_mac, PPE_LDPID_L3_WAN);
	if (ret)
		return ret;
	return cortina_ppe_fdb_static_add(ppe, ppe->router_mac,
					  PPE_LDPID_L3_LAN);
}

/*
 * Egress L3-IF entry @idx: substitute the SMAC selected by @an_sel and, for a
 * non-zero @session, add the PPPoE header. Caller holds reg_lock.
 */
static int cortina_ppe_l3if_set(struct cortina_ppe *ppe, u32 idx, u8 an_sel,
				u16 session)
{
	u32 entry = L3IF_MAC_SA_VLD | L3IF_PAD_CTRL | L3IF_PPPOE_SET |
		    FIELD_PREP(L3IF_MAC_SA_AN_SEL, an_sel);

	if (idx >= L3IF_ENTRIES)
		return -EINVAL;
	if (session)
		entry |= L3IF_PPPOE_VLD |
			 FIELD_PREP(L3IF_PPPOE_SESSION, session);

	writel(entry, ppe->base + REG_L3IF_DATA);
	return cortina_ppe_go(ppe, REG_L3IF_ACCESS,
			      HS_ACCESS_GO | HS_ACCESS_WRITE | idx,
			      HS_ACCESS_GO);
}

/* ---- hash entries ---- */

/* hash a tuple on the SWO engine, the same CRC the lookup computes */
static int cortina_ppe_key_hash(struct cortina_ppe *ppe,
				const struct cortina_ppe_tuple *t,
				u32 *crc32, u16 *crc16)
{
	DECLARE_BITMAP(hdri, HDRI_BITS) = {};
	u32 words[HDRI_BITS / 32];
	unsigned long flags;
	int i, ret;

	ppe_hdri_set(hdri, L4_DP, t->dport);
	ppe_hdri_set(hdri, L4_SP, t->sport);
	if (t->ipv6) {
		for (i = 0; i < 4; i++) {
			bitmap_write(hdri, be32_to_cpu(t->dip6.s6_addr32[3 - i]),
				     HDRI_IP_DA_SHIFT + i * 32, 32);
			bitmap_write(hdri, be32_to_cpu(t->sip6.s6_addr32[3 - i]),
				     HDRI_IP_SA_SHIFT + i * 32, 32);
		}
		ppe_hdri_set(hdri, IP_VER, 1);
	} else {
		ppe_hdri_set(hdri, IP_DA, t->dip);
		ppe_hdri_set(hdri, IP_SA, t->sip);
	}
	ppe_hdri_set(hdri, IP_PROTO, t->proto);
	ppe_hdri_set(hdri, IP_VLD, 1);
	ppe_hdri_set(hdri, T2_CTRL, PPE_PROFILE_ROUTED);
	bitmap_to_arr32(words, hdri, HDRI_BITS);

	spin_lock_irqsave(&ppe->reg_lock, flags);
	writel(0, ppe->base + REG_HS_SWO_IDX);
	for (i = 0; i < ARRAY_SIZE(words); i++)
		writel(words[i], ppe->base + REG_HS_SWO_DAT);
	writel(HS_SWO_IDX_MASK_ID, ppe->base + REG_HS_SWO_IDX);
	writel(PPE_MASK_5TUPLE, ppe->base + REG_HS_SWO_DAT);

	ret = cortina_ppe_poll(ppe, REG_HS_SWO_CTRL, HS_SWO_CTRL_GO);
	if (!ret)
		ret = cortina_ppe_go(ppe, REG_HS_SWO_CTRL, HS_SWO_CTRL_GO,
				     HS_SWO_CTRL_GO);
	if (!ret) {
		writel(HS_SWO_IDX_CRC32, ppe->base + REG_HS_SWO_IDX);
		*crc32 = readl(ppe->base + REG_HS_SWO_DAT);
		writel(HS_SWO_IDX_CRC16, ppe->base + REG_HS_SWO_IDX);
		*crc16 = readl(ppe->base + REG_HS_SWO_DAT) & 0xffff;
	}
	spin_unlock_irqrestore(&ppe->reg_lock, flags);
	return ret;
}

/* read the row, change the slot's nibble, write the row back */
static int cortina_ppe_age_set(struct cortina_ppe *ppe, u32 idx, u32 age)
{
	u32 row = FIELD_PREP(HS_AGE_ACCESS_ROW, idx >> 5);
	u32 reg = REG_HS_AGE_DATA(idx);
	u32 shift = HS_AGE_SHIFT(idx);
	unsigned long flags;
	u32 w;
	int ret;

	spin_lock_irqsave(&ppe->reg_lock, flags);
	ret = cortina_ppe_go(ppe, REG_HS_AGE_ACCESS, row | HS_ACCESS_GO,
			     HS_ACCESS_GO);
	if (!ret) {
		w = readl(ppe->base + reg);
		w &= ~(HS_AGE_MASK << shift);
		w |= (age & HS_AGE_MASK) << shift;
		writel(w, ppe->base + reg);
		ret = cortina_ppe_go(ppe, REG_HS_AGE_ACCESS,
				     row | HS_ACCESS_WRITE | HS_ACCESS_GO,
				     HS_ACCESS_GO);
	}
	spin_unlock_irqrestore(&ppe->reg_lock, flags);
	if (ret)
		dev_err(ppe->dev, "age write idx %u failed (%d)\n", idx, ret);
	return ret;
}

/*
 * Read and clear one age row: returns a bitmap of the slots the hardware
 * re-armed since the last sweep and puts them back to IDLE.
 */
static int cortina_ppe_row_sweep(struct cortina_ppe *ppe, u32 row,
				 u32 *traffic)
{
	unsigned long flags;
	u32 trf = 0;
	int r, i, ret;

	spin_lock_irqsave(&ppe->reg_lock, flags);
	ret = cortina_ppe_go(ppe, REG_HS_AGE_ACCESS, row | HS_ACCESS_GO,
			     HS_ACCESS_GO);
	if (ret)
		goto out;

	for (r = 0; r < HS_AGE_WORDS; r++) {
		u32 slot0 = r * HS_AGE_PER_WORD;
		u32 w = readl(ppe->base + REG_HS_AGE_DATA(slot0));
		u32 n = 0;

		for (i = 0; i < HS_AGE_PER_WORD; i++) {
			u32 shift = HS_AGE_SHIFT(i);
			u32 age = (w >> shift) & HS_AGE_MASK;

			if (age > PPE_AGE_IDLE && age != PPE_AGE_STATIC) {
				trf |= BIT(slot0 + i);
				age = PPE_AGE_IDLE;
			}
			n |= age << shift;
		}
		writel(n, ppe->base + REG_HS_AGE_DATA(slot0));
	}

	ret = cortina_ppe_go(ppe, REG_HS_AGE_ACCESS,
			     row | HS_ACCESS_WRITE | HS_ACCESS_GO, HS_ACCESS_GO);
out:
	spin_unlock_irqrestore(&ppe->reg_lock, flags);
	*traffic = trf;
	return ret;
}

/* drop a deleted entry's action from the on-chip cache */
static int cortina_ppe_cache_invalidate(struct cortina_ppe *ppe, u32 idx,
					u16 crc16)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&ppe->reg_lock, flags);
	writel(FIELD_PREP(HS_CACHE_CTRL_SLOT, idx & (PPE_HASH_WAYS - 1)) |
	       FIELD_PREP(HS_CACHE_CTRL_CRC16, crc16) | HS_CACHE_CTRL_INVALIDATE,
	       ppe->base + REG_HS_CACHE_CTRL);
	ret = cortina_ppe_poll(ppe, REG_HS_CACHE_CTRL_REQ, HS_CACHE_CTRL_REQ_GO);
	if (!ret)
		ret = cortina_ppe_go(ppe, REG_HS_CACHE_CTRL_REQ,
				     readl(ppe->base + REG_HS_CACHE_CTRL_REQ) |
				     HS_CACHE_CTRL_REQ_GO, HS_CACHE_CTRL_REQ_GO);
	spin_unlock_irqrestore(&ppe->reg_lock, flags);
	if (ret)
		dev_err(ppe->dev, "cache invalidate idx %u failed (%d)\n",
			idx, ret);
	return ret;
}

/* the engine reads the action as little-endian 32-bit words */
static void cortina_ppe_act_store(struct cortina_ppe *ppe, u32 idx,
				  const struct cortina_ppe_act *act)
{
	__le32 *dst = ppe->act_tbl + (size_t)idx * PPE_ACT_BYTES;
	u32 w[PPE_ACT_BITS / 32];
	int i;

	bitmap_to_arr32(w, act->bits, PPE_ACT_BITS);
	for (i = 0; i < ARRAY_SIZE(w); i++)
		dst[i] = cpu_to_le32(w[i]);
}

static void cortina_ppe_entry_clear(struct cortina_ppe *ppe, u32 idx)
{
	ppe->key_tbl[idx] = 0;
	memset(ppe->act_tbl + (size_t)idx * PPE_ACT_BYTES, 0, PPE_ACT_BYTES);
	/* the engine must see the cleared entry before the slot is reused */
	wmb();
}

static int cortina_ppe_entry_add(struct cortina_ppe *ppe,
				 const struct cortina_ppe_tuple *t,
				 const struct cortina_ppe_act *act,
				 u32 *idx_out, u16 *crc16_out)
{
	u32 crc32, base, idx;
	u16 crc16;
	int way, ret;

	ret = cortina_ppe_key_hash(ppe, t, &crc32, &crc16);
	if (ret)
		return ret;

	base = crc16 & ~(u32)(PPE_HASH_WAYS - 1);
	for (way = 0; way < PPE_HASH_WAYS; way++)
		if (ppe->shadow_crc32[base + way] == crc32)
			return -EEXIST;
	/* entry 0 stays free: its all-zero cache tag aliases an empty way */
	for (way = base ? 0 : 1; way < PPE_HASH_WAYS; way++)
		if (!ppe->shadow_crc32[base + way])
			break;
	if (way == PPE_HASH_WAYS)
		return -ENOSPC;
	idx = base + way;

	/* action, then key, then the age that makes the entry live */
	cortina_ppe_act_store(ppe, idx, act);
	ppe->key_tbl[idx] = crc32;
	/* both must be in DDR before the age write lets the engine match */
	wmb();

	ret = cortina_ppe_age_set(ppe, idx, PPE_AGE_START);
	if (ret) {
		cortina_ppe_entry_clear(ppe, idx);
		cortina_ppe_age_set(ppe, idx, PPE_AGE_FREE);
		cortina_ppe_cache_invalidate(ppe, idx, crc16);
		return ret;
	}
	/* step down to IDLE so the first sweep only counts real hits */
	cortina_ppe_age_set(ppe, idx, PPE_AGE_IDLE);

	ppe->shadow_crc32[idx] = crc32;
	ppe->shadow_crc16[idx] = crc16;
	*idx_out = idx;
	*crc16_out = crc16;
	return 0;
}

static int cortina_ppe_entry_del(struct cortina_ppe *ppe, u32 idx, u16 crc16)
{
	int age_ret, inv_ret;

	/* key first, so nothing new matches; run every step even on error */
	cortina_ppe_entry_clear(ppe, idx);
	ppe->shadow_crc32[idx] = 0;
	ppe->shadow_crc16[idx] = 0;

	age_ret = cortina_ppe_age_set(ppe, idx, PPE_AGE_FREE);
	inv_ret = cortina_ppe_cache_invalidate(ppe, idx, crc16);
	return age_ret ? age_ret : inv_ret;
}

/* ---- flows ---- */

static void cortina_ppe_flow_free(struct cortina_ppe *ppe,
				  struct cortina_ppe_flow *e)
{
	ppe->flow_by_idx[e->hash_idx] = NULL;
	ppe->row_occ[e->hash_idx / PPE_AGE_SLOTS]--;
	cortina_ppe_entry_del(ppe, e->hash_idx, e->crc16);
	rhashtable_remove_fast(&ppe->flow_table, &e->node,
			       cortina_ppe_ht_params);
	if (e->ds)
		atomic_dec(&ppe->stats.ds_installed);
	atomic_dec(&ppe->stats.installed);
	kfree(e);
}

/* remove the entries of @block, or all of them when @block is NULL */
static void cortina_ppe_flush(struct cortina_ppe *ppe, struct flow_block *block)
{
	u32 idx;

	for (idx = 0; idx < PPE_ENTRIES; idx++) {
		struct cortina_ppe_flow *e = ppe->flow_by_idx[idx];

		if (e && (!block || e->block == block))
			cortina_ppe_flow_free(ppe, e);
	}
}

/*
 * Program the PPPoE egress L3-IF entry with @session (0 clears it). Flows
 * built for another session are flushed. Caller holds flow_lock.
 */
static int cortina_ppe_set_session(struct cortina_ppe *ppe, u16 session)
{
	unsigned long flags;
	int ret;

	if (session != READ_ONCE(ppe->pppoe_session) &&
	    atomic_read(&ppe->stats.installed))
		cortina_ppe_flush(ppe, NULL);

	spin_lock_irqsave(&ppe->reg_lock, flags);
	ret = cortina_ppe_l3if_set(ppe, PPE_PPPOE_L3IF_IDX, PPE_WAN_AN_SEL,
				   session);
	spin_unlock_irqrestore(&ppe->reg_lock, flags);
	if (!ret)
		WRITE_ONCE(ppe->pppoe_session, session);
	dev_dbg(ppe->dev, "PPPoE session %#x %s (%d)\n", session,
		session ? "armed" : "cleared", ret);
	return ret;
}

/* US: egress to the data GEM's T-CONT queue, SMAC (+ PPPoE) via the L3-IF */
static int cortina_ppe_set_us_egress(struct cortina_ppe *ppe,
				     struct cortina_ppe_act *act, u16 pppoe)
{
	u16 gem = READ_ONCE(ppe->data_gem);
	u8 tcont = READ_ONCE(ppe->data_tcont);
	int ret;

	if (!gem)
		return -ENODEV;

	ppe_act_set(act, MRR_VLD, 1);
	ppe_act_set(act, MCGID, PPE_WAN_EGR_MCGID(tcont) & 0xff);
	ppe_act_set(act, MC, 1);
	ppe_act_set(act, DPID_VLD, 1);
	ppe_act_set(act, PERMIT, 1);
	ppe_act_set(act, DPID_PRI, 1);
	ppe_act_set(act, DEEPQ, tcont <= PPE_PON_DEEPQ_TCONT_MAX);
	ppe_act_set(act, T2_CTRL, tcont & 0xf);
	ppe_act_set(act, POP_L3_VLD, 1);
	ppe_act_set(act, CHK_MSK_PTR, PPE_MASK_5TUPLE);
	ppe_act_set(act, CACHE_CTRL, 1);
	ppe_act_set(act, IGR_L3_IF, PPE_LAN_L3IF_IDX);
	ppe_act_set(act, L3_IF_CNT_EN, 1);
	ppe_act_set(act, L3_IF_VLD, 1);

	if (pppoe) {
		if (pppoe != READ_ONCE(ppe->pppoe_session)) {
			ret = cortina_ppe_set_session(ppe, pppoe);
			if (ret)
				return ret;
		}
		ppe_act_set(act, PPPOE_SET, 1);
		ppe_act_set(act, PPPOE_VLD, 1);
		ppe_act_set(act, EGR_L3_IF, PPE_PPPOE_L3IF_IDX);
	} else {
		ppe_act_set(act, EGR_L3_IF, PPE_IPOE_L3IF_IDX);
	}
	return 0;
}

/*
 * The action's TPID encoding indexes the L3FE PP TPID slots (1-based).
 * Return the slot holding @tpid, enabling or registering it if needed.
 */
static int cortina_ppe_tpid_ensure(struct cortina_ppe *ppe, u16 tpid)
{
	void __iomem *base = ppe->base;
	u32 w[2], ctrl, mask;
	int i, free_slot = -1;

	w[0] = readl(base + CA_NI_L3FE_PP_TPID01);
	w[1] = readl(base + CA_NI_L3FE_PP_TPID23);
	ctrl = readl(base + CA_NI_L3FE_PP_TPID_CTRL);
	mask = FIELD_GET(CA_NI_L3FE_PP_TPID_TOP_MASK, ctrl);

	for (i = 0; i < PPE_TPID_SLOTS; i++) {
		u16 slot = (i & 1) ? (w[i >> 1] >> 16) : (w[i >> 1] & 0xffff);
		bool en = mask & BIT(i);

		if (slot == tpid) {
			if (!en) {
				ctrl |= FIELD_PREP(CA_NI_L3FE_PP_TPID_TOP_MASK,
						   BIT(i));
				writel(ctrl, base + CA_NI_L3FE_PP_TPID_CTRL);
			}
			return i;
		}
		if (!en && free_slot < 0)
			free_slot = i;
	}
	if (free_slot < 0)
		return -ENOSPC;

	if (free_slot & 1)
		w[free_slot >> 1] = (w[free_slot >> 1] & 0x0000ffff) |
				    ((u32)tpid << 16);
	else
		w[free_slot >> 1] = (w[free_slot >> 1] & 0xffff0000) | tpid;
	writel(w[free_slot >> 1], base + (free_slot < 2 ?
	       CA_NI_L3FE_PP_TPID01 : CA_NI_L3FE_PP_TPID23));
	ctrl |= FIELD_PREP(CA_NI_L3FE_PP_TPID_TOP_MASK, BIT(free_slot));
	writel(ctrl, base + CA_NI_L3FE_PP_TPID_CTRL);
	return free_slot;
}

static int cortina_ppe_set_us_wan_vlan(struct cortina_ppe *ppe,
				       struct cortina_ppe_act *act, u16 vid)
{
	int slot;

	if (!vid)
		return 0;
	slot = cortina_ppe_tpid_ensure(ppe, ETH_P_8021Q);
	if (slot < 0)
		return slot;
	ppe_act_set(act, VLAN_VLD, 1);
	ppe_act_set(act, VLAN_CNT, 1);
	ppe_act_set(act, TOP_VID, vid);
	ppe_act_set(act, TOP_TPID_ENC, slot + 1);
	return 0;
}

/* DS: LAN-MAC SMAC entry, every hash profile on the 5-tuple mask */
static int cortina_ppe_arm_ds(struct cortina_ppe *ppe)
{
	unsigned long flags;
	int p, ret;

	spin_lock_irqsave(&ppe->reg_lock, flags);
	ret = cortina_ppe_l3if_set(ppe, PPE_LAN_L3IF_IDX, PPE_LAN_AN_SEL, 0);
	if (!ret)
		for (p = 0; p <= PPE_PROFILE_MAX; p++)
			writel(PPE_MASK_5TUPLE,
			       ppe->base + REG_HS_PROFILE_TUPLE0(p));
	spin_unlock_irqrestore(&ppe->reg_lock, flags);
	if (!ret)
		ppe->ds_armed = true;
	return ret;
}

/*
 * DS: egress to the client's LAN port with the LAN router MAC as SMAC; on a
 * tagged WAN the tag is stripped (vlan_vld with zero tags).
 */
static void cortina_ppe_set_ds_egress(struct cortina_ppe_act *act,
				      u32 lan_ldpid, u16 vid)
{
	ppe_act_set(act, MRR_VLD, 1);
	ppe_act_set(act, MCGID, PPE_LAN_EGR_MCGID(lan_ldpid));
	ppe_act_set(act, DPID_VLD, 1);
	ppe_act_set(act, PERMIT, 1);
	ppe_act_set(act, DPID_PRI, 1);
	ppe_act_set(act, DEEPQ, 1);
	ppe_act_set(act, CHK_MSK_PTR, PPE_MASK_5TUPLE);
	ppe_act_set(act, CACHE_CTRL, 1);
	ppe_act_set(act, L3_IF_VLD, 1);
	ppe_act_set(act, EGR_L3_IF, PPE_LAN_L3IF_IDX);
	if (vid)
		ppe_act_set(act, VLAN_VLD, 1);
	/* the ingress L3-IF a DS frame is counted on; PPPoE is stripped */
	ppe_act_set(act, IGR_L3_IF, PPE_IPOE_L3IF_IDX);
	ppe_act_set(act, L3_IF_CNT_EN, 1);
}

/* feed FLOW_CLS_STATS lastused from the age re-arms */
static void cortina_ppe_sweep_work(struct work_struct *work)
{
	struct cortina_ppe *ppe = container_of(to_delayed_work(work),
					       struct cortina_ppe, sweep_work);
	unsigned long traffic;
	u32 row, trf;
	int slot;

	mutex_lock(&ppe->flow_lock);
	for (row = 0; row < PPE_AGE_ROWS; row++) {
		if (!ppe->row_occ[row])
			continue;
		if (cortina_ppe_row_sweep(ppe, row, &trf) || !trf)
			continue;

		traffic = trf;
		for_each_set_bit(slot, &traffic, PPE_AGE_SLOTS) {
			struct cortina_ppe_flow *e =
				ppe->flow_by_idx[row * PPE_AGE_SLOTS + slot];

			if (!e)
				continue;
			e->last_hit = jiffies;
			atomic_inc(e->ds ? &ppe->stats.ds_hits :
				   &ppe->stats.us_hits);
		}
		if (!(row & 0x3f))
			cond_resched();
	}
	mutex_unlock(&ppe->flow_lock);
	schedule_delayed_work(&ppe->sweep_work, msecs_to_jiffies(PPE_SWEEP_MS));
}

static void cortina_ppe_refused_account(struct cortina_ppe *ppe, int err)
{
	if (!err)
		return;
	atomic_inc(&ppe->stats.refused);
	switch (err) {
	case -EOPNOTSUPP:
		atomic_inc(&ppe->stats.refused_unsupp);
		break;
	case -ENOSPC:
		atomic_inc(&ppe->stats.refused_full);
		break;
	case -EEXIST:
		atomic_inc(&ppe->stats.refused_dup);
		break;
	default:
		atomic_inc(&ppe->stats.refused_err);
		break;
	}
}

static void cortina_ppe_wan_encap(struct net_device *dev,
				  struct cortina_ppe_wan_encap *e)
{
	struct net_device_path_ctx ctx = { .dev = dev };
	struct net_device_path_stack stack;
	int i;

	memset(e, 0, sizeof(*e));
	e->vid = -1;
	e->sid = -1;

	rcu_read_lock();
	if (dev_fill_forward_path(&ctx, &stack) >= 0) {
		e->walk_ok = true;
		for (i = 0; i < stack.num_paths; i++) {
			const struct net_device_path *p = &stack.path[i];

			if (p->type == DEV_PATH_VLAN && e->vid < 0) {
				e->vid = p->encap.id;
				e->vproto = p->encap.proto;
			} else if (p->type == DEV_PATH_PPPOE && e->sid < 0) {
				e->sid = p->encap.id;
				ether_addr_copy(e->ac_mac, p->encap.h_dest);
				e->ac_mac_vld = is_valid_ether_addr(p->encap.h_dest);
			}
		}
	}
	rcu_read_unlock();
}

static u16 cortina_ppe_wan_vid(struct net_device *dev)
{
	struct cortina_ppe_wan_encap e;

	if (!dev)
		return 0;
	if (is_vlan_dev(dev))
		return vlan_dev_vlan_id(dev);
	cortina_ppe_wan_encap(dev, &e);
	return e.vid > 0 ? e.vid : 0;
}

/*
 * A tagged WAN is offloadable when the device is the VLAN itself, or a PPPoE
 * device over a single 802.1Q tag whose session and peer MAC are known.
 */
static bool cortina_ppe_wan_vlan_ok(struct net_device *dev, u16 vid,
				    struct cortina_ppe_wan_encap *enc)
{
	if (is_vlan_dev(dev)) {
		memset(enc, 0, sizeof(*enc));
		enc->vid = vid;
		enc->sid = -1;
		return true;
	}
	cortina_ppe_wan_encap(dev, enc);
	return enc->walk_ok && enc->vid == vid &&
	       enc->vproto == htons(ETH_P_8021Q) &&
	       enc->sid >= 0 && enc->sid <= 0xffff && enc->ac_mac_vld;
}

/* what a flow rule asks for, collected before anything is programmed */
struct cortina_ppe_flow_data {
	struct cortina_ppe_tuple	tuple;
	struct net_device		*odev;
	bool				ds;

	bool				nat_addr;
	bool				nat_port;
	u32				nat_ip;
	u16				nat_l4_port;

	u8				dmac[ETH_ALEN];	/* next hop */
	u8				smac[ETH_ALEN];	/* egress device */
	bool				dmac_lo, dmac_hi;
	bool				smac_lo, smac_hi;

	u16				rule_vid;	/* VLAN_PUSH */
	bool				rule_vlan_pop;
	u16				pppoe_sid;	/* PPPOE_PUSH */

	/* WAN encapsulation, from the rule or the forward path */
	struct cortina_ppe_wan_encap	wan;
	u16				wan_vid;
	bool				wan_pppoe;
};

/* match keys: 5-tuple, and the direction from the ingress device */
static int cortina_ppe_flow_parse_match(struct flow_rule *rule,
					struct net_device *dev,
					struct cortina_ppe_flow_data *d)
{
	struct flow_match_control control;
	struct flow_match_basic basic;
	struct flow_match_ports ports;
	bool lan_ingress = false;

	if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CONTROL))
		return -EOPNOTSUPP;
	flow_rule_match_control(rule, &control);
	if (control.key->addr_type != FLOW_DISSECTOR_KEY_IPV4_ADDRS &&
	    control.key->addr_type != FLOW_DISSECTOR_KEY_IPV6_ADDRS)
		return -EOPNOTSUPP;

	if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_BASIC))
		return -EOPNOTSUPP;
	flow_rule_match_basic(rule, &basic);
	if (basic.key->ip_proto != IPPROTO_TCP &&
	    basic.key->ip_proto != IPPROTO_UDP)
		return -EOPNOTSUPP;
	d->tuple.proto = basic.key->ip_proto;

	if (control.key->addr_type == FLOW_DISSECTOR_KEY_IPV6_ADDRS) {
		struct flow_match_ipv6_addrs addrs;

		if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IPV6_ADDRS))
			return -EOPNOTSUPP;
		flow_rule_match_ipv6_addrs(rule, &addrs);
		d->tuple.sip6 = addrs.key->src;
		d->tuple.dip6 = addrs.key->dst;
		d->tuple.ipv6 = true;
	} else if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IPV4_ADDRS)) {
		struct flow_match_ipv4_addrs addrs;

		flow_rule_match_ipv4_addrs(rule, &addrs);
		d->tuple.sip = be32_to_cpu(addrs.key->src);
		d->tuple.dip = be32_to_cpu(addrs.key->dst);
	}

	if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_PORTS))
		return -EOPNOTSUPP;
	flow_rule_match_ports(rule, &ports);
	d->tuple.sport = be16_to_cpu(ports.key->src);
	d->tuple.dport = be16_to_cpu(ports.key->dst);

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_META)) {
		struct net_device *idev;
		struct flow_match_meta meta;
		int ret = 0;

		flow_rule_match_meta(rule, &meta);
		idev = dev_get_by_index(dev_net(dev), meta.key->ingress_ifindex);
		if (idev) {
			lan_ingress = netif_is_any_bridge_port(idev) ||
				      netif_is_bridge_master(idev);
			if (lan_ingress && d->tuple.ipv6 && idev->ieee80211_ptr)
				ret = -EOPNOTSUPP;
			/* a downstream flow arriving on a tagged WAN */
			if (!ret && !lan_ingress) {
				d->wan_vid = cortina_ppe_wan_vid(idev);
				if (d->wan_vid &&
				    !cortina_ppe_wan_vlan_ok(idev, d->wan_vid,
							     &d->wan))
					ret = -EOPNOTSUPP;
			}
			dev_put(idev);
		}
		if (ret)
			return ret;
	}
	d->ds = !lan_ingress;
	return 0;
}

static int cortina_ppe_flow_parse_mangle(const struct flow_action_entry *fa,
					 struct cortina_ppe_flow_data *d)
{
	u32 val = fa->mangle.val, mask = fa->mangle.mask;

	switch (fa->mangle.htype) {
	case FLOW_ACT_MANGLE_HDR_TYPE_IP4:
		/* SNAT upstream, DNAT downstream */
		if (fa->mangle.offset != (d->ds ? offsetof(struct iphdr, daddr) :
						  offsetof(struct iphdr, saddr)))
			return -EOPNOTSUPP;
		d->nat_ip = be32_to_cpu((__force __be32)val);
		d->nat_addr = true;
		return 0;
	case FLOW_ACT_MANGLE_HDR_TYPE_TCP:
	case FLOW_ACT_MANGLE_HDR_TYPE_UDP:
		/* the mangle covers the port word: dport masked in DS, sport in US */
		if (fa->mangle.offset != 0 ||
		    ((__force __be32)mask == ~htonl(0xffff)) != d->ds)
			return -EOPNOTSUPP;
		val = be32_to_cpu((__force __be32)val);
		d->nat_l4_port = d->ds ? val & 0xffff : val >> 16;
		d->nat_port = true;
		return 0;
	case FLOW_ACT_MANGLE_HDR_TYPE_ETH:
		/* next-hop DMAC, and the egress SMAC */
		if (fa->mangle.offset == 0) {
			put_unaligned_le32(val, d->dmac);
			d->dmac_lo = true;
		} else if (fa->mangle.offset == 4 && !(mask & 0xffff)) {
			put_unaligned_le16(val, d->dmac + 4);
			d->dmac_hi = true;
		} else if (fa->mangle.offset == 4 && !(mask & 0xffff0000)) {
			put_unaligned_le16(val >> 16, d->smac);
			d->smac_lo = true;
		} else if (fa->mangle.offset == 8) {
			put_unaligned_le32(val, d->smac + 2);
			d->smac_hi = true;
		}
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int cortina_ppe_flow_parse_actions(struct flow_rule *rule,
					  struct cortina_ppe_flow_data *d)
{
	struct flow_action_entry *fa;
	int i, ret;

	flow_action_for_each(i, fa, &rule->action) {
		switch (fa->id) {
		case FLOW_ACTION_REDIRECT:
			d->odev = fa->dev;
			break;
		case FLOW_ACTION_MANGLE:
			ret = cortina_ppe_flow_parse_mangle(fa, d);
			if (ret)
				return ret;
			break;
		case FLOW_ACTION_CSUM:
			break;
		case FLOW_ACTION_PPPOE_PUSH:
			d->pppoe_sid = fa->pppoe.sid;
			break;
		case FLOW_ACTION_VLAN_PUSH:
			if (fa->vlan.proto != htons(ETH_P_8021Q) || d->rule_vid)
				return -EOPNOTSUPP;
			d->rule_vid = fa->vlan.vid;
			break;
		case FLOW_ACTION_VLAN_POP:
			d->rule_vlan_pop = true;
			break;
		default:
			return -EOPNOTSUPP;
		}
	}
	/* IPv4 requires NAT; IPv6 is routed without NAT */
	if (!d->odev)
		return -EOPNOTSUPP;
	if (!d->tuple.ipv6 && (!d->nat_addr || !d->nat_port))
		return -EOPNOTSUPP;
	if (d->tuple.ipv6 && (d->nat_addr || d->nat_port))
		return -EOPNOTSUPP;
	return 0;
}

/*
 * The WAN tag comes from the rule (a VLAN push upstream, a pop with a VLAN
 * match downstream) or from walking the upstream egress device; a PPPoE
 * session from the rule or from a PPPoE device in the forward path.
 */
static int cortina_ppe_flow_wan_encap(struct cortina_ppe *ppe,
				      struct flow_rule *rule,
				      struct cortina_ppe_flow_data *d)
{
	struct cortina_ppe_wan_encap *wan = &d->wan;
	u16 armed_sid;

	if (d->rule_vid) {
		d->wan_vid = d->rule_vid;
		wan->vid = d->rule_vid;
		wan->vproto = htons(ETH_P_8021Q);
		wan->sid = d->pppoe_sid ? d->pppoe_sid : -1;
		wan->walk_ok = true;
	} else if (d->ds && d->rule_vlan_pop &&
		   flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_VLAN)) {
		struct flow_match_vlan mv;

		flow_rule_match_vlan(rule, &mv);
		d->wan_vid = mv.key->vlan_id;
		wan->vid = mv.key->vlan_id;
		wan->vproto = htons(ETH_P_8021Q);
		wan->walk_ok = true;
		if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_PPPOE)) {
			struct flow_match_pppoe mp;

			flow_rule_match_pppoe(rule, &mp);
			wan->sid = ntohs(mp.key->session_id);
		}
	}
	if (!d->ds && !d->wan_vid) {
		d->wan_vid = cortina_ppe_wan_vid(d->odev);
		if (d->wan_vid &&
		    !cortina_ppe_wan_vlan_ok(d->odev, d->wan_vid, wan))
			return -EOPNOTSUPP;
	}
	if ((d->rule_vid || d->rule_vlan_pop) &&
	    (!d->wan_vid || (d->rule_vid && d->rule_vid != d->wan_vid)))
		return -EOPNOTSUPP;
	/* a tag found only by walking the device needs the rule to edit it */
	if (d->wan_vid && wan->sid < 0 && !d->rule_vid && !d->rule_vlan_pop)
		return -EOPNOTSUPP;

	if (d->wan_vid && wan->sid >= 0) {
		d->wan_pppoe = true;
		if (d->pppoe_sid && d->pppoe_sid != wan->sid)
			return -EOPNOTSUPP;
		if (!d->ds && !d->pppoe_sid)
			d->pppoe_sid = wan->sid;
	}

	/*
	 * The PPPoE header is pushed upstream only; while a session is armed,
	 * an upstream flow without the push is left in software.
	 */
	armed_sid = d->wan_pppoe ? wan->sid : READ_ONCE(ppe->pppoe_session);
	if (d->ds && d->pppoe_sid)
		return -EOPNOTSUPP;
	if (!d->ds && !d->pppoe_sid && armed_sid)
		return -EOPNOTSUPP;
	return 0;
}

/* the action entry, and the WAN MAC and FDB next hop it refers to */
static int cortina_ppe_flow_build(struct cortina_ppe *ppe,
				  struct cortina_ppe_flow_data *d,
				  struct cortina_ppe_act *act)
{
	int lut;

	if (!d->tuple.ipv6) {
		ppe_act_set(act, IP_ADDR_VLD, 1);
		ppe_act_set(act, IP_TYPE, d->ds);
		ppe_act_set(act, IP_ADDR, d->nat_ip);
		ppe_act_set(act, L4_PORT, d->nat_l4_port);
	}

	if (!d->ds) {
		if (d->smac_lo && d->smac_hi && is_valid_ether_addr(d->smac)) {
			unsigned long flags;
			int err;

			spin_lock_irqsave(&ppe->reg_lock, flags);
			err = cortina_ppe_wan_mac_set(ppe, d->smac);
			spin_unlock_irqrestore(&ppe->reg_lock, flags);
			if (err)
				return -EOPNOTSUPP;
		}
		if (cortina_ppe_set_us_egress(ppe, act, d->pppoe_sid) ||
		    cortina_ppe_set_us_wan_vlan(ppe, act, d->wan_vid))
			return -EOPNOTSUPP;
	}

	ppe_act_set(act, TTL_DEC, 1);
	if (d->ds)
		ppe_act_set(act, TTL_ZERO_DROP, 1);

	if (!d->dmac_lo || !d->dmac_hi)
		return -EOPNOTSUPP;
	/* tagged PPPoE upstream: a zero next hop means the session peer */
	if (d->wan_pppoe && d->wan.ac_mac_vld && is_zero_ether_addr(d->dmac)) {
		if (d->ds)
			return -EOPNOTSUPP;
		ether_addr_copy(d->dmac, d->wan.ac_mac);
	}
	if (!is_valid_ether_addr(d->dmac))
		return -EOPNOTSUPP;

	if (!d->ds) {
		lut = cortina_ni_l2fe_fdb_add_idx(ppe->base, d->dmac,
						  CA_NI_RX_L3WAN_LDPID);
		if (lut < 0)
			return -EOPNOTSUPP;
	} else {
		u32 lan_ldpid = 0;

		lut = cortina_ni_l2fe_fdb_lookup_idx(ppe->base, d->dmac,
						     &lan_ldpid);
		if (lut < 0 || lan_ldpid > PPE_LAN_PORT_LDPID_MAX)
			return -EOPNOTSUPP;
		cortina_ppe_set_ds_egress(act, lan_ldpid, d->wan_vid);
	}
	ppe_act_set(act, MAC_DA_IDX, lut);
	ppe_act_set(act, MAC_DA_IDX_VLD, 1);
	return 0;
}

static int cortina_ppe_flow_commit(struct cortina_ppe *ppe,
				   struct flow_cls_offload *f,
				   const struct cortina_ppe_bind *bind,
				   const struct cortina_ppe_flow_data *d,
				   const struct cortina_ppe_act *act)
{
	struct cortina_ppe_flow *flow;
	int err;

	flow = kzalloc(sizeof(*flow), GFP_KERNEL);
	if (!flow)
		return -ENOMEM;
	flow->cookie = f->cookie;
	flow->block = bind->block;
	flow->last_hit = jiffies;
	flow->tuple = d->tuple;
	flow->pppoe_sid = d->pppoe_sid;
	flow->ds = d->ds;

	err = cortina_ppe_entry_add(ppe, &d->tuple, act, &flow->hash_idx,
				    &flow->crc16);
	if (err)
		goto free;

	err = rhashtable_insert_fast(&ppe->flow_table, &flow->node,
				     cortina_ppe_ht_params);
	if (err) {
		cortina_ppe_entry_del(ppe, flow->hash_idx, flow->crc16);
		goto free;
	}
	ppe->flow_by_idx[flow->hash_idx] = flow;
	ppe->row_occ[flow->hash_idx / PPE_AGE_SLOTS]++;
	atomic_inc(&ppe->stats.installed);
	if (d->ds)
		atomic_inc(&ppe->stats.ds_installed);

	dev_dbg(ppe->dev,
		"%s INSTALLED idx=%u crc16=%04x %pI4h:%u->%pI4h:%u proto=%u pppoe=%#x\n",
		d->ds ? "DS" : "US", flow->hash_idx, flow->crc16,
		&d->tuple.sip, d->tuple.sport, &d->tuple.dip, d->tuple.dport,
		d->tuple.proto, d->pppoe_sid);
	return 0;

free:
	kfree(flow);
	return err;
}

static int cortina_ppe_flow_replace(struct cortina_ppe *ppe,
				    struct flow_cls_offload *f,
				    const struct cortina_ppe_bind *bind)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(f);
	struct cortina_ppe_flow_data d = {
		.wan = { .vid = -1, .sid = -1 },
	};
	struct cortina_ppe_act act = {};
	int err;

	if (!ppe->install_ok)
		return -EOPNOTSUPP;
	if (rhashtable_lookup_fast(&ppe->flow_table, &f->cookie,
				   cortina_ppe_ht_params))
		return -EEXIST;

	err = cortina_ppe_flow_parse_match(rule, bind->dev, &d);
	if (err)
		return err;
	if (d.ds && !ppe->ds_armed)
		return -EOPNOTSUPP;

	err = cortina_ppe_flow_parse_actions(rule, &d);
	if (!err)
		err = cortina_ppe_flow_wan_encap(ppe, rule, &d);
	if (!err)
		err = cortina_ppe_flow_build(ppe, &d, &act);
	if (err)
		return err;

	return cortina_ppe_flow_commit(ppe, f, bind, &d, &act);
}

static int cortina_ppe_flow_destroy(struct cortina_ppe *ppe,
				    struct flow_cls_offload *f)
{
	struct cortina_ppe_flow *entry;

	entry = rhashtable_lookup_fast(&ppe->flow_table, &f->cookie,
				       cortina_ppe_ht_params);
	if (!entry)
		return -ENOENT;
	cortina_ppe_flow_free(ppe, entry);
	return 0;
}

static int cortina_ppe_flow_stats(struct cortina_ppe *ppe,
				  struct flow_cls_offload *f)
{
	struct cortina_ppe_flow *entry;

	entry = rhashtable_lookup_fast(&ppe->flow_table, &f->cookie,
				       cortina_ppe_ht_params);
	if (!entry)
		return -ENOENT;
	f->stats.lastused = entry->last_hit;
	return 0;
}

static int cortina_ppe_block_cb(enum tc_setup_type type, void *type_data,
				void *cb_priv)
{
	const struct cortina_ppe_bind *bind = cb_priv;
	struct cortina_ppe *ppe = bind->ppe;
	struct flow_cls_offload *f = type_data;
	int err;

	if (type != TC_SETUP_CLSFLOWER)
		return -EOPNOTSUPP;

	mutex_lock(&ppe->flow_lock);
	switch (f->command) {
	case FLOW_CLS_REPLACE:
		err = cortina_ppe_flow_replace(ppe, f, bind);
		cortina_ppe_refused_account(ppe, err);
		break;
	case FLOW_CLS_DESTROY:
		err = cortina_ppe_flow_destroy(ppe, f);
		break;
	case FLOW_CLS_STATS:
		err = cortina_ppe_flow_stats(ppe, f);
		break;
	default:
		err = -EOPNOTSUPP;
		break;
	}
	mutex_unlock(&ppe->flow_lock);
	return err;
}

static void cortina_ppe_bind_release(void *cb_priv)
{
	kfree(cb_priv);
}

static bool cortina_ppe_block_bound(struct cortina_ppe *ppe,
				    const struct flow_block *block)
{
	struct flow_block_cb *block_cb;

	list_for_each_entry(block_cb, &ppe->block_cb_list, driver_list) {
		const struct cortina_ppe_bind *bind = block_cb->cb_priv;

		if (bind->block == block)
			return true;
	}
	return false;
}

static int cortina_ppe_setup_block(struct cortina_ppe *ppe,
				   struct net_device *dev,
				   struct flow_block_offload *f)
{
	flow_setup_cb_t *cb = cortina_ppe_block_cb;
	struct cortina_ppe_bind *bind;
	struct flow_block_cb *block_cb;

	if (f->binder_type != FLOW_BLOCK_BINDER_TYPE_CLSACT_INGRESS)
		return -EOPNOTSUPP;

	f->driver_block_list = &ppe->block_cb_list;

	switch (f->command) {
	case FLOW_BLOCK_BIND:
		block_cb = flow_block_cb_lookup(f->block, cb, dev);
		if (block_cb) {
			flow_block_cb_incref(block_cb);
			return 0;
		}
		bind = kzalloc(sizeof(*bind), GFP_KERNEL);
		if (!bind)
			return -ENOMEM;
		bind->ppe = ppe;
		bind->dev = dev;
		bind->block = f->block;
		block_cb = flow_block_cb_alloc(cb, dev, bind,
					       cortina_ppe_bind_release);
		if (IS_ERR(block_cb)) {
			kfree(bind);
			return PTR_ERR(block_cb);
		}
		flow_block_cb_incref(block_cb);
		flow_block_cb_add(block_cb, f);
		list_add_tail(&block_cb->driver_list, &ppe->block_cb_list);
		return 0;
	case FLOW_BLOCK_UNBIND:
		block_cb = flow_block_cb_lookup(f->block, cb, dev);
		if (!block_cb)
			return -ENOENT;
		if (!flow_block_cb_decref(block_cb)) {
			flow_block_cb_remove(block_cb, f);
			list_del(&block_cb->driver_list);
			/*
			 * nf_tables unbinds a flowtable before freeing its
			 * flows, so DESTROY never arrives: drop the block's
			 * entries when its last device goes.
			 */
			if (!cortina_ppe_block_bound(ppe, f->block)) {
				mutex_lock(&ppe->flow_lock);
				cortina_ppe_flush(ppe, f->block);
				mutex_unlock(&ppe->flow_lock);
			}
		}
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

/*
 * ndo_setup_tc of the NI netdev (which the DSA LAN ports forward to) and of
 * the GPON WAN netdev: both feed the one engine.
 */
int cortina_ppe_setup_tc(struct net_device *dev, enum tc_setup_type type,
			 void *type_data)
{
	struct cortina_ppe *ppe = READ_ONCE(cortina_ppe_inst);

	if (!ppe)
		return -EOPNOTSUPP;

	switch (type) {
	case TC_SETUP_BLOCK:
	case TC_SETUP_FT:
		return cortina_ppe_setup_block(ppe, dev, type_data);
	default:
		return -EOPNOTSUPP;
	}
}
EXPORT_SYMBOL_GPL(cortina_ppe_setup_tc);

/* ---- NI and GPON interface ---- */

bool cortina_ppe_active(void)
{
	return !!READ_ONCE(cortina_ppe_inst);
}
EXPORT_SYMBOL_GPL(cortina_ppe_active);

void cortina_ppe_wan_path_set(u16 gem_id, u8 tcont_idx)
{
	struct cortina_ppe *ppe = READ_ONCE(cortina_ppe_inst);

	if (!ppe)
		return;
	WRITE_ONCE(ppe->data_gem, gem_id);
	WRITE_ONCE(ppe->data_tcont, tcont_idx);
	if (!gem_id && READ_ONCE(ppe->pppoe_session)) {
		mutex_lock(&ppe->flow_lock);
		cortina_ppe_set_session(ppe, 0);
		mutex_unlock(&ppe->flow_lock);
	}
}
EXPORT_SYMBOL_GPL(cortina_ppe_wan_path_set);

void cortina_ppe_router_mac_set(struct cortina_ppe *ppe, const u8 *mac)
{
	if (!ppe || !mac)
		return;
	ether_addr_copy(ppe->router_mac, mac);
	ppe->router_mac_valid = true;
}

void cortina_ppe_stats(struct cortina_ppe *ppe, u64 out[CA_L3FE_STAT_COUNT])
{
	memset(out, 0, sizeof(u64) * CA_L3FE_STAT_COUNT);
	if (!ppe)
		return;
	out[CA_L3FE_FLOWS_RESIDENT] = atomic_read(&ppe->stats.installed);
	out[CA_L3FE_DS_FLOWS_RESIDENT] = atomic_read(&ppe->stats.ds_installed);
	out[CA_L3FE_US_HITS] = atomic_read(&ppe->stats.us_hits);
	out[CA_L3FE_DS_HITS] = atomic_read(&ppe->stats.ds_hits);
	out[CA_L3FE_FLOWS_REFUSED] = atomic_read(&ppe->stats.refused);
	out[CA_L3FE_REFUSED_UNSUPPORTED] = atomic_read(&ppe->stats.refused_unsupp);
	out[CA_L3FE_REFUSED_TABLE_FULL] = atomic_read(&ppe->stats.refused_full);
	out[CA_L3FE_REFUSED_DUPLICATE] = atomic_read(&ppe->stats.refused_dup);
	out[CA_L3FE_REFUSED_ERROR] = atomic_read(&ppe->stats.refused_err);
}

/* a PPPoE session ends with its PPP device: clear it and its flows */
static int cortina_ppe_netdev_event(struct notifier_block *nb,
				    unsigned long event, void *ptr)
{
	struct cortina_ppe *ppe = container_of(nb, struct cortina_ppe, ppp_nb);
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);

	if (event != NETDEV_DOWN || dev->type != ARPHRD_PPP)
		return NOTIFY_DONE;
	if (READ_ONCE(ppe->pppoe_session)) {
		mutex_lock(&ppe->flow_lock);
		cortina_ppe_set_session(ppe, 0);
		mutex_unlock(&ppe->flow_lock);
	}
	return NOTIFY_DONE;
}

/* hardware forwarding: hash lookups, router MACs, the IPoE and DS L3-IFs */
static void cortina_ppe_hw_setup(struct cortina_ppe *ppe)
{
	unsigned long flags;
	int ret;

	if (cortina_ppe_classify_setup(ppe))
		dev_warn(ppe->dev, "classify setup timed out\n");

	ret = cortina_ppe_l3_forward_enable(ppe);
	if (!ret) {
		spin_lock_irqsave(&ppe->reg_lock, flags);
		ret = cortina_ppe_l3if_set(ppe, PPE_IPOE_L3IF_IDX,
					   PPE_WAN_AN_SEL, 0);
		spin_unlock_irqrestore(&ppe->reg_lock, flags);
	}
	if (ret) {
		dev_warn(ppe->dev, "forwarding setup failed (%d)\n", ret);
		return;
	}
	if (cortina_ppe_arm_ds(ppe))
		dev_warn(ppe->dev, "DS setup failed, upstream only\n");
	ppe->install_ok = true;
}

int cortina_ppe_probe(struct cortina_ni *ni)
{
	struct device *dev = ni->dev;
	struct cortina_ppe *ppe;
	int ret;

	if (!ni->win[CA_NI_WIN_NI])
		return -ENODEV;

	ppe = devm_kzalloc(dev, sizeof(*ppe), GFP_KERNEL);
	if (!ppe)
		return -ENOMEM;
	ppe->dev = dev;
	ppe->base = ni->win[CA_NI_WIN_NI];
	spin_lock_init(&ppe->reg_lock);
	mutex_init(&ppe->flow_lock);
	INIT_LIST_HEAD(&ppe->block_cb_list);
	INIT_DELAYED_WORK(&ppe->sweep_work, cortina_ppe_sweep_work);
	ppe->ppp_nb.notifier_call = cortina_ppe_netdev_event;
	if (ni->tx && ni->tx->netdev)
		cortina_ppe_router_mac_set(ppe, ni->tx->netdev->dev_addr);

	ppe->shadow_crc32 = devm_kcalloc(dev, PPE_ENTRIES, sizeof(u32),
					 GFP_KERNEL);
	ppe->shadow_crc16 = devm_kcalloc(dev, PPE_ENTRIES, sizeof(u16),
					 GFP_KERNEL);
	ppe->flow_by_idx = devm_kcalloc(dev, PPE_ENTRIES,
					sizeof(*ppe->flow_by_idx), GFP_KERNEL);
	ppe->row_occ = devm_kcalloc(dev, PPE_AGE_ROWS, sizeof(u8), GFP_KERNEL);
	if (!ppe->shadow_crc32 || !ppe->shadow_crc16 || !ppe->flow_by_idx ||
	    !ppe->row_occ)
		return -ENOMEM;

	/* the table base registers take 32-bit addresses */
	dma_set_coherent_mask(dev, DMA_BIT_MASK(32));
	ppe->carve = dmam_alloc_coherent(dev, PPE_KEY_TBL_BYTES +
					 PPE_ACT_TBL_BYTES, &ppe->carve_pa,
					 GFP_KERNEL);
	if (!ppe->carve)
		return -ENOMEM;
	ppe->key_tbl = ppe->carve;
	ppe->act_tbl = ppe->carve + PPE_KEY_TBL_BYTES;

	ret = rhashtable_init(&ppe->flow_table, &cortina_ppe_ht_params);
	if (ret)
		return ret;

	ret = cortina_ppe_engine_init(ppe);
	if (ret)
		goto err_ht;
	cortina_ppe_hw_setup(ppe);

	ret = register_netdevice_notifier(&ppe->ppp_nb);
	if (ret)
		goto err_ht;
	schedule_delayed_work(&ppe->sweep_work, msecs_to_jiffies(PPE_SWEEP_MS));

	ni->ppe = ppe;
	WRITE_ONCE(cortina_ppe_inst, ppe);
	return 0;

err_ht:
	rhashtable_destroy(&ppe->flow_table);
	return ret;
}
