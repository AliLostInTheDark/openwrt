// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015 Cortina-Access Limited
 * Copyright (C) BRULE Herman <alpha_one_x86@first-world.info>
 *
 * Cortina NI Ethernet driver for the Realtek RTL9607DQ: TX datapath, the
 * conduit netdev and the US PON (OMCI and WAN data) transmit paths.
 *
 * Frames go out through the DMA-LSO engine. For LAN the 8-byte ring descriptor
 * carries the destination port and CoS (direct TX, FE bypass) and the buffer is
 * a plain Ethernet frame. PON frames use a two-descriptor chain whose first
 * buffer holds a HEADER_A. eth0 uses its own ring for packet order; the PON
 * paths share txq[0]. Completion is read from the hardware read pointer,
 * reclaimed at xmit time and from a timer, since no TX-done IRQ is used.
 */

#include <linux/crc32.h>
#include <linux/bitfield.h>
#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/if_arp.h>
#include <linux/if_vlan.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/of_net.h>
#include <linux/phy.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/skbuff.h>
#include <net/arp.h>
#include <net/dsa.h>
#include <net/net_namespace.h>

#include "cortina-ni.h"

/*
 * Default CPU->LAN egress port. The descriptor DEST is an LDPID that the L2FE
 * ARB maps to the physical port; for LAN NI ports this is the identity map.
 */
#define CA_NI_TX_PORT		3
#define CA_NI_TX_COS		0
#define CA_NI_TX_TXQ		0

#define CA_NI_RECLAIM_INTERVAL	msecs_to_jiffies(10)

/*
 * The frame is not FE-forwarded because that needs an L2FE flood group with
 * the LAN ports and a DFT_FWD entry for the CPU source port, neither of which
 * is programmed.
 */

/* HEADER_A lspid of host-injected US frames; the vendor GPON driver uses 0x11 */
#define CA_NI_PON_TX_LSPID	0x11


/* CPU->LAN frames on the conduit go to every LAN port. */
static u32 ca_ni_lan_tx_ports(void)
{
	return GENMASK(CA_NI_LAN_PORT_COUNT - 1, 0);
}

static inline void __iomem *ni_base(struct cortina_ni *ni)
{
	return ni->win[CA_NI_WIN_NI];
}

static inline void __iomem *dma_base(struct cortina_ni *ni)
{
	return ni->win[CA_NI_WIN_DMA];
}

static inline void ni_rmw(struct cortina_ni *ni, u32 off, u32 clr, u32 set)
{
	writel((readl(ni_base(ni) + off) & ~clr) | set, ni_base(ni) + off);
}

static inline void dma_rmw(struct cortina_ni *ni, u32 off, u32 clr, u32 set)
{
	writel((readl(dma_base(ni) + off) & ~clr) | set, dma_base(ni) + off);
}

/*
 * NI block reset handshake (vendor aal_ni_reset): wait for the NI self-init
 * done flag, then deassert every interface reset. U-Boot has normally done both
 * already, so only warn.
 */
static void cortina_ni_tx_reset_intf(struct cortina_ni *ni)
{
	u32 val;
	int ret;

	ret = readl_poll_timeout(ni_base(ni) + CA_NI_HV_INIT_DONE, val,
				 val & CA_NI_HV_INIT_DONE_NI,
				 CA_NI_TX_POLL_US, CA_NI_TX_POLL_TIMEOUT_US);
	if (ret)
		dev_warn(ni->dev, "NI init-done not set (0x%08x), continuing\n",
			 val);

	writel(0, ni_base(ni) + CA_NI_HV_INTF_RST);
	val = readl(ni_base(ni) + CA_NI_HV_INTF_RST);	/* the vendor driver reads back */
	if (val)
		dev_warn(ni->dev, "INTF_RST readback 0x%08x != 0\n", val);
}

/*
 * Program the DMA-LSO STREAMID table (0x0e0/0x0e4) as the vendor driver does:
 * sid[0..7] = enabled, ldpid 0x21, cos i, pol_id 8 + i; the rest 0. The table
 * cannot be read back on this SoC; only the GO bit of ACCESS is polled.
 */
static void cortina_ni_tx_streamid_init(struct cortina_ni *ni)
{
	unsigned int i;
	u32 val;

	for (i = 0; i < CA_DMA_LSO_STREAMID_ENTRIES; i++) {
		u32 e = 0;

		if (i < CA_DMA_LSO_SID_STOCK_LIVE)
			e = CA_DMA_LSO_SID_EN |
			    CA_DMA_LSO_SID_DEEP_Q |
			    FIELD_PREP(CA_DMA_LSO_SID_LDPID,
				       CA_DMA_LSO_SID_STOCK_LDPID) |
			    FIELD_PREP(CA_DMA_LSO_SID_COS, i) |
			    FIELD_PREP(CA_DMA_LSO_SID_POL_ID, 8 + i);

		writel(e, dma_base(ni) + CA_DMA_LSO_STREAMID_DATA);
		writel(CA_DMA_LSO_BD_ACCESS_GO | CA_DMA_LSO_BD_ACCESS_WRITE | i,
		       dma_base(ni) + CA_DMA_LSO_STREAMID_ACCESS);
		if (readl_poll_timeout(dma_base(ni) + CA_DMA_LSO_STREAMID_ACCESS,
				       val, !(val & CA_DMA_LSO_BD_ACCESS_GO),
				       CA_NI_TX_POLL_US, CA_NI_TX_POLL_TIMEOUT_US)) {
			dev_warn(ni->dev,
				 "streamid: entry %u did not complete (access=0x%08x) - stopping\n",
				 i, readl(dma_base(ni) + CA_DMA_LSO_STREAMID_ACCESS));
			return;
		}
	}
	dev_dbg(ni->dev,
		"STREAMID table written (sid[0..7] en=1 ldpid=0x%02x cos=v pol_id=8+v, sid[8..255]=0; sid0 word=0x%08x)\n",
		CA_DMA_LSO_SID_STOCK_LDPID,
		(u32)(CA_DMA_LSO_SID_EN |
		       FIELD_PREP(CA_DMA_LSO_SID_LDPID, CA_DMA_LSO_SID_STOCK_LDPID) |
		       FIELD_PREP(CA_DMA_LSO_SID_POL_ID, 8)));
}

/*
 * Global TX engine setup (vendor aal_ni_init_tx_dma_lso and
 * rtk_ni_init_tx_dma_lso). Without it descriptors are consumed but no frame
 * moves.
 */
static int cortina_ni_tx_engine_init(struct cortina_ni *ni)
{
	void __iomem *dma = dma_base(ni);
	void __iomem *reo = ni->win[CA_NI_WIN_AXI_REO];
	int i;

	/*
	 * DMA-LSO SRAM test, light-sleep, CCI map, length-fix and interrupt
	 * enable, set to the values the vendor firmware leaves. The generic
	 * "MISC" offsets used before land on the SRAM test/light-sleep controls
	 * on this SoC and stop the engine moving data.
	 */
	writel(0x02020202, dma + CA_DMA_LSO_SRAM_TEST_CTRL0);
	writel(0x02020202, dma + CA_DMA_LSO_SRAM_TEST_CTRL1);
	writel(0x02020202, dma + CA_DMA_LSO_SRAM_TEST_CTRL2);
	writel(0x00000000, dma + CA_DMA_LSO_SRAM_LS_CTRL);
	writel(0x00000002, dma + CA_DMA_LSO_CCI_MAP);
	writel(0x00000001, dma + CA_DMA_LSO_CFG_LENFIX_EN);
	writel(0x0000003f, dma + CA_DMA_LSO_INTENABLE);
	dev_dbg(ni->dev,
		"DMA-LSO globals: sram_test=%08x/%08x/%08x ls=%08x cci_map=%08x lenfix=%08x inten=%08x\n",
		readl(dma + CA_DMA_LSO_SRAM_TEST_CTRL0),
		readl(dma + CA_DMA_LSO_SRAM_TEST_CTRL1),
		readl(dma + CA_DMA_LSO_SRAM_TEST_CTRL2),
		readl(dma + CA_DMA_LSO_SRAM_LS_CTRL),
		readl(dma + CA_DMA_LSO_CCI_MAP),
		readl(dma + CA_DMA_LSO_CFG_LENFIX_EN),
		readl(dma + CA_DMA_LSO_INTENABLE));

	/* enable all 8 TX queues of every DMA-LSO VP */
	for (i = 0; i < CA_DMA_LSO_VP_COUNT; i++)
		writel(CA_DMA_LSO_VP_TXQ_ALL_EN,
		       dma + CA_DMA_LSO_VP_CONTROL(i));

	/* AXI master: outstanding transactions + cacheline transfers */
	writel(readl(dma + CA_DMA_AXIM2_CONFIG) | CA_DMA_AXIM2_CONFIG_BITS,
	       dma + CA_DMA_AXIM2_CONFIG);

	/*
	 * Non-coherent read attributes for all VPs. The vendor uses the
	 * coherent (ACE) pattern, but descriptor fetches over ACE never
	 * complete here. The rings and buffers are cache-maintained by the DMA
	 * API.
	 */
	writel(CA_DMA_LSO_AXI_USER_SEL0_VAL, dma + CA_DMA_LSO_AXI_USER_SEL0);
	for (i = 0; i < 4; i++)
		writel(CA_DMA_LSO_AXI_USER_PAT_NOCOH,
		       dma + CA_DMA_LSO_AXI_USER_PAT0 + i * 4);

	/*
	 * The scheduler TX enable and LSO_CTRL are written in
	 * cortina_ni_tx_engine_enable(), after the rings are programmed.
	 */

	/* AXI reorder slots for the DMA-LSO read path */
	if (reo) {
		for (i = 0; i < CA_AXI_REO_SLOT_COUNT; i++)
			writel(CA_AXI_REO_SLOT_VAL, reo + CA_AXI_REO_SLOT(i));
	} else {
		dev_warn(ni->dev,
			 "axi-reo window unmapped, skipping reorder cfg\n");
	}

	/* VLAN tag type (the vendor firmware reads 0x800088a8) */
	writel(CA_DMA_LSO_VLAN_TAG_TYPE0_VAL, dma + CA_DMA_LSO_VLAN_TAG_TYPE0);

	/*
	 * 0x0e0/0x0e4 is the STREAMID table on this SoC. The VP->LSPID map is
	 * not programmed: its access faults on this SoC.
	 */
	cortina_ni_tx_streamid_init(ni);

	/*
	 * keep the source LSPID from the map table and pad via AFT below
	 * instead of lso_padding (0x2d -> 0x1d), as the vendor driver ends up
	 */
	dma_rmw(ni, CA_DMA_LSO_CTRL, CA_DMA_LSO_CTRL_PAD_EN,
		CA_DMA_LSO_CTRL_LSPID_KEEP);

	/* HW short-frame pad to 64 bytes */
	dma_rmw(ni, CA_DMA_AFT_CTRL, CA_DMA_AFT_PAD_SIZE,
		CA_DMA_AFT_PAD_EN |
		FIELD_PREP(CA_DMA_AFT_PAD_SIZE, CA_DMA_AFT_PAD_SIZE_VAL));

	/*
	 * FE bypass per VP; without it frames go into the forwarding engine and
	 * are dropped
	 */
	for (i = 0; i < CA_DMA_LSO_VP_COUNT; i++)
		dma_rmw(ni, CA_DMA_LSO_VP_HDRA_CFG(i),
			CA_DMA_LSO_HDRA_LDPID, CA_DMA_LSO_HDRA_FEBYPASS);

	return 0;
}

