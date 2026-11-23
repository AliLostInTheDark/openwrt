// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 *
 * IEEE 802.3ah 1G EPON MAC, MPCP, FEC, churning crypto, Dying Gasp, and
 * OAM transport driver for the Realtek RTL9607DQ (Cortina Taurus) SoC.
 *
 * Integrates with the in-kernel xPON/OAM subsystem (net/xpon, net/xpon/oam)
 * and delivers IEEE 802.3ah OAM frames (0x8809 / subtype 0x03) to both
 * net/xpon/oam and userspace OAM daemons (epon-oamd) over the pon0 netdev.
 */

#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/etherdevice.h>
#include <linux/gpio/consumer.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/seq_file.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>
#include <net/xpon.h>
#include <net/xpon/oam.h>

#include "cortina-epon.h"
#include "cortina-epon-serdes.h"
#include "cortina-gpon-bosa.h"
#include "cortina-ni.h"

/* GLB reset / clock / optical status registers (relative to ce->glb) */
#define CE_GLB_EPON_CNTL		0x0074
#define   CE_GLB_EPON_RST_MASK		(GENMASK(3, 0) | GENMASK(7, 5))
#define   CE_GLB_EPON_ONU_MODE		GENMASK(17, 16)
#define   CE_GLB_EPON_ONU_1G		1
#define   CE_GLB_EPON_XG_SEL		BIT(18)
#define CE_GLB_GPON_CNTL		0x007c
#define CE_GLB_OPT_STATUS		0x0090
#define   CE_OPT_RX_LOS			BIT(0)
#define   CE_OPT_TX_FAULT		BIT(1)
#define   CE_OPT_MOD_ABS		BIT(2)
#define   CE_OPT_TX_DIS			BIT(3)
#define CE_GLB_PON_CNTL			0x0094
#define   CE_PON_CNTL_EPON_SEL		BIT(0)
#define   CE_PON_CNTL_SDS_RST_N		BIT(1)
#define   CE_PON_CNTL_PSDS_REG_RST_N	BIT(2)
#define   CE_PON_CNTL_EPON_RELEASE	0x0000030f
#define CE_GLB_PSDS_INIT		0x022c
#define   CE_PSDS_BEN_OEN		BIT(4)
#define   CE_PSDS_POW_PCIX		BIT(5)

/* PON SerDes registers (relative to ce->pon) */
#define CE_PSDS_MODE			0xa02c
#define   CE_PSDS_MODE_EPON_1G		0x0000040c
#define CE_PSDS_RGB8			0xa060
#define   CE_PSDS_RX_LOCK_MASK		0x9c01
#define   CE_PSDS_RX_LOCK_VAL		0x9c00
#define CE_PSDS_GBOX_CTRL		0xa064

/* EPON global & LLID table registers (relative to ce->pon) */
#define CE_EPON_GLB_IRQ			0x0000
#define CE_EPON_GLB_IE			0x0004
#define CE_EPON_STATUS			0x0010
#define   CE_EPON_STS_LOS		BIT(0)
#define   CE_EPON_STS_XG_SYNC		BIT(1)
#define   CE_EPON_STS_1G_LINK		BIT(2)
#define   CE_EPON_STS_PCS_SYNC		BIT(3)
#define CE_EPON_STATIC			0x0014
#define   CE_EPON_STATIC_1G_TOGGLE	BIT(0)
#define   CE_EPON_STATIC_LASER_ON	BIT(1)
#define CE_EPON_MAC_HI			0x0018
#define CE_EPON_MAC_LO			0x001c
#define CE_EPON_LLID_TBL(i)		(0x0064 + (i) * 4)
#define   CE_EPON_LLID_TBL_SIZE		64
#define   CE_LLID_TBL_LLID		GENMASK(15, 0)
#define   CE_LLID_TBL_IDX		GENMASK(21, 16)
#define   CE_LLID_TBL_MC		BIT(22)
#define   CE_LLID_TBL_VALID		BIT(23)
#define   CE_LLID_TBL_MAC		GENMASK(28, 24)
#define CE_EPON_LLID_DATA		0x0164
#define CE_EPON_LLID_ACCESS		0x0168
#define CE_EPON_LLID_RESULT		0x016c
#define CE_EPON_LLID_BC_MAP		0x0170
#define   CE_EPON_BC_LLID_IDX		0x3f
#define CE_EPON_MISC			0x0180
#define   CE_EPON_MISC_LASER_EN		BIT(1)
#define   CE_EPON_MISC_LASER_OFF	BIT(4)
#define   CE_EPON_MISC_FEC_RX_MODE	GENMASK(20, 19)
#define CE_EPON_SW_TX_HDR		0x0184
#define CE_EPON_SW_TX_ACCESS		0x0188
#define CE_EPON_SW_TX_DATA0		0x018c
#define CE_EPON_SW_TX_DATA1		0x0190
#define CE_EPON_SW_TX_CTRL		0x0194
#define CE_EPON_1G_IRQ			0x01a0
#define CE_EPON_1G_IE			0x01a4
#define CE_EPON_SPARE			0x01c8

/* EPON MIB RAM indirect registers */
#define CE_EPON_MIB_STATUS		0x0600
#define CE_EPON_MIB_CTRL		0x0604
#define CE_EPON_MIB_RX_MPCP_ACC		0x0608
#define CE_EPON_MIB_RX_MPCP_DAT		0x060c
#define CE_EPON_MIB_RX_ETH_ACC		0x0618
#define CE_EPON_MIB_RX_ETH_DAT		0x061c
#define CE_EPON_MIB_TX_MPCP_ACC		0x0620
#define CE_EPON_MIB_TX_MPCP_DAT		0x0624
#define CE_EPON_MIB_TX_ETH_ACC		0x0630
#define CE_EPON_MIB_TX_ETH_DAT		0x0634

/* 1G EPON FEC, MAC & churning crypto registers */
#define CE_EPON_1G_FEC_CFG		0x0810
#define   CE_FEC_1G_RX_EN		BIT(0)
#define   CE_FEC_1G_TX_EN		BIT(1)
#define CE_EPON_1G_FEC_CORR_BLK		0x0820
#define CE_EPON_1G_FEC_UNCORR_BLK	0x0824
#define CE_EPON_1G_FEC_GOOD_BLK		0x0828
#define CE_EPON_1G_FEC_TOTAL_BLK	0x082c
#define CE_EPON_1G_FEC_CORR_BYTES	0x0830
#define CE_EPON_1G_TX_CFG		0x0834
#define   CE_EPON_1G_TX_EN		BIT(0)
#define CE_EPON_1G_MAX_PKT		0x0838
#define CE_EPON_1G_RX_CFG		0x088c
#define   CE_EPON_1G_RX_EN		BIT(0)
#define CE_EPON_1G_CRYPTO_CTRL0		0x08a8
#define CE_EPON_1G_CRYPTO_MODE		0x08b4
#define CE_EPON_1G_CRYPTO_KEY(slot)	(0x08b8 + (slot) * 16)
#define CE_EPON_1G_CRYPTO_CFG		0x093c
#define CE_EPON_1G_CRYPTO_CFG2		0x0940
#define CE_EPON_1G_CRYPTO_LLID(ch)	(0x095c + (ch) * 4)

/* RTT, XG/1G common MAC, PCS & drop control */
#define CE_EPON_RTT_RX			0x0f08
#define CE_EPON_RTT_TX			0x0f0c
#define CE_EPON_XG_TX_CFG		0x1004
#define CE_EPON_XG_RX_CFG		0x10b4
#define CE_EPON_RX_LOCALTIME		0x10bc
#define CE_EPON_RX_DROP			0x1250
#define CE_EPON_RX_MPCP_MIB		0x1288
#define CE_EPON_TX_MPCP_MIB		0x12a0
#define CE_EPON_PCS_TX			0x130c
#define CE_EPON_PCS_RX			0x137c
#define CE_EPON_LINK_DETECT		0x13cc

/* MPCP engine registers */
#define CE_MPCP_IRQ			0x2000
#define CE_MPCP_IE			0x2004
#define   CE_MPCP_EV_MASK		(BIT(0) | BIT(1) | BIT(2) | BIT(3) | \
					 BIT(6) | BIT(14))
#define CE_MPCP_INIT			0x2008
#define CE_MPCP_START			0x200c
#define CE_MPCP_REG_STATUS_HI		0x2018
#define CE_MPCP_REG_STATUS_LO		0x201c
#define CE_MPCP_CFG0			0x2020
#define CE_MPCP_CFG			0x2024
#define   CE_MPCP_CFG_REG_EN		BIT(31)
#define   CE_MPCP_CFG_MLLID_EN		BIT(30)
#define CE_MPCP_1G_CFG			0x2028
#define CE_MPCP_LASER_TIME		0x203c
#define CE_MPCP_TIMEOUT_THR		0x204c
#define CE_MPCP_REG_TIMEOUT		0x2050
#define CE_MPCP_OLT_MAC_HI		0x20a0
#define CE_MPCP_OLT_MAC_LO		0x20a4
#define CE_MPCP_REMOTE_INFO		0x20ac
#define CE_MPCP_MIB_GATE_ERR		0x20bc
#define CE_MPCP_MIB_REG_ERR		0x20c0
#define CE_MPCP_SYNC_TIME_RD		0x20c8
#define CE_MPCP_SYNC_TIME		0x20d0
#define CE_MPCP_LASER_STS		0x20d4
#define CE_MPCP_PKT_FLAG		0x20d8
#define   CE_MPCP_FLAG_REG_ACK		3
#define   CE_MPCP_FLAG_REG_NACK		4
#define CE_MPCP_OFFLINE			0x20e0
#define CE_MPCP_MIB_RX_GATE		0x2100
#define CE_MPCP_MIB_TX_REPORT		0x2104

