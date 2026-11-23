// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015 Cortina-Access Limited
 * Copyright (C) BRULE Herman <alpha_one_x86@first-world.info>
 *
 * Cortina-Access NI Ethernet receive path for the Realtek RTL9607DQ: the L3QM
 * CPU-EPP descriptor ring, the CPU empty-buffer pools, NAPI, and the
 * forwarding-engine setup that steers ingress frames to the CPU port.
 *
 * The CPU pools are seeded with DMA buffers.  For every frame steered to CPU
 * port 0 the hardware picks a pool buffer, writes [64B headroom | 8B HEADER_A |
 * (8B HEADER_CPU) | frame] into it and appends an 8-byte descriptor to a
 * 128-entry ring in DDR, advancing a byte-granular write pointer.  The level
 * interrupt fires on ring occupancy; NAPI drains descriptors, maps each buffer
 * back by physical address, copies the frame out, returns the buffer to the
 * pool and publishes the new read pointer.
 *
 * Frames reach the CPU through the full forwarding engine: the L2FE
 * default-forwards an FDB miss to CPU port 0 and the L3FE demux map routes that
 * output into the CPU-EPP ring.
 */

#include <asm/cacheflush.h>
#include <linux/bitfield.h>
#include <linux/crc32.h>
#include <linux/ip.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/if_vlan.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/platform_device.h>
#include <linux/ratelimit.h>
#include <linux/skbuff.h>
#include <linux/unaligned.h>
#include <net/net_namespace.h>
#include <net/ip.h>
#include <net/page_pool/helpers.h>

#include "cortina-ni.h"

/* The RTL9607DQ has six port MAC blocks (0-5); 0xa928 is not a seventh. */
#define CA_NI_RX_PORT_COUNT		6

/* Consumed CPU-pool buffers held back before they are posted again. */
#define CA_NI_RX_DEFER_KEEP		256

/* Deep-queue trigger dbuf_dpid of ARB_CTRL, as stock (0x89c71c82). */
#define CA_NI_L2FE_ARB_DBUF_DPID_VAL	0x08

/* L2TM physical dequeue -> TM-port map: the identity. */
#define CA_NI_L2TM_BM_DQ_TO_TM_PORT_IDENT	0x76543210u

/* EQ pool of the upstream/deep-queue profile (EQ_PROFILE(3)). */
#define CA_NI_RX_US_EQ_POOL		12

/*
 * CPU RX buffers are software-owned (CFG2.cpu_eq = 1): a buffer belongs to
 * the CPU until the driver returns it through the CPU_PUSH_PADDR doorbell,
 * as in the vendor driver.  With hardware ownership the QM frees a buffer as
 * soon as its descriptor is written, and its free list is LIFO, so the two
 * fragments of one IPv4 datagram can land in the same buffer before NAPI
 * copies the first.  The deep-queue consumers (dest ports 16..47, reached by
 * the forwarding engine) keep a hardware-managed pool of their own, see
 * CA_NI_RX_DQ_*.
 *
 * The FBM (RMU buffer allocator) is configured but never enabled: enabled, it
 * DMAs into the CPU-pool region, which no reserved-memory node excludes.
 */

/*
 * Minimum number of buffers a pool seed must achieve.  The seed count itself is
 * the pool's CFG1.total_buf: seeding fewer leaves the QM permanently short
 * (inactive_bid_cntr = total_buf - seeded).
 */
#define CA_NI_RX_PUSH_SEED_MIN		64
/* A full seed must fit inside the sub-region the PA->VA math maps. */
static_assert(CA_NI_RX_EQ_TOTAL_BUF * CA_NI_RX_CPU_POOL0_BUFSZ <=
	      CA_NI_RX_CPU_POOL0_BYTES,
	      "CPU pool0: total_buf x buffer_size overflows its sub-region");
static_assert(CA_NI_RX_CPU_POOL0_BYTES +
	      CA_NI_RX_EQ2_TOTAL_BUF * CA_NI_RX_CPU_POOL1_BUFSZ <=
	      CA_NI_RX_CPU_DRAM_SIZE,
	      "CPU pool1: total_buf x buffer_size overflows the reserved region");
/*
 * Per-poll recycle batch.  The NAPI budget is clamped to this so a poll can
 * never consume more buffers than it can hand back.
 */
#define CA_NI_RX_RECYCLE_MAX		64
/*
 * Deep-queue pool (EQ12).
 *
 * Two consumers reach the CPU through the QM: the direct punt and the
 * deep-queue punt (dest ports 8..15, deep_q=1).  The deep-queue enqueue cannot
 * consume a CPU-pushed buffer; it needs a hardware-managed DRAM pool.  Giving
 * the deep queue its own pool is what lets the direct-punt pools be
 * software-owned.
 *
 * DEST_PORT_EQ_CFG.prof_sel (CA_NI_QM_DEST_PORT_PROF_SEL, bits[3:0]) indexes
 * EQ_PROFILE directly (0..15).  Stock pairs destp8 = 0x0C with prof12 = {EQ12,
 * EQ14} for the deep queue and prof13 = {EQ13, EQ14} for the direct punt.
 */
#define CA_NI_RX_DQ_POOL_OFF		CA_NI_RX_CPU_DRAM_SIZE
#define CA_NI_RX_DQ_POOL_PHYS \
	(CA_NI_RX_CPU_POOL_PHYS + CA_NI_RX_DQ_POOL_OFF)
/*
 * One mapping covers both CPU pools AND the deep-queue pool, so the PA->VA
 * math and the bounds check in cortina_ni_rx_frame() need no special case.
 */
#define CA_NI_RX_MAP_SIZE \
	(CA_NI_RX_CPU_DRAM_SIZE + CA_NI_RX_DQ_DRAM_SIZE)
/*
 * Not stock's CFG2 0x0000ff02 (256B): the direct punt path assumes one buffer
 * per frame.  Index 5 = 2048B as on the CPU pools, with cpu_eq=0 and
 * refill_en=0 as in stock.
 */
#define CA_NI_RX_DQ_CFG2		0x0000ff05u
/*
 * EQ profile for the deep queue: 12, stock's own deep-queue profile index.
 * Profiles 0..7 are filled by a loop in eq_init and 13 is the direct punt's.
 */
#define CA_NI_RX_DQ_PROFILE_SEL		CA_NI_RX_EQ12_PROFILE	/* = 12 */
/* the deep-queue pool is hardware-managed, so both halves of the profile use it */
#define CA_NI_RX_DQ_PROFILE_VAL \
	(FIELD_PREP(CA_NI_QM_EQ_PROF_EQP0, CA_NI_RX_EQ12_ID) | \
	 FIELD_PREP(CA_NI_QM_EQ_PROF_EQP1, CA_NI_RX_EQ12_ID))
static_assert(CA_NI_RX_EQ12_TOTAL_BUF * 2048u <= CA_NI_RX_DQ_DRAM_SIZE,
	      "deep-queue pool: total_buf x buffer_size overflows its region");

/*
 * Ready-poll bound for the NAPI recycle path.  Much tighter than
 * CA_NI_RX_PUSH_TIMEOUT_US (the process-context seed bound): this runs in
 * softirq once per recycled buffer, so a stuck gate must degrade (push_fail,
 * pool shrinks) rather than spin.
 */
#define CA_NI_RX_PUSH_TIMEOUT_NAPI_US	20

/*
 * head_room/tail_room are programmed the same for the first and the
 * continuation buffers, so one window serves both (CA_NI_RX_BUF_USABLE_END).
 */
static_assert(CA_NI_QM_PKT_BUF_HEAD_UNITS * 16 == CA_NI_RX_BUF_HEADROOM,
	      "pkt-buf head_room does not put HEADER_A at CA_NI_RX_HDRA_OFF");
static_assert(CA_NI_RX_CHAIN_MAX_SEGS *
	      (CA_NI_RX_BUF_USABLE_END(CA_NI_RX_CPU_POOL0_BUFSZ) -
	       CA_NI_RX_FRAME_OFF) >= CA_NI_RX_CHAIN_MAX_LEN,
	      "chain: MAX_SEGS cannot carry MAX_LEN out of a CPU-pool buffer");
/* The firmware hardcodes this window as 1600; the derivation must agree. */
static_assert(CA_NI_RX_BUF_USABLE_END(CA_NI_RX_CPU_POOL0_BUFSZ) -
	      CA_NI_RX_BUF_HEADROOM == 1600,
	      "CPU-pool usable window is not the firmware's 1600 bytes");

static inline void __iomem *ni_base(struct cortina_ni *ni)
{
	return ni->win[CA_NI_WIN_NI];
}

static inline void ni_rmw(struct cortina_ni *ni, u32 off, u32 clr, u32 set)
{
	writel((readl(ni_base(ni) + off) & ~clr) | set, ni_base(ni) + off);
}

/*
 * Which CPU pool a buffer belongs to: pool0 (EQ5) occupies the first
 * POOL0_BYTES of the reserved region, pool1 (EQ6) the rest.
 */
static inline u32 cortina_ni_rx_pool_eqid(struct cortina_ni *ni, u32 pa)
{
	u32 off = pa - lower_32_bits(ni->rx->cpu_dram_dma);

	return off < CA_NI_RX_CPU_POOL0_BYTES ? CA_NI_RX_EQ_ID : CA_NI_RX_EQ_ID2;
}

/*
 * Stage one 128-byte-aligned buffer PA into EQ pool @eqid through the CPU push
 * doorbell.  CA_NI_QM_CPU_PUSH_READY bit31 must be polled first: a write while
 * the push stage is full back-pressures the AXI write and hangs the CPU.
 * @timeout_us bounds that poll; the probe-time seed and the NAPI recycle use
 * different bounds.  Returns 0, or -EBUSY if no slot freed in time.
 */
static int cortina_ni_rx_push_buf(struct cortina_ni *ni, u32 eqid, u32 pa,
				  unsigned int timeout_us)
{
	void __iomem *rdy = ni_base(ni) +
			    CA_NI_QM_CPU_PUSH_READY(CA_NI_RX_CPU_PORT);
	unsigned int i;

	for (i = 0; i < timeout_us; i++) {
		if (readl(rdy) & CA_NI_QM_PUSH_READY) {
			writel((pa & CA_NI_QM_PUSH_ADDR) |
			       FIELD_PREP(CA_NI_QM_PUSH_EQID, eqid),
			       ni_base(ni) +
			       CA_NI_QM_CPU_PUSH_PADDR(CA_NI_RX_CPU_PORT));
			return 0;
		}
		udelay(1);
	}
	return -EBUSY;
}

/* ------------------------------------------------------------------ */
/* CPU-pool buffers                                                    */
/* ------------------------------------------------------------------ */

/*
 * One 2048-byte page-pool fragment; returns its bus address (identical to
 * the PA on this SoC, which has no IOMMU) or 0.
 */
static u32 cortina_ni_rx_alloc_buf(struct page_pool *pp)
{
	unsigned int offset;
	struct page *page;

	page = page_pool_dev_alloc_frag(pp, &offset, CA_NI_RX_CPU_POOL0_BUFSZ);
	if (!page)
		return 0;
	return page_pool_get_dma_addr(page) + offset;
}

/*
 * Build the skb for a single-buffer frame.  A page-pool buffer becomes the
 * skb itself and a fresh buffer goes back to the QM in its place (*rpa); any
 * other buffer, or a failed refill, is copied out and *rpa is left as is.
 */
static struct sk_buff *cortina_ni_rx_skb(struct cortina_ni_rx *rx,
					 struct napi_struct *napi, u8 *buf,
					 bool pp_buf, unsigned int off, int len,
					 u32 *rpa)
{
	struct page_pool *pp = napi == &rx->napi_wan ? rx->pp_wan : rx->pp;
	struct sk_buff *skb;
	u32 npa;

	if (pp_buf) {
		npa = cortina_ni_rx_alloc_buf(pp);
		if (npa) {
			skb = napi_build_skb(buf, CA_NI_RX_CPU_POOL0_BUFSZ);
			if (likely(skb)) {
				skb_mark_for_recycle(skb);
				skb_reserve(skb, off);
				skb_put(skb, len);
				*rpa = npa;
				rx->zc_frames++;
				return skb;
			}
			page_pool_put_page(pp, virt_to_head_page(phys_to_virt(npa)),
					   -1, false);
		}
	}

	skb = napi_alloc_skb(napi, len + CA_NI_DSA_TAG_LEN);
	if (skb)
		skb_put_data(skb, buf + off, len);
	return skb;
}
/*
 * Both CPU pools are backed by the reserved DRAM region (rx->cpu_dram) and NAPI
 * copies each frame out of its pool buffer into a fresh skb.  The pools are
 * software-owned (cpu_eq=1): the QM cannot reclaim a buffer until the driver
 * writes {pa[31:7] << 7 | eqid} to the write-only CPU_PUSH_PADDR doorbell
 * (0x63cc + cpu_port * 8), which happens after the copy.
 */

/* ------------------------------------------------------------------ */
/* interrupt mask / ack                                                */
/* ------------------------------------------------------------------ */

/*
 * CPU port 0 owns byte 0 of INT_EN0; a set bit enables the level interrupt
 * of that voq's EPP FIFO.  We only ever use port0/voq0-7, so plain writes
 * (not RMW) keep the ISR-vs-NAPI enable/disable race-free.
 */
static void cortina_ni_rx_irq_set(struct cortina_ni *ni, bool enable)
{
	/*
	 * Only the port-0 interrupt byte is masked; bits[15:8] enable the
	 * writeback engine and must stay set or the ring write pointer stops.
	 */
	writel(enable ? CA_NI_QM_EPP64_INT_EN0_STOCK : CA_NI_QM_EPP64_INT_EN0_MASKED,
	       ni_base(ni) + CA_NI_QM_EPP64_INT_EN0);
}

static irqreturn_t cortina_ni_rx_isr(int irq, void *dev_id)
{
	struct cortina_ni_rx_irqctx *ctx = dev_id;
	struct cortina_ni *ni = ctx->ni;

	ni->rx->irq_hits[ctx->idx]++;

	/*
	 * Mask (this is also the only ack - level by FIFO occupancy).  Port 0
	 * is masked in the QM; the other ports' interrupt bytes also gate the
	 * writeback engine, so those lines are masked at the GIC until the
	 * poll has drained the rings (cortina_ni_rx_rearm()).
	 */
	cortina_ni_rx_irq_set(ni, false);
	if (ctx->idx && !test_and_set_bit(ctx->idx, &ni->rx->irq_off))
		disable_irq_nosync(irq);
	napi_schedule(&ni->rx->napi);
	napi_schedule(&ni->rx->napi_wan);
	return IRQ_HANDLED;
}

/* ------------------------------------------------------------------ */
/* NAPI poll                                                           */
/* ------------------------------------------------------------------ */

/* @voq is a ring index: port * CA_NI_RX_VOQ_COUNT + voq */
static u32 cortina_ni_rx_wptr_voq(struct cortina_ni *ni, unsigned int voq)
{
	u32 w = readl(ni_base(ni) +
		      CA_NI_QM_EPP64_WRPTR(voq / CA_NI_RX_VOQ_COUNT,
					   voq % CA_NI_RX_VOQ_COUNT));

	/* byte offset, wraps at the per-voq ring size (stock count formula) */
	return (w & CA_NI_QM_EPP64_PTR) & (CA_NI_RX_RING_BYTES - 1);
}

static u32 cortina_ni_rx_wptr(struct cortina_ni *ni)
{
	return cortina_ni_rx_wptr_voq(ni, CA_NI_RX_VOQ);
}

/*
 * DS PON control-frame consumer (the cortina-gpon driver registers here).
 * A plain pointer store/load: the hook is set once at GPON probe and only
 * cleared at GPON remove, and the RX path tolerates either value.
 */
static cortina_ni_pon_rx_fn cortina_ni_pon_rx_cb;
static cortina_ni_pon_oam_rx_fn cortina_ni_pon_oam_rx_cb;

/*
 * Downstream delivery counters, callable from the GPON stats work.  Between
 * the GPON MAC and the driver a frame passes PDC -> QM admission (rmu_rx) ->
 * a buffer (no_buf) -> the CPU-EPP ring (wptr) -> NAPI; reading them together
 * shows where frames stop.
 */
static struct cortina_ni *cortina_ni_rx_dump_ni;

/*
 * NI offsets that are defined on this SoC, minus the VOQ_STATUS8+ block which
 * takes a synchronous external abort on read
 */
static const u16 cortina_ni_win_offs[] = {
	0x6000, 0x6004, 0x6008, 0x600c, 0x6100, 0x6104, 0x6108, 0x610c, 0x6110, 0x6114, 0x6118, 0x611c,
	0x6120, 0x6124, 0x6128, 0x6148, 0x61c8, 0x61e8, 0x61ec, 0x61f0, 0x61f4, 0x61f8, 0x6328, 0x6368,
	0x636c, 0x63a8, 0x63ac, 0x63b0, 0x63b4, 0x63b8, 0x63bc, 0x63c0, 0x63c4, 0x6444, 0x6448, 0x6544,
	0x6548, 0x654c, 0x6550, 0x6554, 0x6558, 0x6578, 0x657c, 0x659c, 0x65a0, 0x65a4, 0x6674, 0x6678,
	0x66a4, 0x66a8, 0x66ac, 0x66b0, 0x66b4, 0x66b8, 0x66bc, 0x66c0, 0x66c4, 0x66c8, 0x66cc, 0x66d0,
	0x66d4, 0x66d8, 0x66dc, 0x66e0, 0x66e4, 0x67d8, 0x67dc, 0x67e0, 0x67e4, 0x67e8, 0x67ec, 0x680c,
	0x6810, 0x6814, 0x6818, 0x681c, 0x6820, 0x6824, 0x6828, 0x682c, 0x6830, 0x6834, 0x6838, 0x683c,
	0x6840, 0x6844, 0x6848, 0x684c, 0x6850, 0x6854, 0x6858, 0x685c, 0x6860, 0x6864, 0x6868, 0x686c,
	0x6870, 0x6874, 0x6878, 0x687c, 0x6880, 0x6884, 0x6888, 0x688c, 0x6890, 0x6894, 0x6898, 0x689c,
	0x68a0, 0x68a4, 0x68a8, 0x68ac, 0x68b0, 0x68b4, 0x68b8, 0x68bc, 0x68c0, 0x68c4, 0x68c8, 0x68cc,
	0x68d0, 0x68d4, 0x68d8, 0x68dc, 0x68e0, 0x68e4, 0x68e8, 0x68ec, 0x68f0, 0x68f4, 0x6920, 0x6924,
	0x6928, 0x692c, 0x6930, 0x6934, 0x6938, 0x693c, 0x6940, 0x6944, 0x6948, 0x694c, 0x6950, 0x6954,
	0x6958, 0x695c, 0x6960, 0x6964, 0x6968, 0x696c, 0x6970, 0x6974, 0x7000, 0x7100, 0x7200, 0xa000,
	0xa004, 0xa008, 0xa00c, 0xa010, 0xa014, 0xa018, 0xa01c, 0xa020, 0xa024, 0xa028, 0xa02c, 0xa030,
	0xa034, 0xa038, 0xa048, 0xa07c, 0xa080, 0xa084, 0xa088, 0xa08c, 0xa090, 0xa094, 0xa098, 0xa0c8,
	0xa0cc, 0xa0d0, 0xa0d4, 0xa0d8, 0xa0dc, 0xa0e0, 0xa0e4, 0xa0e8, 0xa0ec, 0xa0f0, 0xa0f4, 0xa0f8,
	0xa0fc, 0xa100, 0xa104, 0xa108, 0xa10c, 0xa110, 0xa114, 0xa118, 0xa11c, 0xa120, 0xa124, 0xa128,
	0xa144, 0xa148, 0xa14c, 0xa150, 0xa154, 0xa158, 0xa15c, 0xa160, 0xa164, 0xa168, 0xa16c, 0xa170,
	0xa174, 0xa178, 0xa17c, 0xa180, 0xa184, 0xa188, 0xa18c, 0xa190, 0xa194, 0xa198, 0xa19c, 0xa1a0,
	0xa1a4, 0xa1a8, 0xa1ac, 0xa1b0, 0xa1b4, 0xa1b8, 0xa1bc, 0xa1c0, 0xa1c4, 0xa1c8, 0xa1cc, 0xa1d0,
	0xa1d4, 0xa1d8, 0xa1dc, 0xa1e0, 0xa1e4, 0xa1e8, 0xa1ec, 0xa1f0, 0xa1f4, 0xa1f8, 0xa1fc, 0xa200,
	0xa204, 0xa208, 0xa20c, 0xa210, 0xa214, 0xa218, 0xa21c, 0xa220, 0xa224, 0xa228, 0xa22c, 0xa240,
	0xa244, 0xa248, 0xa24c, 0xa250, 0xa254, 0xa258, 0xa25c, 0xa260, 0xa264, 0xa268, 0xa270, 0xa274,
	0xa2d8, 0xa2dc, 0xa2e0, 0xa32c, 0xa330, 0xa334, 0xa338, 0xa33c, 0xa340, 0xa35c, 0xa360, 0xa37c,
	0xa380, 0xa39c, 0xa3a0, 0xa3a4, 0xa3a8, 0xa3ac, 0xa3b0, 0xa3b4, 0xa3b8, 0xa3e4, 0xa3e8, 0xa3ec,
	0xa3f0, 0xa3f4, 0xa3f8, 0xa3fc, 0xa400, 0xa404, 0xa408, 0xa40c, 0xa410, 0xa414, 0xa418, 0xa41c,
	0xa420, 0xa424, 0xa428, 0xa42c, 0xa430, 0xa434, 0xa438, 0xa43c, 0xa440, 0xa444, 0xa448, 0xa44c,
	0xa450, 0xa454, 0xa458, 0xa45c, 0xa460, 0xa464, 0xa468, 0xa46c, 0xa470, 0xa474, 0xa478, 0xa47c,
	0xa480, 0xa484, 0xa488, 0xa48c, 0xa490, 0xa494, 0xa498, 0xa49c, 0xa4a0, 0xa4a4, 0xa4a8, 0xa4ac,
	0xa4b0, 0xa4b4, 0xa4b8, 0xa4bc, 0xa4c0, 0xa4c4, 0xa4c8, 0xa4cc, 0xa510, 0xa514, 0xa518, 0xa51c,
	0xa520, 0xa524, 0xa528, 0xa52c, 0xa530, 0xa534, 0xa538, 0xa53c, 0xa540, 0xa544, 0xa548, 0xa54c,
	0xa550, 0xa554, 0xa558, 0xa55c, 0xa560, 0xa564, 0xa568, 0xa56c, 0xa570, 0xa574, 0xa578, 0xa57c,
	0xa580, 0xa584, 0xa588, 0xa58c, 0xa5c0, 0xa5c4, 0xa5c8, 0xa5cc, 0xa5d0, 0xa5d4, 0xa5d8, 0xa5dc,
	0xa5e0, 0xa5e4, 0xa5e8, 0xa5ec, 0xa5f0, 0xa5f4, 0xa5f8, 0xa5fc, 0xa600, 0xa604, 0xa608, 0xa60c,
	0xa610, 0xa614, 0xa618, 0xa61c, 0xa620, 0xa624, 0xa628, 0xa62c, 0xa630, 0xa920, 0xa924, 0xa928,
	0xa92c, 0xa930, 0xa934, 0xa938, 0xa93c, 0xa940, 0xa944, 0xa948, 0xaa20, 0xaa24, 0xaa28, 0xaa2c,
	0xaa30, 0xaa34, 0xaa38, 0xaa3c, 0xaa40, 0xaa44, 0xaa48, 0xaa4c, 0xaa50, 0xaa60, 0xaa64, 0xaa68,
	0xaa6c, 0xaa70, 0xaa74, 0xaa78, 0xaa7c,
};

static void cortina_ni_rx_window_dump(struct cortina_ni *ni, const char *tag)
{
	unsigned int i;

	return;
	/* Only defined offsets: other words in 0x6000-0xaa80 abort on read. */
	dev_dbg(ni->dev, "NIWIN[%s] %zu named offsets\n", tag,
		ARRAY_SIZE(cortina_ni_win_offs));
	for (i = 0; i < ARRAY_SIZE(cortina_ni_win_offs); i += 4)
		dev_dbg(ni->dev, "NIWIN %04x=%08x %04x=%08x %04x=%08x %04x=%08x\n",
			cortina_ni_win_offs[i],
			readl(ni_base(ni) + cortina_ni_win_offs[i]),
			cortina_ni_win_offs[i + 1 < ARRAY_SIZE(cortina_ni_win_offs) ? i + 1 : i],
			readl(ni_base(ni) +
			      cortina_ni_win_offs[i + 1 < ARRAY_SIZE(cortina_ni_win_offs) ? i + 1 : i]),
			cortina_ni_win_offs[i + 2 < ARRAY_SIZE(cortina_ni_win_offs) ? i + 2 : i],
			readl(ni_base(ni) +
			      cortina_ni_win_offs[i + 2 < ARRAY_SIZE(cortina_ni_win_offs) ? i + 2 : i]),
			cortina_ni_win_offs[i + 3 < ARRAY_SIZE(cortina_ni_win_offs) ? i + 3 : i],
			readl(ni_base(ni) +
			      cortina_ni_win_offs[i + 3 < ARRAY_SIZE(cortina_ni_win_offs) ? i + 3 : i]));
	dev_dbg(ni->dev, "NIWIN[%s] done\n", tag);
}

void cortina_ni_rx_delivery_dump(void)
{
	struct cortina_ni *ni = READ_ONCE(cortina_ni_rx_dump_ni);
	struct cortina_ni_rx *rx;
	unsigned int q;
	u32 wsum = 0;

	if (!ni || !ni->rx)
		return;
	rx = ni->rx;
	for (q = 0; q < CA_NI_RX_VOQ_COUNT; q++)
		wsum += cortina_ni_rx_wptr_voq(ni, q);

	{
		/*
		 * Dump the window on the fifth call (about 30 s at stats_s=5),
		 * after the netdev has been opened and its registers written.
		 */
		static unsigned int calls;

		if (++calls == 5)
			cortina_ni_rx_window_dump(ni, "late");
	}
	dev_dbg(ni->dev,
		"DS-DELIVERY: rmu_rx(0x67d8)=%u no_buf(0x6818)=%u tx_cntr(0x67e4)=%u | wptr_sum=0x%x wptr_voq0=0x%x rptr_voq0=0x%03x | pon=%llu frames=%llu polls=%llu | prof%u=0x%08x fifo_cfg=0x%08x\n",
		readl(ni_base(ni) + CA_NI_QM_RX_CNTR),
		readl(ni_base(ni) + CA_NI_QM_RMU_NO_BUF_DROP),
		readl(ni_base(ni) + CA_NI_QM_TX_CNTR),
		wsum, cortina_ni_rx_wptr_voq(ni, 0), rx->rptr[0],
		rx->pon_frames, rx->frames, rx->polls,
		CA_NI_RX_PROFILE_ID,
		readl(ni_base(ni) + CA_NI_QM_CPU_EPP_FIFO_PROF(CA_NI_RX_PROFILE_ID)),
		readl(ni_base(ni) + CA_NI_QM_CPU_EPP_FIFO_CFG(CA_NI_RX_CPU_PORT, 0)));

	/*
	 * The raw registers, plus the writeback error state:
	 *   QM_QM_INT_SRC (0x611c) - b20 cpuepp_fifo_error, b30 axim_cpuepp_resp_error,
	 *     b21 eqm_buf_size, b22 eqm_cfg, b8 rmu0_no_buffer, b9 rmu0_check, b10 rmu0_fifo
	 *   QM_QM_CPU_EPP_STATUS0/1 (0x659c/0x65a0)
	 * and the first ring slot.
	 */
	{
		u32 isrc = readl(ni_base(ni) + 0x611c);

		/*
		 * QM_QM_INT_SRC is write-1-to-clear; clear what was read so the
		 * next sample shows only bits that asserted again.
		 */
		if (isrc)
			writel(isrc, ni_base(ni) + 0x611c);

		dev_dbg(ni->dev,
			"DS-RAW: es_ctrl(0x6108)=%08x epp_sts0(0x659c)=%08x sts1(0x65a0)=%08x | epp_ctrl(0x68f4)=%08x | pa_req eq%u(0x%04x)=%u eq%u=%u | int_src(0x611c)=%08x%s%s%s%s%s (cleared) | ring[0]=%016llx\n",
			readl(ni_base(ni) + CA_NI_QM_ES_CTRL),
			readl(ni_base(ni) + 0x659c), readl(ni_base(ni) + 0x65a0),
			readl(ni_base(ni) + CA_NI_QM_EPP),
			CA_NI_RX_EQ_ID, CA_NI_QM_EQM_PA_REQ(CA_NI_RX_EQ_ID),
			readl(ni_base(ni) + CA_NI_QM_EQM_PA_REQ(CA_NI_RX_EQ_ID)),
			CA_NI_RX_EQ_ID2,
			readl(ni_base(ni) + CA_NI_QM_EQM_PA_REQ(CA_NI_RX_EQ_ID2)),
			isrc,
			(isrc & BIT(20)) ? " CPUEPP_FIFO_ERR" : "",
			(isrc & BIT(30)) ? " AXIM_CPUEPP_RESP_ERR" : "",
			(isrc & BIT(22)) ? " EQM_CFG_ERR" : "",
			(isrc & BIT(21)) ? " EQM_BUF_SIZE_ERR" : "",
			(isrc & GENMASK(5, 4)) ? " MMU_ECC_ERR" : "",
			rx->ring ? le64_to_cpu(READ_ONCE(rx->ring[0])) : 0ULL);

		/*
		 * NI ingress side: whether the downstream frame enters the NI
		 * internal-port block at all.
		 *   intpt_rx climbs         -> it reaches the NI; the loss is QM/EPP-side
		 *   intpt_rx flat, bm_tx up -> it leaves L2TM but no internal port accepts it
		 *   both flat               -> it never leaves the GPON MAC/PDC
		 */
		{
			/*
			 * The INTPT counters are read-and-clear with a single
			 * reader (cortina_ni_nihv_sample()), so these are
			 * totals since boot.
			 */
			u64 nihv[CA_NI_NIHV_CNT_COUNT];

			cortina_ni_nihv_sample(ni, nihv);
			dev_dbg(ni->dev,
				"DS-NI: intpt_rx(0xa92c)=%llu short(0xa928)=%llu miss_sop_eop(0xa924)=%llu intpt_tx(0xa940)=%llu xram_dma(0xaa4c)=%llu bm_tx(0x2140)=%u [read-and-clear registers, accumulated: totals since boot]\n",
				nihv[CA_NI_NIHV_INTPT_RX],
				nihv[CA_NI_NIHV_INTPT_SHORT_ERR],
				nihv[CA_NI_NIHV_INTPT_MISS_SOP_EOP],
				nihv[CA_NI_NIHV_INTPT_TX],
				nihv[CA_NI_NIHV_XRAM_DMA],
				readl(ni_base(ni) + CA_NI_L2TM_BM_TX_PCNT));
		}

		/* every non-zero (cpu_port, voq) EPP write/paddr slot */
		{
			char buf[224];
			int n = 0;
			unsigned int idx;

			for (idx = 0; idx < 64 && n < (int)sizeof(buf) - 24; idx++) {
				u32 wv = readl(ni_base(ni) + 0x7000 + idx * 4);
				u32 pv = readl(ni_base(ni) + 0x7200 + idx * 4);

				if (wv || pv)
					n += scnprintf(buf + n, sizeof(buf) - n,
						       " [p%u/q%u]w=%x,pa=%08x",
						       idx / 8, idx % 8, wv, pv);
			}
			dev_dbg(ni->dev, "DS-EPPSWEEP:%s\n", n ? buf : " ALL 64 SLOTS ZERO");
		}
	}
}
EXPORT_SYMBOL_GPL(cortina_ni_rx_delivery_dump);

void cortina_ni_pon_rx_hook_set(cortina_ni_pon_rx_fn fn)
{
	WRITE_ONCE(cortina_ni_pon_rx_cb, fn);
}
EXPORT_SYMBOL_GPL(cortina_ni_pon_rx_hook_set);

void cortina_ni_pon_oam_rx_hook_set(cortina_ni_pon_oam_rx_fn fn)
{
	WRITE_ONCE(cortina_ni_pon_oam_rx_cb, fn);
}
EXPORT_SYMBOL_GPL(cortina_ni_pon_oam_rx_hook_set);

/*
 * DS PON DATA (WAN) consumer: de-encapsulated data-GEM frames the PDC steers
 * to CPU port 0 arrive on this same EPP ring as plain Ethernet frames whose
 * HEADER_A.lspid = PON (7); the GPON driver registers its WAN netdev here so
 * they are delivered there instead of eth0 (LAN lspids are the NI ports
 * 0..6, so the test cannot steal a LAN frame).  Same store/load discipline
 * as the control-frame hook above.
 */
static struct net_device *cortina_ni_pon_wan_ndev;
static bool (*cortina_ni_pon_wan_rx_fixup)(struct sk_buff *skb);

/*
 * The GPON WAN netdev, and what the GPON driver does to each downstream frame
 * before it is received there (its VLAN service treatment).
 */
void cortina_ni_pon_wan_ndev_set(struct net_device *ndev,
				 bool (*rx_fixup)(struct sk_buff *skb))
{
	WRITE_ONCE(cortina_ni_pon_wan_rx_fixup, rx_fixup);
	WRITE_ONCE(cortina_ni_pon_wan_ndev, ndev);
}

/*
 * The OLT floods other subscribers' broadcasts down every ONU. A tagged frame
 * that no VLAN device of the WAN netdev takes is dropped by the stack anyway;
 * drop it here, before GRO, unless a tap or a bridge could still want it.
 */
static bool cortina_ni_pon_wan_foreign_vlan(struct net_device *wan,
					    const struct sk_buff *skb)
{
#if IS_ENABLED(CONFIG_VLAN_8021Q)
	const struct vlan_ethhdr *veth = (const struct vlan_ethhdr *)skb->data;
	bool foreign;
	u16 vid;

	if (skb->len < VLAN_ETH_HLEN || veth->h_vlan_proto != htons(ETH_P_8021Q) ||
	    (wan->flags & IFF_PROMISC) || rcu_access_pointer(wan->rx_handler))
		return false;
	vid = ntohs(veth->h_vlan_TCI) & VLAN_VID_MASK;
	if (!vid)
		return false;

	rcu_read_lock();
	foreign = !__vlan_find_dev_deep_rcu(wan, htons(ETH_P_8021Q), vid);
	rcu_read_unlock();
	return foreign;
#else
	return false;
#endif
}

static void cortina_ni_pon_wan_rx(struct napi_struct *napi,
				  struct net_device *wan, struct sk_buff *skb)
{
	bool (*fixup)(struct sk_buff *skb) = READ_ONCE(cortina_ni_pon_wan_rx_fixup);

	if ((fixup && fixup(skb)) || cortina_ni_pon_wan_foreign_vlan(wan, skb)) {
		wan->stats.rx_dropped++;
		dev_kfree_skb_any(skb);
		return;
	}
	skb->protocol = eth_type_trans(skb, wan);
	napi_gro_receive(napi, skb);
}
EXPORT_SYMBOL_GPL(cortina_ni_pon_wan_ndev_set);

/*
 * L3FE debug snapshot mux: write (tap_idx << 5) | word_idx to NE+0x30b8 and
 * read the word at NE+0x30bc; 32 words per tap point = the 128-byte HDR_I.
 * Tap 2 is HDR_I between STG1 and the T2 hash, i.e. the T2 lookup input.
 */
#define CA_NI_L3FE_DBG_ADDR		0x30b8
#define CA_NI_L3FE_DBG_DATA		0x30bc
#define CA_NI_L3FE_DBG_TAP_T2IN		2
#define CA_NI_HDRI_WORDS		32

/*
 * HDR_I bit offsets, LSB-first over the 128-byte little-endian descriptor:
 * bit n = word[n >> 5] bit (n & 31).  The L3/L4 offsets match the CN_HDRI_*
 * values used by cortina-ppe.c.
 */
#define HDRI_L4_DP		74	/* 16b dest L4 port */
#define HDRI_L4_SP		90	/* 16b src L4 port */
#define HDRI_L3_TOTLEN		122	/* 14b IP total length */
#define HDRI_L3_CK_ERR		136	/*  1b l3_chksum_err */
#define HDRI_L3_CKSUM		137	/* 16b IP header checksum */
#define HDRI_IP_DA0		233	/* 32b v4 DA (LSW of the 128b field) */
#define HDRI_IP_SA0		361	/* 32b v4 SA (LSW of the 128b field) */
#define HDRI_IP_L4_TYPE		489	/*  3b 0=UDP 1=TCP .. */
#define HDRI_IP_PROTO		492	/*  8b IP protocol */
#define HDRI_IP_IHL		500	/*  4b IHL (words) */
#define HDRI_IP_VER		509	/*  1b 0=IPv4 1=IPv6 */
#define HDRI_IP_VLD		510	/*  1b parsed an IP header */
/* the PPPoE offsets below predate the ip_ver/ip_vld fix and are unverified */
#define HDRI_PPP_PROTO_ENC	506	/*  4b ppp_protocol_enc */
#define HDRI_PPPOE_SESS		510	/* 16b pppoe_session_id */
#define HDRI_PPPOE_TYPE		526	/*  2b pppoe_type */
#define HDRI_INNER_1P		528	/*  3b inner 802.1p */
#define HDRI_TOP_1P		531	/*  3b top 802.1p */
#define HDRI_INNER_DEI		534	/*  1b */
#define HDRI_INNER_VID		535	/* 12b */
#define HDRI_INNER_TPID_ENC	547	/*  3b */
#define HDRI_TOP_DEI		550	/*  1b */
#define HDRI_TOP_VID		551	/* 12b */
#define HDRI_TOP_TPID_ENC	563	/*  3b */
#define HDRI_VLAN_CNT		566	/*  2b tags still ON the frame at T2 */
#define HDRI_ETYPE_ENC		585	/*  4b ethertype_enc */
#define HDRI_ETYPE		589	/* 16b ethertype (post-tag) */
#define HDRI_O_LSPID		816	/*  6b original lspid */
#define HDRI_LSPID		822	/*  6b lspid (post-LPB rewrite) */
#define HDRI_L4_OFFSET		897	/*  8b PE: L4 offset in the frame */
#define HDRI_L3_OFFSET		913	/*  8b PE: L3 offset in the frame */
#define HDRI_PKT_LEN		929	/* 14b PE: orig_packet_len */