/*
 * program one VP/TXQ descriptor ring base and depth through the indirect window
 * (vendor aal_ni_set_dma_lso_base_depth_addr)
 */
static int cortina_ni_tx_ring_program(struct cortina_ni *ni, u8 vp, u8 txq,
				      dma_addr_t base)
{
	void __iomem *dma = dma_base(ni);
	u32 val;
	int ret;

	dev_dbg(ni->dev, "ring-program vp%u txq%u base=%pad\n", vp, txq, &base);
	if (WARN_ON(upper_32_bits(base) || (base & 0xf)))
		return -EINVAL;

	writel((lower_32_bits(base) & ~0xf) |
	       FIELD_PREP(CA_DMA_LSO_BD_DATA0_DEPTH, CA_NI_TX_RING_DEPTH),
	       dma + CA_DMA_LSO_VP_BD_DATA0(vp));
	/* addr[39:32] = 0: the ring sits below 4 GB */
	writel(0, dma + CA_DMA_LSO_VP_BD_DATA1(vp));
	writel(CA_DMA_LSO_BD_ACCESS_GO | CA_DMA_LSO_BD_ACCESS_WRITE |
	       FIELD_PREP(CA_DMA_LSO_BD_ACCESS_TXQ, txq),
	       dma + CA_DMA_LSO_VP_BD_ACCESS(vp));

	ret = readl_poll_timeout(dma + CA_DMA_LSO_VP_BD_ACCESS(vp), val,
				 !(val & CA_DMA_LSO_BD_ACCESS_GO),
				 CA_NI_TX_POLL_US, CA_NI_TX_POLL_TIMEOUT_US);
	if (ret)
		dev_err(ni->dev, "VP%u txq%u ring program timed out\n",
			vp, txq);
	return ret;
}

static int cortina_ni_tx_rings_init(struct cortina_ni *ni)
{
	struct cortina_ni_tx *tx = ni->tx;
	int i, ret;

	for (i = 0; i < CA_NI_TX_NUM_VPS; i++) {
		struct cortina_ni_txq *q = &tx->txq[i];
		u32 wptr, rptr;

		q->vp = CA_NI_TX_VP_BASE + i;
		q->txq = CA_NI_TX_TXQ;
		spin_lock_init(&q->lock);

		q->desc = dmam_alloc_coherent(ni->dev,
					      CA_NI_TX_RING_SIZE *
					      CA_NI_TX_DESC_WORDS * 4,
					      &q->desc_dma, GFP_KERNEL);
		if (!q->desc)
			return -ENOMEM;

		ret = cortina_ni_tx_ring_program(ni, q->vp, q->txq,
						 q->desc_dma);
		if (ret)
			return ret;

		/* adopt whatever pointer state the HW is in (0 after reset) */
		wptr = readl(dma_base(ni) +
			     CA_DMA_LSO_VP_TXQ_WPTR(q->vp, q->txq)) &
			CA_DMA_LSO_PTR_MASK;
		rptr = readl(dma_base(ni) +
			     CA_DMA_LSO_VP_TXQ_RPTR(q->vp, q->txq)) &
			CA_DMA_LSO_PTR_MASK;
		if (wptr >= CA_NI_TX_RING_SIZE || rptr >= CA_NI_TX_RING_SIZE ||
		    wptr != rptr)
			dev_warn(ni->dev,
				 "VP%u txq0 pointers not idle (w=%u r=%u)\n",
				 q->vp, wptr, rptr);
		q->wptr = wptr % CA_NI_TX_RING_SIZE;
		q->finished = rptr % CA_NI_TX_RING_SIZE;

		dev_dbg(ni->dev, "VP%u txq0 ring @%pad (%u desc)\n",
			q->vp, &q->desc_dma, CA_NI_TX_RING_SIZE);
	}
	return 0;
}

/*
 * QM buffer manager (vendor aal_l2_qm_init values); without buffers the egress
 * enqueue fails silently
 */
static void cortina_ni_tx_qm_init(struct cortina_ni *ni)
{
	/* EQ0 disabled/empty; EQ1 enabled, 4K x 64B pool + port private */
	ni_rmw(ni, CA_NI_L2TM_QM_EQ_CFG,
	       CA_NI_L2TM_EQ0_EN | CA_NI_L2TM_EQ0_BUFNUM |
	       CA_NI_L2TM_EQ0_PRVT | CA_NI_L2TM_EQ1_BUFNUM |
	       CA_NI_L2TM_EQ1_PRVT,
	       CA_NI_L2TM_EQ1_EN |
	       FIELD_PREP(CA_NI_L2TM_EQ1_BUFNUM, CA_NI_QM_EQ1_BUFNUM_VAL) |
	       FIELD_PREP(CA_NI_L2TM_EQ1_PRVT, CA_NI_QM_PORT_PRVT_BUFF_NUM));

	/* port-private buffer profile 0 (all ports select it by default) */
	ni_rmw(ni, CA_NI_L2TM_QM_PORT_PRVT_PROF0, 0x7fff,
	       CA_NI_QM_PORT_PRVT_BUFF_NUM);

	/* global buffer thresholds: drop on, no FE back-pressure */
	ni_rmw(ni, CA_NI_L2TM_QM_GLOB_BUF_CFG,
	       CA_NI_L2TM_BUF_NODROP | CA_NI_L2TM_BUF_NONCONG |
	       CA_NI_L2TM_BUF_FE_BP_EN,
	       CA_NI_L2TM_BUF_DROP_EN |
	       FIELD_PREP(CA_NI_L2TM_BUF_NODROP, CA_NI_QM_NODROP_THRESHOLD) |
	       FIELD_PREP(CA_NI_L2TM_BUF_NONCONG,
			  CA_NI_QM_NONCONG_THRESHOLD));
}

/*
 * TM egress scheduler (vendor aal_l2_tm_init): global, per-port and per-VoQ
 * enables; without them TX stalls silently
 */
static void cortina_ni_tx_tm_init(struct cortina_ni *ni)
{
	int i;

	ni_rmw(ni, CA_NI_L2TM_ES_CTRL, 0,
	       CA_NI_L2TM_ES_TX_EN | CA_NI_L2TM_ES_PORT_EN_ALL);

	for (i = 0; i < CA_NI_L2TM_ES_SCH_INSTANCES; i++)
		ni_rmw(ni, CA_NI_L2TM_ES_SCH_CFG(i), 0,
		       CA_NI_L2TM_ES_VOQ_EN_ALL);
}

/*
 * LAN port MACs: TX on for every LAN port (RX is enabled by the link path).
 * Every port must be enabled because the descriptor DEST can name any of them.
 */
