/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2015 Cortina-Access Limited
 * Copyright (C) BRULE Herman <alpha_one_x86@first-world.info>
 *
 * Cortina NI Ethernet driver for the Realtek RTL9607DQ: declarations shared
 * between the core, RX, TX, ethtool, DSA and flow offload parts.
 */

#ifndef _CORTINA_NI_H
#define _CORTINA_NI_H

#include <linux/hrtimer.h>
#include <linux/netdevice.h>
#include <linux/phylink.h>
#include <linux/phy.h>
#include <linux/regmap.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include "cortina-ni-regs.h"

/*
 * platform driver name; also used by "ethtool -i" and as the debugfs directory
 * name
 */
#define CA_NI_DRV_NAME		"cortina-ni"


struct mii_bus;
struct page_pool;
struct dentry;
/*
 * declared before the first prototype that takes one, so the parameter does not
 * get its own scope
 */
struct cortina_ni;

/*
 * The NI_HV internal-port counters are read-and-clear, so they have exactly one
 * reader: cortina_ni_nihv_sample(). It folds each read into a 64-bit
 * driver-side total and returns the totals; /proc and "ethtool -S" use those
 * and must never readl() these offsets themselves. A caller that wants a delta
 * keeps its own copy of the total and subtracts.
 *
 * Read-and-clear is selected by NI 0x2010 bit 31. Accumulating is the safe
 * choice: if the block were left cumulative the totals would over-count visibly
 * instead of losing counts. Anything that changes 0x2010 must revisit this.
 *
 * These are the RTL9607DQ offsets. The generic Cortina set (0xa9bc, 0xa9fc,
 * 0xaa10, ...) is unmapped on this SoC and a read faults the bus.
 */
enum cortina_ni_nihv_cnt {
	CA_NI_NIHV_INTPT_RX,		/* 0xa92c NI_HV_INTPT_RX_PKT_CNT         */
	CA_NI_NIHV_INTPT_MISS_SOP_EOP,	/* 0xa924 INTPT_RX_MISSING_SOP_EOP_CNT   */
	CA_NI_NIHV_INTPT_SHORT_ERR,	/* 0xa928 INTPT_RX_SHORT_ERR_CNT         */
	CA_NI_NIHV_INTPT_TX,		/* 0xa940 NI_HV_INTPT_TX_PKT_CNT         */
	CA_NI_NIHV_XRAM_DMA,		/* 0xaa4c NI_HV_XRAM_DMA_PKT_CNT         */
	CA_NI_NIHV_CNT_COUNT,
};

/*
 * Sample every NI_HV read-and-clear counter once, add it to the driver's
 * running totals and return those totals in @out. The only reader of these
 * registers. Process or softirq context; takes a spinlock, never sleeps.
 * (cortina-ni-ethtool.c)
 */
void cortina_ni_nihv_sample(struct cortina_ni *ni,
			    u64 out[CA_NI_NIHV_CNT_COUNT]);

/*
 * The NI register snapshot for "ethtool -d". The tables live in cortina-ni-rx.c
 * beside the /proc reader that prints them, so there is one list. _len() is in
 * u32 words.
 */
unsigned int cortina_ni_regdump_len(void);
void cortina_ni_regdump_fill(struct cortina_ni *ni, u32 *buf);
/*
 * name and NI-window offset of dump word @i, for debugfs cortina-ni/regdump_map
 */
void cortina_ni_regdump_entry(unsigned int i, const char **name, u32 *off);

/*
 * Read one per-port NI MAC MIB counter through the indirect ACCESS/DATA pair
 * (RX 0xa1a4/0xa1ac, TX 0xa1b0/0xa1b8). Returns ~0 if the access never
 * completes, so a stuck read is visible. May sleep: process context only, never
 * under nihv_lock. (cortina-ni-rx.c)
 */
u32 cortina_ni_rx_mib_read(struct cortina_ni *ni, u32 port, u32 cnt_id);
u32 cortina_ni_tx_mib_read(struct cortina_ni *ni, u32 port, u32 cnt_id);

