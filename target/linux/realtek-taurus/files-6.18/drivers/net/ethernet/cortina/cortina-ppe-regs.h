/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * RTL9607DQ L3FE main-hash flow engine: registers, the hash key descriptor
 * and the action entry layout. Register offsets are from the NI window.
 */

#ifndef _CORTINA_PPE_REGS_H
#define _CORTINA_PPE_REGS_H

#include <linux/bits.h>

#define HS_ACCESS_WRITE			BIT(30)
#define HS_ACCESS_GO			BIT(31)

/* main hash */
#define REG_HS_PROFILE_TUPLE0(p)	(0x3704 + (p) * 0x2c)
#define REG_HS_HASH_INI			0x3834
#define REG_HS_BA_MH1			0x3838	/* key table, phys[39:32] */
#define REG_HS_BA_MH0			0x383c	/* key table, phys[31:0] */
#define REG_HS_BA_MA1			0x3840	/* action table, phys[39:32] */
#define REG_HS_BA_MA0			0x3844	/* action table, phys[31:0] */
#define REG_HS_DEFAULT_ACTION(i)	(0x3860 + (i) * 4)
#define REG_HS_CACHE_INI		0x38a0
#define REG_HS_CACHE_CTRL		0x38ac
#define   HS_CACHE_CTRL_SLOT		GENMASK(4, 0)
#define   HS_CACHE_CTRL_CRC16		GENMASK(20, 5)
#define   HS_CACHE_CTRL_INVALIDATE	BIT(30)
#define REG_HS_CACHE_CTRL_REQ		0x38b0
#define   HS_CACHE_CTRL_REQ_GO		BIT(0)
#define REG_HS_CACHE_MISC		0x38c4
#define REG_HS_SWO_IDX			0x38d8
#define REG_HS_SWO_DAT			0x38dc	/* auto-increments SWO_IDX */
#define REG_HS_SWO_CTRL			0x38e0
#define   HS_SWO_CTRL_GO		BIT(0)
#define HS_SWO_IDX_MASK_ID		32
#define HS_SWO_IDX_CRC32		33
#define HS_SWO_IDX_CRC16		34
#define REG_HS_MASK_ACCESS		0x3910
#define   HS_MASK_ACCESS_IDX		GENMASK(5, 0)
#define   HS_MASK_ACCESS_UPPER		BIT(6)	/* bits 255:128 */
#define REG_HS_MASK_DATA(n)		(0x3920 - (n) * 4)
#define REG_HS_AGING_GRANULARITY	0x3924
#define REG_HS_AGE_ACCESS		0x3928
#define   HS_AGE_ACCESS_ROW		GENMASK(10, 0)
/*
 * Age SRAM: a row holds 32 slots of 4 bits in four DATA words of 8 slots:
 * slots 0-7 in 0x3938, 8-15 in 0x3934, 16-23 in 0x3930, 24-31 in 0x392c.
 */
#define REG_HS_AGE_DATA(slot)		(0x3938 - 4 * (((slot) & 0x1f) >> 3))
#define   HS_AGE_SHIFT(slot)		(((slot) & 7) * 4)
#define   HS_AGE_MASK			0xf
#define HS_AGE_WORDS			4
#define HS_AGE_PER_WORD			8
#define REG_HS_MEM_INI			0x393c
#define   HS_MEM_INI_REQ		BIT(0)
#define REG_HS_CHK_FAIL_CTRL		0x3940
#define REG_HS_RSV0			0x3944
#define REG_HS_RSV1			0x3948
#define REG_HS_DEF_REG(i)		(0x39dc + (i) * 4)
#define REG_AQM_TIMER			0x3aa8
#define REG_AXIM2_CONFIG		0x3c80

/* L3 classifier */
#define REG_CLS_KEY_ACCESS		0x3380
#define   CLS_KEY_ACCESS_IDX		GENMASK(10, 0)
#define CLS_KEY_WORDS			11
#define REG_CLS_FIB_ACCESS		0x33b0
#define REG_CLS_FIB_DATA0		0x33cc	/* DATA1.. at descending offsets */
#define CLS_FIB_WORDS			7