static void cortina_ni_tx_port_mac_init(struct cortina_ni *ni)
{
	unsigned int p;

	/*
	 * connect the port MAC to the internal GPHY over GMII: int_cfg =
	 * GE_GMII, phy_mode = MAC, no loopback. The upper byte 0xcb000000 seen
	 * on the vendor firmware is read-only datapath status.
	 */
	for (p = 0; p < CA_NI_LAN_PORT_COUNT; p++) {
		ni_rmw(ni, CA_NI_PORT_STATIC_CFG(p),
		       CA_NI_PORT_STATIC_INT_CFG | CA_NI_PORT_STATIC_PHY_MODE |
		       CA_NI_PORT_STATIC_LPBK_MODE, 0);

		ni_rmw(ni, CA_NI_PORT_GLB_CFG(p),
		       CA_NI_PORT_GLB_PWR_DWN_TX, 0);

		ni_rmw(ni, CA_NI_PORT_TXMAC_CFG(p),
		       CA_NI_PORT_TXMAC_TX_DRAIN,
		       CA_NI_PORT_TXMAC_TX_EN | CA_NI_PORT_TXMAC_CRC_CALC_EN);
	}

	/*
	 * MAC autosync on (STS_ALL), as the vendor driver does: the MAC follows
	 * the PHY's speed and duplex in hardware
	 */
	ni_rmw(ni, CA_NI_HV_MAC_AUTOSYNC,
	       CA_NI_HV_AUTOSYNC_FC_ALL, CA_NI_HV_AUTOSYNC_STS_ALL);
}

/*
 * L2FE ARB ldpid->pdpid map. The "9th queue" ldpids 0x08..0x0f carry
 * CPU-injected US PON control frames and must route to the PON-OAM egress, or
 * OMCI never reaches the PUC. Index = my_mac[7] | dbuf[6] | ldpid[5:0].
 */
static int cortina_ni_arb_map_one(struct cortina_ni *ni, u32 idx, u32 pdpid)
{
	void __iomem *ni_r = ni_base(ni);
	u32 val;
	int ret;

	writel(pdpid, ni_r + CA_NI_L2FE_ARB_PDPID_DATA);
	writel(CA_DMA_LSO_BD_ACCESS_GO | CA_DMA_LSO_BD_ACCESS_WRITE | idx,
	       ni_r + CA_NI_L2FE_ARB_PDPID_ACCESS);
	ret = readl_poll_timeout(ni_r + CA_NI_L2FE_ARB_PDPID_ACCESS, val,
				 !(val & CA_DMA_LSO_BD_ACCESS_GO),
				 CA_NI_TX_POLL_US, CA_NI_TX_POLL_TIMEOUT_US);
	if (ret)
		dev_warn(ni->dev, "ARB map[0x%02x] timed out\n", idx);
	return ret;
}

/*
 * LAN NI ports 0-6: identity ldpid->pdpid so a direct-TX frame leaves the port
 * named in DEST (vendor aal_port.c global port init). Unmapped entries read 0,
 * which sends every frame to port 0. dbuf=1 rows go to the QM (US PON data);
 * ldpid 7 (PON) goes to the blackhole, as in the vendor map.
 */
static void cortina_ni_arb_lan_map_init(struct cortina_ni *ni)
{
	u32 my_mac, ldpid;

	for (my_mac = 0; my_mac <= 1; my_mac++) {
		for (ldpid = 0; ldpid <= 6; ldpid++) {
			if (cortina_ni_arb_map_one(ni, (my_mac << 7) | ldpid,
						   ldpid))
				return;
			if (cortina_ni_arb_map_one(ni,
						   (my_mac << 7) | BIT(6) | ldpid,
						   CA_NI_PPORT_QM))
				return;
		}
		if (cortina_ni_arb_map_one(ni, (my_mac << 7) | 7,
					   CA_NI_PPORT_BLACKHOLE))
			return;
		if (cortina_ni_arb_map_one(ni, (my_mac << 7) | BIT(6) | 7,
					   CA_NI_PPORT_BLACKHOLE))
			return;
	}
	dev_dbg(ni->dev,
		"L2FE ARB: LAN ldpids 0x00-0x06 -> identity pdpid (eth0 egress)\n");
}

static void cortina_ni_arb_oam_map_init(struct cortina_ni *ni)
{
	u32 my_mac, dbuf, ldpid, idx;

	for (my_mac = 0; my_mac <= 1; my_mac++) {
		for (dbuf = 0; dbuf <= 1; dbuf++) {
			/* 9th queue (control frame inject) -> OAM engine */
			for (ldpid = CA_NI_LDPID_9QUEUE_LO;
			     ldpid <= CA_NI_LDPID_9QUEUE_HI; ldpid++) {
				idx = (my_mac << 7) | (dbuf << 6) | ldpid;
				if (cortina_ni_arb_map_one(ni, idx,
							   CA_NI_PPORT_OAM))
					return;
			}
			/*
			 * CPU_MQ / GEM index (US PON data inject) -> QM, as the
			 * vendor maps all of 0x20..0x3f
			 */
			for (ldpid = CA_NI_LDPID_CPU_MQ_LO;
			     ldpid <= CA_NI_LDPID_CPU_MQ_HI; ldpid++) {
				idx = (my_mac << 7) | (dbuf << 6) | ldpid;
				if (cortina_ni_arb_map_one(ni, idx,
							   CA_NI_PPORT_QM))
					return;
			}
		}
	}
	dev_dbg(ni->dev,
		"L2FE ARB: ldpids 0x08-0x0f -> PON-OAM, 0x20-0x3f -> QM\n");
}

/*
 * Arm the TX DMA/LSO engine. Must run after cortina_ni_tx_rings_init(): an
 * engine enabled before the ring bases are programmed fetches from a garbage
 * address and raises an asynchronous SError.
 */
static void cortina_ni_tx_engine_enable(struct cortina_ni *ni)
{
	void __iomem *dma = dma_base(ni);

	writel(readl(dma + CA_DMA_SS_CTRL) | CA_DMA_SS_CTRL_TX_EN,
	       dma + CA_DMA_SS_CTRL);
	writel(CA_DMA_LSO_CTRL_VAL, dma + CA_DMA_LSO_CTRL);
	readl(dma + CA_DMA_LSO_CTRL);		/* the vendor driver reads back */
}

static int cortina_ni_tx_hw_init(struct cortina_ni *ni)
{
	int ret;

	/* vendor aal_ni_init order: reset, NI globals, TX DMA engine */
	cortina_ni_tx_reset_intf(ni);

	/* unconditional vendor globals (unknown names, vendor values) */
	ni_rmw(ni, CA_NI_HV_CFG_A420, CA_NI_HV_CFG_A420_FIELD,
	       FIELD_PREP(CA_NI_HV_CFG_A420_FIELD, CA_NI_HV_CFG_A420_VAL));
	ni_rmw(ni, CA_NI_HV_CFG_A1B8, CA_NI_HV_CFG_A1B8_FIELD,
	       FIELD_PREP(CA_NI_HV_CFG_A1B8_FIELD, CA_NI_HV_CFG_A1B8_VAL));
	/*
	 * The vendor also writes 0xaaf0 (NI_HV_MCE_CTL_REG); it does not decode
	 * on this SoC and the access takes a synchronous external abort.
	 */

	/* frame length limits, vendor values */
	ni_rmw(ni, CA_NI_HV_PKT_LEN,
	       CA_NI_HV_PKT_LEN_MIN | CA_NI_HV_PKT_LEN_MAX,
	       FIELD_PREP(CA_NI_HV_PKT_LEN_MIN, CA_NI_HV_PKT_LEN_MIN_VAL) |
	       FIELD_PREP(CA_NI_HV_PKT_LEN_MAX, CA_NI_HV_PKT_LEN_MAX_VAL));
	ni_rmw(ni, CA_NI_HV_PKT_LEN_RX, CA_NI_HV_PKT_LEN_RX_MAX,
	       FIELD_PREP(CA_NI_HV_PKT_LEN_RX_MAX, CA_NI_HV_PKT_LEN_MAX_VAL));

	/*
	 * 0xa1bc = INTERNAL_PORT_ID_CFG: keep the mirror bits [13:9] and clear
	 * the stray bit 15 U-Boot leaves (0xbe80, vendor 0x3e80). Bit 20
	 * (l3qmrx_to_lan, the NI->QM LAN handoff) must stay set.
	 */
	ni_rmw(ni, CA_NI_NI_INTERNAL_PORT_ID_CFG,
	       CA_NI_NI_INTERNAL_BIT15,
	       CA_NI_NI_MRR_CFG);

	/*
	 * Not ported, as direct TX does not need it: the 0xa01c port-to-CPU
	 * debug bits, the RX demux config (0xa180/88/8c), SCH-cfg [23:16] = 6
	 * on instances 8/10/13, aal_l2_te/l3_tm/l3_te init and the table
	 * clears.
	 */

	ret = cortina_ni_tx_engine_init(ni);
	if (ret)
		return ret;

	ret = cortina_ni_tx_rings_init(ni);
	if (ret)
		return ret;

	/* arm the engine only now that the rings are programmed */
	cortina_ni_tx_engine_enable(ni);

	cortina_ni_tx_qm_init(ni);
	cortina_ni_tx_tm_init(ni);
	cortina_ni_tx_port_mac_init(ni);
	cortina_ni_arb_oam_map_init(ni);	/* US PON control frame route */
	cortina_ni_arb_lan_map_init(ni);	/* LAN ldpid->pport route */
	return 0;
}

/* caller holds q->lock */
static unsigned int cortina_ni_tx_reclaim_q(struct cortina_ni *ni,
					    struct cortina_ni_txq *q)
{
	struct net_device *ndev = ni->tx->netdev;
	unsigned int freed = 0;
	u32 rptr;