/* software-decoded frame layering */
struct rx_stack_sw {
	u8	tags;			/* VLAN tags found (2 recorded) */
	u16	tpid[2], tci[2];	/* outermost first */
	u16	ethertype;		/* after the last tag */
	bool	pppoe;
	u16	pppoe_sess, ppp_proto;
	bool	ip, inner;		/* outer IPv4 seen / IP-in-IP seen */
	u8	ipver, ihl;		/* outer version, outer IHL bytes */
	u8	proto;			/* INNERMOST IPv4 protocol */
	u32	sa, da;			/* INNERMOST IPv4, host-order value */
	bool	l4;
	u16	sp, dp;			/* innermost L4 ports */
};
/* ------------------------------------------------------------------ */
/* multi-buffer receive: the SOP..EOP descriptor chain                  */
/* ------------------------------------------------------------------ */

/*
 * Where a frame with this HEADER_A is delivered: the GPON WAN netdev, or NULL
 * for eth0.  A de-encapsulated data-GEM frame carries lspid = PON; when the DS
 * data GEM is routed through the L3FE, a frame punted to the CPU carries
 * lspid = L3_WAN instead, because STG0's LPB profile has rewritten it.  Both
 * are WAN-only lspids, so neither can take a LAN frame.  NULL with *is_l3
 * untouched means no WAN lspid; NULL after a WAN lspid means no WAN netdev is
 * registered yet.  Both fall back to eth0.
 */
static inline struct net_device *cortina_ni_rx_wan_dest(u32 hdra_lo, bool *is_l3)
{
	u32 lspid = FIELD_GET(CA_NI_HDRA_W1_LSPID, hdra_lo);
	bool pon = (lspid == CA_NI_LSPID_PON);
	bool l3 = (lspid == CA_NI_LSPID_L3_WAN) &&
		  cortina_ppe_active();

	if (!pon && !l3)
		return NULL;
	*is_l3 = l3;
	return READ_ONCE(cortina_ni_pon_wan_ndev);
}

/* what one descriptor of a chain contributes */
struct ca_ni_chain_seg {
	u32	off;		/* payload offset inside this buffer */
	u32	len;		/* payload bytes to take from it */
	u32	dlen_expect;	/* what this descriptor's own pkt_size should read */
};

enum ca_ni_chain_act {
	CA_NI_CHAIN_OPEN,	/* SOP accepted, more segments expected */
	CA_NI_CHAIN_APPEND,	/* continuation accepted, more expected */
	CA_NI_CHAIN_DONE,	/* EOP accepted, the frame is complete */
	/* ---- malformations; the caller drops the partial frame for each ---- */
	CA_NI_CHAIN_ORPHAN,	/* continuation with no chain open on this voq */
	CA_NI_CHAIN_BADTOTAL,	/* SOP pkt_size out of range, or no room in the window */
	CA_NI_CHAIN_TOOLONG,	/* segment cap hit, or the segments overran total */
	CA_NI_CHAIN_SHORT,	/* EOP with fewer bytes than pkt_size promised */
};

/*
 * The chain state machine, as pure arithmetic: no skb, no MMIO, no netdev.
 * Given this descriptor's SOP bit, the SOP buffer's HEADER_A word, the usable
 * window of the buffer and where a continuation's payload starts, decide what
 * this segment contributes and whether the chain is still well formed.
 *
 * The caller must have dropped any previously held partial frame when @st->open
 * is true and @sop is set; the SOP branch re-initialises the accounting
 * unconditionally, so an unterminated chain cannot outlive the next frame on
 * the same voq.
 *
 * Every malformation return leaves @st dirty on purpose; the caller resets it.
 *
 * Each segment contributes min(bytes still owed, usable window), as the vendor
 * firmware does; the window is derived from the buffer's usable end rather
 * than a literal 1600.  @out->dlen_expect records the descriptor length for
 * the caller's statistics only.
 */
static enum ca_ni_chain_act
ca_ni_chain_step(struct ca_ni_chain_state *st, bool sop, bool eop,
		 u32 hdra_hi, u32 dlen, u32 buf_end, u32 rest_off,
		 struct ca_ni_chain_seg *out)
{
	u32 off, hdr, avail, want, len;

	out->off = out->len = out->dlen_expect = 0;

	if (sop) {
		/*
		 * The SOP descriptor's dlen (counted from HEADER_A at +0x40)
		 * minus the header block is the whole-frame length.  Each buffer
		 * holds one fill window of it, so copy min(window, remaining) per
		 * buffer and complete the frame when nothing remains; the EOP bit
		 * is not consulted.
		 */
		off = CA_NI_RX_FRAME_OFF;
		if (hdra_hi & CA_NI_HDRA_W0_CPU_FLG)
			off += CA_NI_RX_HDR_CPU_LEN;
		hdr = off - CA_NI_RX_HDRA_OFF;
		if (off >= buf_end || dlen <= hdr)
			return CA_NI_CHAIN_BADTOTAL;
		st->total = dlen - hdr;			/* whole-frame length */
		if (st->total < (u32)ETH_HLEN ||
		    st->total > CA_NI_RX_CHAIN_MAX_LEN)
			return CA_NI_CHAIN_BADTOTAL;
		st->got = 0;
		st->segs = 0;
		st->open = true;
	} else {
		if (!st->open)
			return CA_NI_CHAIN_ORPHAN;
		off = rest_off;
		if (off >= buf_end)
			return CA_NI_CHAIN_BADTOTAL;
	}

	if (++st->segs > CA_NI_RX_CHAIN_MAX_SEGS)
		return CA_NI_CHAIN_TOOLONG;

	/*
	 * Take up to the buffer's usable window from @off (frame at +0x48, or
	 * +0x50 with a CPU header on the SOP; payload at rest_off on a
	 * continuation), bounded by the bytes the frame still owes.  Only the
	 * window bytes were DMA'd into this buffer.
	 */
	avail = buf_end - off;
	want = st->total - st->got;
	len = min(want, avail);
	out->off = off;
	out->len = len;
	out->dlen_expect = dlen;
	st->got += len;

	if (st->got >= st->total) {
		/* remaining == 0 -> the frame is complete (EOP bit not needed) */
		st->open = false;
		return CA_NI_CHAIN_DONE;
	}
	return sop ? CA_NI_CHAIN_OPEN : CA_NI_CHAIN_APPEND;
}

/*
 * free a held partial frame without touching the accounting (the SOP path has
 * already re-initialised it for the new chain)
 */
static void cortina_ni_rx_chain_free(struct cortina_ni_rx *rx,
				     struct cortina_ni_rx_chain *ch)
{
	if (!ch->skb)
		return;
	dev_kfree_skb_any(ch->skb);
	ch->skb = NULL;
	rx->chain_abort++;
}

/* abandon a chain entirely: free the partial frame and clear the accounting */
static void cortina_ni_rx_chain_reset(struct cortina_ni_rx *rx,
				      struct cortina_ni_rx_chain *ch)
{
	cortina_ni_rx_chain_free(rx, ch);
	memset(&ch->st, 0, sizeof(ch->st));
}

#if IS_ENABLED(CONFIG_CORTINA_NI_DSA)
/*
 * Tag a HEADER_A LAN frame with its port (HEADER_A.lspid): a 2-byte tail tag
 * the DSA tagger strips, as for TX.  A headerless (swid) frame has no lspid
 * and stays on the conduit.  The skb is allocated with room for the tag.
 */
static inline void cortina_ni_dsa_rx_tag(struct cortina_ni *ni,
					 struct sk_buff *skb, u32 hdra_lo)
{
	u32 lspid = FIELD_GET(CA_NI_HDRA_W1_LSPID, hdra_lo);
	u8 *tag;

	if (!ni->ds || lspid >= CA_NI_DSA_USER_PORTS)
		return;
	tag = skb_put(skb, CA_NI_DSA_TAG_LEN);
	tag[CA_NI_DSA_TAG_PORT_OFF] = CA_NI_DSA_TAG_MAGIC | lspid;
	tag[1] = 0;
}
#endif

/*
 * The descriptor's l4_csum_err bit is board-tested only for untagged,
 * unfragmented IPv4 TCP, single-buffer and chained: it was set for every
 * frame with a wrong TCP checksum and clear for every correct one. Trust a
 * clear bit for that case alone.
 * skb->protocol is ETH_P_XDSA on the DSA conduit; the tag sits at the tail,
 * so the Ethernet type still names the payload.
 */
static void cortina_ni_rx_csum(struct cortina_ni_rx *rx, struct sk_buff *skb,
			       bool csum_err)
{
	const struct iphdr *iph;

	if (!(skb->dev->features & NETIF_F_RXCSUM) || csum_err ||
	    eth_hdr(skb)->h_proto != htons(ETH_P_IP) ||
	    !pskb_may_pull(skb, sizeof(*iph)))
		return;

	iph = (const struct iphdr *)skb->data;
	if (iph->ihl == 5 && iph->protocol == IPPROTO_TCP && !ip_is_fragment(iph)) {
		skb->ip_summed = CHECKSUM_UNNECESSARY;
		rx->csum_ok++;
	}
}

/*
 * Consume one descriptor of a multi-buffer frame, for a descriptor that is not
 * a self-contained frame.
 *
 * Always returns the PA to recycle - on delivery, on a malformation, on an
 * allocation failure.  Each segment is copied out before the batch push at the
 * end of the poll, so every buffer of a chain is handed back exactly once.
 */
static noinline u32 cortina_ni_rx_chain_seg(struct cortina_ni *ni,
					    struct napi_struct *napi,
					    unsigned int voq, u64 desc,
					    const u8 *buf, u32 buf_end, u32 rpa)
{
	struct cortina_ni_rx *rx = ni->rx;
	struct cortina_ni_rx_chain *ch = &rx->chain[voq];
	struct net_device *ndev = rx->netdev;
	u32 lo = lower_32_bits(desc);
	bool sop = !!(lo & CA_NI_RX_DESC_SOP);
	bool eop = !!(lo & CA_NI_RX_DESC_EOP);
	u32 dlen = FIELD_GET(CA_NI_RX_DESC_LEN, desc);
	u32 hdra_hi = 0, hdra_lo = 0;
	struct ca_ni_chain_seg seg;
	enum ca_ni_chain_act act;
	struct net_device *wan;
	bool is_l3 = false;

	/*
	 * The headerless (sw_id != 0) format's chain geometry is unknown: its
	 * frame starts at +0x10, inside the head_room the chain arithmetic
	 * assumes.  Drop it and count it.
	 */
	if (unlikely(FIELD_GET(CA_NI_RX_DESC_SWID, desc))) {
		rx->chain_swid++;
		ndev->stats.rx_errors++;
		net_warn_ratelimited("%s: RX chain: headerless (sw_id) segment, geometry unknown - dropped\n",
				     netdev_name(ndev));
		return rpa;
	}

	if (sop) {
		hdra_hi = get_unaligned_be32(buf + CA_NI_RX_HDRA_OFF);
		hdra_lo = get_unaligned_be32(buf + CA_NI_RX_HDRA_OFF + 4);
		rx->lspid_cnt[FIELD_GET(CA_NI_HDRA_W1_LSPID, hdra_lo) & 0xf]++;
		/*
		 * a chain still open here never got its EOP: drop the partial
		 * frame and let this SOP start over
		 */
		if (unlikely(ch->st.open)) {
			cortina_ni_rx_chain_free(rx, ch);
			rx->chain_reopen++;
			ndev->stats.rx_errors++;
		}
		ch->hdra_lo = hdra_lo;
		ch->csum_err = false;
	}
	/* not known which segment the hardware flags: trust none if any is */
	if (lo & CA_NI_RX_DESC_CSUM_ERR)
		ch->csum_err = true;

	act = ca_ni_chain_step(&ch->st, sop, eop, hdra_hi, dlen, buf_end,
			       rx->chain_rest_off, &seg);

	switch (act) {
	case CA_NI_CHAIN_OPEN:
	case CA_NI_CHAIN_APPEND:
	case CA_NI_CHAIN_DONE:
		break;
	case CA_NI_CHAIN_ORPHAN:
		rx->chain_orphan++;
		ndev->stats.rx_errors++;
		net_warn_ratelimited("%s: RX chain: continuation with no chain open on voq %u (desc %016llx)\n",
				     netdev_name(ndev), voq, desc);
		return rpa;
	case CA_NI_CHAIN_BADTOTAL:
		rx->chain_badtotal++;
		goto drop;
	case CA_NI_CHAIN_TOOLONG:
		rx->chain_toolong++;
		goto drop;
	case CA_NI_CHAIN_SHORT:
		rx->chain_short++;
		goto drop;
	}

	/*
	 * The per-segment descriptor length is recorded, not judged: the vendor
	 * firmware ignores it, so there is no reference for what the hardware
	 * writes there.  Store it beside the derived value for /proc;
	 * chain_dlen_diff only says whether they ever disagreed.
	 */
	if (ch->st.segs <= CA_NI_RX_CHAIN_MAX_SEGS) {
		rx->chain_dlen_seen[ch->st.segs - 1] = dlen;
		rx->chain_dlen_calc[ch->st.segs - 1] = seg.dlen_expect;
	}
	if (dlen != seg.dlen_expect)
		rx->chain_dlen_diff++;

	if (sop) {
		/*
		 * One allocation per chain, sized to the frame total from the
		 * SOP; ca_ni_chain_step() never lets st->got exceed it.
		 * napi_alloc_skb() falls back to kmalloc for a large request;
		 * on failure the chain is dropped below.
		 */
		ch->skb = napi_alloc_skb(napi, ch->st.total + CA_NI_DSA_TAG_LEN);
		if (unlikely(!ch->skb)) {
			rx->drop_nobuf++;
			ndev->stats.rx_dropped++;
			memset(&ch->st, 0, sizeof(ch->st));
			return rpa;
		}
	} else if (unlikely(!ch->skb)) {
		/*
		 * accounting says a chain is open but the allocation failed
		 * earlier: swallow the rest of it quietly
		 */
		if (eop)
			memset(&ch->st, 0, sizeof(ch->st));
		return rpa;
	}

	if (unlikely(skb_tailroom(ch->skb) < (int)seg.len)) {
		/*
		 * cannot happen given the CA_NI_RX_CHAIN_MAX_LEN cap, but a torn
		 * skb must never overrun memory - drop the chain
		 */
		rx->chain_toolong++;
		cortina_ni_rx_chain_free(rx, ch);
		ndev->stats.rx_errors++;
		return rpa;
	}
	skb_put_data(ch->skb, buf + seg.off, seg.len);

	if (act != CA_NI_CHAIN_DONE)
		return rpa;

	/*
	 * Complete frame: same delivery decision as the single-buffer path.
	 * A PON control frame is never chained (an OMCI PDU is tens of bytes),
	 * so the 0xfff1 link type is not re-tested here.
	 */
	{
		struct sk_buff *skb = ch->skb;
		u32 segs = ch->st.segs;
		/*
		 * take the frame length before the skb is handed over:
		 * eth_type_trans() pulls the Ethernet header and
		 * napi_gro_receive() consumes it
		 */
		u32 flen = ch->st.total;

		/*
		 * The QM can append another frame's continuation behind a frame
		 * that already ends at its own IP length.  GRO on such an
		 * oversized skb stalls the TCP stream, so trim an IPv4 frame to
		 * its own L2 + IP length.  This only ever shrinks the skb; a
		 * genuine multi-buffer frame has skb->len == 14 + ip_tot.
		 */
		if (skb->len >= 34) {
			const u8 *dd = skb->data;

			if (dd[12] == 0x08 && dd[13] == 0x00) {
				u32 iptot = ((u32)dd[16] << 8) | dd[17];

				if (iptot >= (u32)(sizeof(struct iphdr)) &&
				    14u + iptot < skb->len) {
					skb_trim(skb, 14u + iptot);
					flen = skb->len;
				}
			}
		}

		ch->skb = NULL;
		memset(&ch->st, 0, sizeof(ch->st));
		rx->chain_frames++;
		rx->chain_segs += segs;
		if (segs > rx->chain_max_segs)
			rx->chain_max_segs = segs;
		wan = cortina_ni_rx_wan_dest(ch->hdra_lo, &is_l3);
		if (wan) {
			if (is_l3)
				rx->wan_l3_frames++;
			else
				rx->wan_frames++;
			wan->stats.rx_packets++;
			wan->stats.rx_bytes += flen;
			cortina_ni_pon_wan_rx(napi, wan, skb);
			return rpa;
		}
		/*
		 * chain frames always carry HEADER_A (headerless swid segments
		 * are dropped above), so ch->hdra_lo has a valid lspid
		 */
		cortina_ni_dsa_rx_tag(ni, skb, ch->hdra_lo);
		skb->protocol = eth_type_trans(skb, ndev);
		cortina_ni_rx_csum(rx, skb, ch->csum_err);
		napi_gro_receive(napi, skb);
		ndev->stats.rx_packets++;
		ndev->stats.rx_bytes += flen;
		rx->frames++;
		rx->bytes += flen;
	}
	return rpa;

drop:
	ndev->stats.rx_errors++;
	net_warn_ratelimited("%s: RX chain: malformed (act %d, segs %u got %u of %u) - partial frame dropped\n",
			     netdev_name(ndev), act, ch->st.segs, ch->st.got,
			     ch->st.total);
	cortina_ni_rx_chain_reset(rx, ch);
	return rpa;
}

/*
 * Consume one CPU-EPP descriptor: copy the frame out of its pool buffer into
 * a fresh skb and deliver it.  Returns the buffer's PA for the caller to hand
 * back to its EQ free list, or 0 if the buffer is not ours to return.
 */
static u32 cortina_ni_rx_frame(struct cortina_ni *ni, struct napi_struct *napi,
			       unsigned int voq, u64 desc)
{
	struct cortina_ni_rx *rx = ni->rx;
	struct net_device *ndev = rx->netdev;
	u32 pa = lower_32_bits(desc) & CA_NI_RX_DESC_PA;
	u32 dlen = FIELD_GET(CA_NI_RX_DESC_LEN, desc);
	u32 swid = FIELD_GET(CA_NI_RX_DESC_SWID, desc);
	u32 base = lower_32_bits(rx->cpu_dram_dma);
	u32 hdra_hi = 0, hdra_lo = 0, off_in_region, buf_max;
	u32 rpa;	/* the PA to hand back, or 0 if this buffer is not ours */
	struct sk_buff *skb;
	bool pp_buf = false;
	u8 *buf;
	unsigned int off;
	int len;

	/* a deep-queue cell in NE SRAM: hardware-owned, mapped uncached */
	if (rx->dq_sram && pa >= rx->dq_sram_pa &&
	    pa - rx->dq_sram_pa <= rx->dq_sram_size - CA_NI_RX_DQ_SRAM_CELL) {
		buf = (u8 *)rx->dq_sram + (pa - rx->dq_sram_pa);
		buf_max = CA_NI_RX_DQ_SRAM_CELL;
		rpa = 0;
		rx->dq_sram_segs++;
		goto have_buf;
	}

	buf_max = CA_NI_RX_BUF_USABLE_END(CA_NI_RX_CPU_POOL0_BUFSZ);

	/* a CPU-pool buffer from the page pool, owned by us since the push */
	if ((pa < base || pa >= base + CA_NI_RX_MAP_SIZE) &&
	    likely(pfn_valid(PHYS_PFN(pa))) &&
	    likely(page_pool_page_is_pp(compound_head(pfn_to_page(PHYS_PFN(pa)))))) {
		buf = phys_to_virt(pa);
		dma_sync_single_for_cpu(ni->dev, pa, buf_max, DMA_FROM_DEVICE);
		rpa = pa;
		pp_buf = true;
		goto have_buf;
	}

	/*
	 * otherwise bufPA (128B-aligned, low 7 bits are flags) must land in the
	 * deep-queue pool of the reserved region
	 */
	if (unlikely(pa < base || pa >= base + CA_NI_RX_MAP_SIZE)) {
		rx->drop_badpa++;
		ndev->stats.rx_errors++;
		net_err_ratelimited("%s: RX desc %016llx: PA outside CPU pool\n",
				    netdev_name(ndev), desc);
		return 0;	/* unknown PA: cannot safely recycle it */
	}
	off_in_region = pa - base;
	buf = (u8 *)rx->cpu_dram + off_in_region;
	/*
	 * One buffer's span, and who owns it.  EQ5 (pool0) below POOL0_BYTES,
	 * EQ6 (pool1) above it, and the deep-queue pool (EQ12) above both.
	 *
	 * A CPU-pool PA is returned to its free list once the frame has been
	 * copied out.  The deep-queue pool is hardware-managed, so its buffers
	 * are not ours to return (rpa = 0): pushing one into a CPU pool's free
	 * list would put the same buffer in two allocators.
	 *
	 * buf_max is the end of the buffer's usable payload window, not the
	 * buffer size: the frame DMA never writes the CA_NI_RX_BUF_TAILROOM the
	 * QM reserves at the back.  See CA_NI_RX_BUF_USABLE_END.
	 */
	if (off_in_region < CA_NI_RX_DQ_POOL_OFF) {
		rpa = pa;
	} else {
		rpa = 0;				/* hardware-managed */
		rx->dq_frames++;
	}

	/*
	 * The NE writes the frame by DMA without snooping the CPU caches and the
	 * pool is mapped cacheable, so drop whatever lines of this buffer the CPU
	 * holds before reading it.  The CPU never writes to the pool, so no line
	 * is dirty and nothing can be written back over the frame.
	 */
	dcache_inval_poc((unsigned long)buf, (unsigned long)buf + buf_max);

have_buf:
	/*
	 * Multi-buffer receive: a descriptor that is not a self-contained frame
	 * (SOP without EOP, or neither) belongs to a chain.
	 */

	if (unlikely(lower_32_bits(desc) & CA_NI_RX_DESC_CSUM_ERR))
		rx->csum_err++;

	if (unlikely((lower_32_bits(desc) & (CA_NI_RX_DESC_SOP |
					     CA_NI_RX_DESC_EOP)) !=
		     (CA_NI_RX_DESC_SOP | CA_NI_RX_DESC_EOP)))
		return cortina_ni_rx_chain_seg(ni, napi, voq, desc, buf, buf_max,
					       rpa);

	if (unlikely(!(lower_32_bits(desc) & CA_NI_RX_DESC_SOP))) {
		/* SOP-less descriptor = jumbo continuation or desync; drop */
		rx->drop_nosop++;
		ndev->stats.rx_errors++;
		return rpa;
	}

	if (likely(!swid)) {
		/*
		 * normal frame: HEADER_A at +0x40 = two big-endian 32-bit
		 * words; frame length = HEADER_A.pkt_size
		 */
		hdra_hi = get_unaligned_be32(buf + CA_NI_RX_HDRA_OFF);
		hdra_lo = get_unaligned_be32(buf + CA_NI_RX_HDRA_OFF + 4);
		rx->lspid_cnt[FIELD_GET(CA_NI_HDRA_W1_LSPID, hdra_lo) & 0xf]++;

		len = FIELD_GET(CA_NI_HDRA_W1_PKT_SIZE, hdra_lo);

		/*
		 * The descriptor's pktlen (written by the EPP writeback engine)
		 * and HEADER_A.pkt_size (written by the RMU frame DMA) describe
		 * the same frame and differ by exactly the 8-byte HEADER_A.  A
		 * mismatch means the buffer no longer holds the frame this
		 * descriptor was written for.  Counted and logged, not dropped.
		 */
		if (unlikely(dlen != len + (CA_NI_RX_FRAME_OFF -
					    CA_NI_RX_HDRA_OFF))) {
			rx->stale_buf++;
			net_warn_ratelimited("%s: RX stale buffer: desc pktlen=%u vs HEADER_A.pkt_size=%d at pa=%08x (buffer reused before the copy)\n",
					     netdev_name(ndev), dlen, len, pa);
		}

		off = CA_NI_RX_FRAME_OFF;
		if (hdra_hi & CA_NI_HDRA_W0_CPU_FLG) {
			off += CA_NI_RX_HDR_CPU_LEN;
			len -= CA_NI_RX_HDR_CPU_LEN;
		}
	} else {
		/*
		 * headerless format (nonzero sw_id, WiFi-FF style):
		 * frame at +0x10, length from the descriptor
		 */
		rx->swid_frames++;
		off = CA_NI_RX_DESC_HDR_LEN;
		len = (int)dlen - CA_NI_RX_DESC_HDR_LEN;
	}

	if (unlikely(len < (int)ETH_HLEN || off + len > buf_max)) {
		rx->drop_len++;
		/*
		 * a runt is a bad frame; an oversize one is a good frame that
		 * does not fit a buffer's usable window
		 */
		if (len < (int)ETH_HLEN)
			rx->drop_runt++;
		else
			rx->drop_oversize++;
		ndev->stats.rx_length_errors++;
		ndev->stats.rx_errors++;
		return rpa;
	}

	/*
	 * DS PON control frame (GPON OMCI): the PDC steers the OMCC DS GEM to
	 * CPU port 0 with a HW-prepended 16-byte PON header — DA 00:13:25:00:
	 * 00:00, SA 00:13:25:00:00:01, ethertype bytes [12:13] = 0xff,0xf1
	 * (vendor CA_PUC_GLOBAL_LNK_TYPE; 0xfff0 = PLOAM/MPCP, which on this
	 * silicon never reaches the CPU — the GPON MAC handles PLOAM in HW).
	 * Strip the header and hand the OMCI PDU to the GPON driver; these
	 * frames never enter the network stack (vendor ca_ni_rx_fill_pkt
	 * does the same data += 16 / len -= 16).  Ethernet frames cannot
	 * false-match: 0xfff1 is not a real ethertype on this LAN.
	 */
	if (unlikely(buf[off + 12] == 0xff && buf[off + 13] == 0xf1)) {
		cortina_ni_pon_oam_rx_fn oam_fn = READ_ONCE(cortina_ni_pon_oam_rx_cb);
		cortina_ni_pon_rx_fn fn = READ_ONCE(cortina_ni_pon_rx_cb);

		rx->pon_frames++;
		if (oam_fn && len > 16 + 14 &&
		    buf[off + 16 + 12] == 0x88 &&
		    buf[off + 16 + 13] == 0x09 &&
		    buf[off + 16 + 14] == 0x03) {
			oam_fn(buf + off + 16, len - 16, buf[off + 11] & 0x3f);
		} else if (fn && len > 16) {
			fn(buf + off + 16, len - 16);
		}
		return rpa;
	}

	/*
	 * DS PON data (WAN): a de-encapsulated data-GEM frame carries
	 * HEADER_A.lspid = PON (7), the lspid our PDC map entry stamps, while
	 * LAN frames carry an NI-port lspid (0..6) and the headerless swid
	 * format parses lspid 0.  Deliver it to the GPON WAN netdev.
	 *
	 * With HW L3 forwarding the DS data GEM is routed through the L3FE
	 * (ldpid L3_WAN); a frame that misses the T2 hash is punted to CPU_0 by
	 * the CLS default action after STG0's LPB profile has rewritten its
	 * lspid PON -> L3_WAN.  It goes to the same WAN netdev, so DHCP/ICMP to
	 * the router still reaches gpon0.  L3_WAN is a WAN-only lspid and this
	 * is gated on the armed engine.
	 */
	if (unlikely(!swid)) {
		/* shared with the chain path, see cortina_ni_rx_wan_dest() */
		bool is_l3wan = false;
		struct net_device *wan = cortina_ni_rx_wan_dest(hdra_lo,
								&is_l3wan);

		if (wan) {
			if (is_l3wan)
				rx->wan_l3_frames++;
			else
				rx->wan_frames++;
			skb = cortina_ni_rx_skb(rx, napi, buf, pp_buf, off, len,
						&rpa);
			if (unlikely(!skb)) {
				rx->drop_nobuf++;
				wan->stats.rx_dropped++;
				return rpa;
			}
			cortina_ni_pon_wan_rx(napi, wan, skb);
			wan->stats.rx_packets++;
			wan->stats.rx_bytes += len;
			return rpa;
		}
		/* no WAN netdev registered: fall through to eth0 */
	}

	skb = cortina_ni_rx_skb(rx, napi, buf, pp_buf, off, len, &rpa);
	if (unlikely(!skb)) {
		rx->drop_nobuf++;
		ndev->stats.rx_dropped++;
		return rpa;
	}
	if (likely(!swid))
		cortina_ni_dsa_rx_tag(ni, skb, hdra_lo);
	skb->protocol = eth_type_trans(skb, ndev);
	cortina_ni_rx_csum(rx, skb, lower_32_bits(desc) & CA_NI_RX_DESC_CSUM_ERR);
	napi_gro_receive(napi, skb);
	ndev->stats.rx_packets++;
	ndev->stats.rx_bytes += len;
	rx->frames++;
	rx->bytes += len;
	return rpa;
}

/* drain one voq's CPU-EPP ring; returns work done, updates rx->rptr[voq] */
static int cortina_ni_rx_poll_voq(struct cortina_ni *ni,
				  struct napi_struct *napi, unsigned int voq,
				  int budget)
{
	struct cortina_ni_rx *rx = ni->rx;
	__le64 *vring = rx->ring +
		(voq / CA_NI_RX_VOQ_COUNT) * (CA_NI_RX_PORT_STRIDE / CA_NI_RX_DESC_SIZE) +
		(voq % CA_NI_RX_VOQ_COUNT) * CA_NI_RX_RING_SLOTS_PER_VOQ;
	u32 recycle[CA_NI_RX_RECYCLE_MAX];
	unsigned int nrecycle = 0, i;
	u32 rptr = rx->rptr[voq];
	u32 wptr;
	int work = 0;

	/*
	 * Never consume more buffers in one poll than can be handed back.  This
	 * clamp can cut a chain in half, which is why the chain state is kept
	 * per voq across polls.
	 */
	if (budget > CA_NI_RX_RECYCLE_MAX)
		budget = CA_NI_RX_RECYCLE_MAX;

	wptr = cortina_ni_rx_wptr_voq(ni, voq);
	dma_rmb();	/* descriptor reads after the pointer read */

	while (work < budget) {
		u64 desc;
		u32 pa;

		if (rptr == wptr) {
			wptr = cortina_ni_rx_wptr_voq(ni, voq);
			dma_rmb();
			if (rptr == wptr)
				break;
		}

		desc = le64_to_cpu(READ_ONCE(vring[rptr / CA_NI_RX_DESC_SIZE]));
		WRITE_ONCE(vring[rptr / CA_NI_RX_DESC_SIZE], 0);
		rptr = (rptr + CA_NI_RX_DESC_SIZE) & (CA_NI_RX_RING_BYTES - 1);
		work++;

		pa = cortina_ni_rx_frame(ni, napi, voq, desc);
		if (pa)
			/*
			 * Return the buffer to the pool the descriptor names
			 * (desc[3:0]), packed as the doorbell encodes it: the
			 * PA is 128-byte aligned, so the low nibble holds the
			 * eqid.
			 */
			recycle[nrecycle++] = (pa & CA_NI_QM_PUSH_ADDR) |
					      (u32)(desc & CA_NI_RX_DESC_EQID);
	}

	dma_wmb();	/* barrier before the rdptr store */
	writel(rptr, ni_base(ni) +
	       CA_NI_QM_EPP64_RDPTR(voq / CA_NI_RX_VOQ_COUNT,
				    voq % CA_NI_RX_VOQ_COUNT));
	rx->rptr[voq] = rptr;
	rx->voq_frames[voq % CA_NI_RX_VOQ_COUNT] += work;

	/*
	 * Return the consumed buffers after the read pointer, every frame
	 * having been copied out.  They go through a deferred-recycle FIFO
	 * first: the oldest are pushed back to the QM until only
	 * CA_NI_RX_DEFER_KEEP remain held, so a just-read buffer is not handed
	 * straight back out of the LIFO free list.
	 */
	spin_lock(&rx->recycle_lock);
	{
		u32 keep = CA_NI_RX_DEFER_KEEP;

		if (keep > CA_NI_RX_DEFER_DEPTH - CA_NI_RX_RECYCLE_MAX - 1)
			keep = CA_NI_RX_DEFER_DEPTH - CA_NI_RX_RECYCLE_MAX - 1;
		for (i = 0; i < nrecycle; i++) {
			rx->defer_ring[(rx->defer_head + rx->defer_count) &
				       (CA_NI_RX_DEFER_DEPTH - 1)] = recycle[i];
			if (rx->defer_count < CA_NI_RX_DEFER_DEPTH)
				rx->defer_count++;
			else	/* ring full (only if keep mis-set): drop oldest below */
				rx->defer_head = (rx->defer_head + 1) &
						 (CA_NI_RX_DEFER_DEPTH - 1);
		}
		while (rx->defer_count > keep) {
			u32 dpa = rx->defer_ring[rx->defer_head];

			if (unlikely(cortina_ni_rx_push_buf(ni, dpa & CA_NI_QM_PUSH_EQID, dpa,
							    CA_NI_RX_PUSH_TIMEOUT_NAPI_US))) {
				rx->push_fail++;
				net_err_ratelimited("%s: RX pool push timeout for pa=%08x - buffer lost, pool will shrink\n",
						    netdev_name(rx->netdev), dpa);
				break;	/* QM not ready: retry these next poll */
			}
			rx->defer_head = (rx->defer_head + 1) &
					 (CA_NI_RX_DEFER_DEPTH - 1);
			rx->defer_count--;
		}
	}
	spin_unlock(&rx->recycle_lock);
	return work;
}

static struct napi_struct *cortina_ni_rx_voq_napi(struct cortina_ni_rx *rx,
						 unsigned int voq)
{
	return voq == CA_NI_RX_WAN_VOQ ? &rx->napi_wan : &rx->napi;
}

/*
 * Both NAPIs share the port-0 interrupt: unmask it only once neither is
 * scheduled. Each clears its own state in napi_complete_done() before looking
 * at the other, so the last one to finish always gets here with both idle.
 */
static void cortina_ni_rx_rearm(struct cortina_ni *ni)
{
	struct cortina_ni_rx *rx = ni->rx;
	unsigned int voq;

	if (napi_is_scheduled(&rx->napi) || napi_is_scheduled(&rx->napi_wan))
		return;

	for (voq = 1; voq < CA_NI_RX_NUM_IRQS; voq++)
		if (test_and_clear_bit(voq, &rx->irq_off))
			enable_irq(rx->irq[voq]);
	cortina_ni_rx_irq_set(ni, true);
	/* close the enable-vs-new-frame race across all voqs */
	for (voq = 0; voq < CA_NI_RX_RING_COUNT; voq++)
		if (cortina_ni_rx_wptr_voq(ni, voq) != READ_ONCE(rx->rptr[voq]) &&
		    napi_schedule(cortina_ni_rx_voq_napi(rx, voq)))
			cortina_ni_rx_irq_set(ni, false);
}

static int cortina_ni_rx_poll(struct napi_struct *napi, int budget)
{
	struct cortina_ni_rx *rx =
		container_of(napi, struct cortina_ni_rx, napi);
	struct cortina_ni *ni = rx->ni;
	unsigned int voq;
	int work = 0;

	rx->polls++;

	/*
	 * Drain the CPU-port VOQs (a frame may land on any of them by
	 * priority) except the WAN one, round-robin within the budget.
	 */
	for (voq = 0; voq < CA_NI_RX_RING_COUNT && work < budget; voq++)
		if (voq != CA_NI_RX_WAN_VOQ)
			work += cortina_ni_rx_poll_voq(ni, napi, voq,
						       budget - work);

	if (work < budget && napi_complete_done(napi, work))
		cortina_ni_rx_rearm(ni);
	return work;
}

static int cortina_ni_rx_poll_wan(struct napi_struct *napi, int budget)
{
	struct cortina_ni_rx *rx =
		container_of(napi, struct cortina_ni_rx, napi_wan);
	struct cortina_ni *ni = rx->ni;
	int work;

	work = cortina_ni_rx_poll_voq(ni, napi, CA_NI_RX_WAN_VOQ, budget);
	if (work < budget && napi_complete_done(napi, work))
		cortina_ni_rx_rearm(ni);
	return work;
}

/* ------------------------------------------------------------------ */
/* RX steering: the full forwarding-engine path                         */
/* ------------------------------------------------------------------ */

/*
 * Program one PLE default-forward table entry: the lookup-miss action @val
 * for traffic type @type of @lspid (indirect read-modify-write).
 */
static int cortina_ni_rx_ple_dft_fwd(struct cortina_ni *ni, u32 lspid,
				     u32 type, u32 val)
{
	void __iomem *acc = ni_base(ni) + CA_NI_PLE_DFT_FWD_ACCESS;
	u32 addr = lspid << 2 | type;
	u32 v;
	int ret;

	writel(val, ni_base(ni) + CA_NI_PLE_DFT_FWD_DATA);
	writel(CA_NI_PLE_ACCESS_GO | CA_NI_PLE_ACCESS_WRITE | addr, acc);
	ret = readl_poll_timeout(acc, v, !(v & CA_NI_PLE_ACCESS_GO),
				 CA_NI_TX_POLL_US, CA_NI_TX_POLL_TIMEOUT_US);
	if (ret)
		return ret;

	/* GO also clears for an index past the table: read the entry back */
	writel(CA_NI_PLE_ACCESS_GO | addr, acc);
	ret = readl_poll_timeout(acc, v, !(v & CA_NI_PLE_ACCESS_GO),
				 CA_NI_TX_POLL_US, CA_NI_TX_POLL_TIMEOUT_US);
	if (ret)
		return ret;
	v = readl(ni_base(ni) + CA_NI_PLE_DFT_FWD_DATA);
	if (v != val) {
		dev_err(ni->dev,
			"PLE dft-fwd entry %#x (lspid %u type %u) read back %#010x, wrote %#010x\n",
			addr, lspid, type, v, val);
		return -EIO;
	}
	return 0;
}