/* One DMA-LSO virtual port (VP) queue */
struct cortina_ni_txq {
	u8		vp;		/* DMA-LSO VP index (CPU n -> VP n+2) */
	/*
	 * DMA-LSO queue index within the VP. Each (vp, txq) is an independent
	 * ring with its own BASE_DEPTH, WPTR/RPTR and scheduler priority.
	 * Upstream OMCI uses txq 7, everything else txq 0.
	 */
	u8		txq;
	__le32		*desc;		/* coherent descriptor ring, 2 words/desc */
	dma_addr_t	desc_dma;
	u16		wptr;		/* next descriptor to fill (SW) */
	u16		finished;	/* oldest un-reclaimed descriptor */
	spinlock_t	lock;		/* xmit vs. reclaim-timer (both BH) */
	struct {
		struct sk_buff	*skb;
		dma_addr_t	addr;
		unsigned int	len;
		/*
		 * Without an skb: 0 = unused, 1 = PON header block (coherent
		 * scratch, nothing to free), >= 2 = PON frame in scratch slot
		 * (pon - 2), released by the reclaim. With an skb: 1 = PON WAN
		 * data frame, counted on the WAN netdev at enqueue.
		 */
		u8		pon;
		/*
		 * 1 = an extra copy of a flooded eth0 frame. It points at the
		 * mapping owned by the last descriptor of the same burst, so
		 * there is nothing to unmap or free here.
		 */
		u8		dup;
	} slot[CA_NI_TX_RING_SIZE];
	/* debug counters */
	u64		enq;
	u64		reclaimed;
};

struct cortina_ni_tx {
	struct net_device	*netdev;
	struct phy_device	*phydev;
	struct cortina_ni_txq	txq[CA_NI_TX_NUM_VPS];
	struct timer_list	reclaim_timer;
	/*
	 * The NI engine takes frames faster than a slow wire carries them and
	 * drops the excess, so each LAN port gets an emulated wire clock and
	 * the queue is stopped while a port has too much queued.
	 */
	struct hrtimer		pace_timer;	/* wakes the queue when a wire has room */
	/* when each wire is idle again, and its speed (0 = no pacing) */
	ktime_t			wire_free[CA_NI_DSA_USER_PORTS];
	u32			wire_ps_per_byte[CA_NI_DSA_USER_PORTS];
	u64			tx_paced;
	struct work_struct	announce_work;	/* gratuitous ARP on link-up */
	u64			drop_nomap;
	u64			drop_linearize;
	u64			drop_oversize;
	u64			tx_busy;

	u64			lan_dup;	/* extra descriptors a flood cost */

	/*
	 * US PON control-frame (OMCI) TX: coherent scratch of
	 * CA_NI_PON_TX_SLOTS slots, each {16B DMA-LSO header block @0, frame
	 * @+32}, sent as a 2-descriptor HEADER_A chain on txq[0]. pon_busy
	 * (slot bitmap) is guarded by txq[0].lock.
	 */
	void			*pon_buf;
	dma_addr_t		pon_buf_dma;
	u32			pon_busy;	/* CA_NI_PON_TX_SLOTS-bit in-use bitmap */
	/* debug counters */
	u64			pon_enq;
	u64			pon_fail;
	u64			pon_data_enq;	/* US WAN data frames enqueued */
	/* NI CPUXRAM management-FIFO TX path (US OMCI) */
};

struct cortina_ni;

struct cortina_ni_rx_irqctx {
	struct cortina_ni	*ni;
	u8			idx;	/* DT interrupt index 0..7 */
};

#define CA_NI_RX_HASH_BITS	11	/* 2048 heads for 880 pool slots */
#define CA_NI_RX_HASH_SIZE	BIT(CA_NI_RX_HASH_BITS)

/*
 * A pool buffer's usable payload window is not its size.
 *
 * QM_DEST_PORTn_PKT_BUF_CFG (0x18041804) reserves head_room =
 * CA_NI_QM_PKT_BUF_HEAD_UNITS (4 x 16B = 64B, which puts HEADER_A at +0x40) and
 * tail_room = CA_NI_QM_PKT_BUF_TAIL_UNITS (0x18 x 16B = 384B). A 2048-byte
 * buffer therefore holds 1600 bytes of frame in [0x40, 0x680). Bytes past that
 * were never written by the hardware for this frame.
 */