/* PUC / PDC registers (relative to ce->pon) */
#define CE_PUC_BTC_CFG			0x8050
#define CE_PUC_RPT_CFG			0x8054
#define   CE_PUC_FEC_OVERHEAD_EN	BIT(20)
#define CE_PDC_MAP_ACCESS		0x9010
#define CE_PDC_MAP_DATA0		0x9014
#define CE_PDC_MAP_DATA1		0x9018

#define CE_EPON_FRAME_LIMIT		1700
#define CE_MPCP_POLL_INTERVAL		msecs_to_jiffies(500)
#define CE_MPCP_NACK_BACKOFF		msecs_to_jiffies(3000)
#define CE_MPCP_HOLDOVER_DEFAULT_MS	1000
#define CE_OAM_SUBTYPE			0x03
#define CE_OAM_FLAG_DYING_GASP		0x0002
#define CE_OAM_CODE_INFO		0x00
#define CE_DYING_GASP_BURST_CNT		3

static const u8 ce_oam_slow_da[ETH_ALEN] = {
	0x01, 0x80, 0xc2, 0x00, 0x00, 0x02
};

struct cortina_epon_llid_state {
	u16 llid;
	bool registered;
	bool crypto_en;
	u64 epoch;
	unsigned long holdover_until;
	u64 rx_oam;
	u64 tx_oam;
};

struct cortina_epon_fec_stats {
	u32 last_corr_blk;
	u32 last_uncorr_blk;
	u32 last_good_blk;
	u32 last_total_blk;
	u32 last_corr_bytes;
	u64 corr_blk;
	u64 uncorr_blk;
	u64 good_blk;
	u64 total_blk;
	u64 corr_bytes;
};

struct cortina_epon {
	struct cortina_epon_plat plat;
	struct xpon_device *xpon;
	struct xpon_oam *oam;
	struct mutex lock;

	bool active;
	bool mac_ready;
	bool los;
	bool link_1g;
	bool oam_link;
	bool fec_rx;
	bool fec_tx;
	enum cortina_epon_laser_mode laser_mode;
	u8 llid_count;
	u32 reg_mask;
	u32 holdover_ms;
	u32 generation;
	u64 epoch;

	/* MPCP NACK quiet-zone backoff */
	bool nack_backoff;
	unsigned long nack_until;
	u32 nack_count;
	u8 last_reg_flag;

	/* Timed TX inhibit (CTC OAM laser control) */
	u16 tx_inhibit_action;
	unsigned long tx_inhibit_until;
	struct delayed_work tx_inhibit_work;

	struct cortina_epon_llid_state llids[CORTINA_EPON_MAX_LLID];
	struct cortina_epon_fec_stats fec;

	u8 mac_addr[ETH_ALEN];
	u8 olt_mac[ETH_ALEN];

	struct delayed_work poll_work;
	struct dentry *dbgfs;
};

static struct cortina_epon *ce_singleton;

static inline u32 ce_rd(struct cortina_epon *ce, u32 off)
{
	return readl(ce->plat.pon + off);
}

static inline void ce_wr(struct cortina_epon *ce, u32 off, u32 val)
{
	writel(val, ce->plat.pon + off);
}

static inline void ce_rmw(struct cortina_epon *ce, u32 off, u32 mask, u32 val)
{
	ce_wr(ce, off, (ce_rd(ce, off) & ~mask) | (val & mask));
}

static inline u32 ce_glb_rd(struct cortina_epon *ce, u32 off)
{
	return readl(ce->plat.glb + off);
}

static inline void ce_glb_wr(struct cortina_epon *ce, u32 off, u32 val)
{
	writel(val, ce->plat.glb + off);
}

static inline void ce_glb_rmw(struct cortina_epon *ce, u32 off,
			      u32 mask, u32 val)
{
	ce_glb_wr(ce, off, (ce_glb_rd(ce, off) & ~mask) | (val & mask));
}

/* ---- GLB reset & 1G EPON SerDes bring-up ---- */

static void ce_glb_reset(struct cortina_epon *ce)
{
	/* Power down SerDes, hold GPON & PON resets, select 1G EPON ONU mode */
	ce_glb_rmw(ce, CE_GLB_PSDS_INIT, CE_PSDS_POW_PCIX | CE_PSDS_BEN_OEN, 0);
	ce_glb_wr(ce, CE_GLB_GPON_CNTL, 0);
	ce_glb_wr(ce, CE_GLB_PON_CNTL, 0);
	ce_glb_wr(ce, CE_GLB_EPON_CNTL,
		  FIELD_PREP(CE_GLB_EPON_ONU_MODE, CE_GLB_EPON_ONU_1G));
	ce_glb_wr(ce, CE_GLB_PON_CNTL, CE_PON_CNTL_PSDS_REG_RST_N);
	mdelay(1);
}

static void ce_psds_init(struct cortina_epon *ce)
{
	unsigned int i;

	/* Select EPON clock domain and pulse psds_reg_rst_n */
	ce_glb_rmw(ce, CE_GLB_PON_CNTL,
		   CE_PON_CNTL_EPON_SEL | CE_PON_CNTL_PSDS_REG_RST_N,
		   CE_PON_CNTL_EPON_SEL);
	mdelay(1);
	ce_glb_rmw(ce, CE_GLB_PON_CNTL,
		   CE_PON_CNTL_PSDS_REG_RST_N, CE_PON_CNTL_PSDS_REG_RST_N);
	mdelay(1);

	/* 1G EPON SerDes rate (sd_s0 = 1, sds_mode_s0 = 0xc) */
	ce_wr(ce, CE_PSDS_MODE, CE_PSDS_MODE_EPON_1G);
	udelay(10);

	/* Load RTL9607DQ 1G EPON analog/CMU/PLL profile */
	for (i = 0; i < ARRAY_SIZE(ce_serdes_epon_1g); i++) {
		ce_wr(ce, ce_serdes_epon_1g[i].off, ce_serdes_epon_1g[i].val);
		udelay(ce_serdes_epon_1g[i].delay_us ?
		       ce_serdes_epon_1g[i].delay_us : 10);
	}

	/* Power up SerDes CMU/PLL while keeping burst enable off */
	ce_glb_rmw(ce, CE_GLB_PSDS_INIT,
		   CE_PSDS_POW_PCIX | CE_PSDS_BEN_OEN, CE_PSDS_POW_PCIX);
	mdelay(10);

	/* Bounded wait for downstream RX clock lock (continues without fiber) */
	for (i = 0; i < 1001; i++) {
		if ((ce_rd(ce, CE_PSDS_RGB8) & CE_PSDS_RX_LOCK_MASK) ==
		    CE_PSDS_RX_LOCK_VAL)
			break;
		mdelay(1);
	}
	dev_dbg(ce->plat.dev,
		"epon: psds rx-lock wait done at %ums (rgb8=0x%08x)\n",
		i, ce_rd(ce, CE_PSDS_RGB8));

	/* Configure gearbox bit ordering for EPON */
	ce_rmw(ce, CE_PSDS_GBOX_CTRL, BIT(4) | BIT(5) | BIT(6) | BIT(7),
	       BIT(4) | BIT(6));
	mdelay(1);
}

static void ce_csr_reset(struct cortina_epon *ce)
{
	ce_glb_rmw(ce, CE_GLB_EPON_CNTL, CE_GLB_EPON_RST_MASK, 0);
	msleep(100);
	ce_glb_rmw(ce, CE_GLB_EPON_CNTL,
		   CE_GLB_EPON_RST_MASK | CE_GLB_EPON_ONU_MODE | CE_GLB_EPON_XG_SEL,
		   CE_GLB_EPON_RST_MASK |
		   FIELD_PREP(CE_GLB_EPON_ONU_MODE, CE_GLB_EPON_ONU_1G));
	msleep(100);
	ce_glb_wr(ce, CE_GLB_PON_CNTL, CE_PON_CNTL_EPON_RELEASE);
	msleep(100);
}

/* ---- Laser & TX power control ---- */

static void ce_laser_apply(struct cortina_epon *ce)
{
	bool force_off = (ce->laser_mode == CORTINA_EPON_LASER_OFF) ||
			 (ce->tx_inhibit_action != 0);

	switch (ce->laser_mode) {
	case CORTINA_EPON_LASER_ON:
		if (!force_off) {
			ce_rmw(ce, CE_EPON_MISC, CE_EPON_MISC_LASER_OFF, 0);
			ce_rmw(ce, CE_EPON_STATIC, CE_EPON_STATIC_LASER_ON,
			       CE_EPON_STATIC_LASER_ON);
			break;
		}
		fallthrough;
	case CORTINA_EPON_LASER_OFF:
		ce_rmw(ce, CE_EPON_MISC, CE_EPON_MISC_LASER_OFF,
		       CE_EPON_MISC_LASER_OFF);
		ce_rmw(ce, CE_EPON_STATIC, CE_EPON_STATIC_LASER_ON, 0);
		break;
	case CORTINA_EPON_LASER_BURST:
	default:
		ce_rmw(ce, CE_EPON_MISC, CE_EPON_MISC_LASER_OFF,
		       force_off ? CE_EPON_MISC_LASER_OFF : 0);
		ce_rmw(ce, CE_EPON_STATIC, CE_EPON_STATIC_LASER_ON, 0);
		break;
	}

	ce_rmw(ce, CE_EPON_MISC, CE_EPON_MISC_LASER_EN,
	       force_off ? 0 : CE_EPON_MISC_LASER_EN);
	if (ce->plat.tx_disable)
		gpiod_set_value_cansleep(ce->plat.tx_disable, force_off ? 1 : 0);
}

