// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) BRULE Herman <alpha_one_x86@first-world.info> (github.com/alphaonex86)
 *
 * Taken from https://github.com/alphaonex86/openwrt.
 *
 * ethtool support for the Cortina NI on the Realtek RTL9607DQ: driver and
 * hardware counters for "ethtool -S", and an NI register snapshot for
 * "ethtool -d" whose layout is published in debugfs as regdump_map.
 *
 * Only registers that exist on this SoC are read. Several NI_HV and QM
 * offsets of the generic Cortina map are unmapped here and fault the bus
 * (0xa9bc, 0xa9f4, 0xa9f8, 0xa9fc, 0xaa10); they must never be added. All
 * offsets come from cortina-ni-regs.h symbols, never literals.
 */

#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/ethtool.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/phy.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/stddef.h>
#include <linux/string.h>

#include "cortina-ni.h"
#include "cortina-ni-regs.h"

static inline void __iomem *ni_base(struct cortina_ni *ni)
{
	return ni->win[CA_NI_WIN_NI];
}

/*
 * The NI_HV internal-port packet counters, in enum cortina_ni_nihv_cnt
 * order. They are read-and-clear, so they may have only one reader; see
 * cortina_ni_nihv_sample() in cortina-ni.h.
 */
static const u32 cortina_ni_nihv_off[CA_NI_NIHV_CNT_COUNT] = {
	[CA_NI_NIHV_INTPT_RX]		= CA_NI_NI_INTPT_RX_PKT_CNT,	/* 0xa92c */
	[CA_NI_NIHV_INTPT_MISS_SOP_EOP]	= CA_NI_NI_INTPT_RX_MISS_SOP_EOP, /* 0xa924 */
	[CA_NI_NIHV_INTPT_SHORT_ERR]	= CA_NI_NI_INTPT_RX_SHORT_ERR,	/* 0xa928 */
	[CA_NI_NIHV_INTPT_TX]		= CA_NI_NI_INTPT_TX_PKT_CNT,	/* 0xa940 */
	[CA_NI_NIHV_XRAM_DMA]		= CA_NI_NI_XRAM_DMA_PKT_CNT,	/* 0xaa4c */
};

void cortina_ni_nihv_sample(struct cortina_ni *ni,
			    u64 out[CA_NI_NIHV_CNT_COUNT])
{
	void __iomem *base = ni_base(ni);
	unsigned long flags;
	unsigned int i;

	if (!base) {
		memset(out, 0, sizeof(*out) * CA_NI_NIHV_CNT_COUNT);
		return;
	}

	/*
	 * Read and accumulate under one lock, so concurrent readers never
	 * publish totals built from different slices of the same count.
	 */
	spin_lock_irqsave(&ni->nihv_lock, flags);
	for (i = 0; i < CA_NI_NIHV_CNT_COUNT; i++) {
		ni->nihv_total[i] += readl(base + cortina_ni_nihv_off[i]);
		out[i] = ni->nihv_total[i];
	}
	spin_unlock_irqrestore(&ni->nihv_lock, flags);
}

enum ca_ni_stat_src {
	CA_ST_RX_U64,		/* u64 at @arg bytes into struct cortina_ni_rx */
	CA_ST_TX_U64,		/* u64 at @arg bytes into struct cortina_ni_tx */
	CA_ST_NI_REG,		/* plain cumulative register at NI + @arg      */
	CA_ST_NIHV,		/* index @arg into the read-and-clear totals   */
	CA_ST_DRV_FLAG,		/* a driver state bit, @arg selects which      */
	CA_ST_L3FE,		/* index @arg into the offload snapshot        */
};

/* CA_ST_DRV_FLAG selectors */
#define CA_ST_FLAG_RX_UP	0

/*
 * A row may stand for a family of counters: @n repeats, with the index
 * substituted into @fmt's %u and added to @arg (scaled by @step).
 * get_strings() and get_ethtool_stats() walk this one table in the same
 * order, so names and values cannot drift apart.
 *
 * Every name must fit ETH_GSTRING_LEN including the index and the NUL.
 */
struct ca_ni_stat_grp {
	const char		*fmt;
	u16			n;
	enum ca_ni_stat_src	src;
	u32			arg;
	u32			step;
};

#define S_RX(f)		((u32)offsetof(struct cortina_ni_rx, f))
#define S_TX(f)		((u32)offsetof(struct cortina_ni_tx, f))