#define CA_NI_RX_BUF_TAILROOM		(CA_NI_QM_PKT_BUF_TAIL_UNITS * 16)
#define CA_NI_RX_BUF_USABLE_END(bufsz)	((bufsz) - CA_NI_RX_BUF_TAILROOM)
/*
 * An EQ0 SRAM buffer as the CPU sees it: the descriptor PA names a 128-byte
 * cell with HEADER_A at +0x40 on the SOP and continuation data at +0x40, so
 * each cell carries at most 0x40 bytes (board-read, TCP FIN 48 + 18 bytes).
 */
#define CA_NI_RX_DQ_SRAM_CELL		0x80u

/*
 * Chain bounds. A malformed chain must cost a counter, never a buffer, a spin
 * or an unbounded skb.
 *
 * MAX_LEN is a policy cap, not a hardware limit: it bounds the single
 * allocation a chain makes (sized from the SOP's pkt_size). The QM coalesces
 * back-to-back frames of a CPU-bound flow into one chain, so it must hold a
 * large HTTP request, not just one MTU-sized frame.
 */
#define CA_NI_RX_CHAIN_MAX_LEN		16384u
/*
 * Segment cap, enough to carry CHAIN_MAX_LEN in 1592-byte segments. A
 * static_assert in cortina-ni-rx.c checks it against the pool geometry.
 */
#define CA_NI_RX_CHAIN_MAX_SEGS		32u

/*
 * Multi-buffer receive: the length accounting of one in-flight SOP..EOP chain.
 * It holds no skb or hardware state, so ca_ni_chain_step() in cortina-ni-rx.c
 * is a pure function. The skb lives in struct cortina_ni_rx_chain.
 */
struct ca_ni_chain_state {
	u32	total;		/* payload bytes the SOP HEADER_A promised */
	u32	got;		/* payload bytes accounted so far */
	u16	segs;		/* descriptors consumed by this chain */
	bool	open;		/* a chain is in flight on this voq */
};

/*
 * One in-flight chain per CPU-port VoQ. A frame's descriptors arrive in order
 * on one VoQ, but the NAPI budget can cut a chain in half, so the state must
 * survive into the next poll without mixing in another VoQ.
 */
struct cortina_ni_rx_chain {
	struct sk_buff			*skb;	/* NULL = nothing held */
	/*
	 * HEADER_A word 1 of the SOP buffer. Delivery is decided when the chain
	 * completes, possibly in a later poll, so the word is kept here rather
	 * than read from rx->last_hdra.
	 */
	u32				hdra_lo;
	/* any segment's descriptor flagged an L4 checksum error */
	bool				csum_err;
	struct ca_ni_chain_state	st;
};

/*
 * Downstream PON data reaches CPU port 0 in its own VoQ (PDC map cos), which
 * has its own NAPI, so WAN and LAN receive run on different CPUs.
 */
#define CA_NI_RX_WAN_VOQ	1