static void ce_tx_inhibit_work(struct work_struct *work)
{
	struct cortina_epon *ce = container_of(to_delayed_work(work),
					       struct cortina_epon,
					       tx_inhibit_work);

	mutex_lock(&ce->lock);
	if (ce->active && ce->tx_inhibit_action &&
	    ce->tx_inhibit_action != 0xffff) {
		if (time_before(jiffies, ce->tx_inhibit_until)) {
			schedule_delayed_work(&ce->tx_inhibit_work,
					      ce->tx_inhibit_until - jiffies);
		} else {
			ce->tx_inhibit_action = 0;
			ce_laser_apply(ce);
			dev_info(ce->plat.dev,
				 "epon: timed laser inhibit expired, TX restored\n");
		}
	}
	mutex_unlock(&ce->lock);
}

/* ---- IEEE 802.3ah 1G FEC & hardware counters ---- */

static void ce_fec_apply(struct cortina_epon *ce)
{
	ce_rmw(ce, CE_EPON_1G_FEC_CFG, CE_FEC_1G_RX_EN | CE_FEC_1G_TX_EN,
	       (ce->fec_rx ? CE_FEC_1G_RX_EN : 0) |
	       (ce->fec_tx ? CE_FEC_1G_TX_EN : 0));
	ce_rmw(ce, CE_EPON_MISC, CE_EPON_MISC_FEC_RX_MODE,
	       FIELD_PREP(CE_EPON_MISC_FEC_RX_MODE, 2));
	/* Keep PUC payload scheduler and MPCP REPORT accounting in sync */
	ce_rmw(ce, CE_PUC_BTC_CFG, CE_PUC_FEC_OVERHEAD_EN,
	       ce->fec_tx ? CE_PUC_FEC_OVERHEAD_EN : 0);
	ce_rmw(ce, CE_PUC_RPT_CFG, CE_PUC_FEC_OVERHEAD_EN,
	       ce->fec_tx ? CE_PUC_FEC_OVERHEAD_EN : 0);
}

static void ce_fec_sample(struct cortina_epon *ce)
{
	u32 corr = ce_rd(ce, CE_EPON_1G_FEC_CORR_BLK);
	u32 uncorr = ce_rd(ce, CE_EPON_1G_FEC_UNCORR_BLK);
	u32 good = ce_rd(ce, CE_EPON_1G_FEC_GOOD_BLK);
	u32 total = ce_rd(ce, CE_EPON_1G_FEC_TOTAL_BLK);
	u32 bytes = ce_rd(ce, CE_EPON_1G_FEC_CORR_BYTES);

	ce->fec.corr_blk += corr - ce->fec.last_corr_blk;
	ce->fec.uncorr_blk += uncorr - ce->fec.last_uncorr_blk;
	ce->fec.good_blk += good - ce->fec.last_good_blk;
	ce->fec.total_blk += total - ce->fec.last_total_blk;
	ce->fec.corr_bytes += bytes - ce->fec.last_corr_bytes;

	ce->fec.last_corr_blk = corr;
	ce->fec.last_uncorr_blk = uncorr;
	ce->fec.last_good_blk = good;
	ce->fec.last_total_blk = total;
	ce->fec.last_corr_bytes = bytes;
}

/* ---- 1G EPON churning decryption engine ---- */

static void ce_crypto_init(struct cortina_epon *ce)
{
	unsigned int ch, slot, w;

	/* Select 1G downstream churning mode */
	ce_rmw(ce, CE_EPON_1G_CRYPTO_MODE, GENMASK(31, 29), 0);
	ce_rmw(ce, CE_EPON_1G_CRYPTO_CFG, BIT(0) | BIT(7), BIT(0));
	ce_rmw(ce, CE_EPON_1G_CRYPTO_CTRL0, BIT(2), 0);
	ce_rmw(ce, CE_EPON_1G_CRYPTO_CFG2, BIT(3), BIT(3));

	for (slot = 0; slot < 2; slot++)
		for (w = 0; w < 4; w++)
			ce_wr(ce, CE_EPON_1G_CRYPTO_KEY(slot) + w * 4, 0);

	for (ch = 0; ch < CORTINA_EPON_MAX_LLID; ch++) {
		ce_wr(ce, CE_EPON_1G_CRYPTO_LLID(ch), 0);
		ce->llids[ch].crypto_en = false;
	}
}

static void ce_crypto_clear_llid(struct cortina_epon *ce, unsigned int ch)
{
	if (ch >= CORTINA_EPON_MAX_LLID)
		return;
	ce_wr(ce, CE_EPON_1G_CRYPTO_LLID(ch), 0);
	ce->llids[ch].crypto_en = false;
}

static int ce_crypto_set_key_1g(struct cortina_epon *ce, unsigned int ch,
				unsigned int slot, const u8 key[3])
{
	u32 base, val;
	unsigned int w;

	if (ch >= ce->llid_count || ch >= CORTINA_EPON_MAX_LLID || slot > 1)
		return -EINVAL;
	if (!ce->active || !ce->mac_ready || !ce->llids[ch].registered)
		return -EAGAIN;

	base = CE_EPON_1G_CRYPTO_KEY(slot);
	ce_wr(ce, base, ((u32)key[0] << 24) | ((u32)key[1] << 16) |
	      ((u32)key[2] << 8));
	for (w = 1; w < 4; w++)
		ce_wr(ce, base + w * 4, 0);

	val = (ce->llids[ch].llid & 0xffff) |
	      (((ch * 2) & 0x7) << 16) | BIT(19);
	ce_wr(ce, CE_EPON_1G_CRYPTO_LLID(ch), val);
	ce->llids[ch].crypto_en = true;
	return 0;
}

/* ---- Hardware LLID table & MPCP configuration ---- */

static int ce_llid_table_update(struct cortina_epon *ce, bool add,
				unsigned int index, unsigned int mac_idx,
				u16 llid, bool multicast)
{
	u32 status;
	int ret;

	ce_wr(ce, CE_EPON_LLID_DATA,
	      FIELD_PREP(CE_LLID_TBL_LLID, llid) |
	      FIELD_PREP(CE_LLID_TBL_IDX, index) |
	      (multicast ? CE_LLID_TBL_MC : 0) |
	      (add ? CE_LLID_TBL_VALID : 0) |
	      FIELD_PREP(CE_LLID_TBL_MAC, mac_idx));
	ce_wr(ce, CE_EPON_LLID_ACCESS, BIT(0));
	ret = readl_poll_timeout(ce->plat.pon + CE_EPON_LLID_ACCESS,
				 status, !(status & BIT(0)), 1, 100000);
	if (ret)
		return ret;

	status = ce_rd(ce, CE_EPON_LLID_RESULT) & 0x7;
	if (add && (status == 6 || status == 7))
		return 0;
	if (!add && (status == 5 || status == 2))
		return 0;
	return status == 1 ? -ENOSPC : -EIO;
}

static int ce_llid_lookup_by_mac(struct cortina_epon *ce, unsigned int mac_idx,
				 u16 *llid_out)
{
	unsigned int i;

	for (i = 0; i < CE_EPON_LLID_TBL_SIZE; i++) {
		u32 entry = ce_rd(ce, CE_EPON_LLID_TBL(i));

		if ((entry & CE_LLID_TBL_VALID) &&
		    !(entry & CE_LLID_TBL_MC) &&
		    FIELD_GET(CE_LLID_TBL_MAC, entry) == mac_idx) {
			*llid_out = FIELD_GET(CE_LLID_TBL_LLID, entry);
			return 0;
		}
	}
	return -ENOENT;
}

static void ce_mpcp_program_mac(struct cortina_epon *ce)
{
	const u8 *m = ce->mac_addr;

	ce_rmw(ce, CE_EPON_MAC_HI, 0xffff, ((u32)m[0] << 8) | m[1]);
	ce_wr(ce, CE_EPON_MAC_LO,
	      ((u32)m[2] << 24) | ((u32)m[3] << 16) |
	      ((u32)m[4] << 8) | m[5]);
}

static void ce_mpcp_read_olt_mac(struct cortina_epon *ce, u8 out[ETH_ALEN])
{
	u32 hi = ce_rd(ce, CE_MPCP_OLT_MAC_HI);
	u32 lo = ce_rd(ce, CE_MPCP_OLT_MAC_LO);

	out[0] = (hi >> 16) & 0xff;
	out[1] = (hi >> 8) & 0xff;
	out[2] = hi & 0xff;
	out[3] = (lo >> 16) & 0xff;
	out[4] = (lo >> 8) & 0xff;
	out[5] = lo & 0xff;
}

static u32 ce_mpcp_read_reg_mask(struct cortina_epon *ce)
{
	u32 lo = ce_rd(ce, CE_MPCP_REG_STATUS_LO);
	u32 mask = 0;
	unsigned int i;

	for (i = 0; i < ce->llid_count && i < CORTINA_EPON_MAX_LLID; i++) {
		if (((lo >> (i * 2)) & 0x3) == 0x3)
			mask |= BIT(i);
	}
	return mask;
}