/* packet editor */
#define REG_PE_PPPOE_CFG		0x3500	/* code, version, type */
#define REG_PE_PPPOE_PROT_CFG		0x3504	/* PPP protocols v4, v6 */
#define REG_PE_CFG			0x351c
#define   PE_CFG_LDPID_BASE		GENMASK(9, 4)
#define   PE_CFG_GEMID_MAP		BIT(10)
#define   PE_CFG_PAD_CTRL		BIT(12)
#define   PE_CFG_RSVD14			BIT(14)
/*
 * Resets to 1: every punt of a T2 miss is then length-checked against the
 * mostly-zero MTU table and large frames are lost.
 */
#define   PE_CFG_MTU_CHK_EN		BIT(31)

/* egress L3 interface table: SMAC substitution and PPPoE encapsulation */
#define REG_L3IF_ACCESS			0x3000
#define REG_L3IF_DATA			0x3004
#define   L3IF_PPPOE_SET		BIT(0)
#define   L3IF_PPPOE_VLD		BIT(1)
#define   L3IF_PPPOE_SESSION		GENMASK(17, 2)
#define   L3IF_MAC_SA_VLD		BIT(18)
#define   L3IF_MAC_SA_AN_SEL		GENMASK(22, 19)
#define   L3IF_PAD_CTRL			BIT(23)
#define L3IF_ENTRIES			32

/* PP field CAMs; MAC-DA entries 0/1 are the LAN/WAN router MACs */
#define REG_PP_FIELD_CAM_ACCESS		0x3200
#define   PP_FIELD_CAM_SEL		GENMASK(19, 16)
#define REG_PP_FIELD_CAM_DATA(n)	(0x3214 - (n) * 4)
#define PP_CAM_WORDS			5
#define PP_CAM_SEL_MAC_DA		3
#define PP_CAM_SEL_LAST			10
#define PP_CAM_ENTRIES			16
#define PP_CAM_MAC_DA_ENTRIES		15
#define   PP_CAM_MAC_DA_VLD		BIT(16)

/* L2FE FDB and logical-to-physical destination map */
#define REG_FDB_CMD_RETURN		0x1c2c
#define REG_FDB_ACCESS			0x1ca0
#define   FDB_OP_REPLACE		0x65
#define REG_FDB_DATA3			0x1ca4
#define REG_FDB_DATA2			0x1ca8
#define REG_FDB_DATA1			0x1cac
#define REG_FDB_DATA0			0x1cb0
#define   FDB_LPID			GENMASK(5, 0)
#define   FDB_VALID			BIT(9)
#define   FDB_STATIC			BIT(19)
#define   FDB_DA_PERMIT			BIT(20)
#define   FDB_SA_PERMIT			BIT(21)
#define REG_PDPID_MAP_ACCESS		0x1654
#define REG_PDPID_MAP_DATA		0x1658
#define   PDPID_IDX_DBUF		BIT(6)
#define   PDPID_IDX_MYMAC		BIT(7)

/*
 * HDR_I: the 1024-bit descriptor the parser builds for a frame and the SWO
 * engine hashes. Each field is a bit offset (LSB first) and a width.
 */
#define HDRI_BITS			1024
#define HDRI_L4_DP_SHIFT		74
#define HDRI_L4_DP_BITS			16
#define HDRI_L4_SP_SHIFT		90
#define HDRI_L4_SP_BITS			16
#define HDRI_IP_DA_SHIFT		233	/* IPv4, or bits 31:0 of IPv6 */
#define HDRI_IP_DA_BITS			32
#define HDRI_IP_SA_SHIFT		361
#define HDRI_IP_SA_BITS			32
#define HDRI_IP_PROTO_SHIFT		492
#define HDRI_IP_PROTO_BITS		8
#define HDRI_IP_VER_SHIFT		509	/* 0: IPv4 */
#define HDRI_IP_VER_BITS		1
#define HDRI_IP_VLD_SHIFT		510
#define HDRI_IP_VLD_BITS		1
#define HDRI_T2_CTRL_SHIFT		961	/* profile stamp */
#define HDRI_T2_CTRL_BITS		4

/*
 * Action entry, normal mode (groups 18 + 20): 256 bits, LSB first. Each field
 * is a bit offset and a width for bitmap_write(); several cross 32-bit words.
 */