struct cortina_ni_rx {
	struct cortina_ni	*ni;
	struct net_device	*netdev;
	struct napi_struct	napi;		/* every VoQ but CA_NI_RX_WAN_VOQ */
	struct napi_struct	napi_wan;	/* CA_NI_RX_WAN_VOQ */
	spinlock_t		recycle_lock;	/* defer_ring and the CPU push doorbell */
	__le64			*ring;		/* coherent EPP descriptor ring (8 voqs) */
	dma_addr_t		ring_dma;
	u32			rptr[CA_NI_RX_RING_COUNT];	/* byte offset per ring (port * 8 + voq) */
	/*
	 * Reserved DRAM region; only its deep-queue pool (EQ12) is used, the
	 * CPU pools take their buffers from the page pools below.
	 */
	void			*cpu_dram;
	dma_addr_t		cpu_dram_dma;
	/* CPU-pool buffers, one pool per NAPI */
	struct page_pool	*pp;
	struct page_pool	*pp_wan;
	/*
	 * NE SRAM (EQ0 deep-queue cells).  The L3FE sends small control
	 * frames for the router (TCP FIN and the like) to the CPU in these
	 * cells; the hardware recycles them.  NULL if the DT has no "sram".
	 */
	void			*dq_sram;
	phys_addr_t		dq_sram_pa;
	u32			dq_sram_size;
	struct cortina_ni_rx_irqctx irqctx[CA_NI_RX_NUM_IRQS];
	unsigned long		irq_off;	/* GIC lines masked until the poll drains */
	int			irq[CA_NI_RX_NUM_IRQS];	/* <0 = not mapped */
	/* GPHY fault poll and reinit, 1 Hz */
	struct delayed_work	recovery_work;
	/* per-bank snapshot taken at probe */
	u16			gphy_cal[CA_NI_GPHY_COUNT][CA_NI_RX_GPHY_CAL_REGS];
	/* debug counters */
	u64			rearms;		/* link-up RX re-arms */
	u64			recoveries;	/* GPHY reinits fired */
	u64			irq_hits[CA_NI_RX_NUM_IRQS];
	u64			polls;
	/*
	 * frames per VoQ: a single flow should land on one VoQ, otherwise the
	 * fixed 0..7 drain order can reorder it
	 */
	u64			voq_frames[CA_NI_RX_VOQ_COUNT];
	u64			frames;
	u64			bytes;
	u64			swid_frames;	/* headerless (sw_id != 0) frames */
	u64			csum_err;	/* descriptor l4_csum_err set */
	u64			csum_ok;	/* marked CHECKSUM_UNNECESSARY */
	u64			zc_frames;	/* delivered without a copy */
	u64			lspid_cnt[16];	/* RX frames per HEADER_A.lspid */
	/*
	 * DS PON frames: OMCI handed to the GPON hook, data (lspid PON) and
	 * L3 miss punts (lspid L3_WAN) delivered to the WAN netdev
	 */
	u64			pon_frames;
	u64			wan_frames;
	u64			wan_l3_frames;
	u64			drop_nosop;	/* descriptor without SOP */
	u64			drop_badpa;	/* PA not in our map */
	u64			drop_len;	/* bad frame length */
	/*
	 * drop_len split by cause: a runt is a bad frame, an oversize one does
	 * not fit the buffer's usable window
	 */
	u64			drop_runt;	/* len < ETH_HLEN */
	u64			drop_oversize;	/* off + len past the usable window */
	u64			drop_nobuf;	/* refill alloc failed */
	/*
	 * Multi-buffer receive: one in-flight chain per VoQ, and a counter for
	 * every way a chain can be malformed, since the hardware is not trusted
	 * to terminate one.
	 */
	struct cortina_ni_rx_chain chain[CA_NI_RX_RING_COUNT];
	u32			chain_rest_off;	/* payload offset in a non-first buffer */
	u64			chain_frames;	/* chains assembled and delivered */
	u64			chain_segs;	/* segments those frames consumed */
	u64			chain_max_segs;	/* deepest chain seen */
	u64			chain_abort;	/* partial frames freed (total) */
	u64			chain_reopen;	/* SOP arrived with a chain still open */
	u64			chain_orphan;	/* non-SOP segment, no chain open */
	u64			chain_badtotal;	/* SOP pkt_size out of range */
	u64			chain_toolong;	/* segment cap hit / segments overran total */
	u64			chain_short;	/* EOP with fewer bytes than promised */
	u64			chain_swid;	/* headerless format: geometry unknown */
	/*
	 * Per-segment pkt_size as reported by the hardware, beside the byte
	 * count derived by the driver, for the last chain assembled. The vendor
	 * driver ignores this field on continuation descriptors, so it is
	 * recorded rather than checked.
	 */
	u32			chain_dlen_seen[CA_NI_RX_CHAIN_MAX_SEGS];
	u32			chain_dlen_calc[CA_NI_RX_CHAIN_MAX_SEGS];
	u64			chain_dlen_diff;	/* how often the two disagreed */
	/*
	 * RX buffer ownership checks (see cpu_pool_push in cortina-ni-rx.c).
	 * stale_buf: the descriptor's pktlen and the buffer's HEADER_A.pkt_size
	 * disagree, so the buffer no longer holds the frame the descriptor was
	 * written for. push_fail: a recycle doorbell timed out and the pool
	 * shrank by one.
	 */
	u64			stale_buf;
	u64			push_fail;
	/*
	 * Deferred recycle FIFO. The QM free list is a LIFO, so a buffer
	 * re-posted at once would be reused for the very next frame and could
	 * be overwritten while a multi-buffer chain still needs it. Each
	 * consumed buffer is held for a while before it is returned.
	 */
#define CA_NI_RX_DEFER_DEPTH	1024u	/* power of 2 */
	u32			defer_ring[CA_NI_RX_DEFER_DEPTH];
	u32			defer_head;	/* index of the oldest held PA */
	u32			defer_count;	/* PAs currently held out of the pool */
	/*
	 * frames delivered from the hardware-managed deep-queue pool (EQ12),
	 * which carries the GPON downstream punt
	 */
	u64			dq_frames;
	u64			dq_sram_segs;	/* descriptors read from NE SRAM */
};