static void ce_mpcp_init(struct cortina_epon *ce)
{
	u32 offline_mask;

	/* Map broadcast LLID (0x7fff/0xffff) to internal index 0x3f */
	ce_rmw(ce, CE_EPON_LLID_BC_MAP, GENMASK(21, 16) | GENMASK(5, 0),
	       FIELD_PREP(GENMASK(21, 16), CE_EPON_BC_LLID_IDX) |
	       CE_EPON_BC_LLID_IDX);

	ce_mpcp_program_mac(ce);

	/* Configure laser on/off times (32 TQ each) and gate/reg timeouts */
	ce_wr(ce, CE_MPCP_LASER_TIME, (0x20 << 8) | 0x20);
	ce_rmw(ce, CE_MPCP_TIMEOUT_THR, GENMASK(29, 16),
	       FIELD_PREP(GENMASK(29, 16), 100));

	/* Enable multi-LLID only when llid_count > 1 */
	ce_rmw(ce, CE_MPCP_CFG, CE_MPCP_CFG_REG_EN | CE_MPCP_CFG_MLLID_EN,
	       CE_MPCP_CFG_REG_EN |
	       (ce->llid_count > 1 ? CE_MPCP_CFG_MLLID_EN : 0));

	/* Take unused LLIDs offline, keep [0..llid_count-1] online */
	offline_mask = ~GENMASK(ce->llid_count - 1, 0);
	ce_wr(ce, CE_MPCP_OFFLINE, offline_mask);

	/* Clear pending interrupts and unmask MPCP/1G events */
	ce_wr(ce, CE_MPCP_IRQ, 0xffffffff);
	ce_wr(ce, CE_MPCP_IE, CE_MPCP_EV_MASK);
	ce_wr(ce, CE_EPON_1G_IRQ, 0xffffffff);
	ce_wr(ce, CE_EPON_1G_IE, BIT(6));
	ce_wr(ce, CE_EPON_GLB_IRQ, 0xffffffff);
	ce_wr(ce, CE_EPON_GLB_IE, BIT(0) | BIT(3));

	/* Start MPCP discovery registration engine */
	ce_rmw(ce, CE_MPCP_START, BIT(0), BIT(0));
}

/* ---- PUC/PDC EPON datapath admission ---- */

static void ce_pdc_map_llid(struct cortina_epon *ce, u32 idx, u32 lspid)
{
	u32 status;

	/* Route downstream LLID frames to CPU port 0 with lspid = PON (7) */
	ce_wr(ce, CE_PDC_MAP_DATA0, lspid & 0x3f);
	ce_wr(ce, CE_PDC_MAP_DATA1, 0);
	ce_wr(ce, CE_PDC_MAP_ACCESS, BIT(31) | BIT(30) | (idx & 0x7f));
	readl_poll_timeout_atomic(ce->plat.pon + CE_PDC_MAP_ACCESS,
				  status, !(status & BIT(31)), 1, 10000);
}

static void ce_datapath_init(struct cortina_epon *ce)
{
	unsigned int i;

	/* Admit upstream VoQ 0..7 for each configured LLID in NI QM */
	if (IS_REACHABLE(CONFIG_CORTINA_NI)) {
		for (i = 0; i < ce->llid_count; i++)
			cortina_ni_qm_voq_admit(i * 8);
	}

	/* Map broadcast LLID and unicast LLIDs in PDC toward CPU/WAN */
	ce_pdc_map_llid(ce, CE_EPON_BC_LLID_IDX, 7);
	for (i = 0; i < ce->llid_count; i++)
		ce_pdc_map_llid(ce, i, 7);
}

/* ---- EPON MAC/PCS/MIB initialization ---- */

static int ce_mac_init(struct cortina_epon *ce)
{
	u32 status, drop_mask = GENMASK(19, 13) | GENMASK(29, 26);
	int ret;

	ce_csr_reset(ce);

	/* MIB read-clear disabled (software accumulates 64-bit deltas) */
	ce_rmw(ce, CE_EPON_MIB_CTRL, BIT(0), 0);
	ce_rmw(ce, CE_EPON_LINK_DETECT, GENMASK(13, 8), 0x3f << 8);
	ce_glb_rmw(ce, CE_GLB_EPON_CNTL, CE_GLB_EPON_XG_SEL, 0);

	/* PCS timing, RTT compensation & local time */
	ce_wr(ce, CE_EPON_PCS_RX, 0x5000);
	ce_wr(ce, CE_EPON_PCS_TX, 0x03020000);
	ce_wr(ce, CE_EPON_RTT_RX, (0x59 << 16) | 0x48);
	ce_wr(ce, CE_EPON_RTT_TX, (0x18 << 16) | 0x16);
	ce_rmw(ce, CE_EPON_RX_LOCALTIME, GENMASK(16, 0), 0x10006);

	/* Normal MPCP-controlled laser burst defaults */
	ce_rmw(ce, CE_EPON_STATIC, CE_EPON_STATIC_LASER_ON, 0);
	ce_rmw(ce, CE_EPON_MISC, GENMASK(1, 0) | BIT(4) | BIT(5),
	       CE_EPON_MISC_LASER_EN);
	ce_rmw(ce, CE_EPON_PCS_TX, GENMASK(20, 16), 2 << 16);

	/* Error drop policy & 1G EPON MAC frame limit */
	ce_rmw(ce, CE_EPON_RX_DROP, drop_mask, drop_mask & ~BIT(28));
	ce_glb_rmw(ce, CE_GLB_EPON_CNTL, CE_GLB_EPON_ONU_MODE,
		   FIELD_PREP(CE_GLB_EPON_ONU_MODE, CE_GLB_EPON_ONU_1G));
	ce_rmw(ce, CE_EPON_XG_RX_CFG, BIT(7) | BIT(9) | GENMASK(30, 17),
	       BIT(9) | (CE_EPON_FRAME_LIMIT << 17));
	ce_rmw(ce, CE_EPON_XG_TX_CFG, BIT(3) | GENMASK(18, 12), 12 << 12);
	ce_rmw(ce, CE_EPON_1G_RX_CFG, CE_EPON_1G_RX_EN, 0);
	ce_rmw(ce, CE_EPON_1G_TX_CFG,
	       BIT(0) | BIT(1) | BIT(3) | GENMASK(23, 20) | BIT(25) | BIT(30),
	       0);
	ce_rmw(ce, CE_EPON_1G_MAX_PKT, GENMASK(13, 0), CE_EPON_FRAME_LIMIT);

	/* Pulse pon_serdes_rst_n & 1G PCS sync bit */
	ce_glb_rmw(ce, CE_GLB_PON_CNTL, CE_PON_CNTL_SDS_RST_N, 0);
	msleep(1);
	ce_glb_rmw(ce, CE_GLB_PON_CNTL, CE_PON_CNTL_SDS_RST_N,
		   CE_PON_CNTL_SDS_RST_N);

	status = ce_rd(ce, CE_EPON_STATIC) & CE_EPON_STATIC_1G_TOGGLE;
	ce_rmw(ce, CE_EPON_STATIC, CE_EPON_STATIC_1G_TOGGLE,
	       status ^ CE_EPON_STATIC_1G_TOGGLE);
	msleep(1);
	ce_rmw(ce, CE_EPON_STATIC, CE_EPON_STATIC_1G_TOGGLE, status);

	/* Initialize MIB RAM and clear MPCP MIB counters */
	ce_rmw(ce, CE_EPON_MIB_CTRL, BIT(1), BIT(1));
	ce_rmw(ce, CE_EPON_MIB_CTRL, BIT(1), 0);
	ret = readl_poll_timeout(ce->plat.pon + CE_EPON_MIB_STATUS,
				 status, status & BIT(0), 1, 100000);
	if (ret) {
		dev_err(ce->plat.dev, "epon: MIB RAM init timed out\n");
		return ret;
	}
	ce_rmw(ce, CE_EPON_RX_MPCP_MIB, BIT(17), BIT(17));
	ce_rmw(ce, CE_EPON_TX_MPCP_MIB, BIT(17), BIT(17));

	/* Configure FEC, 1G churning crypto, datapath & MPCP */
	ce_fec_apply(ce);
	ce_crypto_init(ce);
	ce_datapath_init(ce);
	ce_mpcp_init(ce);

	/* Enable 1G EPON RX/TX MAC, spare bit and burst output */
	ce_rmw(ce, CE_EPON_1G_RX_CFG, CE_EPON_1G_RX_EN, CE_EPON_1G_RX_EN);
	ce_rmw(ce, CE_EPON_1G_TX_CFG, CE_EPON_1G_TX_EN, CE_EPON_1G_TX_EN);
	ce_wr(ce, CE_EPON_SPARE, 0x40000000);
	ce_glb_rmw(ce, CE_GLB_PSDS_INIT, CE_PSDS_BEN_OEN, CE_PSDS_BEN_OEN);
	ce_laser_apply(ce);

	ce->mac_ready = true;
	return 0;
}

/* ---- Hardware SW-frame TX (Dying Gasp OAMPDU) ---- */

static int ce_tx_swframe_atomic(struct cortina_epon *ce, const u8 *frame,
				unsigned int len)
{
	unsigned int padded = max_t(unsigned int, ALIGN(len, 8), 64);
	u8 buf[128] = { };
	u32 status, llid0;
	unsigned int i;

	if (len == 0 || padded > sizeof(buf))
		return -EINVAL;
	if (ce_rd(ce, CE_EPON_SW_TX_CTRL) & BIT(0))
		return -EBUSY;

	memcpy(buf, frame, len);
	llid0 = ce_rd(ce, CE_EPON_LLID_TBL(0)) & 0xffff;
	ce_wr(ce, CE_EPON_SW_TX_HDR, (llid0 << 16) | ((len + 4) & 0xfff));

	for (i = 0; i < padded / 8; i++) {
		const u8 *p = buf + i * 8;

		ce_wr(ce, CE_EPON_SW_TX_DATA0, get_unaligned_be32(p));
		ce_wr(ce, CE_EPON_SW_TX_DATA1, get_unaligned_be32(p + 4));
		ce_wr(ce, CE_EPON_SW_TX_ACCESS, BIT(31) | BIT(30) | i);
		if (readl_poll_timeout_atomic(ce->plat.pon + CE_EPON_SW_TX_ACCESS,
					      status, !(status & BIT(31)),
					      1, 5000))
			return -ETIMEDOUT;
	}

	ce_wr(ce, CE_EPON_SW_TX_CTRL, BIT(0));
	return 0;
}