static const struct ca_ni_stat_grp cortina_ni_stat_grps[] = {
	/* ---- receive, the driver's own software counters ---------------- */
	{ "rx_datapath_up",		1, CA_ST_DRV_FLAG, CA_ST_FLAG_RX_UP },
	{ "rx_frames",			1, CA_ST_RX_U64, S_RX(frames) },
	{ "rx_bytes",			1, CA_ST_RX_U64, S_RX(bytes) },
	{ "rx_napi_polls",		1, CA_ST_RX_U64, S_RX(polls) },
	{ "rx_headerless_frames",	1, CA_ST_RX_U64, S_RX(swid_frames) },
	{ "rx_l4_csum_err",		1, CA_ST_RX_U64, S_RX(csum_err) },
	{ "rx_csum_unnecessary",	1, CA_ST_RX_U64, S_RX(csum_ok) },
	{ "rx_zero_copy",		1, CA_ST_RX_U64, S_RX(zc_frames) },
	{ "rx_pon_omci_frames",		1, CA_ST_RX_U64, S_RX(pon_frames) },
	{ "rx_pon_wan_frames",		1, CA_ST_RX_U64, S_RX(wan_frames) },
	{ "rx_pon_wan_l3_miss_frames",	1, CA_ST_RX_U64, S_RX(wan_l3_frames) },
	{ "rx_deepq_frames",		1, CA_ST_RX_U64, S_RX(dq_frames) },
	{ "rx_deepq_sram_segs",		1, CA_ST_RX_U64, S_RX(dq_sram_segs) },
	{ "rx_drop_no_sop",		1, CA_ST_RX_U64, S_RX(drop_nosop) },
	{ "rx_drop_bad_paddr",		1, CA_ST_RX_U64, S_RX(drop_badpa) },
	{ "rx_drop_bad_len",		1, CA_ST_RX_U64, S_RX(drop_len) },
	{ "rx_drop_runt",		1, CA_ST_RX_U64, S_RX(drop_runt) },
	{ "rx_drop_oversize",		1, CA_ST_RX_U64, S_RX(drop_oversize) },
	{ "rx_drop_no_buffer",		1, CA_ST_RX_U64, S_RX(drop_nobuf) },
	{ "rx_stale_buffer",		1, CA_ST_RX_U64, S_RX(stale_buf) },
	{ "rx_pool_push_fail",		1, CA_ST_RX_U64, S_RX(push_fail) },
	{ "rx_gphy_recoveries",		1, CA_ST_RX_U64, S_RX(recoveries) },
	{ "rx_link_rearms",		1, CA_ST_RX_U64, S_RX(rearms) },
	/*
	 * A flow should stay on one VoQ; several climbing during a single-flow run
	 * means the hardware spread it and the drain order may reorder it.
	 */
	{ "rx_voq%u_frames",	CA_NI_RX_VOQ_COUNT, CA_ST_RX_U64,
	  S_RX(voq_frames), sizeof(u64) },
	/* EPP interrupt index, not the Linux IRQ number */
	{ "rx_epp_irq%u_events", CA_NI_RX_NUM_IRQS, CA_ST_RX_U64,
	  S_RX(irq_hits), sizeof(u64) },
	{ "rx_chain_frames",		1, CA_ST_RX_U64, S_RX(chain_frames) },
	{ "rx_chain_segments",		1, CA_ST_RX_U64, S_RX(chain_segs) },
	/* a high-water mark, not a count */
	{ "rx_chain_max_segments",	1, CA_ST_RX_U64, S_RX(chain_max_segs) },
	{ "rx_chain_abort",		1, CA_ST_RX_U64, S_RX(chain_abort) },
	{ "rx_chain_reopen",		1, CA_ST_RX_U64, S_RX(chain_reopen) },
	{ "rx_chain_orphan",		1, CA_ST_RX_U64, S_RX(chain_orphan) },
	{ "rx_chain_bad_total",		1, CA_ST_RX_U64, S_RX(chain_badtotal) },
	{ "rx_chain_too_long",		1, CA_ST_RX_U64, S_RX(chain_toolong) },
	{ "rx_chain_short",		1, CA_ST_RX_U64, S_RX(chain_short) },
	{ "rx_chain_headerless",	1, CA_ST_RX_U64, S_RX(chain_swid) },
	{ "rx_chain_dlen_mismatch",	1, CA_ST_RX_U64, S_RX(chain_dlen_diff) },

	/* ---- transmit, the driver's own software counters ---------------- */
	{ "tx_drop_no_dma_map",		1, CA_ST_TX_U64, S_TX(drop_nomap) },
	{ "tx_drop_linearize",		1, CA_ST_TX_U64, S_TX(drop_linearize) },
	{ "tx_drop_oversize",		1, CA_ST_TX_U64, S_TX(drop_oversize) },
	{ "tx_ring_busy",		1, CA_ST_TX_U64, S_TX(tx_busy) },
	{ "tx_paced",			1, CA_ST_TX_U64, S_TX(tx_paced) },
	{ "tx_lan_flood_copies",	1, CA_ST_TX_U64, S_TX(lan_dup) },
	{ "tx_pon_omci_frames",		1, CA_ST_TX_U64, S_TX(pon_enq) },
	{ "tx_pon_omci_fail",		1, CA_ST_TX_U64, S_TX(pon_fail) },
	{ "tx_pon_wan_frames",		1, CA_ST_TX_U64, S_TX(pon_data_enq) },
	/* these stride the txq[] array */
	{ "tx_vp%u_enqueued",	CA_NI_TX_NUM_VPS, CA_ST_TX_U64,
	  S_TX(txq[0].enq), sizeof(struct cortina_ni_txq) },
	{ "tx_vp%u_reclaimed",	CA_NI_TX_NUM_VPS, CA_ST_TX_U64,
	  S_TX(txq[0].reclaimed), sizeof(struct cortina_ni_txq) },

	/*
	 * NI internal port counters: totals since boot, accumulated by
	 * cortina_ni_nihv_sample() from the read-and-clear registers.
	 */
	{ "ni_intpt_rx_packets",	1, CA_ST_NIHV, CA_NI_NIHV_INTPT_RX },
	{ "ni_intpt_rx_miss_sop_eop",	1, CA_ST_NIHV,
	  CA_NI_NIHV_INTPT_MISS_SOP_EOP },
	{ "ni_intpt_rx_short_err",	1, CA_ST_NIHV,
	  CA_NI_NIHV_INTPT_SHORT_ERR },
	{ "ni_intpt_tx_packets",	1, CA_ST_NIHV, CA_NI_NIHV_INTPT_TX },
	{ "ni_xram_dma_packets",	1, CA_ST_NIHV, CA_NI_NIHV_XRAM_DMA },

	/*
	 * Engine counters in datapath order: L2FE -> L2TM buffer manager ->
	 * QM/RMU. They are free-running 32-bit registers and wrap.
	 */
	/*
	 * The two L2FE registers have not been seen to count under known traffic,
	 * so they are published as raw values, not as drop counters.
	 */
	{ "l2fe_ni_intf_drop_raw",	1, CA_ST_NI_REG,
	  CA_NI_L2FE_NI_INTF_DROP_CNT },
	{ "l2fe_dos_flood_raw",		1, CA_ST_NI_REG,
	  CA_NI_L2FE_DOS_FLOOD_CNT },
	{ "l2tm_bm_rx_packets",		1, CA_ST_NI_REG, CA_NI_L2TM_BM_RX_PCNT },
	{ "l2tm_bm_tx_packets",		1, CA_ST_NI_REG, CA_NI_L2TM_BM_TX_PCNT },
	{ "l2tm_bm_drop_shared_buffer",	1, CA_ST_NI_REG, CA_NI_L2TM_BM_SB_DPCNT },
	{ "l2tm_bm_drop_header",	1, CA_ST_NI_REG, CA_NI_L2TM_BM_HDR_DPCNT },
	{ "l2tm_bm_drop_threshold",	1, CA_ST_NI_REG, CA_NI_L2TM_BM_TE_DPCNT },
	{ "l2tm_bm_drop_error",		1, CA_ST_NI_REG, CA_NI_L2TM_BM_ERR_DPCNT },
	{ "l2tm_bm_drop_enqueue",	1, CA_ST_NI_REG, CA_NI_L2TM_BM_RX_DPCNT },
	{ "l2tm_bm_drop_no_buffer",	1, CA_ST_NI_REG, CA_NI_L2TM_BM_NOBUF_DPCNT },
	{ "qm_rx_packets",		1, CA_ST_NI_REG, CA_NI_QM_RX_CNTR },
	{ "qm_tx_packets",		1, CA_ST_NI_REG, CA_NI_QM_TX_CNTR },
	{ "qm_drop_no_buffer",		1, CA_ST_NI_REG, CA_NI_QM_RMU_NO_BUF_DROP },
	{ "qm_drop_fe",			1, CA_ST_NI_REG, CA_NI_QM_RMU_FE_DROP },
	{ "qm_drop_rx_eop",		1, CA_ST_NI_REG, CA_NI_QM_RX_EOP_DROP_CNTR },
	{ "qm_drop_rx_len_err",		1, CA_ST_NI_REG, CA_NI_QM_RX_LEN_ERR_CNTR },
	{ "qm_drop_rx_l2te",		1, CA_ST_NI_REG, CA_NI_QM_RX_L2TE_DROP_CNTR },

#if IS_ENABLED(CONFIG_CORTINA_NI_FLOWOFFLOAD)
	/* L3FE flow offload, only when the engine is built in */
	{ "l3fe_flows_resident",	1, CA_ST_L3FE, CA_L3FE_FLOWS_RESIDENT },
	{ "l3fe_ds_flows_resident",	1, CA_ST_L3FE, CA_L3FE_DS_FLOWS_RESIDENT },
	{ "l3fe_us_hits",		1, CA_ST_L3FE, CA_L3FE_US_HITS },
	{ "l3fe_ds_hits",		1, CA_ST_L3FE, CA_L3FE_DS_HITS },
	{ "l3fe_flows_refused",		1, CA_ST_L3FE, CA_L3FE_FLOWS_REFUSED },
	{ "l3fe_refused_unsupported",	1, CA_ST_L3FE, CA_L3FE_REFUSED_UNSUPPORTED },
	{ "l3fe_refused_table_full",	1, CA_ST_L3FE, CA_L3FE_REFUSED_TABLE_FULL },
	{ "l3fe_refused_duplicate",	1, CA_ST_L3FE, CA_L3FE_REFUSED_DUPLICATE },
	{ "l3fe_refused_error",		1, CA_ST_L3FE, CA_L3FE_REFUSED_ERROR },
#endif
};