struct cortina_ni {
	struct device		*dev;
	void __iomem		*win[CA_NI_WIN_COUNT];
	struct regmap		*glb;	/* global SoC registers (syscon) */
	u8			mac[ETH_ALEN];	/* from DT, zero if none */
	struct mii_bus		*mii;
	/* per-internal-PHY page-select shadow (reg 0x1f is not a HW reg) */
	u16			gphy_page[CA_NI_GPHY_COUNT];
	/* internal-GPHY SRAM firmware applied, per bank (one-shot per boot) */
	struct cortina_ni_tx	*tx;
	struct cortina_ni_rx	*rx;
	struct cortina_ppe	*ppe;	/* flow offload, NULL when off */
	/*
	 * NI_HV read-and-clear counter totals, see cortina_ni_nihv_sample().
	 * The lock makes the read and the fold one critical section.
	 */
	spinlock_t		nihv_lock;
	u64			nihv_total[CA_NI_NIHV_CNT_COUNT];
	/* debugfs root (the "ethtool -d" decode map); NULL without debugfs */
	struct dentry		*dbgfs;

#if IS_ENABLED(CONFIG_CORTINA_NI_DSA)
	/* DSA, see cortina-ni-dsa.c */
	struct dsa_switch	*ds;
	bool			sds_up;		/* port 4 SerDes lane is programmed */
	/*
	 * L2 switching: the LAN ports each LAN port may forward to in
	 * hardware (bitmap, same bridge and forwarding), under l2_lock with the
	 * MMSHP and FDB tables
	 */
	struct mutex		l2_lock;
	u8			lan_fwd[CA_NI_LAN_PORT_COUNT];
	u8			stp_fwd;	/* LAN ports in BR_STATE_FORWARDING */
	u8			vlan_aware;	/* LAN ports of a VLAN-filtering bridge */
	struct phylink_pcs	sgmii_pcs;	/* port 4 2500BASE-X PCS */
#endif
};

/*
 * DS PON control-frame hand-off: frames whose 16-byte PON header carries
 * ethertype 0xfff1 (GPON OMCI) are passed to the registered consumer, the
 * cortina-gpon driver, with the header stripped, instead of the network stack.
 * Called from NAPI context.
 */
typedef void (*cortina_ni_pon_rx_fn)(const u8 *pdu, unsigned int len);
void cortina_ni_pon_rx_hook_set(cortina_ni_pon_rx_fn fn);

/*
 * DS EPON OAM control-frame hand-off: frames whose 16-byte PON header carries
 * ethertype 0xfff1 and whose inner frame is IEEE 802.3ah OAM (0x8809 / 0x03)
 * are passed to the registered consumer with the 16-byte header stripped and
 * the LLID index extracted from header byte 11. Called from NAPI context.
 */
typedef void (*cortina_ni_pon_oam_rx_fn)(const u8 *frame, unsigned int len,
					 u8 llid_idx);
void cortina_ni_pon_oam_rx_hook_set(cortina_ni_pon_oam_rx_fn fn);

/*
 * US EPON OAM control-frame TX: prepend the 16-byte PON control header with
 * the target LLID index and transmit via the DMA-LSO PON control queue.
 * Consumes @skb on success; caller retains ownership on error.
 */
int cortina_ni_pon_oam_tx(struct sk_buff *skb, u8 llid_idx);

/*
 * Log the MAC->CPU delivery counters (QM admission, buffer drops, CPU-EPP ring
 * pointers, NAPI progress). Safe to call before the NI is probed.
 */
void cortina_ni_rx_delivery_dump(void);

/*
 * US PON control-frame TX: wrap the OMCI PDU in the 16-byte PON header and
 * queue it on the DMA-LSO ring with the OMCC HEADER_A; the hardware sends it on
 * the OMCC at the next grant. Process or softirq context. Returns 0, -ENODEV
 * (TX not up), -EINVAL (bad length) or -EBUSY (ring or scratch full; the OLT
 * retransmits).
 */