	rptr = readl(dma_base(ni) +
		     CA_DMA_LSO_VP_TXQ_RPTR(q->vp, q->txq)) &
		CA_DMA_LSO_PTR_MASK;
	rptr %= CA_NI_TX_RING_SIZE;

	while (q->finished != rptr) {
		struct sk_buff *skb = q->slot[q->finished].skb;

		if (!skb) {
			u8 pon = q->slot[q->finished].pon;

			if (q->slot[q->finished].dup) {
				/*
				 * extra copy of a flooded frame: the mapping
				 * belongs to the last descriptor of the burst,
				 * which the engine always completes after this
				 * one
				 */
				q->slot[q->finished].dup = 0;
				q->finished = (q->finished + 1) %
					      CA_NI_TX_RING_SIZE;
				q->reclaimed++;
				freed++;
				continue;
			}
			if (!pon) {	/* must not happen: HW advanced past us */
				netdev_err(ndev, "VP%u: hole at %u (rptr %u)\n",
					   q->vp, q->finished, rptr);
				break;
			}
			/*
			 * PON control-frame descriptor: coherent scratch,
			 * nothing to unmap/free; the frame (EOF) descriptor
			 * releases its scratch slot (under this q->lock)
			 */
			if (pon >= 2)
				ni->tx->pon_busy &= ~BIT(pon - 2);
			q->slot[q->finished].pon = 0;
			q->finished = (q->finished + 1) % CA_NI_TX_RING_SIZE;
			q->reclaimed++;
			freed++;
			continue;
		}
		dma_unmap_single(ni->dev, q->slot[q->finished].addr,
				 q->slot[q->finished].len, DMA_TO_DEVICE);
		if (!q->slot[q->finished].pon) {
			ndev->stats.tx_packets++;
			ndev->stats.tx_bytes += q->slot[q->finished].len;
		} else {
			/*
			 * PON data skb (pon=1 with an skb): counted on the WAN
			 * netdev at enqueue, not on eth0
			 */
			q->slot[q->finished].pon = 0;
		}
		dev_consume_skb_any(skb);
		q->slot[q->finished].skb = NULL;
		q->finished = (q->finished + 1) % CA_NI_TX_RING_SIZE;
		q->reclaimed++;
		freed++;
	}
	return freed;
}

static unsigned int cortina_ni_txq_free_desc(struct cortina_ni_txq *q)
{
	if (q->wptr >= q->finished)
		return CA_NI_TX_RING_SIZE - q->wptr - 1 + q->finished;
	return q->finished - q->wptr - 1;
}

/*
 * Most a port may have queued on its emulated wire before the queue stops,
 * and how much may remain queued when it is woken again. The port buffer
 * holds about 160 us at 1G; waking early leaves the wake-up latency enough
 * slack that the wire rarely idles (measured at 1G: 130/60 785 Mbit/s,
 * 150/120 830, both without loss).
 */
#define CA_NI_TX_PACE_NS	150000
#define CA_NI_TX_PACE_WAKE_NS	120000

/* Per-frame wire time: frame + FCS + preamble + inter-frame gap */
#define CA_NI_TX_WIRE_OVERHEAD	24

static enum hrtimer_restart cortina_ni_tx_pace_timer(struct hrtimer *timer)
{
	struct cortina_ni_tx *tx = container_of(timer, struct cortina_ni_tx,
						pace_timer);

	netif_wake_queue(tx->netdev);
	return HRTIMER_NORESTART;
}

static void cortina_ni_tx_reclaim_timer(struct timer_list *t)
{
	struct cortina_ni_tx *tx = timer_container_of(tx, t, reclaim_timer);
	struct cortina_ni *ni = *(struct cortina_ni **)netdev_priv(tx->netdev);
	bool pending = false;
	unsigned int freed = 0;
	int i;

	for (i = 0; i < CA_NI_TX_NUM_VPS; i++) {
		struct cortina_ni_txq *q = &tx->txq[i];

		spin_lock_bh(&q->lock);
		freed += cortina_ni_tx_reclaim_q(ni, q);
		if (q->finished != q->wptr)
			pending = true;
		spin_unlock_bh(&q->lock);
	}

	if (freed && netif_queue_stopped(tx->netdev))
		netif_wake_queue(tx->netdev);
	if (pending)
		mod_timer(&tx->reclaim_timer,
			  jiffies + CA_NI_RECLAIM_INTERVAL);
}

/*
 * Ring the doorbell as the vendor driver does: clear the per-VP MISC_INTERRUPT
 * txq_empty bit of this queue between the descriptor stores and the write
 * pointer update. The barrier makes the descriptors visible before either
 * write.
 */
static void cortina_ni_tx_doorbell(struct cortina_ni *ni,
				   struct cortina_ni_txq *q)
{
	void __iomem *dma = dma_base(ni);

	dma_wmb();
	writel(ca_dma_lso_txq_empty_bit(q->txq),
	       dma + CA_DMA_LSO_VP_MISC_INT(q->vp));
	writel(q->wptr, dma + CA_DMA_LSO_VP_TXQ_WPTR(q->vp, q->txq));
}

/*
 * A frame longer than the 11-bit length of a direct-TX descriptor: the
 * header-mode pair the PON data path uses, SOF = a 16-byte block {LSO para0,
 * LSO para1, HEADER_A} and EOF = the frame, whose length field is 16 bits.
 * HEADER_A steers the frame to one LAN port (ldpid = the NI port) and skips
 * the forwarding engine. The block sits in front of the frame in the skb, so
 * one mapping covers both descriptors; stats are counted here, as for PON
 * data (pon = 1 on both slots).
 */