#define PPE_ACT_BITS			256
#define PPE_ACT_MRR_VLD_SHIFT		0
#define PPE_ACT_MRR_VLD_BITS		1
#define PPE_ACT_DPID_VLD_SHIFT		4
#define PPE_ACT_DPID_VLD_BITS		1
#define PPE_ACT_DPID_PRI_SHIFT		5
#define PPE_ACT_DPID_PRI_BITS		1
#define PPE_ACT_PERMIT_SHIFT		6
#define PPE_ACT_PERMIT_BITS		1
#define PPE_ACT_DEEPQ_SHIFT		7
#define PPE_ACT_DEEPQ_BITS		1
#define PPE_ACT_MCGID_SHIFT		8	/* egress LDPID or multicast group */
#define PPE_ACT_MCGID_BITS		10
#define PPE_ACT_MC_SHIFT		18
#define PPE_ACT_MC_BITS			1
#define PPE_ACT_L3_IF_VLD_SHIFT		28
#define PPE_ACT_L3_IF_VLD_BITS		1
#define PPE_ACT_IGR_L3_IF_SHIFT		30
#define PPE_ACT_IGR_L3_IF_BITS		6
#define PPE_ACT_EGR_L3_IF_SHIFT		36
#define PPE_ACT_EGR_L3_IF_BITS		6
#define PPE_ACT_L3_IF_CNT_EN_SHIFT	42
#define PPE_ACT_L3_IF_CNT_EN_BITS	1
#define PPE_ACT_TTL_DEC_SHIFT		43
#define PPE_ACT_TTL_DEC_BITS		1
#define PPE_ACT_TTL_ZERO_DROP_SHIFT	44
#define PPE_ACT_TTL_ZERO_DROP_BITS	1
#define PPE_ACT_IP_ADDR_VLD_SHIFT	45
#define PPE_ACT_IP_ADDR_VLD_BITS	1
#define PPE_ACT_IP_TYPE_SHIFT		46	/* rewritten address: 0 SA, 1 DA */
#define PPE_ACT_IP_TYPE_BITS		1
#define PPE_ACT_IP_ADDR_SHIFT		47
#define PPE_ACT_IP_ADDR_BITS		32
#define PPE_ACT_MAC_DA_IDX_VLD_SHIFT	80
#define PPE_ACT_MAC_DA_IDX_VLD_BITS	1
#define PPE_ACT_MAC_DA_IDX_SHIFT	81	/* L2FE FDB index of the next hop */
#define PPE_ACT_MAC_DA_IDX_BITS		13
#define PPE_ACT_CHK_MSK_PTR_SHIFT	94	/* recheck mask; 0 punts every hit */
#define PPE_ACT_CHK_MSK_PTR_BITS	6
#define PPE_ACT_CACHE_CTRL_SHIFT	100
#define PPE_ACT_CACHE_CTRL_BITS		2
#define PPE_ACT_POP_L3_VLD_SHIFT	102
#define PPE_ACT_POP_L3_VLD_BITS		1
#define PPE_ACT_T2_CTRL_SHIFT		106
#define PPE_ACT_T2_CTRL_BITS		4
#define PPE_ACT_TOP_VID_SHIFT		147
#define PPE_ACT_TOP_VID_BITS		12
#define PPE_ACT_TOP_TPID_ENC_SHIFT	159	/* L3FE PP TPID slot + 1 */
#define PPE_ACT_TOP_TPID_ENC_BITS	3
#define PPE_ACT_VLAN_CNT_SHIFT		162
#define PPE_ACT_VLAN_CNT_BITS		2
#define PPE_ACT_VLAN_VLD_SHIFT		164
#define PPE_ACT_VLAN_VLD_BITS		1
#define PPE_ACT_PPPOE_VLD_SHIFT		189
#define PPE_ACT_PPPOE_VLD_BITS		1
#define PPE_ACT_PPPOE_SET_SHIFT		190
#define PPE_ACT_PPPOE_SET_BITS		1
#define PPE_ACT_L4_PORT_SHIFT		191
#define PPE_ACT_L4_PORT_BITS		16

#endif /* _CORTINA_PPE_REGS_H */