int cortina_ni_pon_tx(const u8 *pdu, unsigned int len);

/*
 * DS PON data delivery: frames with HEADER_A.lspid = PON go to this netdev (the
 * GPON WAN port) instead of eth0. NULL sends them to eth0.
 */
void cortina_ni_pon_wan_ndev_set(struct net_device *ndev,
				 bool (*rx_fixup)(struct sk_buff *skb));

/*
 * Print both directions' CPU-forward counters (US pon_data_enq, DS
 * wan_l3/wan_pon) as one line, in both /proc/net/cortina_ni_rx and
 * /proc/net/cortina_ni_tx.
 */
struct seq_file;
void cortina_ni_cpu_fwd_show(struct seq_file *m, struct cortina_ni *ni);

/*
 * US PON data TX for the GPON WAN netdev: queue one Ethernet frame toward the
 * PON in upstream VoQ @voq (hw T-CONT voq / 8, queue voq % 8, pol_id voq,
 * fe_bypass); the hardware GEM-encapsulates it with US_PORT_ID[voq]. Consumes the skb and
 * always returns NETDEV_TX_OK; errors are counted in @ndev->stats.
 */
int cortina_ni_qm_voq_admit(unsigned int qm_voq_idx);
netdev_tx_t cortina_ni_pon_data_tx(struct sk_buff *skb,
				   struct net_device *ndev, u16 voq);

int cortina_ni_tx_probe(struct cortina_ni *ni);
int cortina_ni_rx_probe(struct cortina_ni *ni);

/*
 * CPU->LAN egress port binding (cortina-ni-tx.c), fed by the RX side. _learn()
 * binds @sa to the LAN port a frame came in on; @lspid is HEADER_A.lspid, the
 * ingress NI port. _link_set() publishes the ports that have a PHY link, from
 * the 1 Hz poll; a change flushes every binding.
 */

/*
 * Program a static L2FE FDB entry {mac -> ldpid} and return its 13-bit entry
 * index (the L3FE mac_da_idx), or -1. Used by flow offload to resolve the
 * egress DMAC. @base is the NI window. (cortina-ni-rx.c)
 */
int cortina_ni_l2fe_fdb_add_idx(void __iomem *base, const u8 *mac, u32 ldpid);
int cortina_ni_l2fe_fdb_replace(void __iomem *base, const u8 *mac, u32 ldpid);
void cortina_ni_l2fe_mmshp_set(struct cortina_ni *ni, unsigned int lspid);
int cortina_ni_l2fe_fdb_del(void __iomem *base, const u8 *mac);
int cortina_ni_l2fe_fdb_scan(void __iomem *base, u32 *idx, u8 *mac, u16 *vid,
			     u32 *act);

/*
 * Look up @mac in the L2FE FDB without writing and return its 13-bit entry
 * index (mac_da_idx) and forward-to LDPID (the physical port for a LAN port).
 * Used by the DS flow offload to resolve the LAN client's DMAC and egress port.
 * Returns -1 when absent.
 */
int cortina_ni_l2fe_fdb_lookup_idx(void __iomem *base, const u8 *mac,
				   u32 *ldpid_out);

/*
 * L3FE main-hash flow offload (cortina-ppe.c), an nf_flow_table hardware
 * offload backend. A probe failure is not fatal: flows stay in software.
 */
struct cortina_ppe;

/* "ethtool -S" counters, a snapshot that does not touch the engine */
enum cortina_ni_l3fe_stat {
	CA_L3FE_FLOWS_RESIDENT,
	CA_L3FE_DS_FLOWS_RESIDENT,
	CA_L3FE_US_HITS,
	CA_L3FE_DS_HITS,
	CA_L3FE_FLOWS_REFUSED,
	CA_L3FE_REFUSED_UNSUPPORTED,
	CA_L3FE_REFUSED_TABLE_FULL,
	CA_L3FE_REFUSED_DUPLICATE,
	CA_L3FE_REFUSED_ERROR,
	CA_L3FE_STAT_COUNT,
};

#if IS_ENABLED(CONFIG_CORTINA_NI_FLOWOFFLOAD)
int cortina_ppe_probe(struct cortina_ni *ni);
int cortina_ppe_setup_tc(struct net_device *dev, enum tc_setup_type type,
			 void *type_data);