static netdev_tx_t cortina_ni_lan_jumbo_tx(struct cortina_ni *ni,
					   struct sk_buff *skb,
					   struct net_device *ndev,
					   unsigned int port)
{
	struct cortina_ni_tx *tx = ni->tx;
	struct cortina_ni_txq *q = &tx->txq[CA_NI_TX_ETH_RING];
	unsigned int len = skb->len;
	dma_addr_t daddr;
	__le32 *desc, *w;
	u32 lo, hi;

	if (len > FIELD_MAX(CA_NI_PON_HDRA_LO_PKT_SIZE) ||
	    skb_cow_head(skb, CA_NI_PON_HDR_BLK_LEN) || skb_linearize(skb)) {
		tx->drop_oversize++;
		goto drop;
	}

	skb_orphan(skb);
	w = skb_push(skb, CA_NI_PON_HDR_BLK_LEN);
	lo = FIELD_PREP(CA_NI_PON_HDRA_LO_COS, CA_NI_TX_COS) |
	     FIELD_PREP(CA_NI_PON_HDRA_LO_LDPID, port) |
	     FIELD_PREP(CA_NI_PON_HDRA_LO_LSPID, CA_NI_PON_TX_LSPID) |
	     FIELD_PREP(CA_NI_PON_HDRA_LO_PKT_SIZE, len) |
	     CA_NI_PON_HDRA_LO_FE_BYPASS;
	hi = CA_NI_PON_HDRA_HI_NO_DROP;
	w[0] = 0;
	w[1] = cpu_to_le32(len);
	w[2] = cpu_to_le32(hi);
	w[3] = cpu_to_le32(lo);

	daddr = dma_map_single(ni->dev, skb->data, skb->len, DMA_TO_DEVICE);
	if (dma_mapping_error(ni->dev, daddr)) {
		tx->drop_nomap++;
		goto drop;
	}
	WARN_ON_ONCE(upper_32_bits(daddr));

	spin_lock(&q->lock);
	if (cortina_ni_txq_free_desc(q) < CA_NI_TX_RESERVE_DESC + 2) {
		cortina_ni_tx_reclaim_q(ni, q);
		if (cortina_ni_txq_free_desc(q) < CA_NI_TX_RESERVE_DESC + 2) {
			tx->tx_busy++;
			netif_stop_queue(ndev);
			mod_timer(&tx->reclaim_timer,
				  jiffies + CA_NI_RECLAIM_INTERVAL);
			spin_unlock(&q->lock);
			dma_unmap_single(ni->dev, daddr, skb->len, DMA_TO_DEVICE);
			skb_pull(skb, CA_NI_PON_HDR_BLK_LEN);
			return NETDEV_TX_BUSY;
		}
	}

	desc = q->desc + q->wptr * CA_NI_TX_DESC_WORDS;
	desc[0] = cpu_to_le32(lower_32_bits(daddr));
	desc[1] = cpu_to_le32(CA_NI_TX_DESC1_SOF | CA_NI_TX_DESC1_HP0 |
			      FIELD_PREP(CA_NI_TX_DESC1_HDR_LEN,
					 CA_NI_PON_HDR_BLK_LEN));
	q->slot[q->wptr].skb = NULL;
	q->slot[q->wptr].addr = 0;
	q->slot[q->wptr].len = 0;
	q->slot[q->wptr].pon = 1;
	q->wptr = (q->wptr + 1) % CA_NI_TX_RING_SIZE;

	desc = q->desc + q->wptr * CA_NI_TX_DESC_WORDS;
	desc[0] = cpu_to_le32(lower_32_bits(daddr) + CA_NI_PON_HDR_BLK_LEN);
	desc[1] = cpu_to_le32(CA_NI_TX_DESC1_EOF |
			      FIELD_PREP(CA_NI_TX_DESC1_HDR_LEN, len));
	q->slot[q->wptr].skb = skb;
	q->slot[q->wptr].addr = daddr;
	q->slot[q->wptr].len = skb->len;
	q->slot[q->wptr].pon = 1;
	q->wptr = (q->wptr + 1) % CA_NI_TX_RING_SIZE;

	q->enq++;
	ndev->stats.tx_packets++;
	ndev->stats.tx_bytes += len;

	skb_tx_timestamp(skb);
	cortina_ni_tx_doorbell(ni, q);
	spin_unlock(&q->lock);

	mod_timer(&tx->reclaim_timer, jiffies + CA_NI_RECLAIM_INTERVAL);
	return NETDEV_TX_OK;

drop:
	ndev->stats.tx_dropped++;
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

static netdev_tx_t cortina_ni_start_xmit(struct sk_buff *skb,
					 struct net_device *ndev)
{
	struct cortina_ni *ni = *(struct cortina_ni **)netdev_priv(ndev);
	struct cortina_ni_tx *tx = ni->tx;
	struct cortina_ni_txq *q;
	dma_addr_t daddr;
	__le32 *desc;
	u32 word1, w = 0, ports;
	unsigned int len, first, nports, port;
	bool is_dsa = netdev_uses_dsa(ndev);
	u8 dsa_dest = 0;

	/*
	 * net/dsa/tag_cortina.c pushed a CA_NI_DSA_TAG_LEN prefix (MAGIC |
	 * port). Consume it before the padding below and use the port as DEST.
	 * A frame without the magic was sent untagged by the conduit itself
	 * (e.g. IPv6 DAD); it has no port, so drop it.
	 */
	if (is_dsa) {
		if (unlikely(skb->len < CA_NI_DSA_TAG_LEN ||
			     (skb->data[CA_NI_DSA_TAG_PORT_OFF] &
			      CA_NI_DSA_TAG_MAGIC_MASK) != CA_NI_DSA_TAG_MAGIC)) {
			ndev->stats.tx_dropped++;
			dev_kfree_skb_any(skb);
			return NETDEV_TX_OK;
		}
		dsa_dest = skb->data[CA_NI_DSA_TAG_PORT_OFF] &
			   CA_NI_DSA_TAG_PORT_MASK;

		/*
		 * Pace to the port's link speed. This must happen before the
		 * tag is consumed: a frame requeued without it is dropped.
		 */
		if (dsa_dest < CA_NI_DSA_USER_PORTS &&
		    READ_ONCE(tx->wire_ps_per_byte[dsa_dest])) {
			ktime_t now = ktime_get();
			ktime_t free = tx->wire_free[dsa_dest];
			s64 backlog;

			if (ktime_before(free, now))
				free = now;
			free = ktime_add_ns(free,
					    (u64)(max_t(unsigned int,
							skb->len - CA_NI_DSA_TAG_LEN,
							ETH_ZLEN) +
						  CA_NI_TX_WIRE_OVERHEAD) *
					    tx->wire_ps_per_byte[dsa_dest] / 1000);
			tx->wire_free[dsa_dest] = free;

			/*
			 * This frame goes out; stop the queue once the port
			 * has more than CA_NI_TX_PACE_NS queued.
			 */
			backlog = ktime_to_ns(ktime_sub(free, now));
			if (backlog > CA_NI_TX_PACE_NS) {
				tx->tx_paced++;
				netif_stop_queue(ndev);
				hrtimer_start(&tx->pace_timer,
					      ns_to_ktime(backlog -
							  CA_NI_TX_PACE_WAKE_NS),
					      HRTIMER_MODE_REL);
			}
		}

		skb_pull(skb, CA_NI_DSA_TAG_LEN);
	}

	/*
	 * short frames: pad to the wire minimum (also covers the engine's
	 * 34-byte DMA floor); skb freed by the helper on failure
	 */
	if (skb_padto(skb, ETH_ZLEN))
		return NETDEV_TX_OK;
	len = max_t(unsigned int, skb->len, ETH_ZLEN);

	if (unlikely(len > CA_NI_TX_MAX_FRAME)) {
		/* one egress port only: the bridge replicates floods per port */
		if (is_dsa && dsa_dest < CA_NI_DSA_USER_PORTS)
			return cortina_ni_lan_jumbo_tx(ni, skb, ndev, dsa_dest);
		tx->drop_oversize++;
		ndev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	/* one descriptor per frame, as the vendor direct TX does */
	if (unlikely(skb_linearize(skb))) {
		tx->drop_linearize++;
		ndev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	/*
	 * The ring is reclaimed lazily (ring nearly full or the reclaim timer),
	 * so a locally generated skb would stay charged to its socket's send
	 * buffer and make e.g. ICMP echo replies fail with ENOBUFS under load.
	 */
	skb_orphan(skb);

	/*
	 * Packet order: eth0 is a single-queue netdev, so it feeds exactly one
	 * ring. The engine fetches VP rings independently, so spreading one
	 * flow over several rings (the vendor's per-CPU VP choice) reorders it
	 * on the wire, which TCP treats as loss. The qdisc already serialises
	 * xmit, and q->lock only guards against the reclaim timer. eth0 has its
	 * own ring, leaving txq[0] to the PON paths.
	 */
	q = &tx->txq[CA_NI_TX_ETH_RING];

	/*
	 * Egress port set for this frame, computed before the ring check
	 * because a flood needs one descriptor per port. The qdisc serialises
	 * this single-queue netdev, so the plain counters need no atomics.
	 */
	/*
	 * With DSA each frame goes to exactly one port; the bridge replicates
	 * broadcast and unknown unicast per port itself.
	 */
	if (is_dsa)
		ports = BIT(dsa_dest);
	else
		ports = ca_ni_lan_tx_ports();
	/*
	 * An empty set would map the skb and attach it to no descriptor,
	 * leaking both.
	 */
	if (WARN_ONCE(!ports, "lan_tx: empty egress port set\n"))
		ports = BIT(CA_NI_TX_PORT);
	nports = hweight32(ports);

	spin_lock(&q->lock);

	/* reclaim, then check for room (keeping CA_NI_TX_RESERVE_DESC spare) */
	if (cortina_ni_txq_free_desc(q) < CA_NI_TX_RESERVE_DESC + nports) {
		cortina_ni_tx_reclaim_q(ni, q);
		if (cortina_ni_txq_free_desc(q) <
		    CA_NI_TX_RESERVE_DESC + nports) {
			tx->tx_busy++;
			netif_stop_queue(ndev);
			mod_timer(&tx->reclaim_timer,
				  jiffies + CA_NI_RECLAIM_INTERVAL);
			spin_unlock(&q->lock);
			return NETDEV_TX_BUSY;
		}
	}

	daddr = dma_map_single(ni->dev, skb->data, len, DMA_TO_DEVICE);
	if (unlikely(dma_mapping_error(ni->dev, daddr))) {
		tx->drop_nomap++;
		ndev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		spin_unlock(&q->lock);
		return NETDEV_TX_OK;
	}
	/*
	 * the engine takes a 32-bit buffer address (plus the CCI selector);
	 * both DDR pools sit below 4 GB and the DMA mask enforces it
	 */
	WARN_ON_ONCE(upper_32_bits(daddr));

	/*
	 * One direct-TX descriptor per egress port (plain frame, HP=11), all
	 * pointing at the same buffer. The engine consumes the ring in order,
	 * so the last descriptor of the burst owns the skb and the mapping and
	 * the earlier ones are marked dup. TX stats count the frame once.
	 */
	word1 = CA_NI_TX_DESC1_HP1 | CA_NI_TX_DESC1_HP0 |
		CA_NI_TX_DESC1_MODE_DIRECT |
		FIELD_PREP(CA_NI_TX_DESC1_CHK_SEL, CA_NI_TX_CHK_AUTO) |
		FIELD_PREP(CA_NI_TX_DESC1_LEN, len) |
		FIELD_PREP(CA_NI_TX_DESC1_COS, CA_NI_TX_COS);

	first = q->wptr;
	for (port = 0; ports; port++) {
		bool last;

		if (!(ports & BIT(port)))
			continue;
		ports &= ~BIT(port);
		last = !ports;
		w = word1 | FIELD_PREP(CA_NI_TX_DESC1_DEST, port);

		desc = q->desc + q->wptr * CA_NI_TX_DESC_WORDS;
		desc[0] = cpu_to_le32(lower_32_bits(daddr));
		desc[1] = cpu_to_le32(w);

		q->slot[q->wptr].skb = last ? skb : NULL;
		q->slot[q->wptr].addr = last ? daddr : 0;
		q->slot[q->wptr].len = last ? len : 0;
		q->slot[q->wptr].dup = last ? 0 : 1;
		q->wptr = (q->wptr + 1) % CA_NI_TX_RING_SIZE;
		if (!last)
			tx->lan_dup++;
	}
	q->enq++;

	/*
	 * timestamp before the doorbell: after it the reclaim path may free the
	 * skb
	 */
	skb_tx_timestamp(skb);

	cortina_ni_tx_doorbell(ni, q);

	spin_unlock(&q->lock);

	mod_timer(&tx->reclaim_timer, jiffies + CA_NI_RECLAIM_INTERVAL);
	return NETDEV_TX_OK;
}

/* set once at TX probe; the GPON driver's responder calls in through it */
static struct cortina_ni *cortina_ni_pon_tx_ni;

/*
 * 16-byte PON control-frame header (vendor ca_ni_tx_encap_pon_control_packet):
 * fixed DA/SA, link type 0xfff1 = OMCI, byte 14 = 0, byte 15 = (cos > 6), which
 * is 1 for OMCI.
 */
static const u8 cortina_ni_pon_hdr[CA_NI_PON_HDR_LEN] = {
	0x00, 0x13, 0x25, 0x00, 0x00, 0x00,	/* DA */
	0x00, 0x13, 0x25, 0x00, 0x00, 0x01,	/* SA */
	0xff, 0xf1, 0x00, 0x01,			/* OMCI link type, cos > 6 flag */
};

int cortina_ni_pon_tx(const u8 *pdu, unsigned int len)
{
	struct cortina_ni *ni = READ_ONCE(cortina_ni_pon_tx_ni);
	struct cortina_ni_tx *tx;
	struct cortina_ni_txq *q;
	unsigned int frame_len, slot;
	unsigned long free_slots;
	dma_addr_t blk_dma;
	__le32 *desc, *w;
	u32 lo, hi;
	u8 *blk;

	if (!ni || !ni->tx || !ni->tx->pon_buf)
		return -ENODEV;
	tx = ni->tx;
	if (!len || len > CA_NI_PON_TX_PDU_MAX)
		return -EINVAL;
	frame_len = CA_NI_PON_HDR_LEN + len;	/* 64 for a 48B OMCI PDU */

	/*
	 * always txq[0]: pon_busy and the scratch are guarded by its lock; _bh
	 * so the responder may call from NAPI softirq or process context
	 */
	q = &tx->txq[0];
	spin_lock_bh(&q->lock);

	/*
	 * Reclaim on every send: the scratch has far fewer slots than the ring,
	 * and during an OMCI burst (MIB upload, ~1 message per 25 ms) it would
	 * run out long before the ring. A dropped reply desyncs the stateful
	 * MIB upload and the OLT aborts it.
	 */
	cortina_ni_tx_reclaim_q(ni, q);
	/*
	 * Allocate the scratch slot from a masked unsigned long: ffz() of a
	 * full u32 bitmap is undefined and could index past the scratch. A full
	 * scratch must give -EBUSY.
	 */
	free_slots = ~(unsigned long)tx->pon_busy &
		     GENMASK(CA_NI_PON_TX_SLOTS - 1, 0);
	if (cortina_ni_txq_free_desc(q) <= CA_NI_TX_RESERVE_DESC + 2 ||
	    !free_slots) {
		tx->pon_fail++;
		spin_unlock_bh(&q->lock);
		return -EBUSY;		/* caller drops; the OLT retransmits */
	}
	slot = __ffs(free_slots);
	tx->pon_busy |= BIT(slot);

	blk = (u8 *)tx->pon_buf + slot * CA_NI_PON_TX_SLOT_SZ;
	blk_dma = tx->pon_buf_dma + slot * CA_NI_PON_TX_SLOT_SZ;

	/* frame = 16-byte PON header + the OMCI PDU (coherent, no mapping) */
	memcpy(blk + CA_NI_PON_TX_FRAME_OFF, cortina_ni_pon_hdr, CA_NI_PON_HDR_LEN);
	memcpy(blk + CA_NI_PON_TX_FRAME_OFF + CA_NI_PON_HDR_LEN, pdu, len);

	/*
	 * Header block {LSO para0 = 0, LSO para1 = pkt_size, HEADER_A}, stored
	 * little-endian as the vendor driver does. The word at +8 is the
	 * pkt_info half (no_drop/pol_id/cpu_flg, bits 32..63), the word at +12
	 * the cos/ldpid/lspid/pkt_size half (bits 0..31). pol_en stays 0 for
	 * OMCI.
	 */
	lo = FIELD_PREP(CA_NI_PON_HDRA_LO_COS,
			CA_NI_PON_COS) |
	     FIELD_PREP(CA_NI_PON_HDRA_LO_LDPID, CA_NI_PON_LDPID) |
	     FIELD_PREP(CA_NI_PON_HDRA_LO_LSPID, CA_NI_PON_TX_LSPID) |
	     FIELD_PREP(CA_NI_PON_HDRA_LO_PKT_SIZE, frame_len) |
	     CA_NI_PON_HDRA_LO_FE_BYPASS;
	hi = CA_NI_PON_HDRA_HI_NO_DROP |
	     FIELD_PREP(CA_NI_PON_HDRA_HI_POL_ID, CA_NI_PON_POL_ID);
	w = (__le32 *)blk;
	w[0] = 0;
	w[1] = cpu_to_le32(frame_len);
	w[2] = cpu_to_le32(hi);		/* +8: pkt_info word */
	w[3] = cpu_to_le32(lo);		/* +12: cos/ldpid/pkt_size word */

	/* descriptor pair: SOF + HP=01 header block, then the EOF frame */
	desc = q->desc + q->wptr * CA_NI_TX_DESC_WORDS;
	desc[0] = cpu_to_le32(lower_32_bits(blk_dma));
	desc[1] = cpu_to_le32(CA_NI_TX_DESC1_SOF | CA_NI_TX_DESC1_HP0 |
			      FIELD_PREP(CA_NI_TX_DESC1_HDR_LEN,
					 CA_NI_PON_HDR_BLK_LEN));
	q->slot[q->wptr].skb = NULL;
	q->slot[q->wptr].addr = 0;
	q->slot[q->wptr].len = 0;
	q->slot[q->wptr].pon = 1;
	q->wptr = (q->wptr + 1) % CA_NI_TX_RING_SIZE;

	desc = q->desc + q->wptr * CA_NI_TX_DESC_WORDS;
	desc[0] = cpu_to_le32(lower_32_bits(blk_dma + CA_NI_PON_TX_FRAME_OFF));
	desc[1] = cpu_to_le32(CA_NI_TX_DESC1_EOF |
			      FIELD_PREP(CA_NI_TX_DESC1_HDR_LEN, frame_len));
	q->slot[q->wptr].skb = NULL;
	q->slot[q->wptr].addr = 0;
	q->slot[q->wptr].len = 0;
	q->slot[q->wptr].pon = 2 + slot;
	q->wptr = (q->wptr + 1) % CA_NI_TX_RING_SIZE;

	q->enq++;
	tx->pon_enq++;

	cortina_ni_tx_doorbell(ni, q);

	spin_unlock_bh(&q->lock);

	mod_timer(&tx->reclaim_timer, jiffies + CA_NI_RECLAIM_INTERVAL);
	return 0;
}
EXPORT_SYMBOL_GPL(cortina_ni_pon_tx);

int cortina_ni_pon_oam_tx(struct sk_buff *skb, u8 llid_idx)
{
	struct cortina_ni *ni = READ_ONCE(cortina_ni_pon_tx_ni);
	struct cortina_ni_tx *tx;
	struct cortina_ni_txq *q;
	unsigned int len, slot;
	unsigned long free_slots;
	dma_addr_t daddr, blk_dma;
	__le32 *desc, *w;
	u32 lo, hi;
	u8 *hdr, *blk;

	if (!ni || !ni->tx || !ni->tx->pon_buf)
		return -ENODEV;
	tx = ni->tx;

	if (skb->len < ETH_HLEN + 4 ||
	    skb->len + CA_NI_PON_HDR_LEN > CA_NI_TX_MAX_FRAME)
		return -EINVAL;
	if (skb_padto(skb, ETH_ZLEN))
		return -ENOMEM;
	if (skb_cow_head(skb, CA_NI_PON_HDR_LEN) || skb_linearize(skb))
		return -ENOMEM;

	hdr = skb_push(skb, CA_NI_PON_HDR_LEN);
	memcpy(hdr, cortina_ni_pon_hdr, CA_NI_PON_HDR_LEN);
	hdr[5] = llid_idx & 0x3f;
	hdr[15] = 0x00;
	len = skb->len;

	q = &tx->txq[0];
	spin_lock_bh(&q->lock);

	cortina_ni_tx_reclaim_q(ni, q);
	free_slots = ~(unsigned long)tx->pon_busy &
		     GENMASK(CA_NI_PON_TX_SLOTS - 1, 0);
	if (cortina_ni_txq_free_desc(q) <= CA_NI_TX_RESERVE_DESC + 2 ||
	    !free_slots) {
		tx->pon_fail++;
		spin_unlock_bh(&q->lock);
		skb_pull(skb, CA_NI_PON_HDR_LEN);
		return -EBUSY;
	}
	slot = __ffs(free_slots);

	daddr = dma_map_single(ni->dev, skb->data, len, DMA_TO_DEVICE);
	if (unlikely(dma_mapping_error(ni->dev, daddr))) {
		tx->drop_nomap++;
		spin_unlock_bh(&q->lock);
		skb_pull(skb, CA_NI_PON_HDR_LEN);
		return -EIO;
	}
	WARN_ON_ONCE(upper_32_bits(daddr));
	tx->pon_busy |= BIT(slot);

	blk = (u8 *)tx->pon_buf + slot * CA_NI_PON_TX_SLOT_SZ;
	blk_dma = tx->pon_buf_dma + slot * CA_NI_PON_TX_SLOT_SZ;

	lo = FIELD_PREP(CA_NI_PON_HDRA_LO_COS, CA_NI_PON_COS) |
	     FIELD_PREP(CA_NI_PON_HDRA_LO_LDPID, CA_NI_PON_LDPID) |
	     FIELD_PREP(CA_NI_PON_HDRA_LO_LSPID, CA_NI_PON_TX_LSPID) |
	     FIELD_PREP(CA_NI_PON_HDRA_LO_PKT_SIZE, len) |
	     CA_NI_PON_HDRA_LO_FE_BYPASS;
	hi = CA_NI_PON_HDRA_HI_NO_DROP |
	     FIELD_PREP(CA_NI_PON_HDRA_HI_POL_ID, CA_NI_PON_POL_ID);
	w = (__le32 *)blk;
	w[0] = 0;
	w[1] = cpu_to_le32(len);
	w[2] = cpu_to_le32(hi);
	w[3] = cpu_to_le32(lo);

	desc = q->desc + q->wptr * CA_NI_TX_DESC_WORDS;
	desc[0] = cpu_to_le32(lower_32_bits(blk_dma));
	desc[1] = cpu_to_le32(CA_NI_TX_DESC1_SOF | CA_NI_TX_DESC1_HP0 |
			      FIELD_PREP(CA_NI_TX_DESC1_HDR_LEN,
					 CA_NI_PON_HDR_BLK_LEN));
	q->slot[q->wptr].skb = NULL;
	q->slot[q->wptr].addr = 0;
	q->slot[q->wptr].len = 0;
	q->slot[q->wptr].pon = 2 + slot;
	q->wptr = (q->wptr + 1) % CA_NI_TX_RING_SIZE;

	desc = q->desc + q->wptr * CA_NI_TX_DESC_WORDS;
	desc[0] = cpu_to_le32(lower_32_bits(daddr));
	desc[1] = cpu_to_le32(CA_NI_TX_DESC1_EOF |
			      FIELD_PREP(CA_NI_TX_DESC1_HDR_LEN, len));
	q->slot[q->wptr].skb = skb;
	q->slot[q->wptr].addr = daddr;
	q->slot[q->wptr].len = len;
	q->slot[q->wptr].pon = 1;
	q->wptr = (q->wptr + 1) % CA_NI_TX_RING_SIZE;

	q->enq++;
	tx->pon_enq++;

	skb_tx_timestamp(skb);
	cortina_ni_tx_doorbell(ni, q);

	spin_unlock_bh(&q->lock);

	mod_timer(&tx->reclaim_timer, jiffies + CA_NI_RECLAIM_INTERVAL);
	return 0;
}
EXPORT_SYMBOL_GPL(cortina_ni_pon_oam_tx);

/*
 * US PON data (WAN) TX: the same 2-descriptor HEADER_A chain as the OMCI path,
 * but the frame is the mapped skb and HEADER_A is a data header (ldpid = PON
 * data, data cos, no_drop). The SOF slot releases the header block scratch (pon
 * = 2 + slot), the EOF slot carries the skb.
 */
/*
 * VoQ admission (vendor dal_rtl9607f_ponmac_queue_add): point a data VoQ at
 * TE_CB threshold profile 3. Without it frames are never enqueued and nothing
 * counts a drop. RTL9607DQ addresses: | SELECT_MEM ACCESS 0x95c0 / DATA 0x95c4
 * | PROFILE_MEM ACCESS 0x95b0 / DATA0..2 0x95bc, 0x95b8, 0x95b4
 */
#define CA_NI_TECB_SEL_ACCESS	0x95c0
#define CA_NI_TECB_SEL_DATA	0x95c4
#define CA_NI_TECB_PROF_ACCESS	0x95b0
#define CA_NI_TECB_PROF_DATA0	0x95bc
#define CA_NI_TECB_PROF_DATA1	0x95b8
#define CA_NI_TECB_PROF_DATA2	0x95b4
#define CA_NI_TECB_ADMIT_PROFILE 3

int cortina_ni_qm_voq_admit(unsigned int qm_voq_idx)
{
	struct cortina_ni *ni = READ_ONCE(cortina_ni_pon_tx_ni);
	void __iomem *b;

	if (!ni)
		return -ENODEV;
	b = ni_base(ni);
	/* set profile 3 wide open, as the vendor default; idempotent */
	writel(0xffffffff, b + CA_NI_TECB_PROF_DATA0);
	writel(0xffffffff, b + CA_NI_TECB_PROF_DATA1);
	writel(0xffffffff, b + CA_NI_TECB_PROF_DATA2);
	writel(0xc0000000 | CA_NI_TECB_ADMIT_PROFILE, b + CA_NI_TECB_PROF_ACCESS);
	/* point this VoQ at it */
	writel(CA_NI_TECB_ADMIT_PROFILE, b + CA_NI_TECB_SEL_DATA);
	writel(0xc0000000 | (qm_voq_idx & 0xff), b + CA_NI_TECB_SEL_ACCESS);
	return 0;
}
EXPORT_SYMBOL_GPL(cortina_ni_qm_voq_admit);

netdev_tx_t cortina_ni_pon_data_tx(struct sk_buff *skb,
				   struct net_device *ndev, u16 voq)
{
	struct cortina_ni *ni = READ_ONCE(cortina_ni_pon_tx_ni);
	struct cortina_ni_tx *tx;
	struct cortina_ni_txq *q;
	unsigned int len, slot;
	unsigned long free_slots;
	dma_addr_t blk_dma, daddr;
	__le32 *desc, *w;
	u32 lo, hi;
	u8 *blk;

	if (!ni || !ni->tx || !ni->tx->pon_buf) {
		ndev->stats.tx_errors++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}
	tx = ni->tx;

	if (skb_padto(skb, ETH_ZLEN))	/* freed by the helper on failure */
		return NETDEV_TX_OK;
	len = max_t(unsigned int, skb->len, ETH_ZLEN);
	if (unlikely(len > CA_NI_TX_MAX_FRAME || skb_linearize(skb))) {
		ndev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	/*
	 * txq[0], shared with the OMCI path: pon_busy and the scratch live
	 * under its lock (ndo_start_xmit runs with BH off, so plain spin_lock)
	 */
	q = &tx->txq[0];
	spin_lock(&q->lock);

	cortina_ni_tx_reclaim_q(ni, q);	/* reclaim every send, see the OMCI path */
	/* same masked allocation as the OMCI path */
	free_slots = ~(unsigned long)tx->pon_busy &
		     GENMASK(CA_NI_PON_TX_SLOTS - 1, 0);
	if (cortina_ni_txq_free_desc(q) <= CA_NI_TX_RESERVE_DESC + 2 ||
	    !free_slots) {
		tx->pon_fail++;
		ndev->stats.tx_dropped++;
		spin_unlock(&q->lock);
		dev_kfree_skb_any(skb);	/* WAN clients retransmit (DHCP/TCP) */
		return NETDEV_TX_OK;
	}
	slot = __ffs(free_slots);

	daddr = dma_map_single(ni->dev, skb->data, len, DMA_TO_DEVICE);
	if (unlikely(dma_mapping_error(ni->dev, daddr))) {
		tx->drop_nomap++;
		ndev->stats.tx_dropped++;
		spin_unlock(&q->lock);
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}
	WARN_ON_ONCE(upper_32_bits(daddr));
	tx->pon_busy |= BIT(slot);

	blk = (u8 *)tx->pon_buf + slot * CA_NI_PON_TX_SLOT_SZ;
	blk_dma = tx->pon_buf_dma + slot * CA_NI_PON_TX_SLOT_SZ;

	/*
	 * header block {LSO para0 = 0, LSO para1 = pkt_size, HEADER_A} in the
	 * same word order as the OMCI path
	 */
	lo = FIELD_PREP(CA_NI_PON_HDRA_LO_COS, voq % 8) |
	     FIELD_PREP(CA_NI_PON_HDRA_LO_LDPID, CA_NI_PON_DATA_LDPID(voq / 8)) |
	     FIELD_PREP(CA_NI_PON_HDRA_LO_LSPID, CA_NI_PON_TX_LSPID) |
	     FIELD_PREP(CA_NI_PON_HDRA_LO_PKT_SIZE, len) |
	     CA_NI_PON_HDRA_LO_FE_BYPASS;
	hi = CA_NI_PON_HDRA_HI_NO_DROP |
	     FIELD_PREP(CA_NI_PON_HDRA_HI_POL_EN, 1) |
	     FIELD_PREP(CA_NI_PON_HDRA_HI_POL_ID, voq);
	w = (__le32 *)blk;
	w[0] = 0;
	w[1] = cpu_to_le32(len);
	w[2] = cpu_to_le32(hi);
	w[3] = cpu_to_le32(lo);

	/* SOF + HP=01 header block (releases scratch slot on reclaim) */
	desc = q->desc + q->wptr * CA_NI_TX_DESC_WORDS;
	desc[0] = cpu_to_le32(lower_32_bits(blk_dma));
	desc[1] = cpu_to_le32(CA_NI_TX_DESC1_SOF | CA_NI_TX_DESC1_HP0 |
			      FIELD_PREP(CA_NI_TX_DESC1_HDR_LEN,
					 CA_NI_PON_HDR_BLK_LEN));
	q->slot[q->wptr].skb = NULL;
	q->slot[q->wptr].addr = 0;
	q->slot[q->wptr].len = 0;
	q->slot[q->wptr].pon = 2 + slot;
	q->wptr = (q->wptr + 1) % CA_NI_TX_RING_SIZE;

	/*
	 * EOF = the frame (pon=1 with an skb: stats are counted here, not on
	 * eth0)
	 */
	desc = q->desc + q->wptr * CA_NI_TX_DESC_WORDS;
	desc[0] = cpu_to_le32(lower_32_bits(daddr));
	desc[1] = cpu_to_le32(CA_NI_TX_DESC1_EOF |
			      FIELD_PREP(CA_NI_TX_DESC1_HDR_LEN, len));
	q->slot[q->wptr].skb = skb;
	q->slot[q->wptr].addr = daddr;
	q->slot[q->wptr].len = len;
	q->slot[q->wptr].pon = 1;
	q->wptr = (q->wptr + 1) % CA_NI_TX_RING_SIZE;

	q->enq++;
	tx->pon_data_enq++;
	ndev->stats.tx_packets++;
	ndev->stats.tx_bytes += len;

	skb_tx_timestamp(skb);	/* before the doorbell */

	cortina_ni_tx_doorbell(ni, q);

	spin_unlock(&q->lock);

	mod_timer(&tx->reclaim_timer, jiffies + CA_NI_RECLAIM_INTERVAL);
	return NETDEV_TX_OK;
}
EXPORT_SYMBOL_GPL(cortina_ni_pon_data_tx);

/* one gratuitous ARP at link-up, through the normal xmit path */
static void cortina_ni_tx_announce(struct work_struct *work)
{
	struct cortina_ni_tx *tx =
		container_of(work, struct cortina_ni_tx, announce_work);
	struct net_device *ndev = tx->netdev;
	struct sk_buff *skb;

	/*
	 * With DSA eth0 is the conduit: it has no address, and an untagged
	 * frame sent on it would have its first two bytes taken as the DSA tag.
	 */
	if (netdev_uses_dsa(ndev))
		return;

	skb = arp_create(ARPOP_REQUEST, ETH_P_ARP, 0, ndev, 0,
			 NULL, ndev->dev_addr, NULL);
	if (!skb)
		return;
	netdev_dbg(ndev, "sending link-up gratuitous ARP\n");
	dev_queue_xmit(skb);
}

static int cortina_ni_open(struct net_device *ndev)
{
	struct cortina_ni *ni = *(struct cortina_ni **)netdev_priv(ndev);

	/*
	 * eth0 is the DSA conduit: it has no PHY of its own; the user ports
	 * report their link through phylink
	 */
	netif_carrier_on(ndev);
	cortina_ni_rx_open(ni);	/* NAPI + RX IRQ + port RXMAC on */
	netif_start_queue(ndev);
	return 0;
}

static int cortina_ni_stop(struct net_device *ndev)
{
	struct cortina_ni *ni = *(struct cortina_ni **)netdev_priv(ndev);
	struct cortina_ni_tx *tx = ni->tx;
	int i;

	netif_stop_queue(ndev);
	cortina_ni_rx_stop(ni);	/* RXMAC off, IRQ masked, NAPI off */
	if (tx->phydev) {
		phy_stop(tx->phydev);
		phy_disconnect(tx->phydev);
		tx->phydev = NULL;
	}
	cancel_work_sync(&tx->announce_work);
	timer_delete_sync(&tx->reclaim_timer);
	hrtimer_cancel(&tx->pace_timer);

	/*
	 * reclaim whatever completed; anything still in flight stays mapped
	 * until the engine drains it (the rings persist across open/stop)
	 */
	for (i = 0; i < CA_NI_TX_NUM_VPS; i++) {
		struct cortina_ni_txq *q = &tx->txq[i];

		spin_lock_bh(&q->lock);
		cortina_ni_tx_reclaim_q(ni, q);
		if (q->finished != q->wptr)
			netdev_warn(ndev, "VP%u: %u frames still in flight\n",
				    q->vp,
				    (q->wptr + CA_NI_TX_RING_SIZE -
				     q->finished) % CA_NI_TX_RING_SIZE);
		spin_unlock_bh(&q->lock);
	}
	return 0;
}

/*
 * Commit a new MAC, then re-key the MAC-keyed tables (L2FE FDB, my-MAC
 * comparator, PP FIELD-CAM, offload router MAC) from it. netifd applies the
 * factory MAC after the last link-up re-arm, so without this the tables would
 * keep the boot MAC. Only the FDB is re-keyed unless the flow engine is active.
 */
static int cortina_ni_set_mac_address(struct net_device *ndev, void *addr)
{
	struct cortina_ni *ni = *(struct cortina_ni **)netdev_priv(ndev);
	int ret = eth_mac_addr(ndev, addr);

	if (ret)
		return ret;
	cortina_ni_rx_mac_rearm(ni);
	return 0;
}

static const struct net_device_ops cortina_ni_netdev_ops = {
	.ndo_open		= cortina_ni_open,
	.ndo_stop		= cortina_ni_stop,
	.ndo_start_xmit		= cortina_ni_start_xmit,
	.ndo_set_mac_address	= cortina_ni_set_mac_address,
	.ndo_validate_addr	= eth_validate_addr,
#if IS_ENABLED(CONFIG_CORTINA_NI_FLOWOFFLOAD)
	/* L3FE flow engine nf_flow_table offload (cortina-ppe.c) */
	.ndo_setup_tc		= cortina_ppe_setup_tc,
#endif
};
static void cortina_ni_tx_set_mac(struct cortina_ni *ni,
				  struct net_device *ndev)
{
	/*
	 * Without a DT MAC, the factory MAC comes from board.d/02_network
	 * through netifd; ndo_set_mac_address then re-keys the MAC-keyed tables.
	 */
	if (is_valid_ether_addr(ni->mac))
		eth_hw_addr_set(ndev, ni->mac);
	else
		eth_hw_addr_random(ndev);
}

int cortina_ni_tx_probe(struct cortina_ni *ni)
{
	struct net_device *ndev;
	struct cortina_ni_tx *tx;
	struct cortina_ni **priv;
	int ret;

	if (!ni->win[CA_NI_WIN_DMA])
		return dev_err_probe(ni->dev, -ENODEV,
				     "DMA-LSO window not mapped, no TX\n");

	/* the engine hands 32-bit buffer addresses to the DMA */
	ret = dma_set_mask_and_coherent(ni->dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(ni->dev, ret, "no 32-bit DMA\n");

	ndev = devm_alloc_etherdev(ni->dev, sizeof(struct cortina_ni *));
	if (!ndev)
		return -ENOMEM;
	SET_NETDEV_DEV(ndev, ni->dev);
	priv = netdev_priv(ndev);
	*priv = ni;

	tx = devm_kzalloc(ni->dev, sizeof(*tx), GFP_KERNEL);
	if (!tx)
		return -ENOMEM;
	tx->netdev = ndev;
	ni->tx = tx;

	timer_setup(&tx->reclaim_timer, cortina_ni_tx_reclaim_timer, 0);
	hrtimer_setup(&tx->pace_timer, cortina_ni_tx_pace_timer, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL);
	INIT_WORK(&tx->announce_work, cortina_ni_tx_announce);

	/*
	 * US PON control-frame (OMCI) TX scratch; without it
	 * cortina_ni_pon_tx() returns -ENODEV
	 */
	tx->pon_buf = dmam_alloc_coherent(ni->dev,
					  CA_NI_PON_TX_SLOTS *
					  CA_NI_PON_TX_SLOT_SZ,
					  &tx->pon_buf_dma, GFP_KERNEL);
	if (!tx->pon_buf)
		dev_warn(ni->dev, "no PON TX scratch - US OMCI TX disabled\n");

	ret = cortina_ni_tx_hw_init(ni);
	if (ret)
		return ret;

	/*
	 * start the sampler right after the rings are programmed, so the first
	 * sample is the post-probe baseline
	 */

	ndev->netdev_ops = &cortina_ni_netdev_ops;
	/* ethtool -S / -d (cortina-ni-ethtool.c) */
	ndev->ethtool_ops = &cortina_ni_ethtool_ops;
	ndev->min_mtu = ETH_MIN_MTU;
	/*
	 * One TX descriptor carries at most CA_NI_TX_MAX_FRAME bytes. With DSA
	 * the conduit carries the tag in front of the frame and strips it
	 * before DMA, so it may take CA_NI_DSA_TAG_LEN more than a user port.
	 */
	ndev->max_mtu = CA_NI_MAX_MTU + CA_NI_DSA_TAG_LEN;
	ndev->hw_features |= NETIF_F_RXCSUM;
	ndev->features |= NETIF_F_RXCSUM;
	cortina_ni_tx_set_mac(ni, ndev);

	/*
	 * DSA: bind the netdev to the "ethernet@0" child so the CPU port's
	 * "ethernet" phandle resolves to it. Must be set before register_netdev
	 * and cortina_ni_dsa_register().
	 */
	ndev->dev.of_node = of_get_child_by_name(ni->dev->of_node, "ethernet");

	/*
	 * One RX interrupt feeds every frame through a single NAPI instance;
	 * run it in a kthread the scheduler can move off the interrupt CPU.
	 */
	netif_threaded_enable(ndev);
	ret = devm_register_netdev(ni->dev, ndev);
	if (ret)
		return dev_err_probe(ni->dev, ret, "register_netdev failed\n");

	WRITE_ONCE(cortina_ni_pon_tx_ni, ni);	/* open the PON TX entry points */
	dev_dbg(ni->dev, "TX ready: %s -> LAN ports 0..%u (direct-TX)\n",
		ndev->name, CA_NI_LAN_PORT_COUNT - 1);
	return 0;
}