/*
 * Generic indirect-table store: write ACCESS = GO|WR|idx and poll GO clear.
 * Bounded and non-fatal.
 */

static void cortina_ni_rx_ind_store(struct cortina_ni *ni, u32 access_reg, unsigned int idx)
{
	void __iomem *acc = ni_base(ni) + access_reg;
	u32 v;

	writel(CA_NI_IND_ACCESS_GO | CA_NI_IND_ACCESS_WR | idx, acc);
	if (readl_poll_timeout(acc, v, !(v & CA_NI_IND_ACCESS_GO),
			       CA_NI_TX_POLL_US, CA_NI_TX_POLL_TIMEOUT_US))
		dev_warn(ni->dev, "indirect store @0x%x[%u] GO stuck (0x%08x)\n",
			 access_reg, idx, v);
}

/*
 * Generic indirect-table read: write ACCESS = GO|idx (rbw=0) and poll GO clear;
 * the entry is then latched in the DATA registers.  Bounded and non-fatal.
 */
static void cortina_ni_rx_ind_read(struct cortina_ni *ni, u32 access_reg, unsigned int idx)
{
	void __iomem *acc = ni_base(ni) + access_reg;
	u32 v;

	writel(CA_NI_IND_ACCESS_GO | idx, acc);
	if (readl_poll_timeout(acc, v, !(v & CA_NI_IND_ACCESS_GO),
			       CA_NI_TX_POLL_US, CA_NI_TX_POLL_TIMEOUT_US))
		/* ratelimited: /proc show paths call this in a loop */
		dev_warn_ratelimited(ni->dev,
				     "indirect read @0x%x[%u] GO stuck (0x%08x)\n",
				     access_reg, idx, v);
}

/*
 * MMSHP: the logical destination ports @lspid may forward to.  Everything but
 * itself, except that a LAN port reaches only the LAN ports in lan_fwd (its
 * bridge): misses, own-MAC and non-LAN destinations are unaffected.
 */
void cortina_ni_l2fe_mmshp_set(struct cortina_ni *ni, unsigned int lspid)
{
	u64 allow = ~0ULL;

	if (lspid < CA_NI_LAN_PORT_COUNT)
		allow = (allow & ~GENMASK_ULL(CA_NI_LAN_PORT_COUNT - 1, 0)) |
			ni->lan_fwd[lspid];
	allow &= ~BIT_ULL(lspid);

	mutex_lock(&ni->l2_lock);
	writel(upper_32_bits(allow), ni_base(ni) + CA_NI_L2FE_MMSHP_DATA1);
	writel(lower_32_bits(allow), ni_base(ni) + CA_NI_L2FE_MMSHP_DATA0);
	cortina_ni_rx_ind_store(ni, CA_NI_L2FE_MMSHP_ACCESS, lspid);
	mutex_unlock(&ni->l2_lock);
}

/*
 * Program the L2FE per-port profile tables as the stock init does.  Left at
 * reset, the pipeline sends every ingress frame to the blackhole ldpid before
 * the FDB/DFT_FWD forwarding decision:
 *   - ILPB[lspid].stp_mode = 0 = STP drop state (want 3 = forward+learn);
 *   - MMSHP[lspid] = 0 = the port-isolation bitmap allows no destination;
 *   - ELPB[ldpid].egr_stp = 0 = egress STP blocked.
 *
 * Table protocol: DATA registers, then ACCESS=GO|WR|i.  The values are
 * absolute, so this is safe to re-run from the link-up path.
 */
/*
 * L2FE VLAN membership check-id map (stock __g_l2_vlan_port_map):
 * lport -> membership check-id.  NI0-7 -> 0-7, CPU_0 -> 8, CPU_1 -> 9,
 * L3_WAN/L3_LAN (0x18/0x19) -> 15, GEM (0x20+) -> 7.
 */
static const u8 ca_ni_vlan_chkid_map[CA_NI_L2FE_LPORT_COUNT] = {
	 0, 1, 2, 3, 4, 5, 6, 7, 0, 0, 0, 0, 0, 0, 0, 0,
	 8, 9, 0, 10, 11, 12, 13, 14, 15, 15, 0, 0, 0, 0, 0, 0,
	 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
	 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
};

static void cortina_ni_rx_port_profiles_init(struct cortina_ni *ni)
{
	u32 i, d0, d1, d2;

	dev_dbg(ni->dev, "port-prof: programming IPPB/MMSHP/ILPB/ELPB (64 lports)\n");

	for (i = 0; i < CA_NI_L2FE_LPORT_COUNT; i++) {
		/* IPPB: physical -> logical source-port map, identity */
		writel(i, ni_base(ni) + CA_NI_L2FE_IPPB_DATA);
		cortina_ni_rx_ind_store(ni, CA_NI_L2FE_IPPB_ACCESS, i);

		cortina_ni_l2fe_mmshp_set(ni, i);

		/*
		 * MMSHP check-id map: lport -> VLAN membership check-id.  Left
		 * unprogrammed, the membership check passes nothing and LAN
		 * frames never reach the CPU port.
		 */
		writel(ca_ni_vlan_chkid_map[i],
		       ni_base(ni) + CA_NI_L2FE_CHKID_MAP_DATA);
		cortina_ni_rx_ind_store(ni, CA_NI_L2FE_CHKID_MAP_ACCESS, i);

		/* ILPB ingress profile: stp_mode=FWD+LEARN + the stock defaults */
		if (i == CA_NI_LPORT_MC)
			d2 = CA_NI_L2FE_ILPB_D2_MC;
		else if (i < CA_NI_LPORT_GEM_FIRST)
			d2 = CA_NI_L2FE_ILPB_D2_PORT;
		else
			d2 = CA_NI_L2FE_ILPB_D2_GEM;
		d1 = CA_NI_L2FE_ILPB_D1_INIT;
		if (i <= CA_NI_LPORT_ETH_NI6 ||
		    (i >= CA_NI_LPORT_CPU_0 && i <= CA_NI_LPORT_CPU_7))
			d1 |= CA_NI_L2FE_ILPB_D1_STAMOVE;
		/*
		 * Open the CLE_IGR classifier window on the ETH ingress lports
		 * 0..6 only (setting it on the CPU lports is harmful, see
		 * CA_NI_L2FE_ILPB_D1_CLS_EN).  It is set here because steer_init
		 * re-runs on every link change and rewrites ILPB_DATA1 whole.
		 */
		if (i <= CA_NI_LPORT_ETH_NI6)
			d1 |= CA_NI_L2FE_ILPB_D1_CLS_EN |
			      FIELD_PREP(CA_NI_L2FE_ILPB_D1_CLS_LEN,
					 CA_NI_L2FE_ILPB_D1_CLS_LEN_VAL);
		writel(i >= CA_NI_LPORT_GEM_FIRST ? CA_NI_L2FE_ILPB_D4_WAN : 0,
		       ni_base(ni) + CA_NI_L2FE_ILPB_DATA4);
		writel(CA_NI_L2FE_ILPB_D3_INIT, ni_base(ni) + CA_NI_L2FE_ILPB_DATA3);
		writel(d2, ni_base(ni) + CA_NI_L2FE_ILPB_DATA2);
		writel(d1, ni_base(ni) + CA_NI_L2FE_ILPB_DATA1);
		writel(CA_NI_L2FE_ILPB_D0_INIT, ni_base(ni) + CA_NI_L2FE_ILPB_DATA0);
		cortina_ni_rx_ind_store(ni, CA_NI_L2FE_ILPB_ACCESS, i);

		/*
		 * ELPB egress profile: egress STP forward + VLAN-aware (+dest_wan
		 * on the PON-side dest ports and the L3_LAN dest)
		 */
		d0 = (i >= CA_NI_LPORT_GEM_FIRST || i == CA_NI_LPORT_L3_LAN) ?
		     CA_NI_L2FE_ELPB_D0_WAN : CA_NI_L2FE_ELPB_D0_LAN;
		writel(0, ni_base(ni) + CA_NI_L2FE_ELPB_DATA1);
		writel(d0, ni_base(ni) + CA_NI_L2FE_ELPB_DATA0);
		cortina_ni_rx_ind_store(ni, CA_NI_L2FE_ELPB_ACCESS, i);
	}

	/*
	 * L2FE direct config registers stock also moves off their defaults
	 * (incl. PLE_CTL skip_port_lpbk_chk+pon_mode and PARSER_CTRL
	 * arp_op_filter_dis)
	 */
	writel(CA_NI_L2FE_GLB_CTRL_STOCK, ni_base(ni) + CA_NI_L2FE_GLB_CTRL);
	writel(CA_NI_L2FE_TPID_S_STOCK, ni_base(ni) + CA_NI_L2FE_PP_TPID_CMP_S);
	writel(CA_NI_L2FE_TPID_O_STOCK, ni_base(ni) + CA_NI_L2FE_PP_TPID_CMP_O);
	writel(CA_NI_L2FE_PARSER_CTRL_STOCK,
	       ni_base(ni) + CA_NI_L2FE_PP_PARSER_CTRL);
	writel(CA_NI_L2FE_L2_LEARNING_STOCK,
	       ni_base(ni) + CA_NI_L2FE_PLC_L2_LEARNING);
	writel(CA_NI_L2FE_VLAN_MODE_STOCK, ni_base(ni) + CA_NI_L2FE_PLC_VLAN_MODE);
	writel(CA_NI_L2FE_PLE_CTL_STOCK, ni_base(ni) + CA_NI_L2FE_PLE_CTL);
	writel(CA_NI_L2FE_UNKWN_VLAN_DFT1_STOCK,
	       ni_base(ni) + CA_NI_L2FE_PLE_UNKWN_VLAN_DFT1);
	/* PLE trunk and HD_FF registers, stock values */
	writel(CA_NI_L2FE_PLE_TRUNK_STOCK, ni_base(ni) + CA_NI_L2FE_PLE_TRUNK0);
	writel(CA_NI_L2FE_PLE_TRUNK_STOCK, ni_base(ni) + CA_NI_L2FE_PLE_TRUNK1);
	writel(CA_NI_L2FE_PLE_HD_FF_STOCK, ni_base(ni) + CA_NI_L2FE_PLE_HD_FF_CTL);

	/* readback for the boot log */
	cortina_ni_rx_ind_read(ni, CA_NI_L2FE_ILPB_ACCESS, CA_NI_RX_PORT);
	d2 = readl(ni_base(ni) + CA_NI_L2FE_ILPB_DATA2);
	d1 = readl(ni_base(ni) + CA_NI_L2FE_ILPB_DATA1);
	d0 = readl(ni_base(ni) + CA_NI_L2FE_ILPB_DATA0);
	cortina_ni_rx_ind_read(ni, CA_NI_L2FE_MMSHP_ACCESS, CA_NI_RX_PORT);
	dev_dbg(ni->dev,
		"port-prof: ilpb[0] d2=0x%08x d1=0x%08x d0=0x%08x (stp=%lu want 3) mmshp[0]=%08x_%08x ple_ctl=0x%08x\n",
		d2, d1, d0, FIELD_GET(CA_NI_L2FE_ILPB_STP_MODE, d2),
		readl(ni_base(ni) + CA_NI_L2FE_MMSHP_DATA1),
		readl(ni_base(ni) + CA_NI_L2FE_MMSHP_DATA0),
		readl(ni_base(ni) + CA_NI_L2FE_PLE_CTL));
}

/*
 * Program REDIR_LDPID[@idx] -> @dest_ldpid.  Indirect: DATA = target, then
 * ACCESS = GO|WR|idx, poll GO clear.
 */
static void cortina_ni_rx_redir_ldpid_set(struct cortina_ni *ni, u8 idx,
					  u8 dest_ldpid);

/* stock L2FE PDPID_MAP: ldpid -> physical dest port */
static const struct { u8 ldpid, pdpid; } cortina_ni_rx_pdpid_map[] = {
	/*
	 * The whole OMCI ldpid range 0x08..0x0f maps to PPORT_OAM (0x0c) ->
	 * the PUC 9th queue.  Upstream OMCI HEADER_A stamps CA_NI_PON_LDPID =
	 * 0x0f.
	 */
	{ 0x08, 0x0c }, { 0x09, 0x0c }, { 0x0a, 0x0c }, { 0x0b, 0x0c },
	{ 0x0c, 0x0c }, { 0x0d, 0x0c }, { 0x0e, 0x0c }, { 0x0f, 0x0c },
	{ 0x10, 0x09 },
	{ 0x19, 0x0d }, { 0x1d, 0x09 }, { 0x1f, 0x0f }, { 0x32, 0x08 },
};

/*
 * Zero the ARB FLOW_DBUF table, as stock.  With ARB_CTRL.dbuf_sel=1 this table
 * is the deep_q source, and a set entry diverts every LAN frame into the
 * deep-queue path before it can enter the L3FE.  Read entries 0..7 back before
 * and after for the log.
 */