void cortina_ppe_stats(struct cortina_ppe *ppe, u64 out[CA_L3FE_STAT_COUNT]);
/* read-only "ppe_entries" dump of the offloaded flows under @dir */
void cortina_ppe_debugfs_init(struct cortina_ppe *ppe, struct dentry *dir);
/* new router (LAN) MAC: store it, then re-add the router MACs to the CAM */
void cortina_ppe_router_mac_set(struct cortina_ppe *ppe, const u8 *mac);
int cortina_ppe_intf_add(struct cortina_ppe *ppe, const u8 *lan_mac);
/* classifier rows and field CAMs of the stock firmware (RX classifier init) */
int cortina_ppe_cls_stock_load(void __iomem *base);
/**
 * cortina_ppe_active() - whether the flow engine is up
 *
 * When it is, routed frames pass the L3FE: the GPON driver steers the DS data
 * GEM into it instead of to CPU_0 with FE bypass, and the RX path expects
 * hash misses punted with lspid L3_WAN.
 */
bool cortina_ppe_active(void);
/**
 * cortina_ppe_wan_path_set() - set the upstream data path of the WAN
 * @gem_id: OLT-provisioned data GEM port-id, 0 when the path is torn down
 * @tcont_idx: T-CONT index of that GEM
 *
 * Called by the GPON driver. Upstream flows are only offloaded while a data
 * GEM is set; a teardown also clears the PPPoE session and its flows.
 */
void cortina_ppe_wan_path_set(u16 gem_id, u8 tcont_idx);
#else
static inline int cortina_ppe_probe(struct cortina_ni *ni)
{
	return 0;
}

static inline int cortina_ppe_setup_tc(struct net_device *dev,
				       enum tc_setup_type type, void *type_data)
{
	return -EOPNOTSUPP;
}

static inline void cortina_ppe_stats(struct cortina_ppe *ppe,
				     u64 out[CA_L3FE_STAT_COUNT])
{
}

static inline void cortina_ppe_debugfs_init(struct cortina_ppe *ppe,
					    struct dentry *dir)
{
}

static inline void cortina_ppe_router_mac_set(struct cortina_ppe *ppe,
					      const u8 *mac)
{
}

static inline int cortina_ppe_intf_add(struct cortina_ppe *ppe,
				       const u8 *lan_mac)
{
	return 0;
}

static inline int cortina_ppe_cls_stock_load(void __iomem *base)
{
	return 0;
}

static inline bool cortina_ppe_active(void)
{
	return false;
}

static inline void cortina_ppe_wan_path_set(u16 gem_id, u8 tcont_idx)
{
}
#endif
void cortina_ni_rx_open(struct cortina_ni *ni);
void cortina_ni_rx_stop(struct cortina_ni *ni);
void cortina_ni_rx_link_up(struct cortina_ni *ni);	/* phylib link-up hook */
/*
 * re-key the MAC-keyed tables (L2FE FDB, my-MAC comparator, PP FIELD-CAM,
 * offload router MAC) from the current dev_addr; called from
 * .ndo_set_mac_address. No-op unless the flow engine is active.
 */
void cortina_ni_rx_mac_rearm(struct cortina_ni *ni);

/*
 * ethtool support (cortina-ni-ethtool.c). eth0 only: the GPON WAN netdev has no
 * private area, which these ops dereference.
 */
extern const struct ethtool_ops cortina_ni_ethtool_ops;
/*
 * publish the "ethtool -d" decode key at debugfs cortina-ni/regdump_map; a
 * no-op without debugfs
 */
void cortina_ni_debugfs_init(struct cortina_ni *ni);

/*
 * DSA user ports (cortina-ni-dsa.c). _register() is called from the probe after
 * the conduit netdev and the MDIO bus are up. Without CONFIG_CORTINA_NI_DSA
 * these are no-ops.
 */
#if IS_ENABLED(CONFIG_CORTINA_NI_DSA)
extern struct platform_driver cortina_ni_switch_driver;
int cortina_ni_dsa_register(struct cortina_ni *ni);
#else
static inline int cortina_ni_dsa_register(struct cortina_ni *ni) { return 0; }
#endif

#endif /* _CORTINA_NI_H */