void cortina_epon_dying_gasp(struct cortina_epon *ce)
{
	u8 pdu[60] = { };
	unsigned int i;

	if (!ce || !READ_ONCE(ce->active) || !READ_ONCE(ce->mac_ready))
		return;

	/* IEEE 802.3ah Dying Gasp Information OAMPDU */
	memcpy(pdu, ce_oam_slow_da, ETH_ALEN);
	memcpy(pdu + ETH_ALEN, ce->mac_addr, ETH_ALEN);
	put_unaligned_be16(ETH_P_SLOW, pdu + 12);
	pdu[14] = CE_OAM_SUBTYPE;
	put_unaligned_be16(CE_OAM_FLAG_DYING_GASP, pdu + 15);
	pdu[17] = CE_OAM_CODE_INFO;

	for (i = 0; i < CE_DYING_GASP_BURST_CNT; i++)
		ce_tx_swframe_atomic(ce, pdu, sizeof(pdu));
}

/* ---- IEEE 802.3ah OAM RX / TX (net/xpon/oam + pon0 AF_PACKET) ---- */

static int ce_oam_xmit(struct xpon_oam *oam, struct sk_buff *skb, u8 llid_idx)
{
	struct cortina_epon *ce = xpon_oam_priv(oam);
	u8 *eth;
	int ret;

	if (!ce || !READ_ONCE(ce->active) || llid_idx >= ce->llid_count)
		return -ENODEV;

	/* net/xpon/oam passes OAM payload starting at subtype 0x03 */
	if (skb->len < 4 || skb->data[0] != CE_OAM_SUBTYPE)
		return -EINVAL;
	if (skb_cow_head(skb, ETH_HLEN))
		return -ENOMEM;

	eth = skb_push(skb, ETH_HLEN);
	memcpy(eth, ce_oam_slow_da, ETH_ALEN);
	memcpy(eth + ETH_ALEN, ce->mac_addr, ETH_ALEN);
	eth[ETH_ALEN + 5] += llid_idx;
	put_unaligned_be16(ETH_P_SLOW, eth + 2 * ETH_ALEN);

	if (!IS_REACHABLE(CONFIG_CORTINA_NI))
		return -ENODEV;

	ret = cortina_ni_pon_oam_tx(skb, llid_idx);
	if (ret) {
		skb_pull(skb, ETH_HLEN);
		return ret;
	}
	ce->llids[llid_idx].tx_oam++;
	return 0;
}

static const struct xpon_oam_ops ce_oam_ops = {
	.xmit = ce_oam_xmit,
};

static void ce_ni_oam_rx(const u8 *frame, unsigned int len, u8 llid_idx)
{
	struct cortina_epon *ce = READ_ONCE(ce_singleton);
	struct sk_buff *eth_skb, *oam_skb;

	if (!ce || !READ_ONCE(ce->active) || len < ETH_HLEN + 4)
		return;
	if (llid_idx >= CORTINA_EPON_MAX_LLID)
		llid_idx = 0;

	ce->llids[llid_idx].rx_oam++;

	/* 1. Feed in-kernel net/xpon/oam state tracker */
	if (ce->oam && ce->llids[llid_idx].registered) {
		oam_skb = alloc_skb(len - ETH_HLEN, GFP_ATOMIC);
		if (oam_skb) {
			skb_put_data(oam_skb, frame + ETH_HLEN,
				     len - ETH_HLEN);
			xpon_oam_receive(ce->oam, oam_skb, llid_idx);
		}
	}

	/* 2. Deliver full Ethernet OAMPDU to pon0 for userspace epon-oamd */
	if (ce->plat.wan_ndev && netif_running(ce->plat.wan_ndev)) {
		eth_skb = netdev_alloc_skb_ip_align(ce->plat.wan_ndev, len);
		if (eth_skb) {
			skb_put_data(eth_skb, frame, len);
			eth_skb->protocol = eth_type_trans(eth_skb,
							   ce->plat.wan_ndev);
			ce->plat.wan_ndev->stats.rx_packets++;
			ce->plat.wan_ndev->stats.rx_bytes += len;
			netif_rx(eth_skb);
		}
	}
}

netdev_tx_t cortina_epon_wan_xmit(struct cortina_epon *ce,
				  struct sk_buff *skb,
				  struct net_device *ndev)
{
	u8 llid_idx = 0;
	u16 voq;