static void cortina_ni_rx_flow_dbuf_init(struct cortina_ni *ni)
{
	u32 b[8], a[8];
	unsigned int i;

	for (i = 0; i < 8; i++) {
		cortina_ni_rx_ind_read(ni, CA_NI_L2FE_ARB_FLOW_DBUF_ACCESS, i);
		b[i] = readl(ni_base(ni) + CA_NI_L2FE_ARB_FLOW_DBUF_DATA);
	}
	for (i = 0; i < CA_NI_L2FE_ARB_FLOW_DBUF_ENTRIES; i++) {
		writel(0, ni_base(ni) + CA_NI_L2FE_ARB_FLOW_DBUF_DATA);
		cortina_ni_rx_ind_store(ni, CA_NI_L2FE_ARB_FLOW_DBUF_ACCESS, i);
	}
	for (i = 0; i < 8; i++) {
		cortina_ni_rx_ind_read(ni, CA_NI_L2FE_ARB_FLOW_DBUF_ACCESS, i);
		a[i] = readl(ni_base(ni) + CA_NI_L2FE_ARB_FLOW_DBUF_DATA);
	}
	dev_dbg(ni->dev,
		"flow-dbuf(0x165c/0x1660) before[0..7]=%08x %08x %08x %08x %08x %08x %08x %08x\n",
		b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
	dev_dbg(ni->dev,
		"flow-dbuf AFTER[0..7]=%08x %08x %08x %08x %08x %08x %08x %08x (want all 0 = stock, no deep_q marks)\n",
		a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
}

static void cortina_ni_rx_arb_deepq_init(struct cortina_ni *ni)
{
	u32 d0, d1, i, e;

	/*
	 * ARB/PDPID setup for the CPU-bound path.  ARB_CTRL.dbuf_sel (bit1) is
	 * set to match stock (0x89c71c82) in cortina_ni_rx_deepq_sched_init().
	 *
	 * PORT_DBUF[0] = 0 as stock: the LAN->CPU path is never deep-buffer
	 * marked.  Written explicitly so a stale mark cannot survive.
	 */
	writel(0, ni_base(ni) + CA_NI_L2FE_ARB_PORT_DBUF_DATA);
	cortina_ni_rx_ind_store(ni, CA_NI_L2FE_ARB_PORT_DBUF_ACCESS, 0);

	/*
	 * Full PDPID_MAP, stock values:
	 *   DLF/broadcast: DFT_FWD 0x1832 = redirect to mc_group_id[10:1]=0x19
	 *                  (L3_LAN) -> [0x19]=0x0D -> RMU -> L3FE -> L3-CLS
	 *                  trap -> CPU_0;
	 *   my-MAC/L3-hit: CPU LPORT 0x10/0x1d -> [0x10]/[0x1d]=0x09 (CPU);
	 *   [0x1f]=0x0F (blackhole/drop).
	 * Each is written for all 4 {my_mac, dbuf} index combinations so the
	 * frame resolves the same whatever its flags.
	 */
	for (e = 0; e < ARRAY_SIZE(cortina_ni_rx_pdpid_map); e++) {
		u8 pdpid = cortina_ni_rx_pdpid_map[e].pdpid;

		/* L3_LAN (ldpid 0x19) -> pdpid 0x0d, as stock */
		if (cortina_ni_rx_pdpid_map[e].ldpid == CA_NI_RX_L3LAN_LDPID)
			pdpid = CA_NI_RX_L3LAN_PDPID;
		for (i = 0; i < 4; i++) {
			u32 idx = cortina_ni_rx_pdpid_map[e].ldpid |
				  ((i & 1) ? CA_NI_L2FE_PDPID_IDX_DBUF : 0) |
				  ((i & 2) ? CA_NI_L2FE_PDPID_IDX_MYMAC : 0);

			writel(FIELD_PREP(CA_NI_L2FE_PDPID_MAP_PDPID, pdpid),
			       ni_base(ni) + CA_NI_L2FE_PDPID_MAP_DATA);
			cortina_ni_rx_ind_store(ni, CA_NI_L2FE_PDPID_MAP_ACCESS, idx);
		}
	}
	/*
	 * Upstream data frames resolve to ldpid 0x21 (the data GEM).  Map
	 * [0x21]=0x08 = dbuf_dpid so the ARB deep_q comparator fires and they
	 * take the same deep-queue route as OMCI (dbuf_sel=1, no use_hdr_a).
	 */
	for (i = 0; i < 4; i++) {
		u32 idx = 0x21 |
			  ((i & 1) ? CA_NI_L2FE_PDPID_IDX_DBUF : 0) |
			  ((i & 2) ? CA_NI_L2FE_PDPID_IDX_MYMAC : 0);

		writel(FIELD_PREP(CA_NI_L2FE_PDPID_MAP_PDPID, 0x08),
		       ni_base(ni) + CA_NI_L2FE_PDPID_MAP_DATA);
		cortina_ni_rx_ind_store(ni, CA_NI_L2FE_PDPID_MAP_ACCESS, idx);
	}
	dev_dbg(ni->dev, "PDPID_MAP[0x21]=0x08 (US-data ldpid 0x21 -> pdpid 8 = deep_q comparator)\n");
	dev_dbg(ni->dev, "pdpid: map written ([0x19]=0x%02x, [0x32]=0x08 QM); L3_LAN routed to pdpid 0x%02x\n",
		CA_NI_RX_L3LAN_PDPID, CA_NI_RX_L3LAN_PDPID);

	/*
	 * REDIR_LDPID[0x19] is left at its default; read it back for the log.
	 */
	cortina_ni_rx_ind_read(ni, CA_NI_L2FE_REDIR_LDPID_ACCESS, CA_NI_RX_L3LAN_LDPID);
	dev_dbg(ni->dev,
		"arb-deepq: REDIR_LDPID[0x19] data=0x%08x (redirect REMOVED; want NOT rdir_en|0x%x)\n",
		readl(ni_base(ni) + CA_NI_L2FE_REDIR_LDPID_DATA), CA_NI_RX_CPU_LDPID);

	cortina_ni_rx_ind_read(ni, CA_NI_L2FE_PDPID_MAP_ACCESS, CA_NI_RX_QM_REDIR_LDPID);
	d0 = readl(ni_base(ni) + CA_NI_L2FE_PDPID_MAP_DATA) &
	     CA_NI_L2FE_PDPID_MAP_PDPID;
	cortina_ni_rx_ind_read(ni, CA_NI_L2FE_PDPID_MAP_ACCESS,
			       CA_NI_L2FE_PDPID_IDX_DBUF | CA_NI_RX_QM_REDIR_LDPID);
	d1 = readl(ni_base(ni) + CA_NI_L2FE_PDPID_MAP_DATA) &
	     CA_NI_L2FE_PDPID_MAP_PDPID;
	dev_dbg(ni->dev,
		"arb-deepq: base LAN->CPU: PDPID_MAP[0x32]{dbuf0}=0x%x {dbuf1}=0x%x arb_ctrl=0x%08x (want both 0x%x=CPU)\n",
		d0, d1, readl(ni_base(ni) + CA_NI_L2FE_ARB_CTRL),
		CA_NI_RX_CPU_PDPID);
}

/*
 * Clear CAPSRAM_TZCONTROL (all segments non-secure), as stock U-Boot does
 * before ca_init.  A non-secure access to a secure CapSRAM segment, which backs
 * the NI MCE index table and the L2FE ARB indirect tables, raises an SError or
 * stalls the indirect GO.  The readback shows whether the write took.
 */
static void __maybe_unused cortina_ni_rx_capsram_desecure(struct cortina_ni *ni)
{
	void __iomem *cap;
	u32 before, after;

	dev_dbg(ni->dev, "CAPSRAM 1: ioremap TZCONTROL @0x%llx\n",
		(unsigned long long)CA_NI_CAPSRAM_PHYS);
	cap = ioremap(CA_NI_CAPSRAM_PHYS, 0x10);
	if (!cap) {
		dev_err(ni->dev, "capsram: ioremap failed\n");
		return;
	}

	dev_dbg(ni->dev, "CAPSRAM 2: read TZCONTROL\n");
	before = readl(cap + CA_NI_CAPSRAM_TZCONTROL);

	dev_dbg(ni->dev, "CAPSRAM 3: TZCONTROL=0x%08x, writing 0 (all NS)\n",
		before);
	writel(0, cap + CA_NI_CAPSRAM_TZCONTROL);

	after = readl(cap + CA_NI_CAPSRAM_TZCONTROL);
	dev_dbg(ni->dev, "CAPSRAM 4: TZCONTROL now 0x%08x (write %s)\n",
		after, after ? "IGNORED - EL3-protected?" : "TOOK");
	iounmap(cap);
}

/*
 * Multicast flood to the CPU: MC_FIB[16].ldpid = 16 (per-copy CPU action) and
 * ni_mce_indx[G].mc_vec bit 16 (mc_vec bit i selects MC_FIB[i]).  Needs the
 * CapSRAM de-secure above.
 */
static void __maybe_unused cortina_ni_rx_mc_flood_init(struct cortina_ni *ni)
{
	/* MC_FIB[16]: per-copy action -> ldpid 16 (CPU); no VLAN edit */
	dev_dbg(ni->dev, "MCFLOOD 1: MC_FIB[%u] DATA write\n", CA_NI_RX_CPU_LDPID);
	writel(0, ni_base(ni) + CA_NI_L2FE_MC_FIB_DATA0);
	writel(FIELD_PREP(CA_NI_L2FE_MC_FIB_LDPID, CA_NI_RX_CPU_LDPID),
	       ni_base(ni) + CA_NI_L2FE_MC_FIB_DATA1);
	writel(0, ni_base(ni) + CA_NI_L2FE_MC_FIB_DATA2);
	dev_dbg(ni->dev, "MCFLOOD 2: MC_FIB[%u] ACCESS store\n", CA_NI_RX_CPU_LDPID);
	cortina_ni_rx_ind_store(ni, CA_NI_L2FE_MC_FIB_ACCESS, CA_NI_RX_CPU_LDPID);

	/* ni_mce_indx[G].mc_vec = bit 16 (CPU) */
	dev_dbg(ni->dev, "MCFLOOD 3: mce_indx[%u] DATA write (was the SError)\n",
		CA_NI_RX_FLOOD_MCGID);
	writel(BIT(CA_NI_RX_CPU_LDPID), ni_base(ni) + CA_NI_NI_MCE_INDX_DATA0);
	writel(0, ni_base(ni) + CA_NI_NI_MCE_INDX_DATA1);
	dev_dbg(ni->dev, "MCFLOOD 4: mce_indx[%u] ACCESS store\n", CA_NI_RX_FLOOD_MCGID);
	cortina_ni_rx_ind_store(ni, CA_NI_NI_MCE_INDX_ACCESS, CA_NI_RX_FLOOD_MCGID);

	/* pollable read-back for the boot log */
	cortina_ni_rx_ind_read(ni, CA_NI_NI_MCE_INDX_ACCESS, CA_NI_RX_FLOOD_MCGID);
	dev_dbg(ni->dev, "MCFLOOD 5 (survived): mcgid=%u mc_vec.lo=0x%08x\n",
		CA_NI_RX_FLOOD_MCGID,
		readl(ni_base(ni) + CA_NI_NI_MCE_INDX_DATA0));
}

/*
 * MCE group 0x19 (the DFT_FWD target) is left empty, as stock, and the
 * 0x1634 table is written with the stock values.
 */
static void cortina_ni_rx_mc_group_init(struct cortina_ni *ni)
{
	/* empty MCE[0x19] */
	writel(0, ni_base(ni) + CA_NI_NI_MCE_INDX_DATA1);
	writel(0, ni_base(ni) + CA_NI_NI_MCE_INDX_DATA0);
	cortina_ni_rx_ind_store(ni, CA_NI_NI_MCE_INDX_ACCESS, CA_NI_RX_DFT_FWD_MCGID);

	/*
	 * The table at 0x1634 (likely NON_KNOWN_POL_MAP; the real MC_FIB is at
	 * ACCESS 0x1644 and stays empty) gets stock's own content for entries
	 * 0x10..0x1b.  The zero writes to 0x1640/0x163c (DSCP_TE block, no GO)
	 * have no effect but are kept.
	 */
	{
		static const u8 nkpol_d[] = {
			/* 0x10 */ 0x0F, 0x04, 0x0F, 0x09, 0x0F, 0x05,
			/* 0x16 */ 0x0F, 0x0A, 0x0F, 0x0B, 0x0F, 0x0C,
		};
		unsigned int k;

		for (k = 0; k < ARRAY_SIZE(nkpol_d); k++) {
			writel(0, ni_base(ni) + CA_NI_L2FE_DSCP_TE_DATA);
			writel(0, ni_base(ni) + CA_NI_L2FE_DSCP_TE_ACCESS);
			writel(nkpol_d[k], ni_base(ni) + CA_NI_L2FE_NKPOL_MAP_DATA);
			cortina_ni_rx_ind_store(ni, CA_NI_L2FE_NKPOL_MAP_ACCESS,
						CA_NI_RX_CPU_LDPID + k);	/* 0x10 + k */
		}
		cortina_ni_rx_ind_read(ni, CA_NI_L2FE_NKPOL_MAP_ACCESS, CA_NI_RX_L3LAN_LDPID);
		dev_dbg(ni->dev,
			"tbl@0x1634 (ex-\"MC_FIB\", stock-match): [0x19]=0x%08x (want 0x0b); [0x10..0x1b] set\n",
			readl(ni_base(ni) + CA_NI_L2FE_NKPOL_MAP_DATA));
	}

	/* indirect readback; want 0/0 = empty */
	cortina_ni_rx_ind_read(ni, CA_NI_NI_MCE_INDX_ACCESS, CA_NI_RX_DFT_FWD_MCGID);
	dev_dbg(ni->dev,
		"mc-group[0x%x]: EMPTIED mc_vec hi(0xaaf8)=0x%08x lo(0xaafc)=0x%08x (want 0/0 -> DFT_FWD 0x1832 falls through to raw ldpid 0x%x)\n",
		CA_NI_RX_DFT_FWD_MCGID,
		readl(ni_base(ni) + CA_NI_NI_MCE_INDX_DATA1),
		readl(ni_base(ni) + CA_NI_NI_MCE_INDX_DATA0),
		CA_NI_RX_MC_CPU_LDPID);
}

/*
 * Pack a MAC into the FDB key words (vid/scind/dot1p = 0), shared by the
 * append and the lookup paths so both hash to the same bucket.
 */
static void cortina_ni_l2fe_fdb_key(const u8 *mac, u32 *d3, u32 *d2, u32 *d1)
{
	*d3 = (mac[0] >> 5) & 0x7;
	*d2 = ((u32)(mac[0] & 0x1f) << 27) | ((u32)mac[1] << 19) |
	      ((u32)mac[2] << 11) | ((u32)mac[3] << 3) | ((mac[4] >> 5) & 0x7);
	*d1 = (u32)(((mac[4] & 0x1f) << 8) | mac[5]) << 19;
}

/*
 * Issue one OP_READ (look-up) for the packed key and return the 13-bit entry
 * index, or -1 when the key is not present or the engine times out.  DATA0 is
 * not written: on a hit the engine returns the matched entry's action word
 * there, which @act_out (if non-NULL) receives.
 */
static int cortina_ni_l2fe_fdb_read_idx(void __iomem *base, u32 d3, u32 d2,
					u32 d1, u32 *act_out)
{
	u32 acc, cr;

	writel(0, base + CA_NI_L2FE_FDB_CMD_RETURN);
	writel(d3, base + CA_NI_L2FE_FDB_DATA3);
	writel(d2, base + CA_NI_L2FE_FDB_DATA2);
	writel(d1, base + CA_NI_L2FE_FDB_DATA1);
	writel(CA_NI_L2FE_FDB_GO | CA_NI_L2FE_FDB_OP_READ,
	       base + CA_NI_L2FE_FDB_ACCESS);
	if (readl_poll_timeout(base + CA_NI_L2FE_FDB_ACCESS, acc,
			       !(acc & CA_NI_L2FE_FDB_GO), CA_NI_TX_POLL_US,
			       CA_NI_TX_POLL_TIMEOUT_US))
		return -1;
	cr = readl(base + CA_NI_L2FE_FDB_CMD_RETURN);
	if ((cr & 0xf) != CA_NI_L2FE_FDB_STATUS_HIT)
		return -1;			/* not present */
	if (act_out)
		*act_out = readl(base + CA_NI_L2FE_FDB_DATA0);
	return (int)((cr >> 4) & 0x1fff);	/* ext_status[16:4] = entry index */
}

/*
 * Program a static L2FE FDB entry {mac -> ldpid} and return its hash-table
 * entry index (CMD_RETURN.ext_status[16:4], 13 bits), or -1 on failure.  The
 * index is what an L3FE forward action's mac_da_idx refers to: the engine
 * fetches the next-hop DMAC from L2 FDB[idx].  @base is the NI/NE register
 * window.  Used by the flow-offload next-hop path.
 */
static int cortina_ni_l2fe_fdb_put(void __iomem *base, const u8 *mac, u32 ldpid,
				   u32 op)
{
	u32 d0, d1, d2, d3, acc;

	cortina_ni_l2fe_fdb_key(mac, &d3, &d2, &d1);
	d0 = FIELD_PREP(CA_NI_L2FE_FDB_LPID, ldpid) |
	     CA_NI_L2FE_FDB_VALID | CA_NI_L2FE_FDB_STATIC |
	     CA_NI_L2FE_FDB_DA_PERMIT | CA_NI_L2FE_FDB_SA_PERMIT;

	writel(0, base + CA_NI_L2FE_FDB_CMD_RETURN);
	writel(d3, base + CA_NI_L2FE_FDB_DATA3);
	writel(d2, base + CA_NI_L2FE_FDB_DATA2);
	writel(d1, base + CA_NI_L2FE_FDB_DATA1);
	writel(d0, base + CA_NI_L2FE_FDB_DATA0);
	writel(CA_NI_L2FE_FDB_GO | op, base + CA_NI_L2FE_FDB_ACCESS);
	if (readl_poll_timeout(base + CA_NI_L2FE_FDB_ACCESS, acc,
			       !(acc & CA_NI_L2FE_FDB_GO), CA_NI_TX_POLL_US,
			       CA_NI_TX_POLL_TIMEOUT_US))
		return -1;

	/* READ back the key: CMD_RETURN.status[3:0]=0x5 HIT, ext_status[16:4]=idx */
	return cortina_ni_l2fe_fdb_read_idx(base, d3, d2, d1, NULL);
}

int cortina_ni_l2fe_fdb_add_idx(void __iomem *base, const u8 *mac, u32 ldpid)
{
	return cortina_ni_l2fe_fdb_put(base, mac, ldpid, CA_NI_L2FE_FDB_OP_APPEND);
}

/* Program a static {mac -> ldpid} entry, replacing a learned one. */
int cortina_ni_l2fe_fdb_replace(void __iomem *base, const u8 *mac, u32 ldpid)
{
	return cortina_ni_l2fe_fdb_put(base, mac, ldpid,
				       CA_NI_L2FE_FDB_OP_REPLACE) < 0 ? -EIO : 0;
}

/*
 * Look up @mac in the L2FE FDB without changing the table, and return the
 * entry index (the L3FE forward action's mac_da_idx), or -1 if the MAC is not
 * present or the engine timed out.  @ldpid_out (optional) receives the entry's
 * forward-to LDPID; for a LAN NI port that is the physical port number.  Used
 * by the downstream flow-offload path, where the LAN client's MAC has already
 * been learned.  Lookup only: a static entry would pin a learned MAC to a
 * guessed port.
 */
int cortina_ni_l2fe_fdb_lookup_idx(void __iomem *base, const u8 *mac,
				   u32 *ldpid_out)
{
	u32 d1, d2, d3, act = 0;
	int idx;

	cortina_ni_l2fe_fdb_key(mac, &d3, &d2, &d1);
	idx = cortina_ni_l2fe_fdb_read_idx(base, d3, d2, d1, &act);
	if (idx < 0)
		return -1;
	if (!(act & CA_NI_L2FE_FDB_VALID))
		return -1;
	if (ldpid_out)
		*ldpid_out = FIELD_GET(CA_NI_L2FE_FDB_LPID, act);
	return idx;
}

/* Delete the (VLAN 0) FDB entry for @mac (vendor aal_fdb_entry_del). */
int cortina_ni_l2fe_fdb_del(void __iomem *base, const u8 *mac)
{
	u32 acc, d3, d2, d1;

	cortina_ni_l2fe_fdb_key(mac, &d3, &d2, &d1);
	writel(0, base + CA_NI_L2FE_FDB_CMD_RETURN);
	writel(d3, base + CA_NI_L2FE_FDB_DATA3);
	writel(d2, base + CA_NI_L2FE_FDB_DATA2);
	writel(d1, base + CA_NI_L2FE_FDB_DATA1);
	writel(0, base + CA_NI_L2FE_FDB_DATA0);
	writel(CA_NI_L2FE_FDB_GO | CA_NI_L2FE_FDB_OP_DELETE,
	       base + CA_NI_L2FE_FDB_ACCESS);
	return readl_poll_timeout(base + CA_NI_L2FE_FDB_ACCESS, acc,
				  !(acc & CA_NI_L2FE_FDB_GO), CA_NI_TX_POLL_US,
				  CA_NI_TX_POLL_TIMEOUT_US);
}

/*
 * Scan the FDB from entry *@idx on (vendor aal_fdb_entry_scan): the engine
 * returns the first valid entry at or after it. Returns 0 with *@idx set to
 * that entry and @mac, @vid and the action word @act filled, -ENOENT when no
 * entry is left, -ETIMEDOUT when the engine stays busy.
 */
int cortina_ni_l2fe_fdb_scan(void __iomem *base, u32 *idx, u8 *mac, u16 *vid,
			     u32 *act)
{
	u32 acc, cr, d3, d2, d1;

	writel(0, base + CA_NI_L2FE_FDB_CMD_RETURN);
	writel(CA_NI_L2FE_FDB_GO | CA_NI_L2FE_FDB_OP_SCAN |
	       FIELD_PREP(CA_NI_L2FE_FDB_INDEX, *idx),
	       base + CA_NI_L2FE_FDB_ACCESS);
	if (readl_poll_timeout(base + CA_NI_L2FE_FDB_ACCESS, acc,
			       !(acc & CA_NI_L2FE_FDB_GO), CA_NI_TX_POLL_US,
			       CA_NI_TX_POLL_TIMEOUT_US))
		return -ETIMEDOUT;
	cr = readl(base + CA_NI_L2FE_FDB_CMD_RETURN);
	if ((cr & 0xf) != CA_NI_L2FE_FDB_STATUS_HIT)
		return -ENOENT;

	*idx = FIELD_GET(CA_NI_L2FE_FDB_RET_INDEX, cr);
	*act = readl(base + CA_NI_L2FE_FDB_DATA0);

	d3 = readl(base + CA_NI_L2FE_FDB_DATA3);
	d2 = readl(base + CA_NI_L2FE_FDB_DATA2);
	d1 = readl(base + CA_NI_L2FE_FDB_DATA1);
	mac[0] = (d3 << 5) | (d2 >> 27);
	mac[1] = d2 >> 19;
	mac[2] = d2 >> 11;
	mac[3] = d2 >> 3;
	mac[4] = (d2 << 5) | (d1 >> 27);
	mac[5] = d1 >> 19;
	*vid = FIELD_GET(CA_NI_L2FE_FDB_VID, d1);
	return 0;
}

static void cortina_ni_rx_fdb_append(struct cortina_ni *ni, const u8 *mac,
				     u32 ldpid)
{
	int idx = cortina_ni_l2fe_fdb_add_idx(ni_base(ni), mac, ldpid);

	dev_dbg(ni->dev, "fdb-add: %pM -> ldpid 0x%02x : entry_idx=%d %s\n",
		mac, ldpid, idx, idx < 0 ? "(FAILED)" : "(HIT)");
}

/*
 * Static FDB entries for our own MACs, so a frame to them is a known unicast
 * forwarded to the L3 lport (-> L3FE -> my-MAC/CLS trap -> CPU) instead of a
 * DLF.
 */
static void cortina_ni_rx_fdb_add_cpu(struct cortina_ni *ni)
{
	static const u8 def_mac[ETH_ALEN] = { 0x02, 0x96, 0x07, 0xf0, 0x00, 0x01 };
	const u8 *mac = (ni->tx && ni->tx->netdev) ?
			ni->tx->netdev->dev_addr : def_mac;
	u32 acc;
	int ret;

	mutex_lock(&ni->l2_lock);

	/*
	 * (0) one-time FDB engine INIT (opcode 0): the hash table must be built
	 * before the first APPEND, else APPEND does nothing.  No DATA; longer
	 * poll, as it clears the whole table.
	 */
	writel(0, ni_base(ni) + CA_NI_L2FE_FDB_CMD_RETURN);
	writel(CA_NI_L2FE_FDB_GO | CA_NI_L2FE_FDB_OP_INIT,
	       ni_base(ni) + CA_NI_L2FE_FDB_ACCESS);
	ret = readl_poll_timeout(ni_base(ni) + CA_NI_L2FE_FDB_ACCESS, acc,
				 !(acc & CA_NI_L2FE_FDB_GO), 10, 20000);
	if (ret)
		dev_warn(ni->dev, "fdb-add: engine INIT GO stuck\n");

	/*
	 * (1) router (LAN) MAC: to the CPU, or to L3_LAN (0x19, stock's my-MAC
	 * route) when the L3FE routes in hardware
	 */
	cortina_ni_rx_fdb_append(ni, mac, cortina_ppe_active() ?
				 CA_NI_RX_L3LAN_LDPID : CA_NI_RX_CPU_LDPID);

	/*
	 * (2) WAN MAC (base + 1) -> L3_WAN (0x18), only with HW L3 forwarding.
	 * The L2 my-MAC detection stays off and a static FDB entry forwards
	 * my-MAC frames to the L3FE, as in the vendor special-packet layer.
	 * Without it a downstream unicast to the WAN MAC is a DLF in the L2FE
	 * and is flooded instead of delivered.  The PPE probe
	 * installs the same entry at probe time; link-up re-runs this path.
	 */
	if (cortina_ppe_active()) {
		u8 wan_mac[ETH_ALEN];
		u64 v = ((u64)mac[0] << 40) | ((u64)mac[1] << 32) |
			((u64)mac[2] << 24) | ((u64)mac[3] << 16) |
			((u64)mac[4] << 8) | mac[5];

		v++;
		wan_mac[0] = v >> 40;
		wan_mac[1] = v >> 32;
		wan_mac[2] = v >> 24;
		wan_mac[3] = v >> 16;
		wan_mac[4] = v >> 8;
		wan_mac[5] = v;
		cortina_ni_rx_fdb_append(ni, wan_mac, CA_NI_RX_L3WAN_LDPID);
	}
	mutex_unlock(&ni->l2_lock);
}

/*
 * Own-MAC CPU trap through the L3FE.  Comparator A (NI-global
 * 0xa024/a028/a5c0) is the active my-MAC; comparator B (0x3294/98) is unused
 * and chk_mymac_for_lan (0x3400 b21) stays 0, as stock.  Without HW L3
 * forwarding comparator A is programmed with our MAC; with it, comparator A is
 * cleared so a my-MAC frame takes the static FDB route into the L3FE.
 * my_mac_enable (0x3218 bit2) stays set in both cases.  The STG0 LPB profiles
 * hold the my-MAC-hit route.
 */
static void cortina_ni_rx_mymac_trap(struct cortina_ni *ni)
{
	static const u8 def_mac[ETH_ALEN] = { 0x02, 0x96, 0x07, 0xf0, 0x00, 0x01 };
	const u8 *mac = (ni->tx && ni->tx->netdev) ?
			ni->tx->netdev->dev_addr : def_mac;
	bool l2_trap = !cortina_ppe_active();
	u32 det, ctrl, hi_p0;

	/*
	 * (A) NI-global my-MAC: CFG0 = bytes 0-3, CFG1[7:0] = byte 4,
	 * PT[31:24] = byte 5.
	 *
	 * Under HW L3 forwarding the comparator must stay unarmed: a LAN->WAN
	 * transit frame's destination MAC is the gateway MAC, and an armed
	 * comparator sends it to the CPU before the DA-FDB lookup, so it never
	 * reaches the L3FE flow engine.  As in the vendor special-packet layer,
	 * MYMAC is set to 0 and terminating frames reach the CPU through the
	 * L3FE T2-miss -> HS_DEF entry-0 CPU_0 punt.  Only the comparator MAC is
	 * cleared: SPCL_PKT_DET.my_mac_enable (0x3218 bit2) gates the delivery of
	 * the GPON/OMCI control frames to the CPU and must stay set.  The clear is
	 * written explicitly so a later re-arm undoes a probe-time armed state.
	 */
	dev_dbg(ni->dev, "MYMAC 1: comparator A (0xa024/a028/a5c0)\n");
	if (l2_trap) {
		writel(((u32)mac[0] << 24) | ((u32)mac[1] << 16) |
		       ((u32)mac[2] << 8) | mac[3],
		       ni_base(ni) + CA_NI_L3FE_NI_MAC_CFG0);
		ni_rmw(ni, CA_NI_L3FE_NI_MAC_CFG1, CA_NI_L3FE_NI_MAC_BYTE4,
		       mac[4]);
		ni_rmw(ni, CA_NI_L3FE_PT_PORT_STATIC_CFG,
		       CA_NI_L3FE_PT_MAC_BYTE5,
		       FIELD_PREP(CA_NI_L3FE_PT_MAC_BYTE5, mac[5]));
	} else {
		writel(0, ni_base(ni) + CA_NI_L3FE_NI_MAC_CFG0);
		ni_rmw(ni, CA_NI_L3FE_NI_MAC_CFG1, CA_NI_L3FE_NI_MAC_BYTE4, 0);
		ni_rmw(ni, CA_NI_L3FE_PT_PORT_STATIC_CFG,
		       CA_NI_L3FE_PT_MAC_BYTE5, 0);
	}

	/*
	 * enable my-MAC detection (0x3400 b21 chk_mymac_for_lan stays 0, as
	 * stock); kept set with the flow engine too, see above
	 */
	dev_dbg(ni->dev, "MYMAC 2: my_mac_enable (0x3218 bit2)\n");
	ni_rmw(ni, CA_NI_L3FE_SPCL_PKT_DET_CFG, 0, CA_NI_L3FE_MY_MAC_EN);

	if (!l2_trap)
		dev_dbg(ni->dev,
			"mymac-trap: comparator A cleared for the flow engine (0x3218 my_mac_enable kept stock/set) -> transit rides FDB into L3FE\n");

	/*
	 * STG0 LPB profiles and ldpid_map, stock values (direct registers
	 * 0x3408-0x3434).  The WAN profiles (prof0 @HIGH0, prof2 @HIGH2) include
	 * spcl_pkt_en (bit20); the L3 special-packet table behind that bit does
	 * not exist on this SoC, so the bit has no effect.
	 */
	dev_dbg(ni->dev, "MYMAC 3: STG0 LPB match (0x3404-3434)\n");
	hi_p0 = CA_NI_L3FE_LPB_HIGH_P0;
	writel(CA_NI_L3FE_STG0_LDPID_MAP_VAL,
	       ni_base(ni) + CA_NI_L3FE_STG0_LDPID_MAP);
	writel(0, ni_base(ni) + CA_NI_L3FE_STG0_LPB_LOW0);
	writel(CA_NI_L3FE_LPB_MID_SEL0, ni_base(ni) + CA_NI_L3FE_STG0_LPB_MID0);
	writel(hi_p0, ni_base(ni) + CA_NI_L3FE_STG0_LPB_HIGH0);
	writel(0, ni_base(ni) + CA_NI_L3FE_STG0_LPB_LOW1);
	writel(CA_NI_L3FE_LPB_MID_SEL1, ni_base(ni) + CA_NI_L3FE_STG0_LPB_MID1);
	writel(CA_NI_L3FE_LPB_HIGH_P1,
	       ni_base(ni) + CA_NI_L3FE_STG0_LPB_HIGH1);
	writel(0, ni_base(ni) + CA_NI_L3FE_STG0_LPB_LOW2);
	writel(CA_NI_L3FE_LPB_MID_SEL0, ni_base(ni) + CA_NI_L3FE_STG0_LPB_MID2);
	writel(hi_p0, ni_base(ni) + CA_NI_L3FE_STG0_LPB_HIGH2);
	writel(0, ni_base(ni) + CA_NI_L3FE_STG0_LPB_LOW3);
	writel(CA_NI_L3FE_LPB_MID_SEL1, ni_base(ni) + CA_NI_L3FE_STG0_LPB_MID3);
	writel(CA_NI_L3FE_LPB_HIGH_P3,
	       ni_base(ni) + CA_NI_L3FE_STG0_LPB_HIGH3);

	/*
	 * L3FE STG0 my-MAC (0x3210/0x3214) = our MAC with the valid bit
	 * (0x3210 bit16), as stock: LO = valid | mac[0] << 8 | mac[1],
	 * HI = mac[2..5].
	 */
	writel(CA_NI_L3FE_MY_MAC_VALID | ((u32)mac[0] << 8) | mac[1],
	       ni_base(ni) + CA_NI_L3FE_MY_MAC_LO);
	writel(((u32)mac[2] << 24) | ((u32)mac[3] << 16) | ((u32)mac[4] << 8) | mac[5],
	       ni_base(ni) + CA_NI_L3FE_MY_MAC_HI);

	/*
	 * ILPB_LDPID (0x30d8) is left at 0, as stock.  Log it with
	 * LDPID_REMAP_CTRL (0x313c), _SMAC_PRO (0x3140) and _DMAC_PRO (0x3144).
	 */
	dev_dbg(ni->dev,
		"l3fe-loopback: ilpb(0x30d8)=0x%08x(stock 0) elpb0(0x313c)=0x%08x dqvld1(0x3140)=0x%08x dqvld0(0x3144)=0x%08x my_mac lo(0x3210)=0x%08x hi=0x%08x\n",
		readl(ni_base(ni) + CA_NI_L3FE_GLB_ILPB_LDPID),
		readl(ni_base(ni) + 0x313c), readl(ni_base(ni) + 0x3140),
		readl(ni_base(ni) + 0x3144),
		readl(ni_base(ni) + CA_NI_L3FE_MY_MAC_LO),
		readl(ni_base(ni) + CA_NI_L3FE_MY_MAC_HI));

	dev_dbg(ni->dev, "MYMAC 4 (survived - no SError)\n");
	det = readl(ni_base(ni) + CA_NI_L3FE_SPCL_PKT_DET_CFG);
	ctrl = readl(ni_base(ni) + CA_NI_L3FE_STG0_CTRL);
	dev_dbg(ni->dev,
		"mymac-trap: %pM niA cfg0=0x%08x cfg1=0x%08x pt(a5c0)=0x%08x | det=0x%08x(myen=%u) stg0=0x%08x(chklan=%u)\n",
		mac, readl(ni_base(ni) + CA_NI_L3FE_NI_MAC_CFG0),
		readl(ni_base(ni) + CA_NI_L3FE_NI_MAC_CFG1),
		readl(ni_base(ni) + CA_NI_L3FE_PT_PORT_STATIC_CFG),
		det, !!(det & CA_NI_L3FE_MY_MAC_EN),
		ctrl, !!(ctrl & CA_NI_L3FE_CHK_MYMAC_LAN));
	dev_dbg(ni->dev,
		"mymac-trap: STG0 hi[0-3]=0x%08x 0x%08x 0x%08x 0x%08x lo0=0x%08x mid0=0x%08x ldpid_map=0x%08x\n",
		readl(ni_base(ni) + CA_NI_L3FE_STG0_LPB_HIGH0),
		readl(ni_base(ni) + CA_NI_L3FE_STG0_LPB_HIGH1),
		readl(ni_base(ni) + CA_NI_L3FE_STG0_LPB_HIGH2),
		readl(ni_base(ni) + CA_NI_L3FE_STG0_LPB_HIGH3),
		readl(ni_base(ni) + CA_NI_L3FE_STG0_LPB_LOW0),
		readl(ni_base(ni) + CA_NI_L3FE_STG0_LPB_MID0),
		readl(ni_base(ni) + CA_NI_L3FE_STG0_LDPID_MAP));
}

/*
 * Re-key every MAC-keyed admission/offload table from the current netdev MAC
 * (called from .ndo_set_mac_address once dev_addr is committed).  The tables
 * are first keyed at probe and link-up, possibly before userspace sets the
 * board MAC.  Re-armed in order:
 *   1. the offload backend's router-MAC copy (cortina-ppe.c);
 *   2. the L2FE static FDB (fdb_add_cpu's OP_INIT clears the old entries,
 *      then LAN -> L3_LAN and, when enabled, WAN -> L3_WAN are re-added);
 *   3. my-MAC comparator A and the L3FE STG0 my-MAC (mymac_trap);
 *   4. the PP FIELD-CAM router-MAC entries (intf_add: LAN idx 0, WAN idx 1),
 *      used for the ingress mac_da_an_sel stamp and the IPoE egress SMAC.
 * Only with HW L3 forwarding; otherwise own-MAC frames reach the CPU through
 * the DLF path regardless of the FDB key.
 */
void cortina_ni_rx_mac_rearm(struct cortina_ni *ni)
{
	if (!ni->rx || !ni->tx || !ni->tx->netdev)
		return;

	cortina_ni_rx_fdb_add_cpu(ni);
	if (!cortina_ppe_active())
		return;

	cortina_ppe_router_mac_set(ni->ppe, ni->tx->netdev->dev_addr);
	cortina_ni_rx_mymac_trap(ni);
	cortina_ppe_intf_add(ni->ppe, ni->tx->netdev->dev_addr);
	dev_dbg(ni->dev,
		"MAC-keyed admission re-armed for %pM (FDB + my-MAC + router-CAM)\n",
		ni->tx->netdev->dev_addr);
}

/*
 * Program one REDIR_LDPID_CONFIG entry: @idx (a redir LDPID) -> real dest
 * LDPID.  Indirect table: write DATA, then ACCESS = GO|WR|idx, poll GO clear.
 * Bounded and non-fatal.
 */
static void __maybe_unused
cortina_ni_rx_redir_ldpid_set(struct cortina_ni *ni, u8 idx,
			      u8 dest_ldpid)
{
	void __iomem *acc = ni_base(ni) + CA_NI_L2FE_REDIR_LDPID_ACCESS;
	u32 val;
	int ret;

	writel(FIELD_PREP(CA_NI_L2FE_REDIR_RDIR_LDPID, dest_ldpid) |
	       CA_NI_L2FE_REDIR_RDIR_EN,
	       ni_base(ni) + CA_NI_L2FE_REDIR_LDPID_DATA);
	writel(CA_NI_L2FE_REDIR_ACCESS_GO | CA_NI_L2FE_REDIR_ACCESS_WR | idx, acc);
	ret = readl_poll_timeout(acc, val, !(val & CA_NI_L2FE_REDIR_ACCESS_GO),
				 CA_NI_TX_POLL_US, CA_NI_TX_POLL_TIMEOUT_US);
	if (ret)
		dev_warn(ni->dev,
			 "redir_ldpid[0x%x]->0x%x: ACCESS GO stuck (0x%08x)\n",
			 idx, dest_ldpid, val);
}

/*
 * L2FE forwarding-control registers, stock values (0x1404=0x3,
 * 0x1408=0x22000290, 0x140c=0x0c100c10, 0x1504=0x001b0000, 0x1600=0x89c71c82,
 * 0x160c=0x1).  The port-0 DLF->CPU decision is the DFT_FWD redirect (0x1832)
 * in cortina_ni_rx_ple_dft_fwd(); the REDIR_LDPID table is not used.
 * Idempotent.
 */
static void cortina_ni_rx_l2fe_forwarding(struct cortina_ni *ni)
{
	writel(CA_NI_L2FE_DPID_FWD_CTRL_VAL,
	       ni_base(ni) + CA_NI_L2FE_PLC_DPID_FWD_CTRL);
	writel(CA_NI_L2FE_LRN_FWD_CTRL_0_VAL,
	       ni_base(ni) + CA_NI_L2FE_PLC_LRN_FWD_CTRL_0);
	writel(CA_NI_L2FE_LRN_FWD_CTRL_1_VAL,
	       ni_base(ni) + CA_NI_L2FE_PLC_LRN_FWD_CTRL_1);
	writel(CA_NI_L2FE_PLE_DEFAULT_VAL,
	       ni_base(ni) + CA_NI_L2FE_PLE_DEFAULT_REG);
	/*
	 * arb_ctrl: dbuf_dpid[7:4]=8 (deep_q comparator) and cpu_dpid[27:24]=9
	 * (CPU comparator), stock 0x89c71c82
	 */
	writel(CA_NI_L2FE_ARB_CTRL_VAL, ni_base(ni) + CA_NI_L2FE_ARB_CTRL);
	/* deep_q trigger dbuf_dpid[7:4], as stock (ARB_CTRL 0x89c71c82) */
	ni_rmw(ni, CA_NI_L2FE_ARB_CTRL, CA_NI_L2FE_ARB_DBUF_DPID,
	       FIELD_PREP(CA_NI_L2FE_ARB_DBUF_DPID, CA_NI_L2FE_ARB_DBUF_DPID_VAL));
	dev_dbg(ni->dev, "arb: arb_ctrl=0x%08x\n",
		readl(ni_base(ni) + CA_NI_L2FE_ARB_CTRL));
	writel(CA_NI_L2FE_ARB_REG_1608_VAL,
	       ni_base(ni) + CA_NI_L2FE_ARB_REG_1608);
	writel(CA_NI_L2FE_ARB_CTRL_EXT_VAL,
	       ni_base(ni) + CA_NI_L2FE_ARB_CTRL_EXT);
	/*
	 * 0x1614/18 and 0x161c/20 are the REDIR_LDPID and REDIR_DROP_SRC
	 * ACCESS/DATA pairs, not masks.  They are left at their reset value, as
	 * stock; writing them commits indirect entries with stale DATA and
	 * installs an ldpid 0x3f blackhole.  Read back for the log.
	 */
	dev_dbg(ni->dev,
		"l2fe-fwd: arb_ctrl=0x%08x reg1608=0x%08x redir_ldpid[acc/dat]=0x%08x/0x%08x redir_drop_src[acc/dat]=0x%08x/0x%08x ple_dflt=0x%08x\n",
		readl(ni_base(ni) + CA_NI_L2FE_ARB_CTRL),
		readl(ni_base(ni) + CA_NI_L2FE_ARB_REG_1608),
		readl(ni_base(ni) + CA_NI_L2FE_REDIR_LDPID_ACCESS),
		readl(ni_base(ni) + CA_NI_L2FE_REDIR_LDPID_DATA),
		readl(ni_base(ni) + CA_NI_L2FE_REDIR_DROP_SRC_ACCESS),
		readl(ni_base(ni) + CA_NI_L2FE_REDIR_DROP_SRC_DATA),
		readl(ni_base(ni) + CA_NI_L2FE_PLE_DEFAULT_REG));
}

/*
 * L2TM BM dequeue -> TM-port maps and per-port flow-control thresholds (stock
 * __ni_flow_ctrl_init).  U-Boot does not run this, and with the maps unset a
 * CPU-bound frame is dequeued to the wrong TM port.  Idempotent.
 */

static void cortina_ni_rx_flow_ctrl_init(struct cortina_ni *ni)
{
	int i;

	/* flow-control enable (RMW OR bit19) */
	ni_rmw(ni, CA_NI_NI_FLOWCTRL_EN, 0, CA_NI_NI_FLOWCTRL_EN_BIT);

	/*
	 * BM dequeue -> TM-port uplink-flag map: 0, as stock (the vendor's
	 * __ni_flow_ctrl_init value 0x88888888 is not used).
	 */
	writel(0,
	       ni_base(ni) + CA_NI_L2TM_BM_DQ_PORT_MAP);

	/* physical DQ -> TM-port number map (0x212c), then read both back */
	writel(CA_NI_L2TM_BM_DQ_TO_TM_PORT_IDENT, ni_base(ni) + CA_NI_L2TM_BM_DQ_TO_TM_PORT_MAP);
	dev_dbg(ni->dev,
		"bm-dq-map: REAL 0x2124=0x%08x (uplink-flag, want 0x88888888) 0x212c=0x%08x (phys DQ->TMport, set 0x%08x)\n",
		readl(ni_base(ni) + CA_NI_L2TM_BM_DQ_PORT_MAP),
		readl(ni_base(ni) + CA_NI_L2TM_BM_DQ_TO_TM_PORT_MAP),
		CA_NI_L2TM_BM_DQ_TO_TM_PORT_IDENT);

	/* per-port flow-control thresholds 0x9798..0x97b0 */
	for (i = 0; i < CA_NI_NI_FLOWCTRL_THRESH_CNT; i++)
		ni_rmw(ni, CA_NI_NI_FLOWCTRL_THRESH + i * 4,
		       CA_NI_NI_FLOWCTRL_THRESH_CLR, CA_NI_NI_FLOWCTRL_THRESH_VAL);

	/*
	 * L2TM TM-egress -> CPU-queue map block, stock values (U-Boot leaves
	 * 0x2100 in particular unset).  Without them a non-deep CPU frame is
	 * dequeued to the wrong TM output and never reaches RMU0.
	 */
	writel(CA_NI_L2TM_TM_CFG_VAL, ni_base(ni) + CA_NI_L2TM_TM_CFG);
	writel(CA_NI_L2TM_TM_MAP_A_VAL, ni_base(ni) + CA_NI_L2TM_TM_MAP_A);
	/* 0x2114: stock is the identity map 0x76543210 (TM-port N -> queue N) */
	writel(CA_NI_L2TM_TM_TO_CPUQ_VAL,
	       ni_base(ni) + CA_NI_L2TM_TM_MAP_B);
	writel(CA_NI_L2TM_TM_TO_CPUQ_VAL, ni_base(ni) + CA_NI_L2TM_TM_TO_CPUQ_MAP);
	writel(CA_NI_L2TM_TM_MAP_C_VAL, ni_base(ni) + CA_NI_L2TM_TM_MAP_C);
	writel(CA_NI_L2TM_TM_MAP_D_VAL, ni_base(ni) + CA_NI_L2TM_TM_MAP_D);
	dev_dbg(ni->dev,
		"l2tm tm-map: 0x2100=0x%08x 0x2114=0x%08x 0x2118=0x%08x 0x2120=0x%08x\n",
		readl(ni_base(ni) + CA_NI_L2TM_TM_CFG),
		readl(ni_base(ni) + CA_NI_L2TM_TM_MAP_B),
		readl(ni_base(ni) + CA_NI_L2TM_TM_TO_CPUQ_MAP),
		readl(ni_base(ni) + CA_NI_L2TM_TM_MAP_D));

	/*
	 * NI->L3QM ingress-handoff flow control (stock __ni_flow_ctrl_init +
	 * aal_ni_rxmux_fc_thrshld_set).  Without it the NIRX->L3QM FIFO
	 * back-pressures and the QM never sees the frame.
	 */
	{
		u32 pre2914 = readl(ni_base(ni) + CA_NI_NI_FC_2914);

		writel(CA_NI_NI_FC_2914_STOCK, ni_base(ni) + CA_NI_NI_FC_2914);
		dev_dbg(ni->dev, "0x2914 pre=0x%08x -> 0x%08x (stock=0x800073FF)\n",
			pre2914, readl(ni_base(ni) + CA_NI_NI_FC_2914));
	}
	for (i = 0; i < CA_NI_NI_RXMUX_FC_THR_CNT; i++)
		writel(i < CA_NI_NI_RXMUX_FC_THR_LO_CNT ?
		       CA_NI_NI_RXMUX_FC_THR_LO_VAL :
		       CA_NI_NI_RXMUX_FC_THR_HI_VAL,
		       ni_base(ni) + CA_NI_IDX(CA_NI_NI_RXMUX_FC_THR, i,
					       CA_NI_NI_RXMUX_FC_THR_COUNT));
	/*
	 * RX-mux internal-port calendar and control: the calendar grants the
	 * internal port its RX-mux slots.
	 */
	writel(CA_NI_NI_RXMUX_INTPT_CAL_VAL,
	       ni_base(ni) + CA_NI_NI_RXMUX_INTPT_CAL_CFG1);
	writel(CA_NI_NI_RXMUX_INTPT_CAL_VAL,
	       ni_base(ni) + CA_NI_NI_RXMUX_INTPT_CAL_CFG0);
	writel(CA_NI_NI_RXMUX_CTRL_CFG_VAL,
	       ni_base(ni) + CA_NI_NI_RXMUX_CTRL_CFG);
	dev_dbg(ni->dev,
		"rxmux cal1(0xa07c)=0x%08x cal0(0xa080)=0x%08x ctrl(0xa084)=0x%08x fc0(0xa098)=0x%08x\n",
		readl(ni_base(ni) + CA_NI_NI_RXMUX_INTPT_CAL_CFG1),
		readl(ni_base(ni) + CA_NI_NI_RXMUX_INTPT_CAL_CFG0),
		readl(ni_base(ni) + CA_NI_NI_RXMUX_CTRL_CFG),
		readl(ni_base(ni) + CA_NI_NI_RXMUX_FC_THR(0)));

	/*
	 * INTPT internal-port config, stock values: 0xa900 = 0x0000FF00,
	 * 0xa920/a960/a9a0 = 0x00028000.  0xa960 is INTPT[1] = L3QM.
	 */
	u32 p900 = readl(ni_base(ni) + 0xa900);
	u32 p960 = readl(ni_base(ni) + 0xa960);

	writel(0x0000FF00u, ni_base(ni) + 0xa900);
	writel(0x00028000u, ni_base(ni) + 0xa920);
	writel(0x00028000u, ni_base(ni) + 0xa960);	/* INTPT[1] = L3QM */
	writel(0x00028000u, ni_base(ni) + 0xa9a0);
	dev_dbg(ni->dev,
		"0xa900 pre=0x%08x->0x%08x | 0xa960(L3QM) pre=0x%08x->0x%08x (stock a900=ff00, a920/60/a0=0x28000)\n",
		p900, readl(ni_base(ni) + 0xa900),
		p960, readl(ni_base(ni) + 0xa960));

	/* 0xa1b0/b4/b8 are TXMIB registers on this SoC and are not written */
	dev_dbg(ni->dev,
		"ni->qm handoff: 0x2914=0x%08x rxmux_fc[0]=0x%08x rxfifo_thr(0xa1b8)=0x%08x (l3qm_rxfifo_hi[6:0]=0x%02x)\n",
		readl(ni_base(ni) + CA_NI_NI_FC_2914),
		readl(ni_base(ni) + CA_NI_NI_RXMUX_FC_THR(0)),
		readl(ni_base(ni) + CA_NI_NI_RXFIFO_THR_B8),
		readl(ni_base(ni) + CA_NI_NI_RXFIFO_THR_B8) & 0x7f);
}

/*
 * Enable the L2TM egress scheduler (stock aal_l2_tm_init).  At reset ES_CTRL
 * (0x2300) = 0x24000000: tx_en (bit31) = 0 and no port enabled, so the L2TM
 * never drains a buffered frame to any egress port.  ES port 8 (L3QM) drains
 * the deep queue to the QM -> RMU -> CPU.  Stock enables tx_en, every ES port
 * (0-13 and 15; bit14 is reserved) and all 8 VOQs per scheduler.  RMW-OR, so
 * safe to re-run from link-up.
 */
static void cortina_ni_rx_l2tm_es_init(struct cortina_ni *ni)
{
	int i;

	/*
	 * tx_en + per-port enable (ports 0-13, 15); keep the reset deglitch
	 * bits (26, 29)
	 */
	ni_rmw(ni, CA_NI_L2TM_ES_CTRL, 0,
	       CA_NI_L2TM_ES_TX_EN | CA_NI_L2TM_ES_PORT_EN_ALL);

	/*
	 * per-scheduler VOQ enable (bits[7:0]); the reset default is already
	 * 0xff, re-asserted so instance 8 (L3QM) drains all VOQs
	 */
	for (i = 0; i < CA_NI_L2TM_ES_SCH_INSTANCES; i++)
		ni_rmw(ni, CA_NI_L2TM_ES_SCH_CFG(i), 0, CA_NI_L2TM_ES_VOQ_EN_ALL);

	dev_dbg(ni->dev,
		"l2tm-es: es_ctrl=0x%08x sch0=0x%08x sch8(L3QM)=0x%08x (want es_ctrl bit31+bit8 set, sch voq_en=0xff)\n",
		readl(ni_base(ni) + CA_NI_L2TM_ES_CTRL),
		readl(ni_base(ni) + CA_NI_L2TM_ES_SCH_CFG(0)),
		readl(ni_base(ni) + CA_NI_L2TM_ES_SCH_CFG(CA_NI_L2TM_ES_PORT_L3QM)));
}

/*
 * Deep-queue central-buffer scheduler (L2TE_CB + DQSCH).  Stock populates this
 * block; without it a deep_q=1 frame is enqueued into the central buffer but
 * the scheduler never drains it to RMU0.  The table holds stock's direct
 * threshold registers, restricted to registers that exist on this SoC
 * (status/counter registers excluded); the per-VOQ thresholds are indirect
 * tables programmed separately.
 */
static const struct { u16 off; u32 val; } cortina_ni_deepq_cb_cfg_validated[] = {
	{ 0x23b8, 0x01010101 },	/* L2TM_L2TM_ES_PORT3_0_WEIGHT_RATIO_CFG */
	{ 0x23bc, 0x01010101 },	/* L2TM_L2TM_ES_PORT7_4_WEIGHT_RATIO_CFG */
	{ 0x23c0, 0x01010101 },	/* L2TM_L2TM_ES_PORT11_8_WEIGHT_RATIO_CFG */
	{ 0x23c4, 0x01000101 },	/* L2TM_L2TM_ES_PORT15_12_WEIGHT_RATIO_CFG */
	{ 0x23c8, 0x00000014 },	/* L2TM_L2TM_ES_DRR_WEIGHT_BASE */
	{ 0x23cc, 0x00000740 },	/* L2TM_L2TM_ES_CFG */
	{ 0x2404, 0x0700000f },	/* L2TM_L2TE_GLB_CTRL */
	{ 0x2414, 0x000007ff },	/* L2TM_L2TE_GLB_PTP_CFG */
	{ 0x2500, 0x00000500 },	/* L2TM_L2TE_POL_PKT_TYPE_CTRL0 */
	{ 0x2540, 0x4000000a },	/* L2TM_L2TE_POL_PKT_TYPE_PROFILE_MEM_ACCESS */
	{ 0x2544, 0x00000064 },	/* L2TM_L2TE_POL_PKT_TYPE_PROFILE_MEM_DATA2 */
	{ 0x2548, 0x00017c01 },	/* L2TM_L2TE_POL_PKT_TYPE_PROFILE_MEM_DATA1 */
	{ 0x254c, 0x900005f0 },	/* L2TM_L2TE_POL_PKT_TYPE_PROFILE_MEM_DATA0 */
	{ 0x2550, 0x00000502 },	/* L2TM_L2TE_POL_PKT_TYPE_COUNTER_MEM_ACCESS */
	{ 0x2554, 0x00000502 },	/* L2TM_L2TE_POL_PKT_TYPE_COUNTER_MEM_DATA1 */
	{ 0x2558, 0x00000502 },	/* L2TM_L2TE_POL_PKT_TYPE_COUNTER_MEM_DATA0 */
	{ 0x255c, 0x00000502 },	/* L2TM_L2TE_POL_PORT_CTRL0 */
	{ 0x259c, 0x40000006 },	/* L2TM_L2TE_POL_PORT_PROFILE_MEM_ACCESS */
	{ 0x25a0, 0x00000040 },	/* L2TM_L2TE_POL_PORT_PROFILE_MEM_DATA2 */
	{ 0x25a4, 0x7ffff9ff },	/* L2TM_L2TE_POL_PORT_PROFILE_MEM_DATA1 */
	{ 0x25a8, 0xfdffffe7 },	/* L2TM_L2TE_POL_PORT_PROFILE_MEM_DATA0 */
	{ 0x25ac, 0x00000502 },	/* L2TM_L2TE_POL_PORT_COUNTER_MEM_ACCESS */
	{ 0x25b0, 0x00000502 },	/* L2TM_L2TE_POL_PORT_COUNTER_MEM_DATA1 */
	{ 0x25b4, 0x00000502 },	/* L2TM_L2TE_POL_PORT_COUNTER_MEM_DATA0 */
	{ 0x25b8, 0x00000502 },	/* L2TM_L2TE_POL_AGRFLOW_CTRL0 */
	{ 0x25f4, 0x2ff3e723 },	/* L2TM_L2TE_POL_TB_CTRL */
	{ 0x25f8, 0x000001f3 },	/* L2TM_L2TE_POL_TB_CTRL1 */
	{ 0x2700, 0x14141414 },	/* L2TM_L2TE_SHP_IPG_PROFILE */
	{ 0x2714, 0x0000007c },	/* L2TM_L2TE_SHP_CTRL */
	{ 0x2718, 0x40000006 },	/* L2TM_L2TE_SHP_PORT_TBC_MEM_ACCESS */
	{ 0x271c, 0x3ffffe01 },	/* L2TM_L2TE_SHP_PORT_TBC_MEM_DATA1 */
	{ 0x2720, 0x01ffffe7 },	/* L2TM_L2TE_SHP_PORT_TBC_MEM_DATA0 */
	{ 0x274c, 0x2ff3e723 },	/* L2TM_L2TE_SHP_TB_CTRL */
	{ 0x2750, 0x000001f3 },	/* L2TM_L2TE_SHP_TB_CTRL1 */
	{ 0x2d7c, 0x02000010 },	/* L2TM_L2TE_CB_PORT_THRSH_PROFILE0 */
	{ 0x2d80, 0x7fff7fff },	/* L2TM_L2TE_CB_PORT_GRPTHRSH_PROFILE0 */
	{ 0x2db0, 0x00000002 },	/* L2TM_L2TE_CB_VOQ_THRSH_SELECT_MEM_DATA */
	{ 0x2db4, 0x4000000f },	/* L2TM_L2TE_CB_PORT_FREECNT_MEM_ACCESS */
	{ 0x2db8, 0x8e308000 },	/* L2TM_L2TE_CB_PORT_FREECNT_MEM_DATA */
	{ 0x2dcc, 0x0e200020 },	/* L2TM_L2TE_CB_GLB_THRSH */
	{ 0x2dd0, 0x00200010 },	/* L2TM_L2TE_CB_GLB_PRI_THRSH0 */
	{ 0x2df4, 0x0c000100 },	/* L2TM_L2TE_CB_SRC_PORT_THRSH_PROFILE0 */
	{ 0x2e08, 0x02800110 },	/* L2TM_L2TE_CB_SRC0_PRI_THRSH0 */
	{ 0x2e28, 0x02800110 },	/* L2TM_L2TE_CB_SRC1_PRI_THRSH0 */
	{ 0x2e58, 0x00007fff },	/* L2TM_L2TE_CB_DQSCH_EQ_PROFILE_THRSH0 */
	{ 0x2e5c, 0x00200020 },	/* L2TM_L2TE_CB_DQSCH_PORT_THRSH_PROFILE0 */
	{ 0x2ec8, 0x7fff7fff },	/* L2TM_L2TE_CB_WIFI_BUF_THRSH_G0 */
	{ 0x2ee8, 0x7fff7fff },	/* L2TM_L2TE_CB_WIFI_EQ_PROFILE_THRSH */
};

/*
 * L2TM deep-queue per-VoQ admission thresholds.
 *
 * The direct registers in cortina_ni_deepq_cb_cfg_validated[] that hold
 * 0x7fff7fff (0x2d80.., 0x2ec8..0x2ee8) match stock and are not affected here.
 * This covers the two indexed per-VoQ profile tables, written through the
 * ACCESS/DATA protocol:
 *   DQSCH VOQ  ACCESS 0x2e70 / DATA  0x2e74   - stock 0x00700070 (lo = hi = 112)
 *   CB    VOQ  ACCESS 0x2da0 / DATA1 0x2da4   - stock 0x0FFFFFFF
 *                            DATA0 0x2da8     - stock 0xFFFFFFFF
 * By default all 8 entries of both get the permissive 0x7fff7fff: much deeper
 * than stock on the DQSCH table, shallower than stock on the CB table.
 *
 * Only deep-queue traffic is governed by these: HW-offloaded upstream flows
 * when the data T-CONT is <= 7 (CN_L3E_PON_DEEPQ_TCONT_MAX), offloaded
 * downstream flows with hw_ds_deepq=1, and frames trapped to the CPU.
 * CPU-forwarded upstream frames never set deep_q.
 *
 * The active values and a live readback are printed in the "deepq-thrsh"
 * line of /proc/net/cortina_ni_rx.
 */

/*
 * Write the current parameter values into all 8 entries of both per-VoQ
 * threshold profile tables (DQSCH @0x2e70/0x2e74 and CB @0x2da0/0x2da4/0x2da8).
 */
static void cortina_ni_rx_deepq_thrsh_program(struct cortina_ni *ni)
{
	unsigned int i;

	for (i = 0; i < CA_NI_L2TM_DEEPQ_VOQ_ENTRIES; i++) {
		writel(CA_NI_L2TM_DEEPQ_PROFILE_PERMISSIVE,
		       ni_base(ni) + CA_NI_L2TM_DQSCH_VOQ_THRSH_DATA);
		cortina_ni_rx_ind_store(ni, CA_NI_L2TM_DQSCH_VOQ_THRSH_ACCESS, i);

		writel(CA_NI_L2TM_DEEPQ_PROFILE_PERMISSIVE,
		       ni_base(ni) + CA_NI_L2TM_CB_VOQ_THRSH_DATA1);
		writel(CA_NI_L2TM_DEEPQ_PROFILE_PERMISSIVE,
		       ni_base(ni) + CA_NI_L2TM_CB_VOQ_THRSH_DATA0);
		cortina_ni_rx_ind_store(ni, CA_NI_L2TM_CB_VOQ_THRSH_ACCESS, i);
	}
}

/*
 * Deep-queue / DQSCH init (stock aal_l2_tm_cb_init).  Without it a deep_q=1
 * frame is enqueued into the central buffer but never dequeued to RMU0.  The
 * order is mandatory: direct thresholds/watermarks/credits/per-queue config,
 * then the indexed per-VOQ profile tables (two-phase ACCESS/DATA), then ARB
 * dbuf_sel, then the two master enables last and in order: 0x2d0c bit31 (CB
 * scheduler run) and 0x2eec bit31 (ABR arbiter).  Arming before the config
 * is loaded runs an unconfigured scheduler that drops frames.
 */
static void cortina_ni_rx_deepq_sched_init(struct cortina_ni *ni)
{
	unsigned int i;

	/*
	 * Direct L2TE_CB/DQSCH registers.  Only the validated subset is
	 * written: the full vendor table also writes undefined offsets and
	 * status registers, which raises an asynchronous SError here.
	 */
	unsigned int lim = ARRAY_SIZE(cortina_ni_deepq_cb_cfg_validated);

	for (i = 0; i < lim; i++)
		writel(cortina_ni_deepq_cb_cfg_validated[i].val,
		       ni_base(ni) + cortina_ni_deepq_cb_cfg_validated[i].off);
	dev_dbg(ni->dev, "deepq: %u validated CB/DQSCH entries written\n", lim);

	/*
	 * TE_TE_GLB_CTRL (0x9004) deep-queue dequeue-ready gates.  At reset 0
	 * the DQSCH never drains a deep_q=1 frame from the central buffer and
	 * counts no drop.  The vendor's aal_l3_te_init sets both fields to 0xff
	 * (0x00ffff00); only those two fields are modified.
	 */
	ni_rmw(ni, CA_NI_TE_TE_GLB_CTRL,
	       CA_NI_TE_GLB_DEEPQ_VOQ_RDY | CA_NI_TE_GLB_DEEPQ_PORT_RDY,
	       CA_NI_TE_GLB_DEEPQ_VOQ_RDY | CA_NI_TE_GLB_DEEPQ_PORT_RDY);
	dev_dbg(ni->dev,
		"TE_TE_GLB_CTRL(0x9004)=0x%08x [want deepq voq/port rdy => 0x00ffff00; 0x0 = offset undecoded/abort]\n",
		readl(ni_base(ni) + CA_NI_TE_TE_GLB_CTRL));

	/*
	 * TE_CB DQSCH thresholds (0x96xx): port hth=1000 (the reset value 0
	 * keeps the port permanently congested) and voq lth=768, for all 4 port
	 * and 8 voq profiles, as the vendor's profile 0.
	 */

	/*
	 * program the TE_CB DQSCH thresholds (0x96xx) - the block previously written only
	 * at the L2TM twin (0x2e5c/0x2e70).  Port hth=1000 lifts the reset-0 perpetual-congested
	 * backpressure; voq lth=768.  All 4 port + 8 voq profiles = vendor profile-0 value.
	 */
	unsigned int p;

	for (p = 0; p < CA_NI_TE_DQSCH_PORT_THRSH0_COUNT; p++)
		writel(CA_NI_TE_DQSCH_PORT_HTH_VAL,
		       ni_base(ni) + CA_NI_TE_DQSCH_PORT_THRSH0 + p * 4);
	for (p = 0; p < 8; p++) {
		writel(CA_NI_TE_DQSCH_VOQ_LTH_VAL,
		       ni_base(ni) + CA_NI_TE_DQSCH_VOQ_THRSH_DATA);
		cortina_ni_rx_ind_store(ni, CA_NI_TE_DQSCH_VOQ_THRSH_ACCESS, p);
	}
	cortina_ni_rx_ind_read(ni, CA_NI_TE_DQSCH_VOQ_THRSH_ACCESS, 0);
		dev_dbg(ni->dev,
			"TE DQSCH port_thr[0](0x963c)=0x%08x voq_thr[0](0x9658)=0x%08x [want port=0x03e80000 voq=0x300]\n",
			readl(ni_base(ni) + CA_NI_TE_DQSCH_PORT_THRSH0),
			readl(ni_base(ni) + CA_NI_TE_DQSCH_VOQ_THRSH_DATA));

	/*
	 * (2) CB per-port free-buffer count (indexed, all 48 ports); without it
	 * the CB has no free buffers and drops the frame at enqueue.
	 */
	for (i = 0; i < CA_NI_L2TM_CB_PORT_COUNT; i++) {
		writel(CA_NI_L2TM_CB_FREECNT_VAL,
		       ni_base(ni) + CA_NI_L2TM_CB_PORT_FREECNT_DATA);
		cortina_ni_rx_ind_store(ni, CA_NI_L2TM_CB_PORT_FREECNT_ACCESS, i);
	}

	/*
	 * (3) the two indexed per-VOQ profile tables (8 entries each).  A zero
	 * profile drops everything.  Table A = DQSCH VOQ (0x2e74/0x2e70),
	 * table B = CB VOQ (0x2da4+0x2da8/0x2da0).
	 */
	cortina_ni_rx_deepq_thrsh_program(ni);

	/*
	 * (4) ARB_CTRL.dbuf_sel (bit1) = 1 so a PDPID-8 frame takes the
	 * deep-buffer path
	 */
	ni_rmw(ni, CA_NI_L2FE_ARB_CTRL, 0, CA_NI_L2FE_ARB_DBUF_SEL);

	/*
	 * 0x2f00/04/08 are L2TM_L2TE_PM_CTRL / PM_EVENT_CFG_0 / PM_STS (the L2TE
	 * performance monitor; 0x2f08 is a status register), not a DQSCH output
	 * map.  Writing them raises an asynchronous SError, so they are left alone.
	 */

	/* (5) the two master enables, last and in order (cb_ctrl then abr_ctrl) */
	writel(CA_NI_L2TM_CB_CTRL_STOCK, ni_base(ni) + CA_NI_L2TM_CB_CTRL);
	writel(CA_NI_L2TM_CB_ABR_CTRL_STOCK, ni_base(ni) + CA_NI_L2TM_CB_ABR_CTRL);

	/*
	 * (6) TE-block master enables (vendor aal_l3_te_init): TE_FC_CTRL.rx_en
	 * lets frames enter the deep-queue central buffer and
	 * TE_CB_CTRL.scan_enable drains it toward the QM.  RMW keeps the reset
	 * low bits (scan cycle, dync_dq_port_to_pon_sel).
	 */
	u32 fc = readl(ni_base(ni) + CA_NI_TE_FC_CTRL);
	u32 cb = readl(ni_base(ni) + CA_NI_TE_CB_CTRL);

	writel(fc | CA_NI_TE_FC_CTRL_RX_EN, ni_base(ni) + CA_NI_TE_FC_CTRL);
	writel(cb | CA_NI_TE_CB_CTRL_SCAN_EN, ni_base(ni) + CA_NI_TE_CB_CTRL);
	dev_dbg(ni->dev,
		"TE_FC_CTRL(0x9480) 0x%08x->0x%08x (rx_en) | TE_CB_CTRL(0x950c) 0x%08x->0x%08x (scan_en)\n",
		fc, readl(ni_base(ni) + CA_NI_TE_FC_CTRL),
		cb, readl(ni_base(ni) + CA_NI_TE_CB_CTRL));

	dev_dbg(ni->dev,
		"deepq-sched: cb_ctrl(0x2d0c)=0x%08x abr(0x2eec)=0x%08x out_map(0x2f04)=0x%08x dqschA(0x2e70)=0x%08x cbB(0x2da0)=0x%08x arb=0x%08x\n",
		readl(ni_base(ni) + CA_NI_L2TM_CB_CTRL),
		readl(ni_base(ni) + CA_NI_L2TM_CB_ABR_CTRL),
		readl(ni_base(ni) + CA_NI_L2TM_DQSCH_OUT_PORT_MAP),
		readl(ni_base(ni) + CA_NI_L2TM_DQSCH_VOQ_THRSH_ACCESS),
		readl(ni_base(ni) + CA_NI_L2TM_CB_VOQ_THRSH_ACCESS),
		readl(ni_base(ni) + CA_NI_L2FE_ARB_CTRL));
}

/*
 * Enable port MAC blocks 1..5 with the stock values (RXMAC=0x3101,
 * TXMAC=0x04055901, RX_CNTRL=0x08000600).  Port 0 is programmed by the link
 * path.  The loop writes 0xa5c8 + p * 0x90; a seventh block would overlap the
 * INTPT block at 0xa928 (writing it raises an asynchronous SError).
 * Idempotent.
 */
static void cortina_ni_rx_enable_internal_ports(struct cortina_ni *ni)
{
	int p;

	for (p = 1; p < CA_NI_RX_PORT_COUNT; p++) {
		writel(CA_NI_PORT_RXMAC_EN_VAL,
		       ni_base(ni) + CA_NI_PORT_RXMAC_CFG(p));
		writel(CA_NI_PORT_TXMAC_EN_VAL,
		       ni_base(ni) + CA_NI_PORT_TXMAC_CFG(p));
		writel(CA_NI_PORT_RX_CNTRL_STOCK_VAL,
		       ni_base(ni) + CA_NI_PORT_RX_CNTRL_CFG(p));
	}
}

/*
 * KEY rows, struct word0..word10.  The CLS key table is partitioned:
 * profile 0 = KEY[0..63] (WAN ingress), profile 1 = KEY[64..127] (LAN ingress;
 * the STG0 LPB t1_ctrl selects the profile).  The same three rows are
 * installed in both profiles.
 */
static const struct { u16 idx; u32 w[CA_NI_L3FE_CLS_KEY_WORDS]; } cls_key_golden[] = {
	/* profile 0 (WAN) */
	{ 0, { 0xFFFFFEFF, 0xFEFFFFFF, 0xF77FFFFF, 0xFFFFFFFF, 0xFFFDFFFF,
	       0x0000003F, 0, 0, 0, 0, 0x18240000 } },
	{ 1, { 0xFFFFFECF, 0xCFFFFFFF, 0x0007FFFF, 0, 0,
	       0, 0, 0, 0, 0, 0x08040000 } },
	{ 2, { 0xFFFFFFFF, 0xFFFFFFFF, 0x0007FFFF, 0, 0,
	       0, 0, 0, 0, 0, 0x08000000 } },
	/* profile 1 (LAN) - identical rows at +64 */
	{ 64, { 0xFFFFFEFF, 0xFEFFFFFF, 0xF77FFFFF, 0xFFFFFFFF, 0xFFFDFFFF,
		0x0000003F, 0, 0, 0, 0, 0x18240000 } },
	{ 65, { 0xFFFFFECF, 0xCFFFFFFF, 0x0007FFFF, 0, 0,
		0, 0, 0, 0, 0, 0x08040000 } },
	{ 66, { 0xFFFFFFFF, 0xFFFFFFFF, 0x0007FFFF, 0, 0,
		0, 0, 0, 0, 0, 0x08000000 } },
};

/*
 * FIB rows, struct word0..word6 (word0 = DATA0 = 0x33cc).  Profile 0 rows
 * 0/1/2 -> FIB 0,1,4,8; profile 1 rows 64/65/66 -> FIB 256,257,260,264.
 */
static const struct { u16 idx; u32 w[CA_NI_L3FE_CLS_FIB_WORDS]; } cls_fib_golden[] = {
	/* profile 0 (WAN) */
	{ 0,   { 0, 0, 0, 0, 0x1C000000, 0x01000004, 0x00000A00 } },
	{ 1,   { 0, 0, 0, 0, 0x1C000000, 0x01000004, 0x00000A00 } },
	{ 4,   { 0, 0, 0, 0, 0x1C000000, 0x01000004, 0x00000200 } },
	{ 8,   { 0, 0, 0, 0, 0x1C000000, 0x01000004, 0x00000600 } },
	/* profile 1 (LAN) */
	{ 256, { 0, 0, 0, 0, 0x1C000000, 0x01000004, 0x00000A00 } },
	{ 257, { 0, 0, 0, 0, 0x1C000000, 0x01000004, 0x00000A00 } },
	{ 260, { 0, 0, 0, 0, 0x1C000000, 0x01000004, 0x00000200 } },
	{ 264, { 0, 0, 0, 0, 0x1C000000, 0x01000004, 0x00000600 } },
};

/*
 * L3FE parse-error forward actions (aal_l3_specpkt_err_fwd_ctrl_set), stock
 * values; see CA_NI_L3FE_PP_PARSING_ERR_FWD_0 in cortina-ni-regs.h.  Plain
 * config registers, not the SPKTP 0x333c/0x3340 indirect table.
 */
static void cortina_ni_rx_l3fe_specpkt_init(struct cortina_ni *ni)
{
	writel(CA_NI_L3FE_PP_PARSING_ERR_FWD_0_STOCK,
	       ni_base(ni) + CA_NI_L3FE_PP_PARSING_ERR_FWD_0);
	writel(CA_NI_L3FE_PP_PARSING_ERR_FWD_1_STOCK,
	       ni_base(ni) + CA_NI_L3FE_PP_PARSING_ERR_FWD_1);

	dev_dbg(ni->dev,
		"specpkt: parsing_err_fwd0(0x3240)=0x%08x fwd1(0x3244)=0x%08x  [stock: 00008000 00029008]\n",
		readl(ni_base(ni) + CA_NI_L3FE_PP_PARSING_ERR_FWD_0),
		readl(ni_base(ni) + CA_NI_L3FE_PP_PARSING_ERR_FWD_1));
}

/*
 * L3FE global init (vendor aal_l3fe_l2lookup_init and the glb ELPB/deep-queue
 * setters, from aal_l3fe_init).  Left at reset, the L3FE never ingests a frame.
 * The values are the stock ones, which differ from the SDK's derivation
 * (ILPB_LDPID 0x30d8 is left 0, as stock).  Some register names are inherited
 * from another SoC: 0x30a4 is GLB_CFG, 0x30a8 the real LF_CFG and 0x30ac
 * TE_OPTION here.
 */
static void cortina_ni_rx_l3fe_glb_init(struct cortina_ni *ni)
{
	/*
	 * forwarding control 1/2/3 and the 0x30cc slot.  0x30b4
	 * (CLS_STG_MONITOR_RETURN) and 0x30bc (DBG_DAT) are not written; stock
	 * does not write them either.
	 */
	writel(CA_NI_L3FE_GLB_FWD_CTRL_1_VAL, ni_base(ni) + CA_NI_L3FE_GLB_FWD_CTRL_1);
	writel(CA_NI_L3FE_GLB_FWD_CTRL_2_VAL, ni_base(ni) + CA_NI_L3FE_GLB_FWD_CTRL_2);
	writel(CA_NI_L3FE_GLB_FWD_CTRL_3_VAL, ni_base(ni) + CA_NI_L3FE_GLB_FWD_CTRL_3);
	writel(CA_NI_L3FE_GLB_CFG_30CC_VAL, ni_base(ni) + CA_NI_L3FE_GLB_CFG_30CC);

	/*
	 * egress-loopback entry, deep-queue valid vector and deep-queue vector
	 * (the ELPB block that lets an L2FE frame loop into the L3FE ingress)
	 */
	writel(CA_NI_L3FE_GLB_ELPB0_VAL, ni_base(ni) + CA_NI_L3FE_GLB_ELPB0);
	writel(CA_NI_L3FE_GLB_ELPB_DEEPQ_VLD1_VAL,
	       ni_base(ni) + CA_NI_L3FE_GLB_ELPB_DEEPQ_VLD1);
	writel(CA_NI_L3FE_GLB_ELPB_DEEPQ_VLD0_VAL,
	       ni_base(ni) + CA_NI_L3FE_GLB_ELPB_DEEPQ_VLD0);
	writel(CA_NI_L3FE_GLB_ELPB_DEEPQ1_VAL,
	       ni_base(ni) + CA_NI_L3FE_GLB_ELPB_DEEPQ1);
	writel(CA_NI_L3FE_GLB_ELPB_DEEPQ0_VAL,
	       ni_base(ni) + CA_NI_L3FE_GLB_ELPB_DEEPQ0);

	/* the L3FE<->L2FE loopback ldpid binding + the VLAN-edit tpid config */
	writel(CA_NI_L3FE_GLB_L3FE_L2FE_LDPID_VAL,
	       ni_base(ni) + CA_NI_L3FE_GLB_L3FE_L2FE_LDPID);
	writel(CA_NI_L3FE_GLB_VE_VAL, ni_base(ni) + CA_NI_L3FE_GLB_VE);

	/* LDPID_REMAP_VAL0/VAL1, the last two of the LDPID-remap registers */
	writel(CA_NI_L3FE_GLB_LDPID_REMAP_VAL0_VAL,
	       ni_base(ni) + CA_NI_L3FE_GLB_LDPID_REMAP_VAL0);
	writel(CA_NI_L3FE_GLB_LDPID_REMAP_VAL1_VAL,
	       ni_base(ni) + CA_NI_L3FE_GLB_LDPID_REMAP_VAL1);

	dev_dbg(ni->dev,
		"l3fe-glb-init: glb_cfg(0x30a4)=0x%08x lf_cfg(0x30a8)=0x%08x te_opt(0x30ac)=0x%08x res_ctrl(0x30cc)=0x%08x | RELOCATED elpb0(0x313c)=0x%08x dqvld(0x3140/44)=0x%08x/0x%08x dq(0x3148/4c)=0x%08x/0x%08x l2fe_ldpid(0x3150)=0x%08x ve(0x3154)=0x%08x | NEW remap_val0(0x3158)=0x%08x remap_val1(0x315c)=0x%08x  [stock: 80020000 004641f4 00000300 00000e21 | 00007f03 00c00000 00400000 00f00000 00f00000 000c0000 00040000 | 04421088 00002211]\n",
		readl(ni_base(ni) + CA_NI_L3FE_GLB_FWD_CTRL_1),
		readl(ni_base(ni) + CA_NI_L3FE_GLB_FWD_CTRL_2),
		readl(ni_base(ni) + CA_NI_L3FE_GLB_FWD_CTRL_3),
		readl(ni_base(ni) + CA_NI_L3FE_GLB_CFG_30CC),
		readl(ni_base(ni) + CA_NI_L3FE_GLB_ELPB0),
		readl(ni_base(ni) + CA_NI_L3FE_GLB_ELPB_DEEPQ_VLD1),
		readl(ni_base(ni) + CA_NI_L3FE_GLB_ELPB_DEEPQ_VLD0),
		readl(ni_base(ni) + CA_NI_L3FE_GLB_ELPB_DEEPQ1),
		readl(ni_base(ni) + CA_NI_L3FE_GLB_ELPB_DEEPQ0),
		readl(ni_base(ni) + CA_NI_L3FE_GLB_L3FE_L2FE_LDPID),
		readl(ni_base(ni) + CA_NI_L3FE_GLB_VE),
		readl(ni_base(ni) + CA_NI_L3FE_GLB_LDPID_REMAP_VAL0),
		readl(ni_base(ni) + CA_NI_L3FE_GLB_LDPID_REMAP_VAL1));
}

/*
 * L3FE AXI read-reorder channel (vendor aal_l3fe_axi_reo_init, the last step
 * of aal_l3fe_init), at AXI-REO window + 0x2080.  Without it the L3FE cannot
 * fetch a frame from memory.  The values are the SDK's.
 */
static void cortina_ni_rx_l3fe_axi_reo_init(struct cortina_ni *ni)
{
	void __iomem *reo = ni->win[CA_NI_WIN_AXI_REO];

	if (!reo) {
		dev_warn(ni->dev, "RX: L3FE AXI-REO window (idx %d) not mapped\n",
			 CA_NI_WIN_AXI_REO);
		return;
	}
	writel(CA_NI_L3FE_AXI_REO_ORIG_ID_VAL, reo + CA_NI_L3FE_AXI_REO_ORIG_ID);
	writel(CA_NI_L3FE_AXI_REO_NEW_ID_VAL, reo + CA_NI_L3FE_AXI_REO_NEW_ID);
	writel(CA_NI_L3FE_AXI_REO_TOP_ADDR_VAL, reo + CA_NI_L3FE_AXI_REO_TOP_ADDR);
	writel(CA_NI_L3FE_AXI_REO_TOP_ADDR_MASK_VAL,
	       reo + CA_NI_L3FE_AXI_REO_TOP_ADDR_MASK);
	writel(CA_NI_L3FE_AXI_REO_NEW_ID0_VAL, reo + CA_NI_L3FE_AXI_REO_NEW_ID0);
	writel(CA_NI_L3FE_AXI_REO_RD18_VAL, reo + CA_NI_L3FE_AXI_REO_RD18);
	writel(CA_NI_L3FE_AXI_REO_RD24_VAL, reo + CA_NI_L3FE_AXI_REO_RD24);

	dev_dbg(ni->dev,
		"l3fe-axi-reo (win10+0x480, abs 0xf432d480): orig=0x%08x new=0x%08x top=0x%08x mask=0x%08x new0=0x%08x +18=0x%08x +24=0x%08x (want 2/8|/1e8/1e8-mask/9|/FFFFFFFF x2)\n",
		readl(reo + CA_NI_L3FE_AXI_REO_ORIG_ID),
		readl(reo + CA_NI_L3FE_AXI_REO_NEW_ID),
		readl(reo + CA_NI_L3FE_AXI_REO_TOP_ADDR),
		readl(reo + CA_NI_L3FE_AXI_REO_TOP_ADDR_MASK),
		readl(reo + CA_NI_L3FE_AXI_REO_NEW_ID0),
		readl(reo + CA_NI_L3FE_AXI_REO_RD18),
		readl(reo + CA_NI_L3FE_AXI_REO_RD24));
}

static void cortina_ni_rx_cls_init(struct cortina_ni *ni)
{
	unsigned int e, i;

	/*
	 * L3FE STG0_CTRL = stock 0x001c787c first: the reset value 0x001c7c7e
	 * has bits 1 and 10 set (lpb_idx_mode etc.), which mis-indexes the STG0
	 * LPB so the CLS lookup cannot match LAN ingress.
	 */
	writel(CA_NI_L3FE_STG0_CTRL_VAL, ni_base(ni) + CA_NI_L3FE_STG0_CTRL);

	if (IS_ENABLED(CONFIG_CORTINA_NI_FLOWOFFLOAD)) {
		/* the L3FE is in the path: the full stock classifier and CAMs */
		if (cortina_ppe_cls_stock_load(ni_base(ni)))
			dev_warn(ni->dev, "cls: stock classifier load failed\n");
	} else {
		/* commit all FIBs first, then all KEYs (stock aal_l3_cls_add order) */
		for (e = 0; e < ARRAY_SIZE(cls_fib_golden); e++) {
			for (i = 0; i < CA_NI_L3FE_CLS_FIB_WORDS; i++)
				writel(cls_fib_golden[e].w[i],
				       ni_base(ni) + CA_NI_L3FE_CLS_FIB_ACCESS +
				       (CA_NI_L3FE_CLS_FIB_WORDS - i) * 4);
			cortina_ni_rx_ind_store(ni, CA_NI_L3FE_CLS_FIB_ACCESS,
						cls_fib_golden[e].idx);
		}
		for (e = 0; e < ARRAY_SIZE(cls_key_golden); e++) {
			for (i = 0; i < CA_NI_L3FE_CLS_KEY_WORDS; i++)
				writel(cls_key_golden[e].w[i],
				       ni_base(ni) + CA_NI_L3FE_CLS_KEY_ACCESS +
				       (CA_NI_L3FE_CLS_KEY_WORDS - i) * 4);
			cortina_ni_rx_ind_store(ni, CA_NI_L3FE_CLS_KEY_ACCESS,
						cls_key_golden[e].idx);
		}
	}

	/*
	 * With the flow engine active, re-provision the PP FIELD-CAM router-MAC entries
	 * (intf_add).  The CLS rows above already carry stock's t2_ctrl and the
	 * CPU_0 miss disposition: a routed frame hits its row, runs T2, forwards
	 * on a hit and punts to CPU_0 on a miss.
	 */
	if (cortina_ppe_active() && ni->tx && ni->tx->netdev) {
		int ret = cortina_ppe_intf_add(ni->ppe,
					       ni->tx->netdev->dev_addr);

		dev_dbg(ni->dev, "cls: PP MAC-DA router-CAM re-applied %s\n",
			ret ? "FAILED" : "ok");
	}

	/* read back the broadcast row (KEY[2]) for the boot log */
	cortina_ni_rx_ind_read(ni, CA_NI_L3FE_CLS_KEY_ACCESS, 2);
	dev_dbg(ni->dev,
		"cls-trap: stg0_ctrl(0x3400)=0x%08x (want 0x001c787c); rows 0/1/2 + fib 0/1/4/8 written; KEY[2] w0(0x33ac)=0x%08x trailer(0x3384)=0x%08x det_cfg(0x3218)=0x%08x\n",
		readl(ni_base(ni) + CA_NI_L3FE_STG0_CTRL),
		readl(ni_base(ni) + CA_NI_L3FE_CLS_KEY_ACCESS + 11 * 4),
		readl(ni_base(ni) + CA_NI_L3FE_CLS_KEY_ACCESS + 1 * 4),
		readl(ni_base(ni) + CA_NI_L3FE_SPCL_PKT_DET_CFG));
}

/*
 * RX steering through the full forwarding engine: an ingress frame is looked
 * up by the L2FE and, as a lookup miss (DLF), default-forwarded by the PLE to
 * CPU port 0, while the L3FE demux map routes that output into the CPU-EPP
 * ring.  Idempotent, so it is re-run from link-up.
 */
static int cortina_ni_rx_steer_init(struct cortina_ni *ni)
{
	int type, ret;

	/* L2TM BM dequeue -> TM-port map, so a CPU-bound frame reaches the QM */
	cortina_ni_rx_flow_ctrl_init(ni);

	/* deep-queue scheduler: drains the deep queue to the RMU */
	cortina_ni_rx_deepq_sched_init(ni);

	/*
	 * L2TM egress scheduler (ES port 8 = L3QM), off at reset, so the deep
	 * queue is drained to the QM
	 */
	cortina_ni_rx_l2tm_es_init(ni);

	/* L2FE per-port profiles (ILPB stp, MMSHP isolation, ELPB egr-stp, IPPB) */
	cortina_ni_rx_port_profiles_init(ni);

	/*
	 * One-shot L2E hash (FDB) init: an uninitialised hash SRAM could
	 * false-hit and mis-steer unicast.  Only once, as re-running it would
	 * flush learned entries.
	 */
	{
		static bool fdb_hash_ready;

		if (!fdb_hash_ready) {
			u32 acc;

			writel(CA_NI_L2FE_FDB_GO | CA_NI_L2FE_FDB_OP_INIT,
			       ni_base(ni) + CA_NI_L2FE_FDB_ACCESS);
			if (readl_poll_timeout(ni_base(ni) + CA_NI_L2FE_FDB_ACCESS,
					       acc, !(acc & CA_NI_L2FE_FDB_GO),
					       10, 20000))
				dev_warn(ni->dev, "fdb hash init timeout (0x%08x)\n",
					 acc);
			else
				fdb_hash_ready = true;
		}
	}

	/*
	 * L2FE forwarding-control registers; the DFT_FWD loop below sets the
	 * port-0 DLF -> CPU redirect (0x1832)
	 */
	cortina_ni_rx_l2fe_forwarding(ni);

	/*
	 * FDB global control (aal_fdb_ctrl_set 0x1c00/0x1c04), stock values:
	 * enables the per-type FDB forwarding actions so unknown-DA and
	 * broadcast frames take the LRN_FWD_CTRL dest 0x10 (CPU_0).
	 */
	writel(CA_NI_L2FE_FDB_CTRL_0_VAL, ni_base(ni) + CA_NI_L2FE_FDB_CTRL_0);
	writel(CA_NI_L2FE_FDB_CTRL_1_VAL, ni_base(ni) + CA_NI_L2FE_FDB_CTRL_1);

	/* FDB engine init (OP_INIT builds the hash table) and the own-MAC entries */
	cortina_ni_rx_fdb_add_cpu(ni);

	/* ARB deep queue and PDPID map; must run before the DFT_FWD redirect */
	cortina_ni_rx_arb_deepq_init(ni);
	cortina_ni_rx_flow_dbuf_init(ni);	/* zero the deep_q source table, as stock */

	/* MC group setup; must run before the DFT_FWD loop points at the group */
	cortina_ni_rx_mc_group_init(ni);

	/* own-MAC CPU trap (my-MAC comparator and STG0 LPB profiles) */
	cortina_ni_rx_mymac_trap(ni);

	/*
	 * L3FE global init, before the CLS rows, as in the vendor aal_l3fe_init
	 * order; this is what lets a frame enter the L3FE stage
	 */
	cortina_ni_rx_l3fe_glb_init(ni);
	cortina_ni_rx_l3fe_specpkt_init(ni);

	/* the L3FE's own AXI read-reorder channel, last as in aal_l3fe_init */
	cortina_ni_rx_l3fe_axi_reo_init(ni);

	/*
	 * L3-CLS CPU-trap rows; run after the L2FE/MC setup so they override
	 * the broadcast -> L3_LAN (0x19) resolution
	 */
	cortina_ni_rx_cls_init(ni);

	/*
	 * full FE path on every LAN port: bypass off, pass OAM, drop unknown
	 * opcodes (stock policy)
	 */
	{
		unsigned int p;

		for (p = 0; p < CA_NI_LAN_PORT_COUNT; p++)
			ni_rmw(ni, CA_NI_PORT_RX_CNTRL_CFG(p),
			       CA_NI_RX_CNTRL_BYP_EN | CA_NI_RX_CNTRL_BYP_DPID |
			       CA_NI_RX_CNTRL_BYP_COS | CA_NI_RX_CNTRL_UKOP_DROP_DIS,
			       CA_NI_RX_CNTRL_OAM_DROP_DIS);
	}

	/* L3FE demux golden routing map (FE output -> L3QM CPU-EPP) */
	writel(CA_NI_NIRX_L3FE_DEMUX0_VAL, ni_base(ni) + CA_NI_NIRX_L3FE_DEMUX0);
	writel(CA_NI_NIRX_L3FE_DEMUX1_VAL, ni_base(ni) + CA_NI_NIRX_L3FE_DEMUX1);
	writel(CA_NI_NIRX_L3FE_DEMUX2_VAL, ni_base(ni) + CA_NI_NIRX_L3FE_DEMUX_NORM0);
	writel(CA_NI_NIRX_L3FE_DEMUX3_VAL, ni_base(ni) + CA_NI_NIRX_L3FE_DEMUX_NORM1);
	writel(CA_NI_NIRX_L3FE_DEMUX4_VAL, ni_base(ni) + CA_NI_NIRX_L3FE_DEMUX_NORM2);
	writel(CA_NI_NIRX_L3FE_DEMUX5_VAL, ni_base(ni) + CA_NI_NIRX_L3FE_DEMUX_NORM3);

	/*
	 * Deep-queue (deep_q=1) per-ldpid demux table, stock values; it is not
	 * a mirror of the normal table above.
	 */
	writel(CA_NI_NIRX_L3FE_DPQ_DEMUX_48_63_VAL, ni_base(ni) + CA_NI_NIRX_L3FE_DPQ0);
	{
		u32 dpq1 = CA_NI_NIRX_L3FE_DPQ_DEMUX_32_47_VAL;

		writel(dpq1, ni_base(ni) + CA_NI_NIRX_L3FE_DPQ1);
	}
	writel(CA_NI_NIRX_L3FE_DPQ_DEMUX_16_31_VAL, ni_base(ni) + CA_NI_NIRX_L3FE_DPQ2);
	writel(CA_NI_NIRX_L3FE_DPQ_DEMUX_0_15_VAL, ni_base(ni) + CA_NI_NIRX_L3FE_DPQ3);

	/*
	 * Resulting demux/DPQ registers:
	 *   0xa1d4=0  0xa1d8=2  0xa1dc=0x22AA0000  0xa1e0=0xFFFFFFFF
	 *   0xa1ec=0x22AA0000  0xa1f0=0xAAAA0000
	 *   0xa1e4=0 (ldpid 0x32 -> L3QM)  0xa1e8=0
	 */
	dev_dbg(ni->dev,
		"l3fe-demux (0xa1d4-f0): cfg0(0xa1e0)=0x%08x dpq3(0xa1e4)=0x%08x dpq0(0xa1f0)=0x%08x (want golden 0xFFFFFFFF/0x00000000/0xAAAA0000)\n",
		readl(ni_base(ni) + CA_NI_NIRX_L3FE_DEMUX_NORM3),
		readl(ni_base(ni) + CA_NI_NIRX_L3FE_DPQ0),
		readl(ni_base(ni) + CA_NI_NIRX_L3FE_DPQ3));

	/* internal port MAC blocks 1..5 */
	cortina_ni_rx_enable_internal_ports(ni);

	/* 0xa1c0 (INT_PORT_ID_CFG2) is written by cortina_ni_rx_stock_routing() */

	/* second egress-enable layer: all NI egress ports (stock 0x610c) */
	writel(CA_NI_QM_L3TM_NI_PORT_ENA_ALL,
	       ni_base(ni) + CA_NI_QM_L3TM_NI_PORT_ENA);

	/*
	 * Lookup misses (broadcast, unknown unicast, unknown multicast): trap
	 * to the CPU on the LAN ports, so the bridge sees and floods them.
	 *
	 * The PON source port forwards them to L3_LAN instead: under the HW-L3
	 * downstream route (cortina_gpon.hw_l3_ds=1) the DS data GEM enters the
	 * forwarding engines, and a downstream broadcast (ARP for our WAN
	 * address, a broadcast DHCP OFFER/ACK) is a lookup miss on the PON
	 * lspid.  A failure there is logged but does not abort the bring-up.
	 */
	{
		unsigned int p;

		for (p = 0; p < CA_NI_LAN_PORT_COUNT; p++)
			for (type = 0; type < CA_NI_PLE_TYPE_COUNT; type++) {
				ret = cortina_ni_rx_ple_dft_fwd(ni, p, type,
					CA_NI_PLE_DFT_FWD_TO(CA_NI_RX_CPU_LDPID));
				if (ret) {
					dev_err(ni->dev,
						"PLE dft-fwd (lspid %u type %d) timed out\n",
						p, type);
					return ret;
				}
			}

		for (type = 0; type < CA_NI_PLE_TYPE_COUNT; type++) {
			ret = cortina_ni_rx_ple_dft_fwd(ni, CA_NI_LSPID_PON, type,
					CA_NI_PLE_DFT_FWD_TO(CA_NI_RX_L3LAN_LDPID));
			if (ret)
				dev_err(ni->dev,
					"PLE dft-fwd (PON lspid %u type %d) failed (%d): downstream broadcast (an ARP request for our WAN address, a broadcast DHCP OFFER) will be DROPPED instead of trapped to the CPU\n",
					CA_NI_LSPID_PON, type, ret);
		}
		ret = 0;
	}

	/*
	 * Re-arm the L2TE->L3FE ready handshake (NIRX_MISC rdy bits, 0xa1f8)
	 * after the whole L3FE pipeline is configured, as the vendor's
	 * aal_ni_init_ni runs after aal_l3fe_init.  RMW, as stock.
	 */
	ni_rmw(ni, CA_NI_NI_NIRX_MISC_CFG,
	       CA_NI_NI_NIRX_MISC_CLR, CA_NI_NI_NIRX_MISC_RDY_EN);
	dev_dbg(ni->dev,
		"l3fe-handoff re-arm (post-L3FE-config): nirx_misc(0xa1f8)=0x%08x intern_pid(0xa1bc)=0x%08x [want 0x00A87F00]\n",
		readl(ni_base(ni) + CA_NI_NI_NIRX_MISC_CFG),
		readl(ni_base(ni) + CA_NI_NI_INTERNAL_PORT_ID_CFG));

	return 0;
}

/* ------------------------------------------------------------------ */
/* L3QM empty-buffer pool + delivery-chain init (stock                 */
/* aal_l3qm_init_empty_buffer / init_voq / enable_rx, CPU port 0)      */
/* ------------------------------------------------------------------ */

/*
 * Commit the per-EQ CFG0-4 into the empty-buffer manager (stock
 * aal_l3qm_load_eq_config): unlock HDM write protection (0x67fc=0x05102013),
 * load all EQs (0x6408=0x0000ffff, then 0), relock.  Required after
 * programming an EQ: until then the bid range does not latch and pushed PAs
 * pile up in the CPU push stage.  EQ_CFG_LOAD clears itself.
 */
static void cortina_ni_rx_eq_commit(struct cortina_ni *ni)
{
	writel(CA_NI_QM_HDM_UNLOCK, ni_base(ni) + CA_NI_QM_HDM_WRITE_PROT);
	writel(CA_NI_QM_EQ_CFG_LOAD_ALL, ni_base(ni) + CA_NI_QM_EQ_CFG_LOAD);
	writel(0, ni_base(ni) + CA_NI_QM_EQ_CFG_LOAD);
	writel(0, ni_base(ni) + CA_NI_QM_HDM_WRITE_PROT);
}

/*
 * Program one QM AXI-attribute table entry via the indirect protocol
 * (aal_l3qm_set_epp_axi_attrib): write DATA0, trigger ACCESS = GO|write|ADDR,
 * then poll GO (bit31) clear, bounded.  A timeout is logged and skipped.
 */
static void cortina_ni_rx_set_axi_attrib(struct cortina_ni *ni, u32 entry,
					 u32 data0)
{
	u32 acc;
	int i;

	writel(data0, ni_base(ni) + CA_NI_QM_AXI_ATTR_DATA0);
	writel(CA_NI_QM_AXI_ATTR_GO | CA_NI_QM_AXI_ATTR_RBW |
	       FIELD_PREP(CA_NI_QM_AXI_ATTR_ADDR, entry),
	       ni_base(ni) + CA_NI_QM_AXI_ATTR_ACCESS);
	for (i = 0; i < CA_NI_QM_AXI_ATTR_POLL_MAX; i++) {
		acc = readl(ni_base(ni) + CA_NI_QM_AXI_ATTR_ACCESS);
		if (!(acc & CA_NI_QM_AXI_ATTR_GO))
			return;
		udelay(1);
	}
	dev_warn(ni->dev, "AXI-attr entry %u commit timeout (acc=0x%08x)\n",
		 entry, acc);
}

/* read one AXI-attr entry back (ACCESS = GO|entry with RBW=0, poll, DATA0) */
static u32 cortina_ni_rx_get_axi_attrib(struct cortina_ni *ni, u32 entry)
{
	u32 acc = 0;
	int i;

	writel(CA_NI_QM_AXI_ATTR_GO | FIELD_PREP(CA_NI_QM_AXI_ATTR_ADDR, entry),
	       ni_base(ni) + CA_NI_QM_AXI_ATTR_ACCESS);
	for (i = 0; i < CA_NI_QM_AXI_ATTR_POLL_MAX; i++) {
		acc = readl(ni_base(ni) + CA_NI_QM_AXI_ATTR_ACCESS);
		if (!(acc & CA_NI_QM_AXI_ATTR_GO))
			break;
		udelay(1);
	}
	return readl(ni_base(ni) + CA_NI_QM_AXI_ATTR_DATA0);
}

/*
 * Initialise the QM AXI-attribute table before any frame can be admitted.  The
 * entries are invalid at power-on and the QM's buffer DMA stalls on an
 * unprogrammed entry.  The DDR pools and the CPU-EPP entries get the
 * non-coherent DDR bufferable attribute.  Unused EQs keep their reset default.
 */
static void cortina_ni_rx_axi_attrib_init(struct cortina_ni *ni)
{
	static const u32 ddr_eqs[] = {
		CA_NI_RX_EQ_ID, CA_NI_RX_EQ_ID2, CA_NI_RX_EQ8_ID,
	};
	u32 i;

	for (i = 0; i < ARRAY_SIZE(ddr_eqs); i++)
		cortina_ni_rx_set_axi_attrib(ni, CA_NI_QM_AXI_ATTR_EQ_BASE + ddr_eqs[i],
					     CA_NI_QM_AXI_ATTR_DDR_POOL);

	/*
	 * CPU-EPP descriptor ring attribute.  Stock uses the coherent CPU_EPP
	 * attribute 0x12008060; here a coherent write is rejected (0x611c bit30
	 * axim_cpuepp_resp_error) and the descriptor never reaches DRAM while
	 * the write pointer still advances, so by default the ring gets the
	 * same non-coherent DDR attribute as the frame-buffer pools.
	 */
	for (i = 0; i < CA_NI_QM_CPU_PORT_COUNT; i++)
		cortina_ni_rx_set_axi_attrib(ni, CA_NI_QM_AXI_ATTR_CPU_BASE + i,
					     CA_NI_QM_AXI_ATTR_DDR_POOL);

	/* read back the CPU-EPP entries, the two pool EQs and the EPP control block */
	for (i = 0; i < CA_NI_QM_CPU_PORT_COUNT; i++)
		dev_dbg(ni->dev, "axi-attr-readback: cpu_epp[%u] idx%u = 0x%08x\n",
			i, CA_NI_QM_AXI_ATTR_CPU_BASE + i,
			cortina_ni_rx_get_axi_attrib(ni, CA_NI_QM_AXI_ATTR_CPU_BASE + i));
	dev_dbg(ni->dev, "axi-attr-readback: eq13 idx%u=0x%08x  eq14 idx%u=0x%08x\n",
		CA_NI_QM_AXI_ATTR_EQ_BASE + CA_NI_RX_EQ_ID,
		cortina_ni_rx_get_axi_attrib(ni, CA_NI_QM_AXI_ATTR_EQ_BASE + CA_NI_RX_EQ_ID),
		CA_NI_QM_AXI_ATTR_EQ_BASE + CA_NI_RX_EQ_ID2,
		cortina_ni_rx_get_axi_attrib(ni, CA_NI_QM_AXI_ATTR_EQ_BASE + CA_NI_RX_EQ_ID2));
	/*
	 * BURST_BUF_DEBUG_SEG_ID (0x68e8), BURST_BUF_SEG_ID_MONITOR (0x68ec),
	 * DEBUG_CFG (0x68f0), EPP (0x68f4)
	 */
	dev_dbg(ni->dev, "epp-ctrl: 0x68e8=0x%08x 0x68ec=0x%08x 0x68f0=0x%08x 0x68f4=0x%08x\n",
		readl(ni_base(ni) + 0x68e8), readl(ni_base(ni) + 0x68ec),
		readl(ni_base(ni) + 0x68f0), readl(ni_base(ni) + 0x68f4));
}

/*
 * CPU RX delivery chain (stock aal_l3qm_init_empty_buffer_CPU):
 *   ingress -> FE DLF -> CPU dest 9 -> EQ profile -> CPU pools -> CPU-EPP
 * The EQ pool enable is CFG0 bit31; a pool with it clear or with
 * CFG1.total_buf = 0 makes the QM stall without admitting or counting a drop.
 */
/*
 * Program one empty-buffer pool: CFG0 = pool-base PA | enable, CFG1 =
 * {bid_start, total_buf}, CFG2 = (cpu_eq, refill_en, buffer_size), CFG3 =
 * CA_NI_QM_CFG3_CPU_POOL_VAL, CFG4 = 0.  The caller commits (EQ_CFG_LOAD) once after every
 * pool is programmed.
 */
static void cortina_ni_rx_eq_cfg_pool(struct cortina_ni *ni, unsigned int eqid,
				      u32 cfg0, u32 bid_start, u32 total_buf,
				      u32 cfg2)
{
	writel(cfg0, ni_base(ni) + CA_NI_QM_CFG0_EQ(eqid));
	writel(FIELD_PREP(CA_NI_QM_CFG1_BID_START, bid_start) |
	       FIELD_PREP(CA_NI_QM_CFG1_TOTAL_BUF_NUM, total_buf),
	       ni_base(ni) + CA_NI_QM_CFG1_EQ(eqid));
	writel(cfg2, ni_base(ni) + CA_NI_QM_CFG2_EQ(eqid));
	/*
	 * CFG3 = the pool DMA's AXI attributes (qos[3:0], cache[7:4],
	 * snoop[11:8], bar[13:12], domain[15:14], user[21:16], acd_cmd[24],
	 * cache_line_split[25]); CFG4 = axi_top_bit, bits [39:32] of the pool
	 * address.
	 */
	writel(CA_NI_QM_CFG3_CPU_POOL_VAL, ni_base(ni) + CA_NI_QM_CFG3_EQ(eqid));
	writel(0, ni_base(ni) + CA_NI_QM_CFG4_EQ(eqid));
}

/*
 * Hand the software-owned CPU pools their buffers.
 *
 * Must run after the EQ commit and after the RMU0 RX master is enabled: the
 * CPU push stage is only a few entries deep and does not drain until then.
 *
 * The buffers come from the page pool, as the vendor driver pushes freshly
 * allocated skb heads.
 */
static int cortina_ni_rx_push_seed(struct cortina_ni *ni)
{
	/*
	 * nbufs must equal the total_buf each pool was configured with (the
	 * constants cortina_ni_rx_eq_init() passes to CFG1)
	 */
	static const struct {
		u32 eqid, nbufs;
	} pools[] = {
		{ CA_NI_RX_EQ_ID,  CA_NI_RX_EQ_TOTAL_BUF },
		{ CA_NI_RX_EQ_ID2, CA_NI_RX_EQ2_TOTAL_BUF },
	};
	unsigned int p, i, want = 0, done = 0;
	int ret = 0;

	for (p = 0; p < ARRAY_SIZE(pools); p++)
		want += pools[p].nbufs;

	for (p = 0; p < ARRAY_SIZE(pools) && !ret; p++) {
		for (i = 0; i < pools[p].nbufs; i++) {
			u32 pa = cortina_ni_rx_alloc_buf(ni->rx->pp);

			if (!pa) {
				ret = -ENOMEM;
				break;
			}
			ret = cortina_ni_rx_push_buf(ni, pools[p].eqid, pa,
						     CA_NI_RX_PUSH_TIMEOUT_US);
			if (ret)
				break;
			done++;
		}
	}

	if (done < CA_NI_RX_PUSH_SEED_MIN) {
		dev_err(ni->dev,
			"RX: CPU pool seed FAILED - staged only %u of %u buffers (push stage never freed a slot).  RX would starve.\n",
			done, want);
		return -EBUSY;
	}
	if (ret)
		dev_warn(ni->dev,
			 "RX: CPU pool seed SHORT - staged %u of %u buffers; the pools will report an inactive-bid shortfall\n",
			 done, want);
	else
		dev_dbg(ni->dev,
			 "RX: CPU pool seeded %u software-owned buffers (EQ%u=%u + EQ%u=%u, = CFG1.total_buf; expect inactive=0 ready=0)\n",
			 done, CA_NI_RX_EQ_ID, CA_NI_RX_EQ_TOTAL_BUF,
			 CA_NI_RX_EQ_ID2, CA_NI_RX_EQ2_TOTAL_BUF);
	return 0;
}

static struct page_pool *cortina_ni_rx_pp_create(struct cortina_ni *ni,
						 struct napi_struct *napi)
{
	struct page_pool_params pp = {
		.flags		= PP_FLAG_DMA_MAP | PP_FLAG_DMA_SYNC_DEV,
		.pool_size	= CA_NI_RX_EQ_TOTAL_BUF + CA_NI_RX_EQ2_TOTAL_BUF,
		.nid		= NUMA_NO_NODE,
		.dev		= ni->dev,
		.napi		= napi,
		.dma_dir	= DMA_FROM_DEVICE,
		.max_len	= PAGE_SIZE,
	};

	return page_pool_create(&pp);
}

/*
 * One page pool per NAPI: page-pool allocation is single-consumer and the
 * LAN and WAN NAPIs run on different CPUs.  The pools live as long as the
 * device, since buffers handed to the QM cannot be taken back.
 */
static int cortina_ni_rx_pp_init(struct cortina_ni *ni)
{
	struct cortina_ni_rx *rx = ni->rx;

	rx->pp = cortina_ni_rx_pp_create(ni, &rx->napi);
	if (IS_ERR(rx->pp))
		return PTR_ERR(rx->pp);
	rx->pp_wan = cortina_ni_rx_pp_create(ni, &rx->napi_wan);
	if (IS_ERR(rx->pp_wan)) {
		page_pool_destroy(rx->pp);
		return PTR_ERR(rx->pp_wan);
	}
	return 0;
}

/*
 * Log QM_PHY_PORT_STS with the handshake bits decoded (all should climb from
 * 0 toward the 0xa5ffffff default as the NI/TE/ES/AXI blocks come up).
 */
static void cortina_ni_rx_log_qm_sts(struct cortina_ni *ni, const char *stage)
{
	u32 s = readl(ni_base(ni) + CA_NI_QM_PHY_PORT_STS);

	dev_dbg(ni->dev,
		"QM-hs[%s]: phy_sts=0x%08x nirx_port_rdy=0x%02lx te_es_ni_ok=0x%02lx nirx_qm_rdy=%u axi_wr=%u axi_rd=%u\n",
		stage, s,
		(unsigned long)FIELD_GET(CA_NI_QM_STS_NIRX_PORT_RDY, s),
		(unsigned long)FIELD_GET(CA_NI_QM_STS_TE_ES_NI_OK, s),
		!!(s & CA_NI_QM_STS_NIRX_QM_RDY),
		!!(s & CA_NI_QM_STS_AXI_WR_RDY),
		!!(s & CA_NI_QM_STS_AXI_RD_RDY));
}

/*
 * NI-RX -> CPU routing as the stock firmware leaves it: autosync = 0xF and
 * the NI->QM handoff via INTERNAL_PORT_ID_CFG.  The CPU tag is not used on
 * the LAN ports.  The whole-word writes replace whatever tx_hw_init left at
 * 0xa1bc.
 */
static void cortina_ni_rx_stock_routing(struct cortina_ni *ni)
{
	cortina_ni_rx_log_qm_sts(ni, "before");

	writel(CA_NI_QM_AXIM2_STOCK_VAL, ni_base(ni) + CA_NI_QM_AXIM2_CONFIG);
	/*
	 * The per-ldpid L2FE-vs-L3FE ingress fork: L3QMRX_DEMUX_CFG1/0 at
	 * 0xa1c4/0xa1c8 (stock 0 / 0xFFFF7F7F).
	 */
	writel(CA_NI_NIRX_L3FE_DEMUX_CFG1_REAL_VAL,
	       ni_base(ni) + CA_NI_NIRX_L3FE_DEMUX_CFG1_REAL);
	writel(CA_NI_NIRX_L3FE_DEMUX_CFG0_REAL_VAL,
	       ni_base(ni) + CA_NI_NIRX_L3FE_DEMUX_CFG0_REAL);
	/* 0xa1c0 INT_PORT_ID_CFG2 = 0x0024009B */
	writel(CA_NI_NI_INT_PORT_ID_CFG2_VAL,
	       ni_base(ni) + CA_NI_NI_INT_PORT_ID_CFG2);
	/*
	 * INTERNAL_PORT_ID_CFG (0xa1bc): l3qmrx_demux_sel[7:0] = 0 presents
	 * all ports' NI-RX to the L3QM, wan_rxsel[23:22] = 2, with the
	 * mirror/redirect fields cleared (vendor value 0x00800000).
	 */
	writel(0x00800000u,
	       ni_base(ni) + CA_NI_NI_INTERNAL_PORT_ID_CFG);
	/*
	 * NIRX_MISC_CFG (0xa1f8): stock sets the l2te_ni_*_rdy_en bits [13:9]
	 * and clears bit20 by RMW (aal_ni_init_ni)
	 */
	ni_rmw(ni, CA_NI_NI_NIRX_MISC_CFG,
	       CA_NI_NI_NIRX_MISC_CLR, CA_NI_NI_NIRX_MISC_RDY_EN);
	writel(CA_NI_NI_AUTOSYNC_STOCK_VAL,
	       ni_base(ni) + CA_NI_HV_MAC_AUTOSYNC);

	/*
	 * NI_HV_GLB_STATIC_CFG (0xa01c): OR in bits[17:16] (stock 0x1A07002F),
	 * keeping port_to_cpu[3:0]
	 */
	writel(readl(ni_base(ni) + CA_NI_NI_GLB_STATIC_CFG) | CA_NI_NI_GLB_STATIC_L3QM_EN,
	       ni_base(ni) + CA_NI_NI_GLB_STATIC_CFG);

	/* intern_pid must read 0x00A87F00 (demux_sel=0, wan_rxsel=2) */
	dev_dbg(ni->dev,
		"stock-routing: intern_pid(a1bc)=0x%08x [want 0x00A87F00] portorder(a1c0)=0x%08x [want 0x0024009B] qmrx_demux0(a1c8)=0x%08x [want 0xFFFF7F7F] nirx_misc(a1f8)=0x%08x autosync(a010)=0x%08x glb_static(a01c)=0x%08x\n",
		readl(ni_base(ni) + CA_NI_NI_INTERNAL_PORT_ID_CFG),
		readl(ni_base(ni) + CA_NI_NI_INT_PORT_ID_CFG2),
		readl(ni_base(ni) + CA_NI_NIRX_L3FE_DEMUX_CFG0_REAL),
		readl(ni_base(ni) + CA_NI_NI_NIRX_MISC_CFG),
		readl(ni_base(ni) + CA_NI_HV_MAC_AUTOSYNC),
		readl(ni_base(ni) + CA_NI_NI_GLB_STATIC_CFG));

	cortina_ni_rx_log_qm_sts(ni, "stock-routing");
}

/*
 * Stock QM configuration that the minimal init leaves at 0: DWRR VOQ weights
 * (0x656c-0x6660 = 0x01010101, aal_l3qm_config_DWRR), the drain-delivery block
 * and per-queue CPU-EPP FIFO profiles (0x6664-0x67c8), and misc QM config.
 * Written before EQ_CFG_LOAD.
 *
 * Not included: the EQ8/EQ12 pool enables (stock has refill_en=1 from an FBM
 * pool this driver does not run, and enabling them hangs the CPU), 0x67cc
 * (per-queue CPU-EPP FIFO indirect commit, hangs the CPU when written cold),
 * the RMU status block 0x6900-0x69fc (hardware defaults / init-done status)
 * and 0x69c4-0x69e0 (stock's own ring addresses).
 */
static const struct { u16 off; u32 val; } cortina_ni_stock_qm_cfg[] = {
	/* DWRR VOQ weights */
	{ 0x656c, 0x01010101 }, { 0x6570, 0x01010101 }, { 0x6574, 0x01010101 },
	{ 0x6578, 0x01010101 }, { 0x657c, 0x01010101 }, { 0x6580, 0x01010101 },
	{ 0x6584, 0x01010101 }, { 0x6588, 0x01010101 }, { 0x658c, 0x01010101 },
	{ 0x6590, 0x01010101 }, { 0x6594, 0x01010101 }, { 0x6598, 0x01010101 },
	{ 0x659c, 0x01010101 }, { 0x65a0, 0x01010101 }, { 0x65a4, 0x01010101 },
	{ 0x65a8, 0x01010101 }, { 0x65ac, 0x01010101 }, { 0x65b0, 0x01010101 },
	{ 0x65b4, 0x01010101 }, { 0x65b8, 0x01010101 }, { 0x65bc, 0x01010101 },
	{ 0x65c0, 0x01010101 }, { 0x65c4, 0x01010101 }, { 0x65c8, 0x01010101 },
	{ 0x65cc, 0x01010101 }, { 0x65d0, 0x01010101 }, { 0x65d4, 0x01010101 },
	{ 0x65d8, 0x01010101 }, { 0x65dc, 0x01010101 }, { 0x65e0, 0x01010101 },
	{ 0x65e4, 0x01010101 }, { 0x65e8, 0x01010101 }, { 0x65ec, 0x01010101 },
	{ 0x65f0, 0x01010101 }, { 0x65f4, 0x01010101 }, { 0x65f8, 0x01010101 },
	{ 0x65fc, 0x01010101 }, { 0x6600, 0x01010101 }, { 0x6604, 0x01010101 },
	{ 0x6608, 0x01010101 }, { 0x660c, 0x01010101 }, { 0x6610, 0x01010101 },
	{ 0x6614, 0x01010101 }, { 0x6618, 0x01010101 }, { 0x661c, 0x01010101 },
	{ 0x6620, 0x01010101 }, { 0x6624, 0x01010101 }, { 0x6628, 0x01010101 },
	{ 0x662c, 0x01010101 }, { 0x6630, 0x01010101 }, { 0x6634, 0x01010101 },
	{ 0x6638, 0x01010101 }, { 0x663c, 0x01010101 }, { 0x6640, 0x01010101 },
	{ 0x6644, 0x01010101 }, { 0x6648, 0x01010101 }, { 0x664c, 0x01010101 },
	{ 0x6650, 0x01010101 }, { 0x6654, 0x01010101 }, { 0x6658, 0x01010101 },
	{ 0x665c, 0x01010101 }, { 0x6660, 0x01010101 },
	/*
	 * Drain-delivery block: with it at 0 the QM never pushes a drained
	 * deep-queue frame into the CPU-EPP ring.  0x66a4-0x66c0 =
	 * CA_NI_QM_CPU_EPP_FIFO_PROF(n), the per-FIFO delivery descriptor.
	 * 0x6680-0x66a0 are 0 on stock too.
	 */
	{ 0x6664, 0x11111111 }, { 0x6668, 0x11111111 }, { 0x666c, 0x11111111 },
	{ 0x6670, 0x11111111 }, { 0x6674, 0x11111111 }, { 0x6678, 0x11111111 },
	{ 0x667c, 0x000C0114 },
	{ 0x66a4, 0xE0008001 }, { 0x66a8, 0xE0008001 }, { 0x66ac, 0xE0008001 },
	{ 0x66b0, 0xE0008001 }, { 0x66b4, 0xE00040F1 }, { 0x66b8, 0x20006801 },
	{ 0x66bc, 0x2000C801 }, { 0x66c0, 0xE0080001 }, { 0x66cc, 0x00000004 },
	{ 0x66d0, 0x00000004 }, { 0x66d4, 0x00000004 }, { 0x66d8, 0x00000004 },
	{ 0x66dc, 0x00000004 }, { 0x66e0, 0x00000004 }, { 0x66e4, 0x00000004 },
	{ 0x66e8, 0x00000004 }, { 0x66ec, 0x00000004 }, { 0x66f0, 0x00000004 },
	{ 0x66f4, 0x00000004 }, { 0x66f8, 0x00000004 }, { 0x66fc, 0x00000004 },
	{ 0x6700, 0x00000004 }, { 0x6704, 0x00000004 }, { 0x6708, 0x00000004 },
	{ 0x670c, 0x00000006 }, { 0x6710, 0x00000005 }, { 0x6714, 0x00000005 },
	{ 0x6718, 0x00000005 }, { 0x671c, 0x00000006 }, { 0x6720, 0x00000005 },
	{ 0x6724, 0x00000005 }, { 0x6728, 0x00000005 }, { 0x672c, 0x00000007 },
	{ 0x6730, 0x00000007 }, { 0x6734, 0x00000007 }, { 0x6738, 0x00000007 },
	{ 0x673c, 0x00000007 }, { 0x6740, 0x00000007 }, { 0x6744, 0x00000007 },
	{ 0x6748, 0x00000007 }, { 0x674c, 0x00000007 }, { 0x6750, 0x00000007 },
	{ 0x6754, 0x00000007 }, { 0x6758, 0x00000007 }, { 0x675c, 0x00000007 },
	{ 0x6760, 0x00000007 }, { 0x6764, 0x00000007 }, { 0x6768, 0x00000007 },
	{ 0x676c, 0x00000007 }, { 0x6770, 0x00000007 }, { 0x6774, 0x00000007 },
	{ 0x6778, 0x00000007 }, { 0x677c, 0x00000007 }, { 0x6780, 0x00000007 },
	{ 0x6784, 0x00000007 }, { 0x6788, 0x00000007 }, { 0x678c, 0x00000007 },
	{ 0x6790, 0x00000007 }, { 0x6794, 0x00000007 }, { 0x6798, 0x00000007 },
	{ 0x679c, 0x00000007 }, { 0x67a0, 0x00000007 }, { 0x67a4, 0x00000007 },
	{ 0x67a8, 0x00000007 }, { 0x67ac, 0x00000007 }, { 0x67b0, 0x00000007 },
	{ 0x67b4, 0x00000007 }, { 0x67b8, 0x00000007 }, { 0x67bc, 0x00000007 },
	{ 0x67c0, 0x00000007 }, { 0x67c4, 0x00000007 }, { 0x67c8, 0x00000007 },
	/* int-en / maps / misc QM config */
	{ 0x69b4, 0x80080000 }, { 0x69bc, 0x06061616 },
	{ 0x69f8, 0x000000FF }, { 0x6a00, 0x0000FF00 },
	{ 0x6110, 0x0000FFFF }, { 0x6118, 0x00000100 }, { 0x611c, 0x10000000 },
	{ 0x6120, 0xE6D54F85 },
};

/*
 * EQM buffer-availability and RMU0 admission counters: pa_req
 * (0x63bc/0x63c0), RMU0 no-buffer drops (0x6940), RMU0 RX (0x6900) and the
 * EPP write pointer (0x7000).
 */
static void cortina_ni_rx_eqm_readback(struct cortina_ni *ni, const char *label)
{
	dev_dbg(ni->dev,
		"eqm-readback(%s): pa_req eq13(0x63bc)=0x%08x eq14(0x63c0)=0x%08x | eq_prof5(0x613c)=0x%08x | fifo_prof4(0x66b4)=0x%08x (want 0xE00040F1) | int_en0(0x6110)=0x%08x en1(0x6114)=0x%08x refill_en(0x611c)=0x%08x REAL_int_src(0x6120)=0x%08x [b22=eqm_cfg_err b21=buf_size b20=cpuepp_fifo] | no_buf(0x6940)=%u rmu_rx(0x6900)=%u tx_cntr(0x690c)=%u epp_wptr(0x7000)=0x%06x\n",
		label,
		readl(ni_base(ni) + 0x63bc),
		readl(ni_base(ni) + 0x63c0),
		readl(ni_base(ni) + CA_NI_QM_EQ_PROFILE(CA_NI_RX_EQ_PROFILE_SEL)),
		readl(ni_base(ni) + CA_NI_QM_CPU_EPP_FIFO_PROF(CA_NI_RX_PROFILE_ID)),
		readl(ni_base(ni) + CA_NI_QM_EPP64_INT_EN0),
		readl(ni_base(ni) + CA_NI_QM_EPP64_INT_EN1),
		readl(ni_base(ni) + 0x611c),
		readl(ni_base(ni) + 0x6120),
		readl(ni_base(ni) + CA_NI_QM_RMU_NO_BUF_DROP),
		readl(ni_base(ni) + CA_NI_QM_RX_CNTR),
		readl(ni_base(ni) + CA_NI_QM_TX_CNTR),
		cortina_ni_rx_wptr(ni));
}

/*
 * The subset of the stock QM table whose registers exist on this SoC, at
 * their addresses here (several are relocated relative to the generic map).
 * Taurus has only four port-weight registers, so PORT39_32/PORT47_40 do not
 * exist; the unnamed DWRR-weight and per-queue CPU-EPP-FIFO arrays and the
 * status/counter registers are left out.
 */
static const struct { u16 off; u32 val; } cortina_ni_stock_qm_cfg_validated[] = {
	{ 0x6544, 0x11111111 },	/* QM_QM_PORT7_0_WEIGHT_RATIO_CFG   [reloc 0x6664] */
	{ 0x6548, 0x11111111 },	/* QM_QM_PORT15_8_WEIGHT_RATIO_CFG  [reloc 0x6668] */
	{ 0x654c, 0x11111111 },	/* QM_QM_PORT23_16_WEIGHT_RATIO_CFG [reloc 0x666c] */
	{ 0x6550, 0x11111111 },	/* QM_QM_PORT31_24_WEIGHT_RATIO_CFG [reloc 0x6670] */
	{ 0x6554, 0x000C0114 },	/* QM_QM_DRR_WEIGHT_BASE            [reloc 0x667c] */
	{ 0x65a4, 0x00000004 },	/* QM_QM_CPU_EPP_FIFO0_0_CFG        [reloc 0x66cc] */
	/* CPU-EPP interrupt enable for all CPU ports (one byte per port) */
	{ 0x6110, 0xFFFFFFFF },	/* QM_QM_EPP64_P3_0_INTERRUPT_EN */
	{ 0x6118, 0x00000100 },	/* QM_QM_EQ_REFILL_INTERRUPT_EN */
	{ 0x611c, 0x10000000 },	/* QM_QM_INT_SRC: W1C clear of fbm_returns_no_buffer */
	{ 0x6120, 0xE6D54F85 },	/* QM_QM_INT_SRCE */
};

static void __maybe_unused cortina_ni_rx_match_stock_qm(struct cortina_ni *ni)
{
	unsigned int k;

	for (k = 0; k < ARRAY_SIZE(cortina_ni_stock_qm_cfg_validated); k++)
		writel(cortina_ni_stock_qm_cfg_validated[k].val,
		       ni_base(ni) + cortina_ni_stock_qm_cfg_validated[k].off);
	dev_dbg(ni->dev, "match-stock-qm: validated table, %zu of %zu entries written\n",
		ARRAY_SIZE(cortina_ni_stock_qm_cfg_validated),
		ARRAY_SIZE(cortina_ni_stock_qm_cfg_validated));
}

static int cortina_ni_rx_eq_init(struct cortina_ni *ni)
{
	u32 cfg0_p0, cfg0_p1, cfg2_p0, cfg2_p1;
	u32 sts;
	int i, ret;

	/* (0) NI-RX -> QM routing */
	cortina_ni_rx_stock_routing(ni);

	/*
	 * (1) wait for the QM block's own init to finish (stock
	 * aal_l3qm_check_init_done); bounded and non-fatal: the bit does not
	 * reliably read 1.
	 */
	ret = readl_poll_timeout(ni_base(ni) + CA_NI_QM_PHY_PORT_STS, sts,
				 sts & CA_NI_QM_INIT_DONE, 10,
				 CA_NI_QM_INIT_DONE_TIMEOUT_US);
	if (ret)
		dev_warn(ni->dev, "RX: QM init-done not seen (sts=0x%x) - is the TQM reset firing?\n",
			 sts);
	else
		dev_dbg(ni->dev, "RX: QM init-done OK (sts=0x%x)\n", sts);

	/* (2) master L3QM RX off while (re)programming (stock order) */
	ni_rmw(ni, CA_NI_QM_RMU0_CTRL, CA_NI_QM_RMU0_RX_EN, 0);

	/*
	 * report every EQ already holding a bid range (an overlap with ours
	 * would corrupt the pool accounting)
	 */
	for (i = 0; i < CA_NI_QM_EQ_COUNT; i++) {
		u32 c1 = readl(ni_base(ni) + CA_NI_QM_CFG1_EQ(i));

		if (FIELD_GET(CA_NI_QM_CFG1_TOTAL_BUF_NUM, c1))
			dev_dbg(ni->dev,
				"RX: EQ%d pre-set: bid_start=%lu num=%lu\n", i,
				FIELD_GET(CA_NI_QM_CFG1_BID_START, c1),
				FIELD_GET(CA_NI_QM_CFG1_TOTAL_BUF_NUM, c1));
	}

	/*
	 * (3) CPU empty-buffer pools EQ5 (pool0) / EQ6 (pool1), software-owned:
	 * cpu_eq=1 and CFG0 = eq_en alone, as the vendor's
	 * aal_l3qm_init_empty_buffer_CPU; a cpu_eq=1 pool with a base is a
	 * configuration the EQM rejects (eqm_cfg_error).
	 */
	cfg0_p0 = CA_NI_QM_CFG0_EQ_EN;
	cfg0_p1 = CA_NI_QM_CFG0_EQ_EN;
	cfg2_p0 = CA_NI_QM_EQ13_CFG2 | CA_NI_QM_CFG2_CPU_EQ;
	cfg2_p1 = CA_NI_QM_EQ14_CFG2 | CA_NI_QM_CFG2_CPU_EQ;
	/*
	 * EQ0, the SRAM deep-queue pool, before the CPU pools so the same
	 * EQ_CFG_LOAD commit latches it.  Written as five literal words because
	 * cortina_ni_rx_eq_cfg_pool() derives CFG1/CFG3/CFG4 itself.
	 */
	writel(CA_NI_QM_EQ0_CFG0, ni_base(ni) + CA_NI_QM_CFG0_EQ(CA_NI_RX_EQ0_ID));
	writel(CA_NI_QM_EQ0_CFG1, ni_base(ni) + CA_NI_QM_CFG1_EQ(CA_NI_RX_EQ0_ID));
	writel(CA_NI_QM_EQ0_CFG2, ni_base(ni) + CA_NI_QM_CFG2_EQ(CA_NI_RX_EQ0_ID));
	writel(CA_NI_QM_EQ0_CFG3, ni_base(ni) + CA_NI_QM_CFG3_EQ(CA_NI_RX_EQ0_ID));
	writel(0, ni_base(ni) + CA_NI_QM_CFG4_EQ(CA_NI_RX_EQ0_ID));
	dev_dbg(ni->dev,
		"EQ0 SRAM deep-queue pool cfg0=0x%08x cfg1=0x%08x cfg2=0x%08x (243 x 1024B @0x80000080)\n",
		readl(ni_base(ni) + CA_NI_QM_CFG0_EQ(CA_NI_RX_EQ0_ID)),
		readl(ni_base(ni) + CA_NI_QM_CFG1_EQ(CA_NI_RX_EQ0_ID)),
		readl(ni_base(ni) + CA_NI_QM_CFG2_EQ(CA_NI_RX_EQ0_ID)));

	cortina_ni_rx_eq_cfg_pool(ni, CA_NI_RX_EQ_ID, cfg0_p0,
				  CA_NI_RX_EQ_BID_START, CA_NI_RX_EQ_TOTAL_BUF,
				  cfg2_p0);
	cortina_ni_rx_eq_cfg_pool(ni, CA_NI_RX_EQ_ID2, cfg0_p1,
				  CA_NI_RX_EQ2_BID_START, CA_NI_RX_EQ2_TOTAL_BUF,
				  cfg2_p1);
	dev_dbg(ni->dev,
		"RX: CPU pools EQ%u/EQ%u %s (cfg0=0x%08x/0x%08x cfg2=0x%08x/0x%08x)\n",
		CA_NI_RX_EQ_ID, CA_NI_RX_EQ_ID2,
		"software-owned (cpu_eq=1, push recycle)",
		cfg0_p0, cfg0_p1, cfg2_p0, cfg2_p1);

	/*
	 * (3a) The deep-queue pool EQ12, hardware-managed, needed when the CPU
	 * pools are software-owned (see CA_NI_RX_DQ_POOL_OFF).  cpu_eq=0 with a
	 * base inside the reserved region: the QM builds the free list itself at
	 * the EQ_CFG_LOAD commit, and cortina_ni_rx_frame() returns 0 for a PA
	 * in this region so it is never pushed back.
	 */
	cortina_ni_rx_eq_cfg_pool(ni, CA_NI_RX_EQ12_ID,
				  (CA_NI_RX_DQ_POOL_PHYS &
				   CA_NI_QM_CFG0_PHY_ADDR_START) |
				  CA_NI_QM_CFG0_EQ_EN,
				  CA_NI_RX_EQ12_BID_START,
				  CA_NI_RX_EQ12_TOTAL_BUF,
				  CA_NI_RX_DQ_CFG2);
	/*
	 * both halves of the profile point at EQ12: the deep queue
	 * cannot use a pushed buffer.  Profile 12 is outside the 0..7
	 * range filled below and written nowhere else.
	 */
	writel(CA_NI_RX_DQ_PROFILE_VAL,
	       ni_base(ni) +
	       CA_NI_QM_EQ_PROFILE(CA_NI_RX_DQ_PROFILE_SEL));
	dev_dbg(ni->dev,
		"RX: deep-queue pool EQ%u hardware-managed @0x%08x (%u bufs, bid 0x%x, cfg2=0x%08x) -> EQ_PROFILE(%u)=0x%02x\n",
		CA_NI_RX_EQ12_ID, (u32)CA_NI_RX_DQ_POOL_PHYS,
		CA_NI_RX_EQ12_TOTAL_BUF,
		(u32)CA_NI_RX_EQ12_BID_START,
		(u32)CA_NI_RX_DQ_CFG2, CA_NI_RX_DQ_PROFILE_SEL,
		(u32)CA_NI_RX_DQ_PROFILE_VAL);

	/*
	 * (3b) EQ_PROFILE(13) for the CPU-bound frame.
	 * DEST_PORT_EQ_CFG.prof_sel (GENMASK(3, 0)) indexes EQ_PROFILE directly.
	 */
	writel(CA_NI_RX_EQ_PROFILE_VAL,
	       ni_base(ni) + CA_NI_QM_EQ_PROFILE(CA_NI_RX_EQ_PROFILE));
	/*
	 * EQ profiles 0..7 (dest port 0 uses one of these; ports 1..7 are not
	 * otherwise programmed): profile 0 = {EQ0, none}, 1 = 0x21, 5 = 0xF5,
	 * 6 = 0xF6, others 0.  Profile 4 is written further down.
	 */
	{
		static const u8 profile_val[8] = {
			0xf0, 0x21, 0x00, 0x00, 0x00, 0xf5, 0xf6, 0x00,
		};

		for (i = 0; i < 8; i++) {
			if (i == 4)
				continue;	/* written below */
			writel(profile_val[i],
			       ni_base(ni) + CA_NI_IDX(CA_NI_QM_EQ_PROFILE, i,
						       CA_NI_QM_EQ_PROFILE_COUNT));
		}
	}
	/*
	 * EQ_PROFILE_GLOBAL(0..2) at 0x6200/04/08 alias EQ1's CFG1/2/3.
	 */
	for (i = 0; i < CA_NI_QM_EQ_PROFILE_GLOBAL_COUNT; i++)
		writel(CA_NI_RX_CPU_PROFILE_VAL,
		       ni_base(ni) + CA_NI_IDX(CA_NI_QM_EQ_PROFILE_GLOBAL, i,
					CA_NI_QM_EQ_PROFILE_GLOBAL_COUNT));
	/*
	 * Dest port 0 -> EQ profile 4, and profile 4 -> {eqp0 = EQ4, eqp1 =
	 * EQ5}: one profile per CPU port.
	 * Must come after the 0..7 profile fill above.
	 */
	writel(FIELD_PREP(CA_NI_QM_EQ_PROF_EQP0, CA_NI_RX_EQ_ID) |
	       FIELD_PREP(CA_NI_QM_EQ_PROF_EQP1, CA_NI_RX_EQ_ID2),
	       ni_base(ni) + CA_NI_QM_EQ_PROFILE(4));
	writel((CA_NI_NI_DESTPORT0_STOCK_VAL & ~CA_NI_QM_DEST_PORT_PROF_SEL) |
	       FIELD_PREP(CA_NI_QM_DEST_PORT_PROF_SEL, 4),
	       ni_base(ni) + CA_NI_QM_DEST_PORT_EQ_CFG(0));
	/*
	 * Dest ports 8 and 9 (the deep-queue CPU egress ports, which carry the
	 * downstream punt) -> EQ profile 0 = EQ0, the SRAM deep-queue pool.
	 */
	writel(FIELD_PREP(CA_NI_QM_DEST_PORT_PROF_SEL, CA_NI_RX_EQ0_PROFILE),
	       ni_base(ni) + CA_NI_QM_DEST_PORT_EQ_CFG(8));
	writel(FIELD_PREP(CA_NI_QM_DEST_PORT_PROF_SEL, CA_NI_RX_EQ0_PROFILE),
	       ni_base(ni) + CA_NI_QM_DEST_PORT_EQ_CFG(9));
	dev_dbg(ni->dev,
		"destp8(0x6168)=0x%08x destp9(0x616c)=0x%08x prof0(0x6128)=0x%08x\n",
		readl(ni_base(ni) + CA_NI_QM_DEST_PORT_EQ_CFG(8)),
		readl(ni_base(ni) + CA_NI_QM_DEST_PORT_EQ_CFG(9)),
		readl(ni_base(ni) + CA_NI_QM_EQ_PROFILE(0)));
	dev_dbg(ni->dev,
		"destp0(0x6148)=0x%08x prof4(0x6138)=0x%08x\n",
		readl(ni_base(ni) + CA_NI_QM_DEST_PORT_EQ_CFG(0)),
		readl(ni_base(ni) + CA_NI_QM_EQ_PROFILE(4)));
	/*
	 * Deep-queue dest ports 8..15 (incl. the CPU slot 15) use the
	 * hardware-managed deep-queue profile: the deep-queue enqueue cannot
	 * consume a pushed buffer from the software-owned CPU pools.
	 */
	for (i = CA_NI_RX_DEEPQ_DEST_PORT_LO; i <= CA_NI_RX_DEEPQ_DEST_PORT_HI; i++)
		writel(FIELD_PREP(CA_NI_QM_DEST_PORT_PROF_SEL,
				  CA_NI_RX_DQ_PROFILE_SEL),
		       ni_base(ni) + CA_NI_IDX(CA_NI_QM_DEST_PORT_EQ_CFG, i,
					       CA_NI_QM_DEST_PORT_ENTRIES));
	/*
	 * Dest ports 16..0x2f, configured as stock does.  They carry the
	 * downstream data punt: the per-GEM PDC map stamps data GEMs with ldpid
	 * 0x18 (L3_WAN), which lands here, and a forwarding-engine consumer
	 * cannot draw from a cpu_eq=1 pool, so they use the hardware-managed
	 * deep-queue profile.
	 */
	for (i = CA_NI_RX_DEEPQ_DEST_PORT_HI + 1; i <= CA_NI_QM_DEST_PORT_LAST; i++)
		writel(FIELD_PREP(CA_NI_QM_DEST_PORT_PROF_SEL,
				  CA_NI_RX_DQ_PROFILE_SEL),
		       ni_base(ni) + CA_NI_IDX(CA_NI_QM_DEST_PORT_EQ_CFG, i,
					       CA_NI_QM_DEST_PORT_ENTRIES));

	/*
	 * Point EQ_PROFILE(3) at the deep-queue pool and route the
	 * PON/deep-queue dest ports 8..0x2f through it, overriding the loops
	 * above, so an upstream frame selects a pool the RMU can allocate from.
	 */
	unsigned int p;

	writel(FIELD_PREP(CA_NI_QM_EQ_PROF_EQP0, CA_NI_RX_US_EQ_POOL) |
	       FIELD_PREP(CA_NI_QM_EQ_PROF_EQP1, CA_NI_RX_US_EQ_POOL),
	       ni_base(ni) + CA_NI_QM_EQ_PROFILE(3));
	for (p = CA_NI_RX_DEEPQ_DEST_PORT_LO; p <= CA_NI_QM_DEST_PORT_LAST; p++)
		writel(FIELD_PREP(CA_NI_QM_DEST_PORT_PROF_SEL, 3),
		       ni_base(ni) + CA_NI_IDX(CA_NI_QM_DEST_PORT_EQ_CFG, p,
					       CA_NI_QM_DEST_PORT_ENTRIES));
	dev_dbg(ni->dev,
		"EQ_PROFILE(3)=0x%08x dp8=0x%08x dp17=0x%08x (prof_sel=3 -> EQ12, non-truncating)\n",
		readl(ni_base(ni) + CA_NI_QM_EQ_PROFILE(3)),
		readl(ni_base(ni) + CA_NI_IDX(CA_NI_QM_DEST_PORT_EQ_CFG, 8,
					      CA_NI_QM_DEST_PORT_ENTRIES)),
		readl(ni_base(ni) + CA_NI_IDX(CA_NI_QM_DEST_PORT_EQ_CFG, 17,
					      CA_NI_QM_DEST_PORT_ENTRIES)));

	/* 0x6ab0 = 0x300, stock value */
	writel(CA_NI_QM_ES_CTRL2_STOCK_VAL, ni_base(ni) + CA_NI_QM_ES_CTRL2);

	/*
	 * Dest-port pkt_buf table: head 4 (HEADER_A at +0x40) + tail 0x18, for
	 * the first and the rest buffers (0x18041804), on all eight ports as
	 * stock.  A port whose PKT_BUF_CFG is 0 has no head/tail reservation and
	 * the QM cannot carve a buffer for it.
	 */
	for (i = 0; i < CA_NI_QM_PKT_BUF_PORTS; i++)
		ni_rmw(ni, CA_NI_QM_DEST_PORT_PKT_BUF_CFG(i),
		       CA_NI_QM_PKT_BUF_HEAD_FIRST | CA_NI_QM_PKT_BUF_TAIL_FIRST |
		       CA_NI_QM_PKT_BUF_HEAD_REST | CA_NI_QM_PKT_BUF_TAIL_REST,
		       FIELD_PREP(CA_NI_QM_PKT_BUF_HEAD_FIRST, CA_NI_QM_PKT_BUF_HEAD_UNITS) |
		       FIELD_PREP(CA_NI_QM_PKT_BUF_TAIL_FIRST, CA_NI_QM_PKT_BUF_TAIL_UNITS) |
		       FIELD_PREP(CA_NI_QM_PKT_BUF_HEAD_REST, CA_NI_QM_PKT_BUF_HEAD_UNITS) |
		       FIELD_PREP(CA_NI_QM_PKT_BUF_TAIL_REST, CA_NI_QM_PKT_BUF_TAIL_UNITS));

	dev_dbg(ni->dev,
		"pkt_buf[0..7]: %08x %08x %08x %08x %08x %08x %08x %08x  [stock: all 18041804]\n",
		readl(ni_base(ni) + CA_NI_QM_DEST_PORT_PKT_BUF_CFG(0)),
		readl(ni_base(ni) + CA_NI_QM_DEST_PORT_PKT_BUF_CFG(1)),
		readl(ni_base(ni) + CA_NI_QM_DEST_PORT_PKT_BUF_CFG(2)),
		readl(ni_base(ni) + CA_NI_QM_DEST_PORT_PKT_BUF_CFG(3)),
		readl(ni_base(ni) + CA_NI_QM_DEST_PORT_PKT_BUF_CFG(4)),
		readl(ni_base(ni) + CA_NI_QM_DEST_PORT_PKT_BUF_CFG(5)),
		readl(ni_base(ni) + CA_NI_QM_DEST_PORT_PKT_BUF_CFG(6)),
		readl(ni_base(ni) + CA_NI_QM_DEST_PORT_PKT_BUF_CFG(7)));

	/* QM AXI-attribute table for our EQ pools and the CPU-EPP ports */
	cortina_ni_rx_axi_attrib_init(ni);
	/*
	 * The validated stock QM table (the raw generic table hangs this SoC).
	 */
	cortina_ni_rx_match_stock_qm(ni);

	/*
	 * (4) commit: latch the bid ranges into the empty-buffer manager (stock
	 * aal_l3qm_load_eq_config)
	 */
	cortina_ni_rx_eq_commit(ni);
	dev_dbg(ni->dev,
		"RX: committed EQ%d(%u)+EQ%d(%u), bid 0x%x/0x%x\n",
		CA_NI_RX_EQ_ID, CA_NI_RX_EQ_TOTAL_BUF,
		CA_NI_RX_EQ_ID2, CA_NI_RX_EQ2_TOTAL_BUF,
		CA_NI_RX_EQ_BID_START, CA_NI_RX_EQ2_BID_START);

	/*
	 * (5) QM VOQ enable (stock aal_l3qm_init_voq): OR 0xff into voq_en[7:0]
	 * of every QM SCH_CFG (0x6424.., step 4).  The RMU admits a frame into a
	 * QM VOQ, and admission into a disabled VOQ fails without a drop count.
	 */
	for (i = 0; i < CA_NI_QM_VOQ_EN_COUNT; i++)
		ni_rmw(ni, CA_NI_QM_VOQ_EN(i), 0, CA_NI_QM_VOQ_EN_ALL);
	dev_dbg(ni->dev, "qm-voq: 0x6424=0x%08x 0x6428=0x%08x (want voq_en[7:0]=0xff)\n",
		readl(ni_base(ni) + CA_NI_QM_VOQ_EN(0)),
		readl(ni_base(ni) + CA_NI_QM_VOQ_EN(1)));

	return 0;
}

/*
 * Enable the L3QM egress scheduler master (stock aal_l3qm_enable_tx), which
 * moves an enqueued VOQ descriptor into the CPU-EPP ring.  Sets tx_en, the
 * NI-egress byte and the stock counter-increment config and clears cpu_en:
 * the per-CPU-port drain is armed at open (cortina_ni_rx_es_cpu()), so RX stays
 * quiescent until ndo_open.
 */
static void cortina_ni_rx_es_enable(struct cortina_ni *ni)
{
	/*
	 * stock 0x8462FFFF: set tx_en/ni_en/inccfg and clear the stray bit25;
	 * cpu_en is armed by es_cpu at open
	 */
	ni_rmw(ni, CA_NI_QM_ES_CTRL,
	       CA_NI_QM_ES_CPU_EN | CA_NI_QM_ES_NI_EN |
	       CA_NI_QM_ES_INCCFG_PKT | CA_NI_QM_ES_INCCFG_ERR |
	       CA_NI_QM_ES_RSVD25,
	       CA_NI_QM_ES_TX_EN |
	       FIELD_PREP(CA_NI_QM_ES_NI_EN, 0xff) |
	       FIELD_PREP(CA_NI_QM_ES_INCCFG_PKT, CA_NI_QM_ES_INCCFG_PKT_VAL) |
	       FIELD_PREP(CA_NI_QM_ES_INCCFG_ERR, CA_NI_QM_ES_INCCFG_ERR_VAL));
	/*
	 * 0x6108 is the ES_CTRL register; 0x7108 is EPP64_RDPTR(cpu_port 0,
	 * voq 2) and is not written here.
	 */
}

/*
 * Arm/disarm the CPU-port drain (stock aal_l3qm_enable_tx_cpu).  Stock enables
 * all CPU ports (cpu_en byte = 0xff) at open, giving es_ctrl = 0x8462FFFF.
 */
static void cortina_ni_rx_es_cpu(struct cortina_ni *ni, bool enable)
{
	u32 mask = FIELD_PREP(CA_NI_QM_ES_CPU_EN, CA_NI_QM_ES_CPU_EN_ALL);

	if (enable)
		ni_rmw(ni, CA_NI_QM_ES_CTRL, CA_NI_QM_ES_RSVD25, mask);
	else
		ni_rmw(ni, CA_NI_QM_ES_CTRL, mask, 0);
}

/*
 * FBM pools the RMU allocates CPU RX buffers from.  The pool is chosen by the
 * frame's queue: a deep_q=1 / dest-port-8 / voqid=8 frame draws from the
 * deep-queue pool (stock fbm_pool_id 7), others from pool 0.  Each pool needs
 * its own exstack (pointer spill) and buffer region in the reserved memory;
 * buffers are 2048B.
 */
static const struct { u8 id; u32 exstack; u32 buf_base; } cortina_ni_fbm_pools[] = {
	{ 0, 0x0A000000u, 0x09404000u },	/* cpu_pool0 (non-deep) */
	{ 7, 0x0A010000u, 0x0A100000u },	/* deep-queue pool (voqid=8) */
};

/*
 * RMU AXI read/write reorder engine (stock axi_reo_rd_init/wr_init, run after
 * enable_rx), on its own window (DT idx 10).  Without it the RMU's AXI
 * transactions never complete and a frame reaching the QM is never admitted.
 * Stock values: three channel blocks, READ (0x000), WRITE (0x400) and WRITE2
 * (0x480), each with 7 non-zero words; every other offset stays 0.
 */
static const struct { u16 off; u32 val; } cortina_ni_axi_reo_cfg[] = {
	{ 0x000, 0x0000000F }, { 0x004, 0x8000000C }, { 0x008, 0x10000000 },
	{ 0x00c, 0x10000000 }, { 0x010, 0x8000000D }, { 0x018, 0xFFFFFFFF },
	{ 0x024, 0xFFFFFFFF },
	{ 0x400, 0x00000004 }, { 0x404, 0x8000000C }, { 0x408, 0x10000000 },
	{ 0x40c, 0x10000000 }, { 0x410, 0x8000000D }, { 0x418, 0xFFFFFFFF },
	{ 0x424, 0xFFFFFFFF },
	{ 0x480, 0x00000002 }, { 0x484, 0x80000008 }, { 0x488, 0x10000000 },
	{ 0x48c, 0x10000000 }, { 0x490, 0x80000009 }, { 0x498, 0xFFFFFFFF },
	{ 0x4a4, 0xFFFFFFFF },
};

static void cortina_ni_rx_axi_reo_init(struct cortina_ni *ni)
{
	void __iomem *reo = ni->win[CA_NI_WIN_AXI_REO];
	unsigned int i;

	if (!reo) {
		dev_warn(ni->dev,
			 "RX: AXI-reorder window (idx %d) not mapped - RMU DMA stalls\n",
			 CA_NI_WIN_AXI_REO);
		return;
	}
	for (i = 0; i < ARRAY_SIZE(cortina_ni_axi_reo_cfg); i++)
		writel(cortina_ni_axi_reo_cfg[i].val,
		       reo + cortina_ni_axi_reo_cfg[i].off);

	dev_dbg(ni->dev,
		"RX: AXI-reorder init (%zu regs): rd[0x00/0x0c/0x10]=0x%08x/0x%08x/0x%08x wr[0x400]=0x%08x wr2[0x480]=0x%08x\n",
		ARRAY_SIZE(cortina_ni_axi_reo_cfg),
		readl(reo + 0x000), readl(reo + 0x00c), readl(reo + 0x010),
		readl(reo + 0x400), readl(reo + 0x480));
}

/* ------------------------------------------------------------------ */
/* L3QM CPU-EPP ring init (stock aal_l3qm_init_cpu_epp, port0/voq0)    */
/* ------------------------------------------------------------------ */

static void cortina_ni_rx_epp_init(struct cortina_ni *ni)
{
	struct cortina_ni_rx *rx = ni->rx;
	u32 wptr;
	unsigned int i;

	/*
	 * CPU-EPP interrupt enables, stock values: 0x6110 = 0x0000FFFF
	 * (bits[15:8] enable the writeback engine), 0x6114 (EN1, CPU ports 4-7)
	 * = 0, 0x6118 = 0x00000100 (refill threshold).
	 */
	writel(CA_NI_QM_EPP64_INT_EN0_STOCK, ni_base(ni) + CA_NI_QM_EPP64_INT_EN0);
	writel(0, ni_base(ni) + CA_NI_QM_EPP64_INT_EN1);
	writel(CA_NI_QM_EPP64_INT_EN2_STOCK, ni_base(ni) + CA_NI_QM_EPP64_INT_EN2);

	/* no QM interrupt handler: mask its sources and clear what latched */
	writel(0, ni_base(ni) + CA_NI_QM_INT_SRCE);
	writel(readl(ni_base(ni) + CA_NI_QM_INT_SRC), ni_base(ni) + CA_NI_QM_INT_SRC);

	/* linear port->voq map */
	ni_rmw(ni, CA_NI_QM_CPU_EPP_CFG(CA_NI_RX_CPU_PORT),
	       CA_NI_QM_EPP_MAP_MODE, 0);

	/* no descriptor coalescing timer */
	writel(0, ni_base(ni) + CA_NI_QM_CPU_EPP_CT_CFG);

	/*
	 * CPU-EPP FIFO profile, as the vendor's aal_l3qm_init_cpu_epp (RMW,
	 * keeping the other bits):
	 *   size      = per-voq FIFO entries / 4 (128 / 4 = 32)
	 *   timer_ths = 0xf
	 *   high_ths  = (NAPI budget / 16) + 1 (64 -> 5)
	 */
	{
		u32 prof = readl(ni_base(ni) + CA_NI_QM_CPU_EPP_FIFO_PROF(CA_NI_RX_PROFILE_ID));

		prof &= ~(CA_NI_QM_EPP_PROF_SIZE | CA_NI_QM_EPP_PROF_TIMER_THS |
			  CA_NI_QM_EPP_PROF_HIGH_THS);
		prof |= FIELD_PREP(CA_NI_QM_EPP_PROF_SIZE, CA_NI_RX_EPP_PROF_SIZE) |
			FIELD_PREP(CA_NI_QM_EPP_PROF_TIMER_THS, 0xf) |
			FIELD_PREP(CA_NI_QM_EPP_PROF_HIGH_THS, CA_NI_RX_EPP_HIGH_THS);
		writel(prof, ni_base(ni) + CA_NI_QM_CPU_EPP_FIFO_PROF(CA_NI_RX_PROFILE_ID));
		dev_dbg(ni->dev,
			"epp-fifo-prof%u(0x%04x) = 0x%08x (size=%u units, timer_ths=0xf, high_ths=%u)\n",
			CA_NI_RX_PROFILE_ID,
			CA_NI_QM_CPU_EPP_FIFO_PROF(CA_NI_RX_PROFILE_ID), prof,
			CA_NI_RX_EPP_PROF_SIZE, CA_NI_RX_EPP_HIGH_THS);
	}

	/*
	 * Arm all 8 VOQs of the CPU port: each gets the FIFO profile and its own
	 * ring region (ring_dma + voq * RING_BYTES).
	 */
	WARN_ON_ONCE(upper_32_bits(rx->ring_dma));
	for (i = 0; i < CA_NI_RX_VOQ_COUNT; i++) {
		u32 vphys = lower_32_bits(rx->ring_dma) + i * CA_NI_RX_RING_BYTES;

		ni_rmw(ni, CA_NI_QM_CPU_EPP_FIFO_CFG(CA_NI_RX_CPU_PORT, i),
		       CA_NI_QM_EPP_PROFILE_SEL,
		       FIELD_PREP(CA_NI_QM_EPP_PROFILE_SEL, CA_NI_RX_PROFILE_ID));

		writel(vphys, ni_base(ni) +
		       CA_NI_QM_EPP64_PADDR_START(CA_NI_RX_CPU_PORT, i));

		/* CPU port 1 (hash-engine punts) has its own rings */
		ni_rmw(ni, CA_NI_QM_CPU_EPP_FIFO_CFG(1, i),
		       CA_NI_QM_EPP_PROFILE_SEL,
		       FIELD_PREP(CA_NI_QM_EPP_PROFILE_SEL,
				  CA_NI_RX_PROFILE_ID));
		writel(lower_32_bits(rx->ring_dma) + CA_NI_RX_PORT_STRIDE +
		       i * CA_NI_RX_RING_BYTES,
		       ni_base(ni) + CA_NI_QM_EPP64_PADDR_START(1, i));
		rx->rptr[CA_NI_RX_VOQ_COUNT + i] =
			cortina_ni_rx_wptr_voq(ni, CA_NI_RX_VOQ_COUNT + i);
		writel(rx->rptr[CA_NI_RX_VOQ_COUNT + i],
		       ni_base(ni) + CA_NI_QM_EPP64_RDPTR(1, i));

		/* adopt the HW write pointer (0 after reset) as this voq's start */
		wptr = cortina_ni_rx_wptr_voq(ni, i);
		if (wptr)
			dev_warn(ni->dev, "RX voq%u wptr not idle at init (0x%x)\n",
				 i, wptr);
		rx->rptr[i] = wptr;
		writel(wptr, ni_base(ni) +
		       CA_NI_QM_EPP64_RDPTR(CA_NI_RX_CPU_PORT, i));
	}

	/*
	 * 0x6a3c bit2 (cmd_mode/GO, the EPP writeback-engine run bit and 64-bit
	 * descriptor mode) last, after every voq's FIFO paddr, profile and rdptr
	 * are set, as stock does.  Setting it earlier leaves the engine without
	 * writeback.
	 */
	ni_rmw(ni, CA_NI_QM_EPP, 0, CA_NI_QM_EPP_CMD_MODE_64);
	dev_dbg(ni->dev, "epp-init: cmd_mode/GO(0x6a3c)=0x%08x set LAST (after per-voq paddr)\n",
		readl(ni_base(ni) + CA_NI_QM_EPP));

	/*
	 * The egress-scheduler enable pair (0x6a20 TX path, 0x6a00 CPU path),
	 * stock 0x0000FF00 each, written after the GO since arming the EPP
	 * engine clears 0x6a00.  Without the CPU-path enable CPU_0 egress is
	 * never serviced.
	 */
	writel(CA_NI_QM_EPP_EGR_EN_ALL, ni_base(ni) + CA_NI_QM_EPP_TX_EGR_EN);
	writel(CA_NI_QM_EPP_EGR_EN_ALL, ni_base(ni) + CA_NI_QM_EPP_CPU_EGR_EN);
	dev_dbg(ni->dev, "epp-egr: tx(0x6a20)=0x%08x cpu(0x6a00)=0x%08x (want both 0x0000ff00)\n",
		readl(ni_base(ni) + CA_NI_QM_EPP_TX_EGR_EN),
		readl(ni_base(ni) + CA_NI_QM_EPP_CPU_EGR_EN));
}

/* ------------------------------------------------------------------ */
/* GPHY fault poll + port reinit (stock aal_internal_phy_recovery)     */
/* ------------------------------------------------------------------ */

/*
 * The internal GPHY can wedge its RX path while the link still reads up and
 * every MAC/QM register looks normal.  Stock polls a GPHY fault latch (page
 * 0xb90 reg 19) at 1 Hz on every port and does a full port interface reinit
 * when it is set; toggling rx_en/es_cpu/ndo_open does not clear the state.
 */

static inline void __iomem *cortina_ni_rx_gphy(struct cortina_ni *ni)
{
	/* the gphy window is optional in the DT: NULL = feature unavailable */
	if (!ni->win[CA_NI_WIN_GPHY])
		return NULL;
	return ni->win[CA_NI_WIN_GPHY] + CA_NI_GPHY_BANK(CA_NI_RX_PORT);
}

static u32 cortina_ni_rx_gphy_fault(struct cortina_ni *ni)
{
	void __iomem *gphy = cortina_ni_rx_gphy(ni);
	u32 v;

	if (!gphy)
		return 0;
	v = readl(gphy + CA_NI_GPHY_FAULT) & 0xffff;
	/*
	 * 0xbad0 is the OCP read-timeout value (see cortina-ni-regs.h), not a
	 * fault latch.  Treating it as a fault would reinit port 0 every tick,
	 * and the reinit rewrites the shared GPHY-wrapper EN1, breaking the
	 * interface of the other ports.
	 */
	return (v == 0xbad0) ? 0 : v;
}

/* analog calibration registers stock reloads after the reinit */
static const u32 cortina_ni_rx_gphy_cal_off[CA_NI_RX_GPHY_CAL_REGS] = {
	CA_NI_GPHY_EXT(0xbcd, 22),	/* rc_cal_len_l */
	CA_NI_GPHY_EXT(0xbcd, 23),
	CA_NI_GPHY_EXT(0xbcf, 18),	/* r_cal (tapbin A-D) */
	CA_NI_GPHY_EXT(0xbcf, 19),
	CA_NI_GPHY_EXT(0xbcf, 20),
	CA_NI_GPHY_EXT(0xbcf, 21),
	CA_NI_GPHY_EXT(0xbca, 22),	/* amp_cal (ibadj) */
};

static void cortina_ni_rx_gphy_cal_save(struct cortina_ni *ni)
{
	void __iomem *gphy = ni->win[CA_NI_WIN_GPHY];
	unsigned int b;
	int i;

	if (!gphy)
		return;

	/*
	 * Taken at probe, from the calibration U-Boot left.  Every bank is
	 * saved so the per-port interface reinit can restore any port's values.
	 */
	for (b = 0; b < CA_NI_GPHY_COUNT; b++)
		for (i = 0; i < CA_NI_RX_GPHY_CAL_REGS; i++)
			ni->rx->gphy_cal[b][i] =
				readl(gphy + CA_NI_GPHY_BANK(b) +
				      cortina_ni_rx_gphy_cal_off[i]);
}

static void cortina_ni_rx_gphy_intf_rst_pulse(struct cortina_ni *ni)
{
	writel(CA_NI_HV_INTF_RST_GPHY(CA_NI_RX_PORT),
	       ni_base(ni) + CA_NI_HV_INTF_RST);
	usleep_range(1000, 1500);	/* stock: 1 ms */
	writel(0, ni_base(ni) + CA_NI_HV_INTF_RST);
}

/*
 * Force the GPHY-wrapper enables to the stock golden steady state (EN0 =
 * 0xFF000000, EN1 = 0x1001).  EN1 bit12 (patch_phy_done) is the GPHY->port-MAC
 * datapath release - the RX determinism gate.  Idempotent; also clears any
 * stray per-port EN1[4+p] toggle so we land on 0x1001 after a fault reinit.
 * Re-run on every link-up in case phylib's PHY handling disturbed the wrapper.
 */
static void cortina_ni_rx_wrap_establish(struct cortina_ni *ni)
{
	void __iomem *wrap = ni->win[CA_NI_WIN_GPHY_WRAP];

	if (!wrap)
		return;
	writel(CA_NI_GPHY_WRAP_EN0_VAL, wrap + CA_NI_GPHY_WRAP_EN0);
	writel(CA_NI_GPHY_WRAP_EN1_VAL, wrap + CA_NI_GPHY_WRAP_EN1);
}

/*
 * the stock reinit sequence, port 0 only; every step is a plain register
 * write
 */
static void cortina_ni_rx_gphy_reinit(struct cortina_ni *ni)
{
	void __iomem *gphy = cortina_ni_rx_gphy(ni);
	void __iomem *wrap = ni->win[CA_NI_WIN_GPHY_WRAP];
	u32 val;
	int i;

	if (!gphy)
		return;

	/*
	 * serialise against phylib's MDIO polling of the same GPHY; with
	 * skip_mdio=1 there is no MDIO bus (ni->mii == NULL) and nothing to
	 * race
	 */
	if (ni->mii)
		mutex_lock(&ni->mii->mdio_lock);

	cortina_ni_rx_gphy_intf_rst_pulse(ni);

	/* re-enable the GPHY uC patch/self-check */
	val = readl(gphy + CA_NI_GPHY_PATCH_EN);
	writel(val | CA_NI_GPHY_PATCH_EN_BIT, gphy + CA_NI_GPHY_PATCH_EN);

	/* wrapper interface toggle */
	if (wrap) {
		val = readl(wrap + CA_NI_GPHY_WRAP_EN1);
		writel(val & ~CA_NI_GPHY_WRAP_EN1_IF(CA_NI_RX_PORT),
		       wrap + CA_NI_GPHY_WRAP_EN1);
		writel(val | CA_NI_GPHY_WRAP_EN1_IF(CA_NI_RX_PORT),
		       wrap + CA_NI_GPHY_WRAP_EN1);
	}

	cortina_ni_rx_gphy_intf_rst_pulse(ni);
	msleep(200);			/* stock: 200 ms settle */

	/*
	 * restore the probe-time calibration snapshot (register file only; the
	 * DSP-SRAM mirror is uC-patch specific, see cortina-ni-regs.h)
	 */
	for (i = 0; i < CA_NI_RX_GPHY_CAL_REGS; i++)
		writel(ni->rx->gphy_cal[CA_NI_RX_PORT][i],
		       gphy + cortina_ni_rx_gphy_cal_off[i]);

	/* power the PHY back up + release the page-0xa46 hold bit */
	val = readl(gphy + CA_NI_GPHY_BMCR);
	writel(val & ~CA_NI_GPHY_BMCR_PDOWN, gphy + CA_NI_GPHY_BMCR);
	val = readl(gphy + CA_NI_GPHY_HOLD);
	writel(val & ~CA_NI_GPHY_HOLD_BIT, gphy + CA_NI_GPHY_HOLD);

	/*
	 * land on the stock steady wrapper state (EN1 = 0x1001, per-port toggle
	 * cleared, patch_phy_done set) so RX resumes after the reinit
	 */
	cortina_ni_rx_wrap_establish(ni);

	if (ni->mii)
		mutex_unlock(&ni->mii->mdio_lock);
}

/*
 * 1 Hz self-rearming poll, the stock cadence.  Runs between open and stop;
 * each pass is one register read unless a fault is latched.
 */
static void cortina_ni_rx_recovery_work(struct work_struct *work)
{
	struct cortina_ni_rx *rx = container_of(to_delayed_work(work),
						struct cortina_ni_rx,
						recovery_work);
	struct cortina_ni *ni = rx->ni;
	u32 fault = cortina_ni_rx_gphy_fault(ni);

	if (unlikely(fault)) {
		dev_warn(ni->dev,
			 "GPHY port %d fault latch 0x%04x - reinit (#%llu)\n",
			 CA_NI_RX_PORT, fault, rx->recoveries + 1);
		cortina_ni_rx_gphy_reinit(ni);
		rx->recoveries++;
		dev_dbg(ni->dev, "GPHY port %d reinit done (latch now 0x%04x)\n",
			 CA_NI_RX_PORT, cortina_ni_rx_gphy_fault(ni));
	}

	/*
	 * Datapath bring-up: until every GPHY bank is patched, run the link-up
	 * bring-up here, so the LAN ports come up even when eth0's PHY never
	 * reports link.  Idempotent, capped by rearms.
	 */

	/*
	 * Keep the CPU-port (eth0) carrier up once the datapath has been armed
	 * (rx->rearms > 0): eth0 is the CPU<->switch port, and a carrier-off
	 * bridge member drops every LAN frame.  Done every tick so a later
	 * netif_carrier_off (re-ifup, phy_stop, a port-0 bounce) is undone
	 * within 1 s.
	 */
	if (rx->rearms && rx->netdev && !netif_carrier_ok(rx->netdev)) {
		netif_carrier_on(rx->netdev);
		netdev_dbg(rx->netdev,
			   "CPU-port carrier re-asserted (switch datapath up)\n");
	}

	schedule_delayed_work(&rx->recovery_work, HZ);
}

/*
 * phylib link-up hook (called from cortina_ni_tx_adjust_link): idempotently
 * re-arm the whole RX delivery chain, so anything a boot-time link bounce
 * may have clobbered is re-applied, and immediately run one GPHY fault
 * check - the wedge, when it happens, latches across exactly such a bounce.
 */
void cortina_ni_rx_link_up(struct cortina_ni *ni)
{
	struct cortina_ni_rx *rx = ni->rx;
	unsigned int p;

	if (!rx)
		return;		/* TX-only mode */

	/*
	 * re-establish the GPHY -> port-MAC datapath (wrapper EN1
	 * bit12) first: without it the PHY links but no frame reaches
	 * the MAC
	 */
	cortina_ni_rx_wrap_establish(ni);

	/*
	 * re-assert the MAC<->GPHY internal GMII interface
	 * (int_cfg=GE_GMII, phy_mode=MAC, MAC loopback off) on every
	 * LAN port; stock has STATIC=0xcb000200 on all ports.  The
	 * upper byte 0xCB is read-only status; only the writable
	 * fields are touched.
	 */
	for (p = 0; p < CA_NI_LAN_PORT_COUNT; p++)
		ni_rmw(ni, CA_NI_PORT_STATIC_CFG(p),
		       CA_NI_PORT_STATIC_INT_CFG | CA_NI_PORT_STATIC_PHY_MODE |
		       CA_NI_PORT_STATIC_LPBK_MODE, 0);

	/* autosync = 0xF, as stock */
	writel(CA_NI_NI_AUTOSYNC_STOCK_VAL,
	       ni_base(ni) + CA_NI_HV_MAC_AUTOSYNC);

	/*
	 * re-apply the full FE-path config: steer/demux/DLF, ES master (RSVD25
	 * clear), cpu_en=0xff, RMU RX and port MAC RX (stock 0x3001)
	 */
	cortina_ni_rx_steer_init(ni);
	cortina_ni_rx_es_enable(ni);
	cortina_ni_rx_es_cpu(ni, true);
	ni_rmw(ni, CA_NI_QM_RMU0_CTRL, 0, CA_NI_QM_RMU0_RX_EN);
	/* power up RX and enable the RX MAC on every LAN port */
	{
		unsigned int p;

		for (p = 0; p < CA_NI_LAN_PORT_COUNT; p++) {
			ni_rmw(ni, CA_NI_PORT_GLB_CFG(p),
			       CA_NI_PORT_GLB_PWR_DWN_RX, 0);
			ni_rmw(ni, CA_NI_PORT_RXMAC_CFG(p),
			       CA_NI_PORT_RXMAC_STOCK_CLR,
			       CA_NI_PORT_RXMAC_RX_EN |
			       CA_NI_PORT_RXMAC_STOCK_SET);
		}
	}

	rx->rearms++;
	dev_dbg(ni->dev,
		"RX (re)armed on link-up (#%llu): wrap_en1=0x%08x rxmac=0x%08x es_ctrl=0x%08x gphy_fault=0x%04x\n",
		rx->rearms,
		ni->win[CA_NI_WIN_GPHY_WRAP] ?
		readl(ni->win[CA_NI_WIN_GPHY_WRAP] + CA_NI_GPHY_WRAP_EN1) : 0,
		readl(ni_base(ni) + CA_NI_PORT_RXMAC_CFG(CA_NI_RX_PORT)),
		readl(ni_base(ni) + CA_NI_QM_ES_CTRL),
		cortina_ni_rx_gphy_fault(ni));

	/*
	 * eth0 is the CPU<->switch port, not a single physical link: once the
	 * switch datapath is armed, force its carrier up so the bridge keeps
	 * forwarding on it.  Per-port link is handled by the switch.
	 */
	if (rx->netdev && !netif_carrier_ok(rx->netdev))
		netif_carrier_on(rx->netdev);

	/* fault check now instead of waiting for the next 1 Hz tick */
	if (cortina_ni_rx_gphy(ni))
		mod_delayed_work(system_wq, &rx->recovery_work, 0);
}

/* ------------------------------------------------------------------ */
/* open/stop hooks (called from the netdev ops in cortina-ni-tx.c)     */
/* ------------------------------------------------------------------ */

void cortina_ni_rx_open(struct cortina_ni *ni)
{
	if (!ni->rx)
		return;		/* TX-only mode (RX probe failed/absent) */

	napi_enable(&ni->rx->napi);
	napi_enable(&ni->rx->napi_wan);

	/*
	 * arm the CPU-port drain (stock enable_tx_cpu at ca_ni_open) so the ES
	 * scheduler starts writing descriptors for our CPU port
	 */
	cortina_ni_rx_es_cpu(ni, true);

	/*
	 * port-0 MAC RX on, stock pattern 0x3001: rx_en + bit12/bit13 set,
	 * bit8 clear.
	 */
	ni_rmw(ni, CA_NI_PORT_RXMAC_CFG(CA_NI_RX_PORT),
	       CA_NI_PORT_RXMAC_STOCK_CLR,
	       CA_NI_PORT_RXMAC_RX_EN | CA_NI_PORT_RXMAC_STOCK_SET);
	ni_rmw(ni, CA_NI_PORT_GLB_CFG(CA_NI_RX_PORT),
	       CA_NI_PORT_GLB_PWR_DWN_RX, 0);

	cortina_ni_rx_irq_set(ni, true);

	/*
	 * start the 1 Hz GPHY fault poll (stock runs it for the device's whole
	 * lifetime; ours runs while the netdev is up)
	 */
	if (cortina_ni_rx_gphy(ni))
		schedule_delayed_work(&ni->rx->recovery_work, HZ);
}

void cortina_ni_rx_stop(struct cortina_ni *ni)
{
	unsigned int i;

	if (!ni->rx)
		return;

	cancel_delayed_work_sync(&ni->rx->recovery_work);

	/*
	 * Stop new ingress first, then quiesce the drain side.  Buffers already
	 * pushed to the hardware pool cannot be popped back; they are reused on
	 * the next open.
	 */
	ni_rmw(ni, CA_NI_PORT_RXMAC_CFG(CA_NI_RX_PORT),
	       CA_NI_PORT_RXMAC_RX_EN, 0);
	cortina_ni_rx_es_cpu(ni, false);	/* disarm the CPU-port drain */
	cortina_ni_rx_irq_set(ni, false);
	napi_disable(&ni->rx->napi);
	napi_disable(&ni->rx->napi_wan);

	/*
	 * NAPI is quiesced, so release any partially assembled frame instead
	 * of holding its skb across a close/open.
	 */
	for (i = 0; i < CA_NI_RX_RING_COUNT; i++)
		cortina_ni_rx_chain_reset(ni->rx, &ni->rx->chain[i]);
}

/* ------------------------------------------------------------------ */
/* /proc/net/cortina_ni_rx and register dump helpers                   */
/* ------------------------------------------------------------------ */

/*
 * Read one NI MIB counter for @port via an indirect ACCESS/DATA0 pair (stock
 * __ni_eth_port_rx_mib_get).  Bounded poll; returns ~0 on timeout so a stuck
 * access is visible.  Process context only (readl_poll_timeout may sleep).
 */
static u32 cortina_ni_mib_read_at(struct cortina_ni *ni, u32 acc_off, u32 data_off,
				  u32 port, u32 cnt_id)
{
	u32 val;

	writel(CA_NI_MIB_ACCESS_GO |
	       FIELD_PREP(CA_NI_MIB_ACCESS_OPCODE, CA_NI_MIB_OP_READ_ONLY) |
	       FIELD_PREP(CA_NI_MIB_ACCESS_PORT, port) |
	       FIELD_PREP(CA_NI_MIB_ACCESS_CNTID, cnt_id),
	       ni_base(ni) + acc_off);
	if (readl_poll_timeout(ni_base(ni) + acc_off, val,
			       !(val & CA_NI_MIB_ACCESS_GO),
			       CA_NI_MIB_POLL_US, CA_NI_MIB_POLL_TIMEOUT_US))
		return ~0u;
	return readl(ni_base(ni) + data_off);
}

/* RX MIB read, for the DSA port statistics */
u32 cortina_ni_rx_mib_read(struct cortina_ni *ni, u32 port, u32 cnt_id)
{
	return cortina_ni_mib_read_at(ni, CA_NI_HV_RXMIB_ACCESS,
				      CA_NI_HV_RXMIB_DATA0, port, cnt_id);
}

/* TX MIB read, for the DSA port statistics */
u32 cortina_ni_tx_mib_read(struct cortina_ni *ni, u32 port, u32 cnt_id)
{
	return cortina_ni_mib_read_at(ni, CA_NI_HV_TXMIB_ACCESS,
				      CA_NI_HV_TXMIB_DATA0, port, cnt_id);
}

/*
 * Curated NI-window register snapshot.  Every offset is inside the 64K NI
 * window and read with a plain readl (no indirect access, no side effects).
 */
static const struct {
	const char	*name;
	u32		off;
} cortina_ni_rx_regs[] = {
	/* NI_HV globals */
	{ "hv_init_done",	CA_NI_HV_INIT_DONE },
	{ "hv_intf_rst",	CA_NI_HV_INTF_RST },
	{ "hv_mac_autosync",	CA_NI_HV_MAC_AUTOSYNC },
	{ "hv_pkt_len",		CA_NI_HV_PKT_LEN },
	{ "hv_pkt_len_rx",	CA_NI_HV_PKT_LEN_RX },
	{ "hv_cfg_a1b8",	CA_NI_HV_CFG_A1B8 },
	{ "ni_intern_portid",	CA_NI_NI_INTERNAL_PORT_ID_CFG },
	{ "hv_cfg_a420",	CA_NI_HV_CFG_A420 },
	{ "hv_cfg_aaf0",	CA_NI_HV_CFG_AAF0 },
	/* L3FE demux golden routing map (FE output -> CPU-EPP) */
	{ "l3fe_demux0_a190",	CA_NI_NIRX_L3FE_DEMUX0 },
	{ "l3fe_demux1_a194",	CA_NI_NIRX_L3FE_DEMUX1 },
	{ "l3fe_demux2_a198",	CA_NI_NIRX_L3FE_DEMUX2 },
	{ "l3fe_demux3_a19c",	CA_NI_NIRX_L3FE_DEMUX3 },
	{ "l3fe_demux4_a1a0",	CA_NI_NIRX_L3FE_DEMUX4 },
	{ "l3fe_demux5_a1a4",	CA_NI_NIRX_L3FE_DEMUX5 },
	/* deep_q=1 demux: ldpid 0x32 = dpq_48_63 (0xa1a8) bits[5:4], want 0 = L3QM */
	{ "dpq_demux_a1a8",	CA_NI_NIRX_L3FE_DPQ_DEMUX_48_63 },
	{ "dpq_demux_a1ac",	CA_NI_NIRX_L3FE_DPQ_DEMUX_32_47 },
	{ "dpq_demux_a1b0",	CA_NI_NIRX_L3FE_DPQ_DEMUX_16_31 },
	{ "dpq_demux_a1b4",	CA_NI_NIRX_L3FE_DPQ_DEMUX_0_15 },
	/* L2FE forwarding-control (stock 0x140c=0x0c100c10, 0x160c=0x1) */
	{ "l2fe_lrn_fwd1_140c",	CA_NI_L2FE_PLC_LRN_FWD_CTRL_1 },
	{ "l2fe_arb_ext_160c",	CA_NI_L2FE_ARB_CTRL_EXT },
	/* flow-control init: BM dq -> TM-port map, STG0 ctrl, thresholds */
	{ "bm_dq_port_map_2124", CA_NI_L2TM_BM_DQ_PORT_MAP },
	{ "ni_flowctrl_en_3400", CA_NI_NI_FLOWCTRL_EN },
	{ "ni_flowthr0_9798",	CA_NI_NI_FLOWCTRL_THRESH },
	/* port-0 MAC block */
	{ "p0_glb",		CA_NI_PORT_GLB_CFG(CA_NI_RX_PORT) },
	{ "p0_rxmac",		CA_NI_PORT_RXMAC_CFG(CA_NI_RX_PORT) },
	{ "p0_txmac",		CA_NI_PORT_TXMAC_CFG(CA_NI_RX_PORT) },
	{ "p0_rx_cntrl",	CA_NI_PORT_RX_CNTRL_CFG(CA_NI_RX_PORT) },
	/* L2TM (TX-side scheduler/QM, dumped for completeness) */
	{ "l2tm_qm_eq_cfg",	CA_NI_L2TM_QM_EQ_CFG },
	{ "l2tm_qm_glob_buf",	CA_NI_L2TM_QM_GLOB_BUF_CFG },
	{ "l2tm_qm_prvt_prof0",	CA_NI_L2TM_QM_PORT_PRVT_PROF0 },
	{ "l2tm_es_ctrl",	CA_NI_L2TM_ES_CTRL },
	{ "l2tm_es_sch0",	CA_NI_L2TM_ES_SCH_CFG(0) },
	/* L3QM / RMU (RX drain path) */
	{ "qm_rmu0_ctrl",	CA_NI_QM_RMU0_CTRL },
	{ "qm_es_ctrl_6108",	CA_NI_QM_ES_CTRL },
	{ "qm_l3tm_ni_ena",	CA_NI_QM_L3TM_NI_PORT_ENA },	/* 0x610c */
	{ "qm_int_en0",		CA_NI_QM_EPP64_INT_EN0 },
	{ "qm_int_en1",		CA_NI_QM_EPP64_INT_EN1 },
	{ "qm_eq_profile13",	CA_NI_QM_EQ_PROFILE(CA_NI_RX_EQ_PROFILE) },  /* {eqp0=13,eqp1=14} */
	{ "qm_eq14_cfg1",	CA_NI_QM_CFG1_EQ(CA_NI_RX_EQ_ID2) },
	{ "qm_eq14_pa_req",	CA_NI_QM_EQM_PA_REQ(CA_NI_RX_EQ_ID2) },
	{ "qm_es_ctrl2",	CA_NI_QM_ES_CTRL2 },
	{ "qm_epp_cmd",		CA_NI_QM_EPP },
	{ "qm_destp0_eq_cfg",	CA_NI_QM_DEST_PORT_EQ_CFG(CA_NI_RX_CPU_DEST_PORT) },
	{ "qm_destp0_pkt_buf",	CA_NI_QM_DEST_PORT_PKT_BUF_CFG(CA_NI_RX_CPU_PORT) },
	{ "qm_eq13_cfg0",	CA_NI_QM_CFG0_EQ(CA_NI_RX_EQ_ID) },
	{ "qm_eq13_cfg1",	CA_NI_QM_CFG1_EQ(CA_NI_RX_EQ_ID) },
	{ "qm_eq13_cfg2",	CA_NI_QM_CFG2_EQ(CA_NI_RX_EQ_ID) },
	{ "qm_eq13_cfg3",	CA_NI_QM_CFG3_EQ(CA_NI_RX_EQ_ID) },
	{ "qm_eq13_pa_req",	CA_NI_QM_EQM_PA_REQ(CA_NI_RX_EQ_ID) },
	{ "qm_push_ready0",	CA_NI_QM_CPU_PUSH_READY(CA_NI_RX_CPU_PORT) },
	{ "qm_voq_en0",		CA_NI_QM_VOQ_EN(CA_NI_RX_CPU_PORT) },
	{ "qm_eq_cfg_load",	CA_NI_QM_EQ_CFG_LOAD },
	{ "qm_hdm_wr_prot",	CA_NI_QM_HDM_WRITE_PROT },
	{ "qm_rx_status0",	CA_NI_QM_RX_STATUS0 },
	{ "qm_rx_status1",	CA_NI_QM_RX_STATUS1 },
	{ "qm_tx_cntr",		CA_NI_QM_TX_CNTR },
	{ "qm_rx_cntr",		CA_NI_QM_RX_CNTR },
	/* CPU-EPP FIFO config + ring pointers (port0/voq0) */
	{ "qm_cpu_epp_cfg0",	CA_NI_QM_CPU_EPP_CFG(CA_NI_RX_CPU_PORT) },
	{ "qm_cpu_epp_ct",	CA_NI_QM_CPU_EPP_CT_CFG },
	{ "qm_epp_fifo_prof0_66a4",	CA_NI_QM_CPU_EPP_FIFO_PROF(0) },	/* stock 0xE0008001 */
	{ "qm_epp_fifo_prof4",	CA_NI_QM_CPU_EPP_FIFO_PROF(CA_NI_RX_PROFILE_ID) },
	{ "qm_epp_fifo_cfg0",	CA_NI_QM_CPU_EPP_FIFO_CFG(CA_NI_RX_CPU_PORT,
							 CA_NI_RX_VOQ) },
	{ "qm_epp_wrptr0",	CA_NI_QM_EPP64_WRPTR(CA_NI_RX_CPU_PORT,
						     CA_NI_RX_VOQ) },
	{ "qm_epp_rdptr0",	CA_NI_QM_EPP64_RDPTR(CA_NI_RX_CPU_PORT,
						     CA_NI_RX_VOQ) },
	{ "qm_epp_paddr0",	CA_NI_QM_EPP64_PADDR_START(CA_NI_RX_CPU_PORT,
							   CA_NI_RX_VOQ) },
};
/*
 * QM + L2TM block sweep, restricted to offsets that are defined on this SoC.
 * Several offsets that exist on other members of the family abort on read
 * here (e.g. QM_VOQ_STATUS8 at 0x6978: this SoC has 8 VoQs; the
 * BACK_PRES_CTRL / VP_BACK_PRES_CTRL / STC_SPSRAM block around 0x6a40).
 * Array registers are expanded, so e.g. all of CPUEPP64_FIFO_WRPTR
 * (0x7000-0x70fc) is included.  Read-only.
 */
static const u16 cortina_ni_qmdump_offs[] = {
	0x2000, 0x2004, 0x2008, 0x200c, 0x2010, 0x2014, 0x2018, 0x201c, 0x2020, 0x2024, 0x2100, 0x2104,
	0x2108, 0x210c, 0x2110, 0x2114, 0x2118, 0x211c, 0x2120, 0x2124, 0x2128, 0x212c, 0x2130, 0x2134,
	0x2138, 0x213c, 0x2140, 0x2144, 0x2148, 0x214c, 0x2150, 0x2154, 0x2158, 0x215c, 0x2160, 0x2164,
	0x2168, 0x216c, 0x2170, 0x2174, 0x2178, 0x217c, 0x2180, 0x2184, 0x2188, 0x218c, 0x2190, 0x2194,
	0x2198, 0x219c, 0x21a0, 0x21a4, 0x21a8, 0x21ac, 0x21b0, 0x21b4, 0x2200, 0x2204, 0x2208, 0x220c,
	0x2210, 0x2214, 0x2218, 0x221c, 0x2220, 0x2224, 0x2228, 0x222c, 0x2230, 0x2234, 0x2238, 0x223c,
	0x2240, 0x2244, 0x2248, 0x224c, 0x2250, 0x2254, 0x2258, 0x225c, 0x2260, 0x2264, 0x2268, 0x226c,
	0x2270, 0x2274, 0x2278, 0x227c, 0x2280, 0x2284, 0x2288, 0x228c, 0x2290, 0x2294, 0x2298, 0x229c,
	0x22a0, 0x22a4, 0x22a8, 0x2300, 0x2304, 0x2308, 0x230c, 0x2310, 0x2314, 0x2318, 0x231c, 0x2320,
	0x2324, 0x2328, 0x232c, 0x2330, 0x2334, 0x2338, 0x233c, 0x2340, 0x2344, 0x2348, 0x234c, 0x2350,
	0x2354, 0x2358, 0x235c, 0x2360, 0x2364, 0x2368, 0x236c, 0x2370, 0x2374, 0x2378, 0x237c, 0x2380,
	0x2384, 0x2388, 0x238c, 0x2390, 0x2394, 0x2398, 0x239c, 0x23a0, 0x23a4, 0x23a8, 0x23ac, 0x23b0,
	0x23b4, 0x23b8, 0x23bc, 0x23c0, 0x23c4, 0x23c8, 0x23cc, 0x23d0, 0x23d4, 0x23d8, 0x23dc, 0x23e0,
	0x23e4, 0x23e8, 0x23ec, 0x23f0, 0x23f4, 0x23f8, 0x23fc, 0x6000, 0x6004, 0x6008, 0x600c, 0x6100,
	0x6104, 0x6108, 0x610c, 0x6110, 0x6114, 0x6118, 0x611c, 0x6120, 0x6124, 0x6128, 0x612c, 0x6130,
	0x6134, 0x6138, 0x613c, 0x6140, 0x6144, 0x6148, 0x614c, 0x6150, 0x6154, 0x6158, 0x615c, 0x6160,
	0x6164, 0x6168, 0x616c, 0x6170, 0x6174, 0x6178, 0x617c, 0x6180, 0x6184, 0x6188, 0x618c, 0x6190,
	0x6194, 0x6198, 0x619c, 0x61a0, 0x61a4, 0x61a8, 0x61ac, 0x61b0, 0x61b4, 0x61b8, 0x61bc, 0x61c0,
	0x61c4, 0x61c8, 0x61cc, 0x61d0, 0x61d4, 0x61d8, 0x61dc, 0x61e0, 0x61e4, 0x61e8, 0x61ec, 0x61f0,
	0x61f4, 0x61f8, 0x61fc, 0x6200, 0x6204, 0x6208, 0x620c, 0x6210, 0x6214, 0x6218, 0x621c, 0x6220,
	0x6224, 0x6228, 0x622c, 0x6230, 0x6234, 0x6238, 0x623c, 0x6240, 0x6244, 0x6248, 0x624c, 0x6250,
	0x6254, 0x6258, 0x625c, 0x6260, 0x6264, 0x6268, 0x626c, 0x6270, 0x6274, 0x6278, 0x627c, 0x6280,
	0x6284, 0x6288, 0x628c, 0x6290, 0x6294, 0x6298, 0x629c, 0x62a0, 0x62a4, 0x62a8, 0x62ac, 0x62b0,
	0x62b4, 0x62b8, 0x62bc, 0x62c0, 0x62c4, 0x62c8, 0x62cc, 0x62d0, 0x62d4, 0x62d8, 0x62dc, 0x62e0,
	0x62e4, 0x62e8, 0x62ec, 0x62f0, 0x62f4, 0x62f8, 0x62fc, 0x6300, 0x6304, 0x6308, 0x630c, 0x6310,
	0x6314, 0x6318, 0x631c, 0x6320, 0x6324, 0x6328, 0x632c, 0x6330, 0x6334, 0x6338, 0x633c, 0x6340,
	0x6344, 0x6348, 0x634c, 0x6350, 0x6354, 0x6358, 0x635c, 0x6360, 0x6364, 0x6368, 0x636c, 0x6370,
	0x6374, 0x6378, 0x637c, 0x6380, 0x6384, 0x6388, 0x638c, 0x6390, 0x6394, 0x6398, 0x639c, 0x63a0,
	0x63a4, 0x63a8, 0x63ac, 0x63b0, 0x63b4, 0x63b8, 0x63bc, 0x63c0, 0x63c4, 0x63c8, 0x63cc, 0x63d0,
	0x63d4, 0x63d8, 0x63dc, 0x63e0, 0x63e4, 0x63e8, 0x63ec, 0x63f0, 0x63f4, 0x63f8, 0x63fc, 0x6400,
	0x6404, 0x6408, 0x640c, 0x6410, 0x6414, 0x6418, 0x641c, 0x6420, 0x6424, 0x6428, 0x642c, 0x6430,
	0x6434, 0x6438, 0x643c, 0x6440, 0x6444, 0x6448, 0x644c, 0x6450, 0x6454, 0x6458, 0x645c, 0x6460,
	0x6464, 0x6468, 0x646c, 0x6470, 0x6474, 0x6478, 0x647c, 0x6480, 0x6484, 0x6488, 0x648c, 0x6490,
	0x6494, 0x6498, 0x649c, 0x64a0, 0x64a4, 0x64a8, 0x64ac, 0x64b0, 0x64b4, 0x64b8, 0x64bc, 0x64c0,
	0x64c4, 0x64c8, 0x64cc, 0x64d0, 0x64d4, 0x64d8, 0x64dc, 0x64e0, 0x64e4, 0x64e8, 0x64ec, 0x64f0,
	0x64f4, 0x64f8, 0x64fc, 0x6500, 0x6504, 0x6508, 0x650c, 0x6510, 0x6514, 0x6518, 0x651c, 0x6520,
	0x6524, 0x6528, 0x652c, 0x6530, 0x6534, 0x6538, 0x653c, 0x6540, 0x6544, 0x6548, 0x654c, 0x6550,
	0x6554, 0x6558, 0x655c, 0x6560, 0x6564, 0x6568, 0x656c, 0x6570, 0x6574, 0x6578, 0x657c, 0x6580,
	0x6584, 0x6588, 0x658c, 0x6590, 0x6594, 0x6598, 0x659c, 0x65a0, 0x65a4, 0x65a8, 0x65ac, 0x65b0,
	0x65b4, 0x65b8, 0x65bc, 0x65c0, 0x65c4, 0x65e4, 0x6604, 0x6624, 0x6644, 0x6664, 0x6684, 0x66a4,
	0x66a8, 0x66ac, 0x66b0, 0x66b4, 0x66b8, 0x66bc, 0x66c0, 0x66c4, 0x66c8, 0x66cc, 0x66d0, 0x66d4,
	0x66d8, 0x66dc, 0x66e0, 0x66e4, 0x66e8, 0x66ec, 0x66f0, 0x66f4, 0x66f8, 0x66fc, 0x6700, 0x6704,
	0x6708, 0x670c, 0x6710, 0x6714, 0x6718, 0x671c, 0x6720, 0x6724, 0x6728, 0x672c, 0x6730, 0x6734,
	0x6738, 0x673c, 0x6740, 0x6744, 0x6748, 0x674c, 0x6750, 0x6754, 0x6758, 0x675c, 0x6760, 0x6764,
	0x6768, 0x676c, 0x6770, 0x6774, 0x6778, 0x677c, 0x6780, 0x6784, 0x6788, 0x678c, 0x6790, 0x6794,
	0x6798, 0x679c, 0x67a0, 0x67a4, 0x67a8, 0x67ac, 0x67b0, 0x67b4, 0x67b8, 0x67bc, 0x67c0, 0x67c4,
	0x67c8, 0x67cc, 0x67d0, 0x67d4, 0x67d8, 0x67dc, 0x67e0, 0x67e4, 0x67e8, 0x67ec, 0x67f0, 0x67f4,
	0x67f8, 0x67fc, 0x6800, 0x6804, 0x6808, 0x680c, 0x6810, 0x6814, 0x6818, 0x681c, 0x6820, 0x6824,
	0x6828, 0x682c, 0x6830, 0x6834, 0x6838, 0x683c, 0x6840, 0x6844, 0x6848, 0x684c, 0x6850, 0x6854,
	0x6858, 0x685c, 0x6860, 0x6864, 0x6868, 0x686c, 0x6870, 0x6874, 0x6878, 0x687c, 0x6880, 0x6884,
	0x6888, 0x688c, 0x6890, 0x6894, 0x6898, 0x689c, 0x68a0, 0x68a4, 0x68a8, 0x68ac, 0x68b0, 0x68b4,
	0x68b8, 0x68bc, 0x68c0, 0x68c4, 0x68c8, 0x68cc, 0x68d0, 0x68d4, 0x68d8, 0x68dc, 0x68e0, 0x68e4,
	0x68e8, 0x68ec, 0x68f0, 0x68f4, 0x6920, 0x6924, 0x6928, 0x692c, 0x6930, 0x6934, 0x6938, 0x693c,
	0x6940, 0x6944, 0x6948, 0x694c, 0x6950, 0x6954, 0x6958, 0x695c, 0x6960, 0x6964, 0x6968, 0x696c,
	0x6970, 0x6974, 0x7000, 0x7004, 0x7008, 0x700c, 0x7010, 0x7014, 0x7018, 0x701c, 0x7020, 0x7024,
	0x7028, 0x702c,
};

/* ------------------------------------------------------------------ */
/* `ethtool -d`: the same curated snapshot, through a standard interface */
/* ------------------------------------------------------------------ */

/*
 * Word order: the named registers (cortina_ni_rx_regs[]) first, then the
 * QM/L2TM block sweep (cortina_ni_qmdump_offs[]).  The length is derived from
 * the two tables.  Both halves are plain reads of NI-window offsets that are
 * mapped on this SoC, so a dump has no side effects.
 *
 * The order is the ABI of `ethtool -d` (version 2, see cortina-ni-ethtool.c):
 * appending is fine, reordering or removing needs a version bump.
 */
unsigned int cortina_ni_regdump_len(void)
{
	return ARRAY_SIZE(cortina_ni_rx_regs) +
	       ARRAY_SIZE(cortina_ni_qmdump_offs);
}

void cortina_ni_regdump_fill(struct cortina_ni *ni, u32 *buf)
{
	unsigned int i, n = 0;

	if (!ni_base(ni)) {
		memset(buf, 0, cortina_ni_regdump_len() * sizeof(*buf));
		return;
	}
	for (i = 0; i < ARRAY_SIZE(cortina_ni_rx_regs); i++)
		buf[n++] = readl(ni_base(ni) + cortina_ni_rx_regs[i].off);
	for (i = 0; i < ARRAY_SIZE(cortina_ni_qmdump_offs); i++)
		buf[n++] = readl(ni_base(ni) + cortina_ni_qmdump_offs[i]);
}

/*
 * Decode key for word @i of the dump above: its name and NI-window offset,
 * from the same tables as the dump.  The sweep half has no names.
 */
void cortina_ni_regdump_entry(unsigned int i, const char **name, u32 *off)
{
	if (i < ARRAY_SIZE(cortina_ni_rx_regs)) {
		*name = cortina_ni_rx_regs[i].name;
		*off = cortina_ni_rx_regs[i].off;
		return;
	}
	i -= ARRAY_SIZE(cortina_ni_rx_regs);
	if (i < ARRAY_SIZE(cortina_ni_qmdump_offs)) {
		*name = "qm_l2tm_sweep";
		*off = cortina_ni_qmdump_offs[i];
		return;
	}
	*name = "<out of range>";
	*off = 0;
}

/*
 * CPU-forwarding counters for both directions, printed identically into
 * /proc/net/cortina_ni_rx and /proc/net/cortina_ni_tx:
 *   us_data_enq (tx->pon_data_enq) - upstream LAN->WAN frames the CPU
 *                                    enqueued to the PON TX;
 *   ds_wan_l3 (rx->wan_l3_frames)  - downstream frames the HW-L3 engine
 *                                    missed and punted to the CPU
 *                                    (lspid = L3_WAN);
 *   ds_wan_pon (rx->wan_frames)    - downstream frames with lspid = PON:
 *                                    terminating traffic, plus every DS frame
 *                                    when HW-L3 forwarding is off.
 */
void cortina_ni_cpu_fwd_show(struct seq_file *m, struct cortina_ni *ni)
{
	struct cortina_ni_rx *rx = ni->rx;
	struct cortina_ni_tx *tx = ni->tx;

	seq_printf(m,
		   "cpu_fwd: us_data_enq=%llu [UPSTREAM only: LAN->WAN frames the CPU enqueued to PON TX] ds_wan_l3=%llu [DOWNSTREAM: HW-L3 miss punted to CPU] ds_wan_pon=%llu [DOWNSTREAM: lspid=PON, incl. terminating traffic] -- BOTH directions must stay FLAT to claim HW offload; us_data_enq alone proves nothing about DS\n",
		   tx ? tx->pon_data_enq : 0ULL,
		   rx ? rx->wan_l3_frames : 0ULL,
		   rx ? rx->wan_frames : 0ULL);
}
/* ------------------------------------------------------------------ */
/* probe                                                               */
/* ------------------------------------------------------------------ */

static int cortina_ni_rx_irqs_init(struct cortina_ni *ni)
{
	struct platform_device *pdev = to_platform_device(ni->dev);
	struct cortina_ni_rx *rx = ni->rx;
	int i, irq, ret, got = 0;

	/*
	 * DT lists the 8 per-cpu-port EPP interrupts first (SPI 0x54..0x5b);
	 * SPI 0x54 should be cpu port 0.  Request all 8 so a different
	 * ordering shows up as a nonzero irq_hits[i!=0] instead of silence
	 * (HW-verify #3); every handler drains the same port0/voq0 ring.
	 */
	for (i = 0; i < CA_NI_RX_NUM_IRQS; i++) {
		rx->irq[i] = -1;
		rx->irqctx[i].ni = ni;
		rx->irqctx[i].idx = i;

		irq = platform_get_irq_optional(pdev, i);
		if (irq < 0)
			continue;

		ret = devm_request_irq(ni->dev, irq, cortina_ni_rx_isr, 0,
				       devm_kasprintf(ni->dev, GFP_KERNEL,
						      "%s-rx%d",
						      dev_name(ni->dev), i),
				       &rx->irqctx[i]);
		if (ret) {
			dev_warn(ni->dev, "cannot request RX irq %d (#%d)\n",
				 irq, i);
			continue;
		}
		rx->irq[i] = irq;
		got++;
	}

	if (rx->irq[0] < 0) {
		dev_err(ni->dev, "RX interrupt 0 (SPI 0x54) unavailable\n");
		return -ENXIO;
	}
	dev_dbg(ni->dev, "RX: %d EPP interrupts requested (irq0=%d)\n",
		 got, rx->irq[0]);
	return 0;
}

int cortina_ni_rx_probe(struct cortina_ni *ni)
{
	struct cortina_ni_rx *rx;
	struct resource *res;
	int ret;

	if (!ni->tx || !ni->tx->netdev)
		return dev_err_probe(ni->dev, -ENODEV,
				     "RX needs the TX netdev first\n");

	rx = devm_kzalloc(ni->dev, sizeof(*rx), GFP_KERNEL);
	if (!rx)
		return -ENOMEM;
	rx->ni = ni;
	rx->netdev = ni->tx->netdev;
	WRITE_ONCE(cortina_ni_rx_dump_ni, ni);	/* open cortina_ni_rx_delivery_dump() */
	INIT_DELAYED_WORK(&rx->recovery_work, cortina_ni_rx_recovery_work);
	/* where a chain continuation's payload starts, resolved once */
	rx->chain_rest_off = CA_NI_RX_HDRA_OFF;
	ni->rx = rx;
	dev_dbg(ni->dev,
		 "RX: multi-buffer receive ON (chain payload window %u B/buffer, continuation at +0x%02x, max %u segs, max %u B)\n",
		 CA_NI_RX_BUF_USABLE_END(CA_NI_RX_CPU_POOL0_BUFSZ) -
		 CA_NI_RX_BUF_HEADROOM, rx->chain_rest_off,
		 CA_NI_RX_CHAIN_MAX_SEGS, CA_NI_RX_CHAIN_MAX_LEN);

	/*
	 * Save the GPHY calibration left by U-Boot and log the fault latch,
	 * so a wedge already present at probe shows in the boot log.
	 */
	cortina_ni_rx_gphy_cal_save(ni);
	dev_dbg(ni->dev, "RX: GPHY port %d fault latch 0x%04x at probe\n",
		 CA_NI_RX_PORT, cortina_ni_rx_gphy_fault(ni));

	/*
	 * The CPU-EPP descriptor ring lives at a fixed address in the reserved
	 * DDR region (QM register 0x7200, VoQ stride 0x400); the QM only
	 * writes there.  The NE DMA is not coherent, so map it write-combined
	 * (uncached) and read the hardware writeback directly.
	 */
	rx->ring_dma = CA_NI_RX_RING_PHYS;
	rx->ring = devm_memremap(ni->dev, CA_NI_RX_RING_PHYS,
				 CA_NI_RX_RING_TOTAL_BYTES, MEMREMAP_WC);
	if (IS_ERR_OR_NULL(rx->ring)) {
		dev_err(ni->dev, "RX ring memremap(%pa) failed\n",
			&rx->ring_dma);
		ni->rx = NULL;
		return -ENOMEM;
	}
	/*
	 * Fill the ring with the 0xdeadbeef sentinel before EPP/RMU0 are
	 * armed, as the vendor driver does.
	 */
	{
		u32 *r = (u32 *)rx->ring;
		unsigned int n;

		for (n = 0; n < CA_NI_RX_RING_TOTAL_BYTES / sizeof(u32); n++)
			r[n] = 0xDEADBEEFu;
		dma_wmb();
		dev_dbg(ni->dev, "RX ring: seeded 0x%x u32 = 0xDEADBEEF (uncached WC) @0x%08x\n",
			 (unsigned int)(CA_NI_RX_RING_TOTAL_BYTES / sizeof(u32)),
			 (u32)CA_NI_RX_RING_PHYS);
	}

	/*
	 * The CPU buffer pools also sit at a fixed address inside the NE's
	 * reserved DDR window, so every buffer address the RMU sees is inside
	 * the window.  The mapping covers both CPU pools and the deep-queue
	 * pool that follows them.
	 *
	 * The buffers are mapped cacheable: uncached reads limited receive to
	 * about 0.5 Gbit/s.  cortina_ni_rx_frame() invalidates a buffer's cache
	 * lines before reading it.
	 */
	rx->cpu_dram_dma = CA_NI_RX_CPU_POOL_PHYS;
	rx->cpu_dram = devm_memremap(ni->dev, CA_NI_RX_CPU_POOL_PHYS,
				     CA_NI_RX_MAP_SIZE, MEMREMAP_WB);
	if (IS_ERR_OR_NULL(rx->cpu_dram)) {
		dev_err(ni->dev, "RX: CPU-pool memremap(%pa) failed\n",
			&rx->cpu_dram_dma);
		ni->rx = NULL;
		return -ENOMEM;
	}
	dev_dbg(ni->dev,
		 "RX: CPU-pool DRAM @%pad size %u (reserved-window; %u CPU pools + %u deep-queue @0x%08x)\n",
		 &rx->cpu_dram_dma, CA_NI_RX_MAP_SIZE, CA_NI_RX_CPU_DRAM_SIZE,
		 CA_NI_RX_DQ_DRAM_SIZE, (u32)CA_NI_RX_DQ_POOL_PHYS);

	res = platform_get_resource_byname(to_platform_device(ni->dev),
					   IORESOURCE_MEM, "sram");
	if (res) {
		rx->dq_sram = devm_memremap(ni->dev, res->start,
					    resource_size(res), MEMREMAP_WC);
		if (IS_ERR(rx->dq_sram)) {
			dev_warn(ni->dev, "RX: NE SRAM %pR not mapped\n", res);
			rx->dq_sram = NULL;
		} else {
			rx->dq_sram_pa = res->start;
			rx->dq_sram_size = resource_size(res);
		}
	}

	/* U-Boot may have left the port 0 MAC RX on; keep it off until open */
	ni_rmw(ni, CA_NI_PORT_RXMAC_CFG(CA_NI_RX_PORT),
	       CA_NI_PORT_RXMAC_RX_EN, 0);

	ret = cortina_ni_rx_eq_init(ni);
	if (ret) {
		ni->rx = NULL;
		return ret;
	}
	cortina_ni_rx_epp_init(ni);
	dev_dbg(ni->dev, "RX: EQ pool + EPP ring init done\n");

	/*
	 * Enable the L3QM egress scheduler before RX, in the vendor order.  It
	 * lets queued descriptors reach the CPU-EPP ring; the per-port cpu_en
	 * stays off until open.
	 */
	cortina_ni_rx_es_enable(ni);

	/*
	 * L3QM RX on.  The engine must run before buffers are pushed; ingress
	 * cannot flow yet because the port 0 MAC RX is still off.
	 */
	ni_rmw(ni, CA_NI_QM_RMU0_CTRL, 0, CA_NI_QM_RMU0_RX_EN);

	/*
	 * L2TM to QM-RMU0 internal port handoff, in the vendor order.  Without
	 * it an upstream frame leaves the BM but no internal port delivers it:
	 * 0x6968 bit 1 releases the head-of-line packet into RMU0 (paired with
	 * 0xa1bc bit 20), 0xa330 low byte = 0x55 is the TM to NI FIFO threshold
	 * and 0x68f8 sets pon_en/pon_offset (downstream VoQ mapping).
	 */
	writel(0x00040302u, ni_base(ni) + CA_NI_QM_ES_CTRL2);
	/* bit 20 steers L3QM RX to the LAN */
	writel((readl(ni_base(ni) + 0xa330) & 0xff00u) | 0x55u,
	       ni_base(ni) + 0xa330);
	writel((readl(ni_base(ni) + CA_NI_QM_UPPER_LDPID_MAP) & ~0xfu) | 0xau,
	       ni_base(ni) + CA_NI_QM_UPPER_LDPID_MAP);
	dev_dbg(ni->dev,
		 "0x6968=0x%08x 0xa1bc=0x%08x 0xa330=0x%08x 0x68f8=0x%08x\n",
		 readl(ni_base(ni) + CA_NI_QM_ES_CTRL2),
		 readl(ni_base(ni) + CA_NI_NI_INTERNAL_PORT_ID_CFG),
		 readl(ni_base(ni) + 0xa330),
		 readl(ni_base(ni) + CA_NI_QM_UPPER_LDPID_MAP));

	cortina_ni_rx_eqm_readback(ni, "after RMU0 enable, pre-populate");

	/*
	 * Stage the software-owned pools now: the EQ config is committed and
	 * RMU0 runs, while the port 0 MAC RX is still off.
	 */
	ret = cortina_ni_rx_pp_init(ni);
	if (!ret)
		ret = cortina_ni_rx_push_seed(ni);
	if (ret) {
		ni->rx = NULL;
		return ret;
	}

	/* RMU AXI reorder engine; without it RMU dequeue DMA never completes */
	cortina_ni_rx_axi_reo_init(ni);

	/*
	 * The cpu_eq=0 pools populate themselves at the EQ_CFG_LOAD commit.
	 * Their pa_req/inactive counters must read 0 and stay 0.
	 */
	dev_dbg(ni->dev,
		 "RX pool self-populated (%u+%u DRAM bufs @0x%08x): eq5_pa_req=0x%08x eq6_pa_req=0x%08x wptr=0x%06x (want pa_req 0)\n",
		 CA_NI_RX_EQ_TOTAL_BUF, CA_NI_RX_EQ2_TOTAL_BUF,
		 (u32)CA_NI_RX_CPU_POOL_PHYS,
		 readl(ni_base(ni) + CA_NI_QM_EQM_PA_REQ(CA_NI_RX_EQ_ID)),
		 readl(ni_base(ni) + CA_NI_QM_EQM_PA_REQ(CA_NI_RX_EQ_ID2)),
		 cortina_ni_rx_wptr(ni));
	cortina_ni_rx_eqm_readback(ni, "self-populating pools (pa_req should stay 0)");

	/*
	 * Clear the latched eqm_cfg_error (QM_INT_SRC bit 22, write 1 to
	 * clear).  0x6120 is the enable mask, not a status latch.
	 */
	writel(BIT(22), ni_base(ni) + 0x611c);
	dev_dbg(ni->dev, "eqm_cfg_error W1C: int_src 0x611c=0x%08x en_mask 0x6120=0x%08x (want 0x611c bit22 CLEAR, 0x6120=0xe6d54f85)\n",
		 readl(ni_base(ni) + 0x611c), readl(ni_base(ni) + 0x6120));

	ret = cortina_ni_rx_steer_init(ni);
	if (ret) {
		/* steer failed: RX can't deliver, but don't take TX down */
		dev_err(ni->dev, "RX steer init failed (%d) - staying TX-only\n",
			ret);
		ni_rmw(ni, CA_NI_QM_RMU0_CTRL, CA_NI_QM_RMU0_RX_EN, 0);
		ni->rx = NULL;
		return 0;
	}
	dev_dbg(ni->dev, "RX: steer done\n");

	spin_lock_init(&rx->recycle_lock);
	netif_napi_add(rx->netdev, &rx->napi, cortina_ni_rx_poll);
	netif_napi_add(rx->netdev, &rx->napi_wan, cortina_ni_rx_poll_wan);

	ret = cortina_ni_rx_irqs_init(ni);
	if (ret) {
		/* no RX IRQ: keep TX alive rather than fail the whole device */
		dev_err(ni->dev, "RX irq init failed (%d) - staying TX-only\n",
			ret);
		netif_napi_del(&rx->napi_wan);
		netif_napi_del(&rx->napi);
		ni_rmw(ni, CA_NI_QM_RMU0_CTRL, CA_NI_QM_RMU0_RX_EN, 0);
		ni->rx = NULL;
		return 0;
	}

	/* final RX path configuration (MAC rx_en is set at open) */
	dev_dbg(ni->dev,
		 "RX ready: port %d -> cpu0/voq0, %u DRAM buffers\n",
		 CA_NI_RX_PORT, CA_NI_RX_EQ_TOTAL_BUF + CA_NI_RX_EQ2_TOTAL_BUF);
	dev_dbg(ni->dev,
		 "RX golden: static_cfg(a5c0)=0x%08x rx_cntrl=0x%08x rxmac=0x%08x es_ctrl=0x%08x l3tm=0x%08x\n",
		 readl(ni_base(ni) + CA_NI_PORT_STATIC_CFG(CA_NI_RX_PORT)),
		 readl(ni_base(ni) + CA_NI_PORT_RX_CNTRL_CFG(CA_NI_RX_PORT)),
		 readl(ni_base(ni) + CA_NI_PORT_RXMAC_CFG(CA_NI_RX_PORT)),
		 readl(ni_base(ni) + CA_NI_QM_ES_CTRL),
		 readl(ni_base(ni) + CA_NI_QM_L3TM_NI_PORT_ENA));
	dev_dbg(ni->dev,
		 "RX golden demux: a190=0x%08x a194=0x%08x a19c=0x%08x a1a0=0x%08x\n",
		 readl(ni_base(ni) + CA_NI_NIRX_L3FE_DEMUX0),
		 readl(ni_base(ni) + CA_NI_NIRX_L3FE_DEMUX1),
		 readl(ni_base(ni) + CA_NI_NIRX_L3FE_DEMUX_NORM1),
		 readl(ni_base(ni) + CA_NI_NIRX_L3FE_DEMUX_NORM2));
	dev_dbg(ni->dev,
		 "RX delivery chain: destp9(61a4)=0x%08x prof13(615c)=0x%08x eq13_cfg1(6350)=0x%08x eq13_cfg2(6354)=0x%08x pkt_buf(6228)=0x%08x\n",
		 readl(ni_base(ni) + CA_NI_QM_DEST_PORT_EQ_CFG(CA_NI_RX_CPU_DEST_PORT)),
		 readl(ni_base(ni) + CA_NI_QM_EQ_PROFILE(CA_NI_RX_EQ_PROFILE)),
		 readl(ni_base(ni) + CA_NI_QM_CFG1_EQ(CA_NI_RX_EQ_ID)),
		 readl(ni_base(ni) + CA_NI_QM_CFG2_EQ(CA_NI_RX_EQ_ID)),
		 readl(ni_base(ni) + CA_NI_QM_DEST_PORT_PKT_BUF_CFG(CA_NI_RX_CPU_PORT)));
	dev_dbg(ni->dev,
		 "RX misc match: intern_portid(a1bc)=0x%08x autosync(a010)=0x%08x l2tm_glob(2210)=0x%08x\n",
		 readl(ni_base(ni) + CA_NI_NI_INTERNAL_PORT_ID_CFG),
		 readl(ni_base(ni) + CA_NI_HV_MAC_AUTOSYNC),
		 readl(ni_base(ni) + CA_NI_L2TM_QM_GLOB_BUF_CFG));
	return 0;
}