/*
 * One sample of each shared source per "ethtool -S", so a read-and-clear
 * register is read once per call, not once per row.
 */
struct ca_ni_stat_ctx {
	u64	nihv[CA_NI_NIHV_CNT_COUNT];
#if IS_ENABLED(CONFIG_CORTINA_NI_FLOWOFFLOAD)
	u64	l3fe[CA_L3FE_STAT_COUNT];
#endif
};

static u64 ca_ni_stat_value(struct cortina_ni *ni,
			    const struct ca_ni_stat_grp *g, unsigned int i,
			    const struct ca_ni_stat_ctx *ctx)
{
	switch (g->src) {
	case CA_ST_RX_U64:
		if (!ni->rx)
			return 0;	/* rx_datapath_up says why */
		return *(const u64 *)((const u8 *)ni->rx + g->arg +
				      (size_t)i * g->step);
	case CA_ST_TX_U64:
		if (!ni->tx)
			return 0;
		return *(const u64 *)((const u8 *)ni->tx + g->arg +
				      (size_t)i * g->step);
	case CA_ST_NI_REG:
		if (!ni_base(ni))
			return 0;
		return readl(ni_base(ni) + g->arg + (size_t)i * g->step);
	case CA_ST_NIHV:
		return ctx->nihv[g->arg + i];
	case CA_ST_DRV_FLAG:
		switch (g->arg) {
		case CA_ST_FLAG_RX_UP:
			return ni->rx ? 1 : 0;
		}
		return 0;
#if IS_ENABLED(CONFIG_CORTINA_NI_FLOWOFFLOAD)
	case CA_ST_L3FE:
		return ctx->l3fe[g->arg + i];
#endif
	default:
		return 0;
	}
}