	if (!ce || !READ_ONCE(ce->active) || !READ_ONCE(ce->reg_mask)) {
		ndev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	/* Route IEEE 802.3ah OAM frames through the PON control path */
	if (skb->len >= ETH_HLEN + 4 &&
	    get_unaligned_be16(skb->data + 12) == ETH_P_SLOW &&
	    skb->data[14] == CE_OAM_SUBTYPE) {
		if (IS_REACHABLE(CONFIG_CORTINA_NI) &&
		    !cortina_ni_pon_oam_tx(skb, llid_idx)) {
			ce->llids[llid_idx].tx_oam++;
			ndev->stats.tx_packets++;
			ndev->stats.tx_bytes += skb->len;
			return NETDEV_TX_OK;
		}
		ndev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	/* Ordinary Ethernet data frame on active LLID VoQ */
	voq = (llid_idx * 8) + (skb->priority & 7);
	if (IS_REACHABLE(CONFIG_CORTINA_NI))
		return cortina_ni_pon_data_tx(skb, ndev, voq);

	ndev->stats.tx_dropped++;
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

/* ---- MPCP state machine, NACK backoff & holdover supervisor ---- */

static void ce_poll_work(struct work_struct *work)
{
	struct cortina_epon *ce = container_of(to_delayed_work(work),
					       struct cortina_epon, poll_work);
	u32 status, mpcp_irq, raw_reg, eff_reg;
	bool los, link_1g, any_reg, notify = false;
	unsigned long now = jiffies;
	unsigned int i;

	mutex_lock(&ce->lock);
	if (!ce->active || !ce->mac_ready) {
		mutex_unlock(&ce->lock);
		return;
	}

	status = ce_rd(ce, CE_EPON_STATUS);
	los = !!(ce_glb_rd(ce, CE_GLB_OPT_STATUS) & CE_OPT_RX_LOS) ||
	      !!(status & CE_EPON_STS_LOS);
	link_1g = !los && !!(status & CE_EPON_STS_1G_LINK);

	if (los != ce->los) {
		ce->los = los;
		notify = true;
		if (ce->xpon)
			xpon_device_report_optical(ce->xpon, !los, los);
		dev_info(ce->plat.dev, "epon: optical %s\n",
			 los ? "LOS" : "signal detected");
	}
	if (link_1g != ce->link_1g) {
		ce->link_1g = link_1g;
		notify = true;
	}

	/* Service pending MPCP interrupt flags & NACK quiet-zone backoff */
	mpcp_irq = ce_rd(ce, CE_MPCP_IRQ) & CE_MPCP_EV_MASK;
	if (mpcp_irq) {
		ce_wr(ce, CE_MPCP_IRQ, mpcp_irq);
		if (mpcp_irq & BIT(1)) {
			u8 flag = (ce_rd(ce, CE_MPCP_PKT_FLAG) >> 16) & 0xff;

			ce->last_reg_flag = flag;
			if (flag == CE_MPCP_FLAG_REG_NACK) {
				ce_rmw(ce, CE_MPCP_START, BIT(0), 0);
				ce->nack_backoff = true;
				ce->nack_until = now + CE_MPCP_NACK_BACKOFF;
				ce->nack_count++;
				notify = true;
				dev_info(ce->plat.dev,
					 "epon: MPCP REGISTER NACK, backing off\n");
			} else if (flag == CE_MPCP_FLAG_REG_ACK) {
				if (ce->nack_backoff)
					notify = true;
				ce->nack_backoff = false;
			}
		}
	}

	/* Re-arm MPCP discovery once quiet-zone backoff expires */
	if (ce->nack_backoff && time_after_eq(now, ce->nack_until)) {
		ce->nack_backoff = false;
		notify = true;
		ce_rmw(ce, CE_MPCP_START, BIT(0), BIT(0));
	}

	raw_reg = link_1g ? ce_mpcp_read_reg_mask(ce) : 0;
	eff_reg = raw_reg;

	/* Apply optical holdover before dropping an already-registered LLID */
	for (i = 0; i < ce->llid_count; i++) {
		struct cortina_epon_llid_state *st = &ce->llids[i];

		if (raw_reg & BIT(i)) {
			st->holdover_until = 0;
			continue;
		}
		if (!st->registered || !ce->holdover_ms)
			continue;
		if (!st->holdover_until)
			st->holdover_until = now +
					     msecs_to_jiffies(ce->holdover_ms);
		if (time_before(now, st->holdover_until))
			eff_reg |= BIT(i);
	}

	if (eff_reg != ce->reg_mask) {
		u32 gained = eff_reg & ~ce->reg_mask;
		u32 lost = ce->reg_mask & ~eff_reg;

		ce->epoch++;
		ce->generation++;
		if (lost & BIT(0))
			ce->oam_link = false;
		notify = true;
		for (i = 0; i < ce->llid_count; i++) {
			struct cortina_epon_llid_state *st = &ce->llids[i];

			if (gained & BIT(i)) {
				u16 llid = 0;

				ce_llid_lookup_by_mac(ce, i, &llid);
				st->llid = llid;
				st->registered = true;
				st->epoch = ce->epoch;
				st->holdover_until = 0;
				ce_mpcp_read_olt_mac(ce, ce->olt_mac);
				if (ce->oam)
					xpon_oam_llid_registered(ce->oam, i,
								 llid);
				dev_info(ce->plat.dev,
					 "epon: LLID[%u] registered (0x%04x, OLT %pM)\n",
					 i, llid, ce->olt_mac);
			} else if (lost & BIT(i)) {
				st->registered = false;
				st->llid = 0;
				st->holdover_until = 0;
				ce_crypto_clear_llid(ce, i);
				if (ce->oam)
					xpon_oam_llid_unregistered(ce->oam, i);
				dev_info(ce->plat.dev,
					 "epon: LLID[%u] deregistered\n", i);
			}
		}

		ce->reg_mask = eff_reg;
		any_reg = !!eff_reg;

		if (ce->plat.wan_ndev) {
			if (any_reg)
				netif_carrier_on(ce->plat.wan_ndev);
			else
				netif_carrier_off(ce->plat.wan_ndev);
		}
		if (ce->xpon) {
			xpon_device_report_carrier(ce->xpon, any_reg);
			xpon_device_report_registration(ce->xpon,
				any_reg ? XPON_REGISTRATION_OPERATIONAL :
				link_1g ? XPON_REGISTRATION_DISCOVERY :
					  XPON_REGISTRATION_DOWN);
		}
		if (ce->plat.led_update)
			ce->plat.led_update(ce->plat.cookie, any_reg, los);
	}

	ce_fec_sample(ce);
	schedule_delayed_work(&ce->poll_work, CE_MPCP_POLL_INTERVAL);
	mutex_unlock(&ce->lock);

	if (notify)
		sysfs_notify(&ce->plat.dev->kobj, NULL, "epon_state");
}

/* ---- Hardware MIB indirect read helper ---- */

static int ce_mib_read(struct cortina_epon *ce, u32 acc_reg, u32 dat_reg,
		       u32 idx, u32 *val)
{
	u32 status;
	int ret;

	ce_wr(ce, acc_reg, BIT(31) | idx);
	ret = readl_poll_timeout(ce->plat.pon + acc_reg,
				 status, !(status & BIT(31)), 1, 10000);
	if (ret)
		return ret;
	*val = ce_rd(ce, dat_reg);
	return 0;
}

/* ---- Debugfs diagnostics (/sys/kernel/debug/cortina-gpon/epon/) ---- */

static int ce_dbg_status_show(struct seq_file *m, void *v)
{
	struct cortina_epon *ce = m->private;
	u32 status, opt, laser_sts;

	mutex_lock(&ce->lock);
	seq_printf(m, "active        : %u\n", ce->active);
	seq_printf(m, "mac_ready     : %u\n", ce->mac_ready);
	seq_printf(m, "onu_mac       : %pM\n", ce->mac_addr);
	seq_printf(m, "olt_mac       : %pM\n", ce->olt_mac);
	seq_printf(m, "llid_count    : %u\n", ce->llid_count);
	seq_printf(m, "reg_mask      : 0x%08x\n", ce->reg_mask);
	seq_printf(m, "fec_rx        : %u\n", ce->fec_rx);
	seq_printf(m, "fec_tx        : %u\n", ce->fec_tx);
	seq_printf(m, "laser_mode    : %u\n", ce->laser_mode);
	seq_printf(m, "holdover_ms   : %u\n", ce->holdover_ms);
	seq_printf(m, "nack_count    : %u (backoff=%u last_flag=%u)\n",
		   ce->nack_count, ce->nack_backoff, ce->last_reg_flag);
	if (ce->active && ce->mac_ready) {
		status = ce_rd(ce, CE_EPON_STATUS);
		opt = ce_glb_rd(ce, CE_GLB_OPT_STATUS);
		laser_sts = ce_rd(ce, CE_MPCP_LASER_STS);
		seq_printf(m, "epon_status   : 0x%08x (1g_link=%u pcs_sync=%u los=%u)\n",
			   status, !!(status & CE_EPON_STS_1G_LINK),
			   !!(status & CE_EPON_STS_PCS_SYNC),
			   !!(status & CE_EPON_STS_LOS));
		seq_printf(m, "opt_status    : 0x%08x (rx_los=%u tx_fault=%u mod_abs=%u tx_dis=%u)\n",
			   opt, !!(opt & CE_OPT_RX_LOS),
			   !!(opt & CE_OPT_TX_FAULT),
			   !!(opt & CE_OPT_MOD_ABS),
			   !!(opt & CE_OPT_TX_DIS));
		seq_printf(m, "sync_time     : %u TQ\n",
			   ce_rd(ce, CE_MPCP_SYNC_TIME_RD) & 0xffff);
		seq_printf(m, "laser_time    : on=%u TQ off=%u TQ\n",
			   laser_sts & 0xff, (laser_sts >> 8) & 0xff);
		seq_printf(m, "remote_info   : 0x%04x\n",
			   ce_rd(ce, CE_MPCP_REMOTE_INFO) & 0xffff);
	}
	mutex_unlock(&ce->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ce_dbg_status);

static int ce_dbg_llids_show(struct seq_file *m, void *v)
{
	struct cortina_epon *ce = m->private;
	unsigned int i;

	mutex_lock(&ce->lock);
	seq_puts(m, "idx  reg  llid    crypto  epoch  rx_oam  tx_oam\n");
	for (i = 0; i < ce->llid_count; i++) {
		const struct cortina_epon_llid_state *st = &ce->llids[i];

		seq_printf(m, "%-4u %-4u 0x%04x  %-7u %-6llu %-7llu %llu\n",
			   i, st->registered, st->llid, st->crypto_en,
			   st->epoch, st->rx_oam, st->tx_oam);
	}
	mutex_unlock(&ce->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ce_dbg_llids);

static int ce_dbg_counters_show(struct seq_file *m, void *v)
{
	struct cortina_epon *ce = m->private;
	u32 rx_gate = 0, tx_report = 0, rx_eth = 0, tx_eth = 0;
	u32 rx_mpcp_disc = 0, tx_mpcp_reg_req = 0;

	mutex_lock(&ce->lock);
	if (ce->active && ce->mac_ready) {
		ce_fec_sample(ce);
		rx_gate = ce_rd(ce, CE_MPCP_MIB_RX_GATE);
		tx_report = ce_rd(ce, CE_MPCP_MIB_TX_REPORT);
		ce_mib_read(ce, CE_EPON_MIB_RX_MPCP_ACC,
			    CE_EPON_MIB_RX_MPCP_DAT, 0x401, &rx_mpcp_disc);
		ce_mib_read(ce, CE_EPON_MIB_TX_MPCP_ACC,
			    CE_EPON_MIB_TX_MPCP_DAT, 0x400, &tx_mpcp_reg_req);
		ce_mib_read(ce, CE_EPON_MIB_RX_ETH_ACC,
			    CE_EPON_MIB_RX_ETH_DAT, 0, &rx_eth);
		ce_mib_read(ce, CE_EPON_MIB_TX_ETH_ACC,
			    CE_EPON_MIB_TX_ETH_DAT, 0, &tx_eth);
	}
	seq_printf(m, "mpcp_rx_gate       : %u\n", rx_gate);
	seq_printf(m, "mpcp_tx_report     : %u\n", tx_report);
	seq_printf(m, "mpcp_rx_discovery  : %u\n", rx_mpcp_disc);
	seq_printf(m, "mpcp_tx_reg_req    : %u\n", tx_mpcp_reg_req);
	seq_printf(m, "eth_rx_frames      : %u\n", rx_eth);
	seq_printf(m, "eth_tx_frames      : %u\n", tx_eth);
	seq_printf(m, "fec_corr_blocks    : %llu\n", ce->fec.corr_blk);
	seq_printf(m, "fec_uncorr_blocks  : %llu\n", ce->fec.uncorr_blk);
	seq_printf(m, "fec_good_blocks    : %llu\n", ce->fec.good_blk);
	seq_printf(m, "fec_total_blocks   : %llu\n", ce->fec.total_blk);
	seq_printf(m, "fec_corr_bytes     : %llu\n", ce->fec.corr_bytes);
	mutex_unlock(&ce->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ce_dbg_counters);

static int ce_dbg_fec_rx_get(void *data, u64 *val)
{
	struct cortina_epon *ce = data;

	mutex_lock(&ce->lock);
	*val = ce->fec_rx;
	mutex_unlock(&ce->lock);
	return 0;
}

static int ce_dbg_fec_rx_set(void *data, u64 val)
{
	struct cortina_epon *ce = data;

	mutex_lock(&ce->lock);
	ce->fec_rx = !!val;
	if (ce->active && ce->mac_ready)
		ce_fec_apply(ce);
	mutex_unlock(&ce->lock);
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(ce_dbg_fec_rx_fops, ce_dbg_fec_rx_get,
			 ce_dbg_fec_rx_set, "%llu\n");

static int ce_dbg_fec_tx_get(void *data, u64 *val)
{
	struct cortina_epon *ce = data;

	mutex_lock(&ce->lock);
	*val = ce->fec_tx;
	mutex_unlock(&ce->lock);
	return 0;
}

static int ce_dbg_fec_tx_set(void *data, u64 val)
{
	struct cortina_epon *ce = data;

	mutex_lock(&ce->lock);
	ce->fec_tx = !!val;
	if (ce->active && ce->mac_ready)
		ce_fec_apply(ce);
	mutex_unlock(&ce->lock);
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(ce_dbg_fec_tx_fops, ce_dbg_fec_tx_get,
			 ce_dbg_fec_tx_set, "%llu\n");

static int ce_dbg_laser_mode_get(void *data, u64 *val)
{
	struct cortina_epon *ce = data;

	mutex_lock(&ce->lock);
	*val = ce->laser_mode;
	mutex_unlock(&ce->lock);
	return 0;
}

static int ce_dbg_laser_mode_set(void *data, u64 val)
{
	struct cortina_epon *ce = data;

	if (val > CORTINA_EPON_LASER_OFF)
		return -EINVAL;
	mutex_lock(&ce->lock);
	ce->laser_mode = val;
	if (ce->active && ce->mac_ready)
		ce_laser_apply(ce);
	mutex_unlock(&ce->lock);
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(ce_dbg_laser_mode_fops, ce_dbg_laser_mode_get,
			 ce_dbg_laser_mode_set, "%llu\n");

static int ce_dbg_tx_inhibit_set(void *data, u64 val)
{
	struct cortina_epon *ce = data;

	if (val > 0xffff)
		return -EINVAL;
	mutex_lock(&ce->lock);
	ce->tx_inhibit_action = val;
	if (ce->active && ce->mac_ready)
		ce_laser_apply(ce);
	if (val && val != 0xffff) {
		ce->tx_inhibit_until = jiffies + msecs_to_jiffies(val * 1000);
		mod_delayed_work(system_wq, &ce->tx_inhibit_work,
				 msecs_to_jiffies(val * 1000));
	} else {
		cancel_delayed_work(&ce->tx_inhibit_work);
	}
	mutex_unlock(&ce->lock);
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(ce_dbg_tx_inhibit_fops, NULL,
			 ce_dbg_tx_inhibit_set, "%llu\n");

static int ce_dbg_llid_count_get(void *data, u64 *val)
{
	struct cortina_epon *ce = data;

	mutex_lock(&ce->lock);
	*val = ce->llid_count;
	mutex_unlock(&ce->lock);
	return 0;
}

static int ce_dbg_llid_count_set(void *data, u64 val)
{
	struct cortina_epon *ce = data;

	if (val < 1 || val > CORTINA_EPON_MAX_LLID)
		return -EINVAL;
	mutex_lock(&ce->lock);
	ce->llid_count = val;
	if (ce->active && ce->mac_ready) {
		ce_rmw(ce, CE_MPCP_CFG, CE_MPCP_CFG_MLLID_EN,
		       ce->llid_count > 1 ? CE_MPCP_CFG_MLLID_EN : 0);
		ce_wr(ce, CE_MPCP_OFFLINE, ~GENMASK(ce->llid_count - 1, 0));
	}
	mutex_unlock(&ce->lock);
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(ce_dbg_llid_count_fops, ce_dbg_llid_count_get,
			 ce_dbg_llid_count_set, "%llu\n");

static ssize_t ce_dbg_crypto_key_write(struct file *file,
				       const char __user *ubuf,
				       size_t count, loff_t *ppos)
{
	struct cortina_epon *ce = file->private_data;
	unsigned int ch, slot, k0, k1, k2;
	char buf[32];
	u8 key[3];
	int ret;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';

	if (sscanf(buf, "%u %u %2x%2x%2x", &ch, &slot, &k0, &k1, &k2) != 5)
		return -EINVAL;

	key[0] = k0;
	key[1] = k1;
	key[2] = k2;

	mutex_lock(&ce->lock);
	ret = ce_crypto_set_key_1g(ce, ch, slot, key);
	mutex_unlock(&ce->lock);
	return ret ? ret : count;
}

static const struct file_operations ce_dbg_crypto_key_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = ce_dbg_crypto_key_write,
	.llseek = noop_llseek,
};

static int ce_dbg_dying_gasp_set(void *data, u64 val)
{
	struct cortina_epon *ce = data;

	if (val)
		cortina_epon_dying_gasp(ce);
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(ce_dbg_dying_gasp_fops, NULL,
			 ce_dbg_dying_gasp_set, "%llu\n");

void cortina_epon_debugfs_init(struct cortina_epon *ce, struct dentry *parent)
{
	if (!ce || IS_ERR_OR_NULL(parent))
		return;

	ce->dbgfs = debugfs_create_dir("epon", parent);
	if (IS_ERR_OR_NULL(ce->dbgfs))
		return;

	debugfs_create_file("status", 0444, ce->dbgfs, ce, &ce_dbg_status_fops);
	debugfs_create_file("llids", 0444, ce->dbgfs, ce, &ce_dbg_llids_fops);
	debugfs_create_file("counters", 0444, ce->dbgfs, ce,
			    &ce_dbg_counters_fops);
	debugfs_create_file("fec_rx", 0644, ce->dbgfs, ce, &ce_dbg_fec_rx_fops);
	debugfs_create_file("fec_tx", 0644, ce->dbgfs, ce, &ce_dbg_fec_tx_fops);
	debugfs_create_file("laser_mode", 0644, ce->dbgfs, ce,
			    &ce_dbg_laser_mode_fops);
	debugfs_create_file("tx_inhibit", 0200, ce->dbgfs, ce,
			    &ce_dbg_tx_inhibit_fops);
	debugfs_create_file("llid_count", 0644, ce->dbgfs, ce,
			    &ce_dbg_llid_count_fops);
	debugfs_create_file("crypto_key", 0200, ce->dbgfs, ce,
			    &ce_dbg_crypto_key_fops);
	debugfs_create_file("dying_gasp", 0200, ce->dbgfs, ce,
			    &ce_dbg_dying_gasp_fops);
	debugfs_create_u32("holdover_ms", 0644, ce->dbgfs, &ce->holdover_ms);
}

/* ---- Public lifecycle API ---- */

struct cortina_epon *cortina_epon_create(const struct cortina_epon_plat *plat)
{
	struct cortina_epon *ce;

	if (!plat || !plat->dev || !plat->pon || !plat->glb)
		return ERR_PTR(-EINVAL);

	ce = kzalloc(sizeof(*ce), GFP_KERNEL);
	if (!ce)
		return ERR_PTR(-ENOMEM);

	ce->plat = *plat;
	mutex_init(&ce->lock);
	INIT_DELAYED_WORK(&ce->poll_work, ce_poll_work);
	INIT_DELAYED_WORK(&ce->tx_inhibit_work, ce_tx_inhibit_work);

	ce->llid_count = 1;
	ce->holdover_ms = CE_MPCP_HOLDOVER_DEFAULT_MS;
	ce->laser_mode = CORTINA_EPON_LASER_BURST;

	if (plat->mac_addr && is_valid_ether_addr(plat->mac_addr))
		ether_addr_copy(ce->mac_addr, plat->mac_addr);
	else
		eth_random_addr(ce->mac_addr);

	ce_singleton = ce;
	return ce;
}

void cortina_epon_destroy(struct cortina_epon *ce)
{
	if (!ce)
		return;
	cortina_epon_stop(ce);
	debugfs_remove_recursive(ce->dbgfs);
	if (ce_singleton == ce)
		ce_singleton = NULL;
	kfree(ce);
}

int cortina_epon_start(struct cortina_epon *ce, struct xpon_device *xpon)
{
	int ret;

	if (!ce)
		return -ENODEV;

	mutex_lock(&ce->lock);
	if (ce->active) {
		mutex_unlock(&ce->lock);
		return 0;
	}

	ce->xpon = xpon;
	ce->reg_mask = 0;
	ce->los = true;
	ce->link_1g = false;
	ce->nack_backoff = false;
	memset(ce->llids, 0, sizeof(ce->llids));
	memset(&ce->fec, 0, sizeof(ce->fec));

	ce_glb_reset(ce);
	ce_psds_init(ce);
	ret = ce_mac_init(ce);
	if (ret) {
		mutex_unlock(&ce->lock);
		return ret;
	}

	if (ce->plat.tx_pins)
		ce->plat.tx_pins(ce->plat.cookie, true);
	if (ce->plat.bosa_cal) {
		mdelay(10);
		cg_bosa_init(ce->plat.dev, ce->plat.bosa_cal);
	}

	if (xpon && IS_ENABLED(CONFIG_XPON_OAM)) {
		ce->oam = xpon_oam_register(xpon, &ce_oam_ops, ce);
		if (IS_ERR(ce->oam)) {
			dev_warn(ce->plat.dev,
				 "epon: xpon_oam_register failed: %ld\n",
				 PTR_ERR(ce->oam));
			ce->oam = NULL;
		}
	}

	if (IS_REACHABLE(CONFIG_CORTINA_NI)) {
		cortina_ni_pon_oam_rx_hook_set(ce_ni_oam_rx);
		if (ce->plat.wan_ndev)
			cortina_ni_pon_wan_ndev_set(ce->plat.wan_ndev, NULL);
	}

	ce->active = true;
	if (xpon)
		xpon_device_report_registration(xpon, XPON_REGISTRATION_DOWN);

	schedule_delayed_work(&ce->poll_work, CE_MPCP_POLL_INTERVAL);
	dev_info(ce->plat.dev, "epon: 1G EPON MAC initialized (ONU %pM)\n",
		 ce->mac_addr);
	mutex_unlock(&ce->lock);
	return 0;
}

void cortina_epon_stop(struct cortina_epon *ce)
{
	unsigned int i;

	if (!ce)
		return;

	mutex_lock(&ce->lock);
	if (!ce->active) {
		mutex_unlock(&ce->lock);
		return;
	}
	ce->active = false;
	mutex_unlock(&ce->lock);

	cancel_delayed_work_sync(&ce->poll_work);
	cancel_delayed_work_sync(&ce->tx_inhibit_work);

	mutex_lock(&ce->lock);
	if (IS_REACHABLE(CONFIG_CORTINA_NI))
		cortina_ni_pon_oam_rx_hook_set(NULL);

	for (i = 0; i < ce->llid_count; i++) {
		if (ce->llids[i].registered) {
			ce->llids[i].registered = false;
			if (ce->oam)
				xpon_oam_llid_unregistered(ce->oam, i);
		}
		ce_llid_table_update(ce, false, i, i, ce->llids[i].llid, false);
		ce_crypto_clear_llid(ce, i);
	}

	if (ce->oam) {
		xpon_oam_unregister(ce->oam);
		ce->oam = NULL;
	}

	/* Disable MPCP, 1G EPON MAC TX/RX, and burst enable */
	ce_rmw(ce, CE_MPCP_START, BIT(0), 0);
	ce_wr(ce, CE_MPCP_IE, 0);
	ce_wr(ce, CE_EPON_GLB_IE, 0);
	ce_rmw(ce, CE_EPON_1G_TX_CFG, CE_EPON_1G_TX_EN, 0);
	ce_rmw(ce, CE_EPON_1G_RX_CFG, CE_EPON_1G_RX_EN, 0);
	ce_glb_rmw(ce, CE_GLB_PSDS_INIT, CE_PSDS_BEN_OEN, 0);
	if (ce->plat.tx_pins)
		ce->plat.tx_pins(ce->plat.cookie, false);
	if (ce->plat.wan_ndev)
		netif_carrier_off(ce->plat.wan_ndev);

	ce->mac_ready = false;
	ce->reg_mask = 0;
	dev_info(ce->plat.dev, "epon: 1G EPON MAC stopped\n");
	mutex_unlock(&ce->lock);
}

bool cortina_epon_active(const struct cortina_epon *ce)
{
	return ce && READ_ONCE(ce->active);
}

/* ---- Userspace sysfs ABI (epon-oamd, luci-app-epon, luci-app-xpon) ---- */

ssize_t cortina_epon_state_show(struct cortina_epon *ce, char *buf)
{
	ssize_t n;

	if (!ce)
		return -ENODEV;

	mutex_lock(&ce->lock);
	n = sysfs_emit(buf,
		       "mode=%s signal=%u sync=%u registered=0x%x oam_link=%u generation=%u holdover_ms=%u nack_silent=%u\n",
		       ce->active ? "epon" : "gpon",
		       ce->active ? !ce->los : 0,
		       ce->active ? ce->link_1g : 0,
		       ce->active ? ce->reg_mask : 0,
		       ce->active ? ce->oam_link : 0,
		       ce->generation,
		       ce->holdover_ms,
		       ce->nack_backoff);
	mutex_unlock(&ce->lock);
	return n;
}

ssize_t cortina_epon_mac_show(struct cortina_epon *ce, char *buf)
{
	ssize_t n;

	if (!ce)
		return -ENODEV;

	mutex_lock(&ce->lock);
	n = sysfs_emit(buf, "%pM\n", ce->mac_addr);
	mutex_unlock(&ce->lock);
	return n;
}

ssize_t cortina_epon_mac_store(struct cortina_epon *ce, const char *buf,
			       size_t count)
{
	u8 mac[ETH_ALEN];

	if (!ce)
		return -ENODEV;
	if (!mac_pton(buf, mac) || !is_valid_ether_addr(mac))
		return -EINVAL;

	mutex_lock(&ce->lock);
	ether_addr_copy(ce->mac_addr, mac);
	if (ce->active && ce->mac_ready)
		ce_mpcp_program_mac(ce);
	mutex_unlock(&ce->lock);
	return count;
}

ssize_t cortina_epon_oam_link_store(struct cortina_epon *ce, const char *buf,
				    size_t count)
{
	unsigned int up = 0, gen = 0;
	int n;

	if (!ce)
		return -ENODEV;

	n = sscanf(buf, "%u %u", &up, &gen);
	if (n < 1 || up > 1)
		return -EINVAL;

	mutex_lock(&ce->lock);
	if (up && n == 2 && gen != ce->generation) {
		mutex_unlock(&ce->lock);
		return -ESTALE;
	}
	ce->oam_link = !!up;
	mutex_unlock(&ce->lock);

	sysfs_notify(&ce->plat.dev->kobj, NULL, "epon_state");
	return count;
}

ssize_t cortina_epon_register_store(struct cortina_epon *ce, const char *buf,
				    size_t count)
{
	bool en;
	int ret;

	if (!ce)
		return -ENODEV;
	ret = kstrtobool(buf, &en);
	if (ret)
		return ret;

	mutex_lock(&ce->lock);
	if (!ce->active || !ce->mac_ready) {
		mutex_unlock(&ce->lock);
		return -EAGAIN;
	}
	if (en) {
		ce->nack_backoff = false;
		ce_rmw(ce, CE_MPCP_START, BIT(0), BIT(0));
	} else {
		ce_rmw(ce, CE_MPCP_START, BIT(0), 0);
		ce->oam_link = false;
	}
	mutex_unlock(&ce->lock);

	sysfs_notify(&ce->plat.dev->kobj, NULL, "epon_state");
	return count;
}

ssize_t cortina_epon_key_store(struct cortina_epon *ce, const char *buf,
			       size_t count)
{
	unsigned int ch, slot, k0, k1, k2;
	u8 key[3];
	int ret;

	if (!ce)
		return -ENODEV;
	if (sscanf(buf, "%u %u %2x%2x%2x", &ch, &slot, &k0, &k1, &k2) != 5)
		return -EINVAL;

	key[0] = k0;
	key[1] = k1;
	key[2] = k2;

	mutex_lock(&ce->lock);
	ret = ce_crypto_set_key_1g(ce, ch, slot, key);
	mutex_unlock(&ce->lock);
	return ret ? ret : count;
}

ssize_t cortina_epon_holdover_show(struct cortina_epon *ce, char *buf)
{
	ssize_t n;

	if (!ce)
		return -ENODEV;

	mutex_lock(&ce->lock);
	n = sysfs_emit(buf, "%u\n", ce->holdover_ms);
	mutex_unlock(&ce->lock);
	return n;
}

ssize_t cortina_epon_holdover_store(struct cortina_epon *ce, const char *buf,
				    size_t count)
{
	u32 ms;
	int ret;

	if (!ce)
		return -ENODEV;
	ret = kstrtou32(buf, 10, &ms);
	if (ret || ms > 10000)
		return ret ? ret : -EINVAL;

	mutex_lock(&ce->lock);
	ce->holdover_ms = ms;
	mutex_unlock(&ce->lock);

	sysfs_notify(&ce->plat.dev->kobj, NULL, "epon_state");
	return count;
}

ssize_t cortina_epon_tx_hold_show(struct cortina_epon *ce, char *buf)
{
	ssize_t n;

	if (!ce)
		return -ENODEV;

	mutex_lock(&ce->lock);
	n = sysfs_emit(buf, "%u\n", ce->tx_inhibit_action == 0xffff);
	mutex_unlock(&ce->lock);
	return n;
}

ssize_t cortina_epon_tx_hold_store(struct cortina_epon *ce, const char *buf,
				   size_t count)
{
	bool hold;
	int ret;

	if (!ce)
		return -ENODEV;
	ret = kstrtobool(buf, &hold);
	if (ret)
		return ret;

	mutex_lock(&ce->lock);
	ce->tx_inhibit_action = hold ? 0xffff : 0;
	cancel_delayed_work(&ce->tx_inhibit_work);
	if (ce->active && ce->mac_ready)
		ce_laser_apply(ce);
	mutex_unlock(&ce->lock);
	return count;
}

ssize_t cortina_epon_tx_hold_seconds_show(struct cortina_epon *ce, char *buf)
{
	unsigned int rem = 0;

	if (!ce)
		return -ENODEV;

	mutex_lock(&ce->lock);
	if (ce->tx_inhibit_action && ce->tx_inhibit_action != 0xffff &&
	    time_before(jiffies, ce->tx_inhibit_until))
		rem = jiffies_to_msecs(ce->tx_inhibit_until - jiffies) / 1000;
	mutex_unlock(&ce->lock);
	return sysfs_emit(buf, "%u\n", rem);
}

ssize_t cortina_epon_tx_hold_seconds_store(struct cortina_epon *ce,
					   const char *buf, size_t count)
{
	u32 sec;
	int ret;

	if (!ce)
		return -ENODEV;
	ret = kstrtou32(buf, 10, &sec);
	if (ret || sec >= 0xffff)
		return ret ? ret : -EINVAL;

	mutex_lock(&ce->lock);
	if (ce->tx_inhibit_action == 0xffff) {
		mutex_unlock(&ce->lock);
		return -EBUSY;
	}
	ce->tx_inhibit_action = sec;
	if (ce->active && ce->mac_ready)
		ce_laser_apply(ce);
	if (sec) {
		ce->tx_inhibit_until = jiffies + msecs_to_jiffies(sec * 1000);
		mod_delayed_work(system_wq, &ce->tx_inhibit_work,
				 msecs_to_jiffies(sec * 1000));
	} else {
		cancel_delayed_work(&ce->tx_inhibit_work);
	}
	mutex_unlock(&ce->lock);
	return count;
}