/*
 * The netdev private area is a single struct cortina_ni pointer. Do not
 * use these ops on the GPON WAN netdev, whose private area is empty.
 */
static struct cortina_ni *cortina_ni_of_netdev(struct net_device *dev)
{
	return *(struct cortina_ni **)netdev_priv(dev);
}

static int cortina_ni_get_sset_count(struct net_device *dev, int sset)
{
	unsigned int i, n = 0;

	if (sset != ETH_SS_STATS)
		return -EOPNOTSUPP;

	for (i = 0; i < ARRAY_SIZE(cortina_ni_stat_grps); i++)
		n += cortina_ni_stat_grps[i].n;
	return n;
}

static void cortina_ni_get_strings(struct net_device *dev, u32 sset, u8 *data)
{
	unsigned int g, i;

	if (sset != ETH_SS_STATS)
		return;

	for (g = 0; g < ARRAY_SIZE(cortina_ni_stat_grps); g++) {
		const struct ca_ni_stat_grp *grp = &cortina_ni_stat_grps[g];

		for (i = 0; i < grp->n; i++) {
			/* formats without %u ignore the index */
			snprintf((char *)data, ETH_GSTRING_LEN, grp->fmt, i);
			data += ETH_GSTRING_LEN;
		}
	}
}

static void cortina_ni_get_ethtool_stats(struct net_device *dev,
					 struct ethtool_stats *stats, u64 *data)
{
	struct cortina_ni *ni = cortina_ni_of_netdev(dev);
	struct ca_ni_stat_ctx ctx;
	unsigned int g, i, n = 0;

	cortina_ni_nihv_sample(ni, ctx.nihv);
#if IS_ENABLED(CONFIG_CORTINA_NI_FLOWOFFLOAD)
	cortina_ppe_stats(ni->ppe, ctx.l3fe);
#endif

	for (g = 0; g < ARRAY_SIZE(cortina_ni_stat_grps); g++) {
		const struct ca_ni_stat_grp *grp = &cortina_ni_stat_grps[g];

		for (i = 0; i < grp->n; i++)
			data[n++] = ca_ni_stat_value(ni, grp, i, &ctx);
	}
}

/*
 * "ethtool -d": a flat u32 array of NI register values. The name and
 * offset of each word are in debugfs cortina-ni/regdump_map, generated
 * from the same tables.
 *
 * Version 2: the word order is the ABI. Appending is fine; reordering or
 * removing words needs a new version.
 */
#define CA_NI_REGDUMP_VERSION	2

static int cortina_ni_get_regs_len(struct net_device *dev)
{
	return (int)(cortina_ni_regdump_len() * sizeof(u32));
}

static void cortina_ni_get_regs(struct net_device *dev,
				struct ethtool_regs *regs, void *p)
{
	regs->version = CA_NI_REGDUMP_VERSION;
	cortina_ni_regdump_fill(cortina_ni_of_netdev(dev), p);
}

static void cortina_ni_get_drvinfo(struct net_device *dev,
				   struct ethtool_drvinfo *info)
{
	strscpy(info->driver, CA_NI_DRV_NAME, sizeof(info->driver));
	strscpy(info->bus_info, dev_name(cortina_ni_of_netdev(dev)->dev),
		sizeof(info->bus_info));
}

const struct ethtool_ops cortina_ni_ethtool_ops = {
	.get_drvinfo		= cortina_ni_get_drvinfo,
	.get_link		= ethtool_op_get_link,
	.get_link_ksettings	= phy_ethtool_get_link_ksettings,
	.get_sset_count		= cortina_ni_get_sset_count,
	.get_strings		= cortina_ni_get_strings,
	.get_ethtool_stats	= cortina_ni_get_ethtool_stats,
	.get_regs_len		= cortina_ni_get_regs_len,
	.get_regs		= cortina_ni_get_regs,
};

/*
 * Decode key for "ethtool -d": one line per word with index, name and NI-
 * window offset. No register access.
 */
static int cortina_ni_dbgfs_regmap_show(struct seq_file *m, void *v)
{
	unsigned int i, n = cortina_ni_regdump_len();

	seq_printf(m, "# ethtool -d words: %u (u32 each, NI window)\n", n);
	seq_printf(m, "# regdump version: %u\n", CA_NI_REGDUMP_VERSION);
	seq_puts(m, "# index name offset\n");
	for (i = 0; i < n; i++) {
		const char *name;
		u32 off;

		cortina_ni_regdump_entry(i, &name, &off);
		seq_printf(m, "%u %s 0x%04x\n", i, name, off);
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(cortina_ni_dbgfs_regmap);

static void cortina_ni_debugfs_release(void *data)
{
	debugfs_remove_recursive(data);
}

void cortina_ni_debugfs_init(struct cortina_ni *ni)
{
	struct dentry *d;

	/*
	 * With debugfs off this returns an ERR_PTR, which must not be handed to
	 * devm.
	 */
	d = debugfs_create_dir(CA_NI_DRV_NAME, NULL);
	if (IS_ERR_OR_NULL(d))
		return;
	ni->dbgfs = d;

	/* tie teardown to the device so a module unload removes the dentry */
	if (devm_add_action_or_reset(ni->dev, cortina_ni_debugfs_release, d)) {
		ni->dbgfs = NULL;
		return;
	}

	debugfs_create_file("regdump_map", 0444, d, ni,
			    &cortina_ni_dbgfs_regmap_fops);
	cortina_ppe_debugfs_init(ni->ppe, d);
}
