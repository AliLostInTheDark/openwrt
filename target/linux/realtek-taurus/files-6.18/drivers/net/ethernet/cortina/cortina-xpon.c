// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 *
 * Cortina GPON MAC glue for the xPON/OMCI framework (net/xpon).
 *
 * The framework owns the OMCI channel towards userspace (omcid) and the
 * registration state published to the rest of the system.  This file moves
 * OMCI PDUs between it and the MAC driver and reports the MAC's activation
 * state, ONU-ID, OMCC GEM port and loss of signal.  The MAC driver supplies
 * the hardware through struct cx_hw_ops, so nothing here touches registers.
 *
 * The MAC driver installs the hardware data path itself from the OLT's T-CONT
 * and GEM provisioning, which it follows in the OMCI it receives, so
 * set_tcont and set_gem_port have nothing to program.
 */

#include <linux/crc32.h>
#include <linux/ethtool.h>
#include <linux/if_bridge.h>
#include <linux/if_vlan.h>
#include <linux/linkmode.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <linux/skbuff.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>
#include <net/netlink.h>
#include <net/rtnetlink.h>
#include <net/xpon.h>
#include <net/xpon/omci.h>

#include "cortina-gpon-bosa.h"
#include "cortina-gpon-ddm.h"
#include "cortina-xpon.h"

#define CX_OMCI_PDU_LEN		48
#define CX_OMCI_MIC_LEN		4
#define CX_OMCI_EXT_HDR_LEN	10
#define CX_OMCI_BASELINE_ID	0x0a
#define CX_OMCI_EXTENDED_ID	0x0b
#define CX_STATE_O5		5

#define CX_TCONT_BASE		0x8000

static const bool cx_enable = true;

static struct {
	struct xpon_device *xpon;
	struct omci_device *odev;
	struct net_device *wan;
	struct device *dev;
	void *cookie;
	const struct cx_hw_ops *hw;
	struct mutex lock;		/* serialises OMCI backend calls */
	struct work_struct event_work;
	atomic_long_t ev;
	int chan_gem;			/* last OMCC GEM given to the framework, -1 none */
	int state;			/* last state given to the framework */
	bool operational;
	struct work_struct uni_work;
	unsigned long uni_locked;	/* LAN ports the OLT locked, bit N-1 for lanN */
	unsigned long uni_applied;	/* the locks taken down so far (uni_work) */
	struct omci_uni_attrs uni_attrs[4];	/* the OLT's PPTP UNI attributes, lanN */
	unsigned long uni_attrs_set;	/* uni_attrs[N-1] holds a value */
	unsigned long uni_attrs_dirty;	/* not applied yet (uni_work) */
} cx = {
	.lock = __MUTEX_INITIALIZER(cx.lock),
	.chan_gem = -1,
};

bool cx_active(void)
{
	return READ_ONCE(cx.odev);
}

struct cx_service {
	u32 cookie;
	u16 gem;
	u16 vlan;			/* VID on the PON */
	u16 uni_vid;			/* VID on the router side, when translated */
	u8 mode;			/* enum omci_uni_vlan_mode */
	u8 pcp;
	bool pcp_valid;
	bool vlan_valid;
	bool def;
	bool multicast;
	bool used;
};

static struct cx_service cx_svc[CX_SERVICES];
/* one past the highest used slot: the per-packet lookups stop there */
static unsigned int cx_svc_end;
static DEFINE_SPINLOCK(cx_svc_lock);

struct cx_bucket {
	u64 rate;			/* bytes/s, 0 = unlimited */
	u64 burst;			/* bytes */
	u64 tokens;			/* bytes x NSEC_PER_SEC */
	u64 stamp;			/* ns */
};

static struct {
	u16 gem;
	bool used;
	struct cx_bucket b[2];		/* [0] downstream, [1] upstream */
} cx_rate[CX_DATA_GEM_MAX];
/* GEMs with a peak rate; none set lets every frame skip the policer */
static unsigned int cx_rate_used;
static DEFINE_SPINLOCK(cx_rate_lock);

/* ---- OMCI device operations ---- */

static int cx_backend_begin(struct omci_device *odev)
{
	return mutex_trylock(&cx.lock) ? 0 : -EBUSY;
}

static void cx_backend_end(struct omci_device *odev)
{
	mutex_unlock(&cx.lock);
}

static int cx_backend_identity(struct omci_device *odev,
			       const struct omci_identity *id)
{
	int ret;

	if (id->valid & OMCI_IDENTITY_F_PASSWORD) {
		ret = cx.hw->set_password(cx.cookie, id->password);
		if (ret)
			return ret;
	}
	if (!(id->valid & OMCI_IDENTITY_F_SERIAL_NUMBER))
		return 0;
	return cx.hw->set_serial(cx.cookie, id->serial_number);
}

static int cx_backend_reset(struct omci_device *odev)
{
	spin_lock_bh(&cx_svc_lock);
	memset(cx_svc, 0, sizeof(cx_svc));
	cx_svc_end = 0;
	spin_unlock_bh(&cx_svc_lock);
	spin_lock_bh(&cx_rate_lock);
	memset(cx_rate, 0, sizeof(cx_rate));
	WRITE_ONCE(cx_rate_used, 0);
	spin_unlock_bh(&cx_rate_lock);
	return 0;
}

static int cx_start(struct omci_device *odev)
{
	return 0;
}

static void cx_stop(struct omci_device *odev)
{
}

static int cx_xmit(struct omci_device *odev, struct sk_buff *skb, u16 gem_port_id)
{
	int gem = cx.hw->omcc_gem(cx.cookie);
	int rc;

	if (gem < 0 || gem_port_id != gem || cx.hw->state(cx.cookie) != CX_STATE_O5)
		return -ENOLINK;
	if (skb->len < 4)
		return -EMSGSIZE;
	/* The NI transmit path carries the fixed 48-byte baseline PDU only. */
	if (skb->data[3] != CX_OMCI_BASELINE_ID)
		return -EPROTONOSUPPORT;

	/* omcid supplies the PDU without its integrity field. */
	if (skb->len == CX_OMCI_PDU_LEN - CX_OMCI_MIC_LEN) {
		u32 mic = ~crc32_be(~0U, skb->data, skb->len);

		if (skb_tailroom(skb) < CX_OMCI_MIC_LEN &&
		    pskb_expand_head(skb, 0, CX_OMCI_MIC_LEN, GFP_ATOMIC))
			return -ENOMEM;
		put_unaligned_be32(mic, skb_put(skb, CX_OMCI_MIC_LEN));
	} else if (skb->len != CX_OMCI_PDU_LEN) {
		return -EMSGSIZE;
	}

	rc = cx.hw->omci_tx(cx.cookie, skb->data);
	if (!rc)
		dev_kfree_skb_any(skb);
	return rc;
}

static int cx_get_ani_topology(struct omci_device *odev,
			       struct omci_ani_topology *t)
{
	/* Data T-CONTs with the eight upstream queues of their VoQ banks. */
	*t = (struct omci_ani_topology) {
		.tcont_base = CX_TCONT_BASE,
		.scheduler_base = 0x8000,
		.queue_base = 0x8000,
		.maximum_queue_size = 3276,
		.allocated_queue_size = 3276,
		.tcont_count = CX_DATA_TCONT_MAX,
		.queues_per_tcont = 8,
		.queue_config_option = 1,
		.scheduler_policy = 2,
		.gem_port_count = CX_DATA_GEM_MAX,
		.qos_configuration = BIT(3) | BIT(4),
		.downstream_queues_per_uni = 8,
		/* PPTP UNI 0x0101 and VEIP 0x0a01 must stay apart */
		.flags = OMCI_TOPOLOGY_F_FULL_UNI_ENTITY,
	};
	return 0;
}

static int cx_set_tcont(struct omci_device *odev, u16 entity, u16 alloc_id,
			bool valid)
{
	return 0;
}

static int cx_set_gem_port(struct omci_device *odev, u16 entity, u16 gem_port_id,
			   u16 tcont_entity_id, u8 direction, bool valid,
			   bool encrypted)
{
	return 0;
}

/*
 * GEM policers. Every data frame passes the CPU (pon0 is the VEIP), so the
 * peak rate of a GEM's traffic descriptor (ME 280) is enforced here with a
 * token bucket per GEM and direction, upstream in cx_us_service() and
 * downstream in cx_ds_service().
 */

/* At least a few full-size frames, else a tenth of a second at the rate. */
#define CX_BURST_MIN		(4 * 1522)

static int cx_set_gem_rate(struct omci_device *odev, u16 gem, bool upstream,
			   u32 rate, u32 burst)
{
	struct cx_bucket *b;
	unsigned int i, free = CX_DATA_GEM_MAX;

	spin_lock_bh(&cx_rate_lock);
	for (i = 0; i < CX_DATA_GEM_MAX; i++) {
		if (cx_rate[i].used && cx_rate[i].gem == gem)
			break;
		if (!cx_rate[i].used && free == CX_DATA_GEM_MAX)
			free = i;
	}
	if (i == CX_DATA_GEM_MAX) {
		if (!rate || free == CX_DATA_GEM_MAX) {
			spin_unlock_bh(&cx_rate_lock);
			return rate ? -ENOSPC : 0;
		}
		i = free;
		memset(&cx_rate[i], 0, sizeof(cx_rate[i]));
		cx_rate[i].gem = gem;
		cx_rate[i].used = true;
		WRITE_ONCE(cx_rate_used, cx_rate_used + 1);
	}
	b = &cx_rate[i].b[upstream];
	b->rate = rate;
	b->burst = max_t(u64, burst ?: rate / 10, CX_BURST_MIN);
	b->tokens = b->burst * NSEC_PER_SEC;
	b->stamp = ktime_get_ns();
	if (!cx_rate[i].b[0].rate && !cx_rate[i].b[1].rate) {
		cx_rate[i].used = false;
		WRITE_ONCE(cx_rate_used, cx_rate_used - 1);
	}
	spin_unlock_bh(&cx_rate_lock);

	dev_dbg(cx.dev, "GEM %u %s peak %u B/s burst %llu B\n", gem,
		upstream ? "upstream" : "downstream", rate, b->burst);
	return 0;
}

/* True when a frame of @len bytes on @gem exceeds its peak rate. */
static bool cx_police(u16 gem, bool upstream, unsigned int len)
{
	struct cx_bucket *b;
	bool drop = false;
	unsigned int i;
	u64 now, max;

	if (!READ_ONCE(cx_rate_used))
		return false;

	spin_lock(&cx_rate_lock);
	for (i = 0; i < CX_DATA_GEM_MAX; i++)
		if (cx_rate[i].used && cx_rate[i].gem == gem)
			break;
	if (i < CX_DATA_GEM_MAX && cx_rate[i].b[upstream].rate) {
		b = &cx_rate[i].b[upstream];
		now = ktime_get_ns();
		max = b->burst * NSEC_PER_SEC;
		/* a second of refill at most keeps the product in 64 bits */
		b->tokens = min(max, b->tokens +
				min_t(u64, now - b->stamp, NSEC_PER_SEC) * b->rate);
		b->stamp = now;
		if (b->tokens < (u64)len * NSEC_PER_SEC)
			drop = true;
		else
			b->tokens -= (u64)len * NSEC_PER_SEC;
	}
	spin_unlock(&cx_rate_lock);
	return drop;
}

#define CX_ETHERNET_SLOT	1
#define CX_LAN_PORTS		4

static_assert(ARRAY_SIZE(cx.uni_attrs) == CX_LAN_PORTS);

/* uni_attrs and its bitmaps, between the backend and uni_work */
static DEFINE_SPINLOCK(cx_uni_lock);

/*
 * G.988 9.5.1 auto-detection configuration: the speed (0 any) and duplex
 * (DUPLEX_UNKNOWN any) a code allows.  False for codes we do not know.
 */
static bool cx_uni_mode(u8 code, int *speed, int *duplex)
{
	static const struct { u8 code; int speed, duplex; } modes[] = {
		{ 0x00, 0, DUPLEX_UNKNOWN },	{ 0x01, SPEED_10, DUPLEX_FULL },
		{ 0x02, SPEED_100, DUPLEX_FULL }, { 0x03, SPEED_1000, DUPLEX_FULL },
		{ 0x04, 0, DUPLEX_FULL },	{ 0x06, SPEED_10000, DUPLEX_FULL },
		{ 0x07, SPEED_2500, DUPLEX_FULL }, { 0x08, SPEED_5000, DUPLEX_FULL },
		{ 0x10, SPEED_10, DUPLEX_UNKNOWN }, { 0x11, SPEED_10, DUPLEX_HALF },
		{ 0x12, SPEED_100, DUPLEX_HALF }, { 0x13, SPEED_1000, DUPLEX_HALF },
		{ 0x14, 0, DUPLEX_HALF },	{ 0x20, SPEED_1000, DUPLEX_UNKNOWN },
		{ 0x30, SPEED_100, DUPLEX_UNKNOWN },
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(modes); i++) {
		if (modes[i].code == code) {
			*speed = modes[i].speed;
			*duplex = modes[i].duplex;
			return true;
		}
	}
	return false;
}

/*
 * Restrict what the port advertises to the modes the OLT allows; autoneg
 * stays on, so the link partner still learns them.  Left alone when nothing
 * changes, since a new advertisement restarts negotiation.
 */
static void cx_uni_autodetect(struct net_device *dev, u8 code)
{
	static const struct { int bit, speed, duplex; } modes[] = {
		{ ETHTOOL_LINK_MODE_10baseT_Half_BIT, SPEED_10, DUPLEX_HALF },
		{ ETHTOOL_LINK_MODE_10baseT_Full_BIT, SPEED_10, DUPLEX_FULL },
		{ ETHTOOL_LINK_MODE_100baseT_Half_BIT, SPEED_100, DUPLEX_HALF },
		{ ETHTOOL_LINK_MODE_100baseT_Full_BIT, SPEED_100, DUPLEX_FULL },
		{ ETHTOOL_LINK_MODE_1000baseT_Half_BIT, SPEED_1000, DUPLEX_HALF },
		{ ETHTOOL_LINK_MODE_1000baseT_Full_BIT, SPEED_1000, DUPLEX_FULL },
		{ ETHTOOL_LINK_MODE_1000baseX_Full_BIT, SPEED_1000, DUPLEX_FULL },
		{ ETHTOOL_LINK_MODE_2500baseT_Full_BIT, SPEED_2500, DUPLEX_FULL },
		{ ETHTOOL_LINK_MODE_2500baseX_Full_BIT, SPEED_2500, DUPLEX_FULL },
		{ ETHTOOL_LINK_MODE_5000baseT_Full_BIT, SPEED_5000, DUPLEX_FULL },
		{ ETHTOOL_LINK_MODE_10000baseT_Full_BIT, SPEED_10000, DUPLEX_FULL },
	};
	struct ethtool_link_ksettings ks;
	__ETHTOOL_DECLARE_LINK_MODE_MASK(adv);
	int speed, duplex, left = 0;
	unsigned int i;

	/* auto/auto keeps the board's own advertisement (lan4's 2.5G setup) */
	if (!code || !cx_uni_mode(code, &speed, &duplex) ||
	    !dev->ethtool_ops->set_link_ksettings ||
	    __ethtool_get_link_ksettings(dev, &ks))
		return;

	linkmode_copy(adv, ks.link_modes.supported);
	for (i = 0; i < ARRAY_SIZE(modes); i++) {
		if ((speed && modes[i].speed != speed) ||
		    (duplex != DUPLEX_UNKNOWN && modes[i].duplex != duplex))
			linkmode_clear_bit(modes[i].bit, adv);
		else if (linkmode_test_bit(modes[i].bit, adv))
			left++;
	}
	if (!left) {
		dev_warn(cx.dev, "%s: auto-detection 0x%02x allows no supported mode\n",
			 dev->name, code);
		return;
	}
	if (ks.base.autoneg == AUTONEG_ENABLE &&
	    linkmode_equal(adv, ks.link_modes.advertising))
		return;
	linkmode_copy(ks.link_modes.advertising, adv);
	ks.base.autoneg = AUTONEG_ENABLE;
	if (dev->ethtool_ops->set_link_ksettings(dev, &ks))
		dev_warn(cx.dev, "%s: auto-detection 0x%02x not applied\n",
			 dev->name, code);
}

/* pause frames on or off; a pause time of 0 turns them off (G.988 9.5.1) */
static void cx_uni_pause(struct net_device *dev, u16 quanta)
{
	struct ethtool_pauseparam p = { };

	if (!dev->ethtool_ops->get_pauseparam || !dev->ethtool_ops->set_pauseparam)
		return;
	dev->ethtool_ops->get_pauseparam(dev, &p);
	if (p.rx_pause == !!quanta && p.tx_pause == !!quanta)
		return;
	p.rx_pause = p.tx_pause = !!quanta;
	dev->ethtool_ops->set_pauseparam(dev, &p);
}

/*
 * Ageing and the MAC learning limit belong to the bridge the LAN port is in:
 * through the bridge they reach the switch (switchdev ageing).  0 restores
 * the default ageing and lifts the limit.
 */
static void cx_uni_bridge(struct net_device *dev, u32 ageing_s, u16 learn_limit)
{
	struct net_device *br = netdev_master_upper_dev_get(dev);
	struct {
		struct nlattr nla;
		u32 val;
	} ageing = {
		.nla = { .nla_len = nla_attr_size(sizeof(u32)),
			 .nla_type = IFLA_BR_AGEING_TIME },
		.val = (ageing_s ?: BR_DEFAULT_AGEING_TIME / HZ) * USER_HZ,
	}, limit = {
		.nla = { .nla_len = nla_attr_size(sizeof(u32)),
			 .nla_type = IFLA_BR_FDB_MAX_LEARNED },
		.val = learn_limit,
	};
	struct nlattr *data[IFLA_BR_MAX + 1] = {
		[IFLA_BR_AGEING_TIME] = &ageing.nla,
		[IFLA_BR_FDB_MAX_LEARNED] = &limit.nla,
	};
	struct nlattr *tb[IFLA_MAX + 1] = { };

	if (!br || !netif_is_bridge_master(br) || !br->rtnl_link_ops->changelink)
		return;
	if (br->rtnl_link_ops->changelink(br, tb, data, NULL))
		dev_warn(cx.dev, "%s: ageing/learning limit not applied\n", br->name);
}

/*
 * The maximum frame size only ever raises the port MTU: a smaller one would
 * cut the LAN below what the rest of the router uses.
 */
static void cx_uni_frame_size(struct net_device *dev, u16 max_frame)
{
	int mtu = (int)max_frame - ETH_HLEN - ETH_FCS_LEN;

	mtu = min_t(int, mtu, dev->max_mtu);
	if (mtu > (int)dev->mtu && dev_set_mtu(dev, mtu))
		dev_warn(cx.dev, "%s: MTU %d not applied\n", dev->name, mtu);
}

static void cx_uni_attrs_apply(struct net_device *dev,
			       const struct omci_uni_attrs *a)
{
	cx_uni_pause(dev, a->pause_quanta);
	cx_uni_autodetect(dev, a->auto_detection);
	cx_uni_frame_size(dev, a->max_frame_size);
	cx_uni_bridge(dev, a->ageing_s, a->learn_limit);
	if (a->down_kbps || a->up_kbps)
		dev_info(cx.dev, "%s: UNI rate limit %u/%u kbit/s not supported\n",
			 dev->name, a->down_kbps, a->up_kbps);
}

/*
 * Apply the OLT's settings for the LAN ports: the administrative state
 * (locked is down) and the PPTP Ethernet UNI attributes.
 */
static void cx_uni_fn(struct work_struct *work)
{
	unsigned long locked = READ_ONCE(cx.uni_locked);
	unsigned long changed = locked ^ cx.uni_applied;
	struct omci_uni_attrs attrs[CX_LAN_PORTS];
	unsigned long dirty;
	unsigned int port;

	spin_lock(&cx_uni_lock);
	dirty = cx.uni_attrs_dirty;
	cx.uni_attrs_dirty = 0;
	memcpy(attrs, cx.uni_attrs, sizeof(attrs));
	spin_unlock(&cx_uni_lock);

	/* only ports whose lock changed: a port down for other reasons stays so */
	cx.uni_applied = locked;
	rtnl_lock();
	for (port = 1; port <= CX_LAN_PORTS; port++) {
		struct net_device *dev;
		char name[IFNAMSIZ];
		bool lock = test_bit(port - 1, &locked);

		if (!test_bit(port - 1, &changed) && !test_bit(port - 1, &dirty))
			continue;
		snprintf(name, sizeof(name), "lan%u", port);
		dev = __dev_get_by_name(&init_net, name);
		if (!dev)
			continue;
		if (test_bit(port - 1, &dirty))
			cx_uni_attrs_apply(dev, &attrs[port - 1]);
		if (!test_bit(port - 1, &changed))
			continue;
		dev_info(cx.dev, "%s %s by the OLT\n", name,
			 lock ? "locked" : "unlocked");
		dev_change_flags(dev, lock ? dev->flags & ~IFF_UP :
					     dev->flags | IFF_UP, NULL);
	}
	rtnl_unlock();
}

/*
 * An administrative lock of a PPTP Ethernet UNI takes its LAN port down.
 * The VEIP is the router's own WAN: the OLT stops its service instead.
 */
static int cx_set_uni(struct omci_device *odev, u16 entity, bool enable)
{
	unsigned int port = entity & 0xff;
	bool was;

	if (entity >> 8 != CX_ETHERNET_SLOT || !port || port > CX_LAN_PORTS)
		return 0;
	was = test_bit(port - 1, &cx.uni_locked);
	if (was == !enable)
		return 0;
	if (enable)
		clear_bit(port - 1, &cx.uni_locked);
	else
		set_bit(port - 1, &cx.uni_locked);
	schedule_work(&cx.uni_work);
	return 0;
}

static bool cx_uni_attrs_equal(const struct omci_uni_attrs *a,
			       const struct omci_uni_attrs *b)
{
	return a->pause_quanta == b->pause_quanta && a->ageing_s == b->ageing_s &&
	       a->learn_limit == b->learn_limit && a->down_kbps == b->down_kbps &&
	       a->up_kbps == b->up_kbps && a->auto_detection == b->auto_detection &&
	       a->max_frame_size == b->max_frame_size;
}

/* applied in uni_work, under RTNL */
static int cx_set_uni_attrs(struct omci_device *odev, u16 entity,
			    const struct omci_uni_attrs *attrs)
{
	unsigned int port = entity & 0xff;
	bool same;

	if (entity >> 8 != CX_ETHERNET_SLOT || !port || port > CX_LAN_PORTS)
		return 0;
	spin_lock(&cx_uni_lock);
	same = test_bit(port - 1, &cx.uni_attrs_set) &&
	       cx_uni_attrs_equal(&cx.uni_attrs[port - 1], attrs);
	if (!same) {
		cx.uni_attrs[port - 1] = *attrs;
		__set_bit(port - 1, &cx.uni_attrs_set);
		__set_bit(port - 1, &cx.uni_attrs_dirty);
	}
	spin_unlock(&cx_uni_lock);
	if (!same)
		schedule_work(&cx.uni_work);
	return 0;
}

/* T-CONT scheduling: WRR weights by priority, otherwise strict priority */
static int cx_set_tcont_qos(struct omci_device *odev, u16 entity,
			    const struct omci_tcont_qos *qos)
{
	bool wrr = qos->enabled && qos->policy == 2;

	return cx.hw->tcont_qos(cx.cookie, entity, wrr ? qos->weight : NULL);
}

static int cx_get_telemetry(struct omci_device *odev, struct omci_telemetry *t)
{
	struct cg_bosa_ddm d;
	struct cx_fec f;

	cx.hw->fec(cx.cookie, &f);
	if (f.downstream >= 0) {
		t->downstream_fec = f.downstream ? OMCI_FEC_STATUS_UP :
				    OMCI_FEC_STATUS_DOWN;
		t->valid |= OMCI_TELEMETRY_F_FEC_DOWNSTREAM;
	}
	if (f.upstream >= 0) {
		t->upstream_fec = f.upstream ? OMCI_FEC_STATUS_UP :
				  OMCI_FEC_STATUS_DOWN;
		t->valid |= OMCI_TELEMETRY_F_FEC_UPSTREAM;
	}
	t->fec_corrected_bytes = f.corrected_bytes;
	t->fec_corrected_codewords = f.corrected_codewords;
	t->fec_uncorrectable_codewords = f.uncorrectable_codewords;
	t->fec_total_codewords = f.total_codewords;
	t->valid |= OMCI_TELEMETRY_F_FEC_COUNTERS;

	if (cg_bosa_ddm_read(cx.dev, &d) != CG_DDM_OK)
		return 0;
	t->bosa_temperature_mc = cg_ddm_temp_mdegc(d.temp);
	t->bosa_voltage_uv = cg_ddm_vcc_mv(d.vcc) * 1000;
	t->bosa_bias_ua = cg_ddm_bias_ua(d.bias);
	t->bosa_tx_power_nw = (u32)d.tx_pwr * 100;	/* 0.1 uW units */
	t->bosa_rx_power_nw = (u32)d.rx_pwr * 100;
	t->valid |= OMCI_TELEMETRY_F_BOSA_TEMPERATURE |
		    OMCI_TELEMETRY_F_BOSA_VOLTAGE |
		    OMCI_TELEMETRY_F_BOSA_BIAS |
		    OMCI_TELEMETRY_F_BOSA_TX_POWER |
		    OMCI_TELEMETRY_F_BOSA_RX_POWER;
	return 0;
}

static void cx_set_operational(struct omci_device *odev, bool operational)
{
	if (operational != READ_ONCE(cx.operational))
		dev_info(cx.dev, "OMCI agent %s\n",
			operational ? "operational" : "not operational");
	WRITE_ONCE(cx.operational, operational);
}

/* ---- VLAN services ---- */

/*
 * omcid hands over each upstream service already normalised from the OLT's
 * ME 84/130/171 graph: which VLAN (and p-bit) the router's side uses, what it
 * becomes on the PON and which GEM it rides. pon0 is the VEIP, so the
 * treatment is applied here in software: cx_us_service() on each frame the
 * router sends, cx_ds_service() on each frame it receives.
 */
static u16 cx_svc_lan_vid(const struct cx_service *s)
{
	return s->mode == OMCI_UNI_VLAN_TRANSLATE ||
	       s->mode == OMCI_UNI_VLAN_STACKED ? s->uni_vid : s->vlan;
}

static int cx_replace_service(struct omci_device *odev,
			      const struct omci_service_config *c)
{
	struct cx_service *slot = NULL, svc;
	unsigned int i;
	bool changed = true;

	spin_lock_bh(&cx_svc_lock);
	for (i = 0; i < CX_SERVICES; i++) {
		if (cx_svc[i].used && cx_svc[i].cookie == c->cookie) {
			slot = &cx_svc[i];
			break;
		}
		if (!slot && !cx_svc[i].used)
			slot = &cx_svc[i];
	}
	svc = (struct cx_service) {
		.cookie = c->cookie,
		.gem = c->gem_port_id,
		.vlan = c->vlan_id & VLAN_VID_MASK,
		.uni_vid = c->uni_vlan_id & VLAN_VID_MASK,
		.mode = c->uni_vlan_mode,
		.pcp = c->pcp & 7,
		.pcp_valid = c->pcp_valid,
		.vlan_valid = c->vlan_valid,
		.def = c->default_service,
		.multicast = c->multicast,
		.used = true,
	};
	if (slot) {
		/* the agent re-applies its whole set on every OLT change */
		changed = !slot->used || slot->gem != svc.gem ||
			  slot->vlan != svc.vlan || slot->uni_vid != svc.uni_vid ||
			  slot->mode != svc.mode || slot->pcp != svc.pcp ||
			  slot->pcp_valid != svc.pcp_valid ||
			  slot->vlan_valid != svc.vlan_valid ||
			  slot->def != svc.def || slot->multicast != svc.multicast;
		*slot = svc;
		cx_svc_end = max_t(unsigned int, cx_svc_end, slot - cx_svc + 1);
	}
	spin_unlock_bh(&cx_svc_lock);
	if (!slot)
		return -ENOSPC;
	if (c->direction & 1)	/* ANI-bound: the GEM's T-CONT schedules it */
		cx.hw->gem_prio(cx.cookie, c->gem_port_id, c->queue);
	if (!changed)
		return 0;
	dev_dbg(cx.dev, "service %u: GEM %u VLAN %u mode %u uni %u%s%s\n",
		c->cookie, c->gem_port_id, c->vlan_valid ? c->vlan_id : 0,
		c->uni_vlan_mode, c->uni_vlan_id,
		c->default_service ? " default" : "",
		c->multicast ? " multicast" : "");
	return 0;
}

static int cx_delete_service(struct omci_device *odev, u32 cookie)
{
	unsigned int i;

	spin_lock_bh(&cx_svc_lock);
	for (i = 0; i < cx_svc_end; i++)
		if (cx_svc[i].used && cx_svc[i].cookie == cookie)
			cx_svc[i].used = false;
	while (cx_svc_end && !cx_svc[cx_svc_end - 1].used)
		cx_svc_end--;
	spin_unlock_bh(&cx_svc_lock);
	return 0;
}

static void cx_vlan_push(struct sk_buff *skb, u16 tci)
{
	memmove(skb_push(skb, VLAN_HLEN), skb->data + VLAN_HLEN, 2 * ETH_ALEN);
	put_unaligned_be16(ETH_P_8021Q, skb->data + 2 * ETH_ALEN);
	put_unaligned_be16(tci, skb->data + 2 * ETH_ALEN + 2);
}

static void cx_vlan_pop(struct sk_buff *skb)
{
	memmove(skb->data + VLAN_HLEN, skb->data, 2 * ETH_ALEN);
	skb_pull(skb, VLAN_HLEN);
}

/* The outer 802.1Q tag of an Ethernet frame at skb->data, or false. */
static bool cx_vlan_tci(const struct sk_buff *skb, u16 *tci)
{
	if (skb->len < ETH_HLEN + VLAN_HLEN ||
	    get_unaligned_be16(skb->data + 2 * ETH_ALEN) != ETH_P_8021Q)
		return false;
	*tci = get_unaligned_be16(skb->data + 2 * ETH_ALEN + 2);
	return true;
}

/*
 * Pick the service for a frame the router sends on pon0 and apply its
 * treatment. A service that names a p-bit beats one that does not; the
 * default service takes what nothing else matches. Returns the GEM port-id,
 * 0 when no service matches (the frame leaves unchanged on the default GEM),
 * or a negative errno when the frame cannot be rewritten or exceeds its
 * GEM's peak rate.
 */
int cx_us_service(struct sk_buff *skb)
{
	const struct cx_service *best = NULL, *def = NULL;
	struct cx_service s;
	bool tagged;
	u16 tci = 0;
	unsigned int i;

	tagged = cx_vlan_tci(skb, &tci);

	spin_lock(&cx_svc_lock);
	for (i = 0; i < cx_svc_end; i++) {
		const struct cx_service *c = &cx_svc[i];

		if (!c->used || c->multicast)
			continue;
		if (c->def) {
			def = def ?: c;
			continue;
		}
		if (!c->vlan_valid)
			continue;
		if (c->mode == OMCI_UNI_VLAN_UNTAGGED ? tagged :
		    !tagged || (tci & VLAN_VID_MASK) != cx_svc_lan_vid(c))
			continue;
		if (c->pcp_valid && (!tagged || tci >> VLAN_PRIO_SHIFT != c->pcp))
			continue;
		if (!best || (c->pcp_valid && !best->pcp_valid))
			best = c;
	}
	best = best ?: def;
	if (best)
		s = *best;
	spin_unlock(&cx_svc_lock);

	if (!best)
		return 0;
	if (skb_ensure_writable(skb, ETH_HLEN + VLAN_HLEN) ||
	    skb_cow_head(skb, VLAN_HLEN))
		return -ENOMEM;

	if (s.vlan_valid) {
		u16 pcp = s.pcp_valid ? s.pcp : (tci >> VLAN_PRIO_SHIFT);

		switch (s.mode) {
		case OMCI_UNI_VLAN_TRANSLATE:
			if (tagged)
				put_unaligned_be16((tci & ~VLAN_VID_MASK) | s.vlan,
						   skb->data + 2 * ETH_ALEN + 2);
			break;
		case OMCI_UNI_VLAN_UNTAGGED:
		case OMCI_UNI_VLAN_STACKED:
			if (s.mode == OMCI_UNI_VLAN_STACKED || !tagged)
				cx_vlan_push(skb, pcp << VLAN_PRIO_SHIFT | s.vlan);
			break;
		}
	}
	if (cx_police(s.gem, true, skb->len))
		return -ENOBUFS;
	return s.gem;
}

/*
 * Undo the service treatment on a downstream frame before pon0 receives it:
 * a translated VID goes back to the router's, a pushed tag comes off.
 * Returns true when the frame exceeds its GEM's peak rate and must be dropped.
 */
bool cx_ds_service(struct sk_buff *skb)
{
	struct cx_service s = { };
	unsigned int i;
	u16 tci;

	if (!cx_vlan_tci(skb, &tci))
		return false;

	spin_lock(&cx_svc_lock);
	for (i = 0; i < cx_svc_end; i++)
		if (cx_svc[i].used && cx_svc[i].vlan_valid &&
		    cx_svc[i].mode != OMCI_UNI_VLAN_SAME &&
		    cx_svc[i].vlan == (tci & VLAN_VID_MASK)) {
			s = cx_svc[i];
			break;
		}
	spin_unlock(&cx_svc_lock);

	if (!s.used)
		return false;
	if (cx_police(s.gem, false, skb->len))
		return true;
	if (skb_ensure_writable(skb, ETH_HLEN + VLAN_HLEN))
		return false;
	if (s.mode == OMCI_UNI_VLAN_TRANSLATE)
		put_unaligned_be16((tci & ~VLAN_VID_MASK) | s.uni_vid,
				   skb->data + 2 * ETH_ALEN + 2);
	else
		cx_vlan_pop(skb);
	return false;
}

static const struct omci_device_ops cx_ops = {
	.backend_begin		= cx_backend_begin,
	.backend_end		= cx_backend_end,
	.backend_identity	= cx_backend_identity,
	.backend_reset		= cx_backend_reset,
	.start			= cx_start,
	.stop			= cx_stop,
	.xmit			= cx_xmit,
	.get_ani_topology	= cx_get_ani_topology,
	.set_tcont		= cx_set_tcont,
	.set_tcont_qos		= cx_set_tcont_qos,
	.set_gem_port		= cx_set_gem_port,
	.set_gem_rate		= cx_set_gem_rate,
	.set_uni		= cx_set_uni,
	.set_uni_attrs		= cx_set_uni_attrs,
	.replace_service	= cx_replace_service,
	.delete_service		= cx_delete_service,
	.get_telemetry		= cx_get_telemetry,
	.set_operational	= cx_set_operational,
};

/* ---- reporting the MAC's state ---- */

static void cx_push_identity(struct omci_device *odev)
{
	struct omci_identity id = { };

	if (cx.hw->get_serial(cx.cookie, id.serial_number))
		return;
	memcpy(id.vendor_id, id.serial_number, sizeof(id.vendor_id));
	id.valid = OMCI_IDENTITY_F_SERIAL_NUMBER | OMCI_IDENTITY_F_VENDOR_ID;
	omci_device_set_identity_info(odev, &id);
	omci_device_set_identity(odev, id.serial_number, id.password);
}

static void cx_deliver_state(struct omci_device *odev, int state)
{
	enum xpon_registration_state reg;

	omci_device_set_state(odev, state);
	xpon_device_report_carrier(cx.xpon, state == CX_STATE_O5);
	switch (state) {
	case 2:
		reg = XPON_REGISTRATION_DISCOVERY;
		break;
	case 3:
	case 4:
	case 6:
		reg = XPON_REGISTRATION_REGISTERING;
		break;
	case CX_STATE_O5:
		reg = XPON_REGISTRATION_OPERATIONAL;
		break;
	default:
		reg = XPON_REGISTRATION_DOWN;
		break;
	}
	xpon_device_report_registration(cx.xpon, reg);
}

/*
 * The MAC reports from interrupt bottom halves and timers; the framework's
 * notifications sleep.  The MAC only posts what changed and this worker reads
 * the live values and delivers them in process context.
 */
static void cx_event_fn(struct work_struct *work)
{
	struct omci_device *odev = READ_ONCE(cx.odev);
	unsigned long ev = atomic_long_xchg(&cx.ev, 0);
	int state, onu_id, gem, los;

	if (!odev)
		return;
	state = cx.hw->state(cx.cookie);
	onu_id = cx.hw->onu_id(cx.cookie);
	gem = cx.hw->omcc_gem(cx.cookie);
	los = cx.hw->los(cx.cookie);

	if ((ev & CX_EV_LOS) && los >= 0)
		xpon_device_report_optical(cx.xpon, !los, los);
	if (ev & CX_EV_IDENTITY)
		cx_push_identity(odev);
	if ((ev & CX_EV_ONU_ID) && onu_id >= 0)
		omci_device_set_onu_id(odev, onu_id);

	if (state != CX_STATE_O5)
		gem = -1;
	if (gem != cx.chan_gem) {
		omci_device_set_channel(odev, gem < 0 ? 0 : gem, gem >= 0);
		cx.chan_gem = gem;
	}
	if (state != cx.state) {
		cx_deliver_state(odev, state);
		cx.state = state;
	}
}

void cx_notify(unsigned long events)
{
	if (!READ_ONCE(cx.odev))
		return;
	atomic_long_or(events, &cx.ev);
	schedule_work(&cx.event_work);
}

/* A downstream OMCI PDU from the MAC; true if the framework took it. */
bool cx_omci_rx(const u8 *msg, unsigned int len)
{
	struct omci_device *odev = READ_ONCE(cx.odev);
	struct sk_buff *skb;
	u32 flags = 0;
	int gem;

	if (!odev)
		return false;
	gem = cx.hw->omcc_gem(cx.cookie);
	if (gem < 0 || len < 4)
		return true;

	if (msg[3] == CX_OMCI_BASELINE_ID && len >= CX_OMCI_PDU_LEN) {
		u32 want = get_unaligned_be32(msg + CX_OMCI_PDU_LEN - CX_OMCI_MIC_LEN);

		len = CX_OMCI_PDU_LEN;
		flags = OMCI_F_MIC_PRESENT;
		if (want == ~crc32_be(~0U, msg, len - CX_OMCI_MIC_LEN))
			flags |= OMCI_F_MIC_VALID;
	} else if (msg[3] == CX_OMCI_EXTENDED_ID && len >= CX_OMCI_EXT_HDR_LEN) {
		unsigned int pdu_len = CX_OMCI_EXT_HDR_LEN +
				       get_unaligned_be16(msg + 8) + CX_OMCI_MIC_LEN;

		if (pdu_len > len || pdu_len > OMCI_MAX_PDU_LEN)
			return true;
		len = pdu_len;
		flags = OMCI_F_MIC_PRESENT;
	} else {
		return true;
	}

	skb = alloc_skb(len, GFP_ATOMIC);
	if (!skb)
		return true;
	skb_put_data(skb, msg, len);
	omci_device_receive(odev, skb, gem, flags);
	return true;
}

/* ---- registration ---- */

static int cx_set_mode(struct xpon_device *xpon, enum xpon_mode mode)
{
	if (cx.hw && cx.hw->set_mode)
		return cx.hw->set_mode(cx.cookie, xpon, mode);
	return mode == XPON_MODE_GPON ? 0 : -EOPNOTSUPP;
}

static const struct xpon_device_ops cx_xpon_ops = {
	.set_mode = cx_set_mode,
};

int cx_attach(struct device *dev, struct net_device *wan, void *cookie,
	      const struct cx_hw_ops *hw)
{
	struct xpon_device_desc desc = {
		.modes = XPON_MODE_CAP(XPON_MODE_GPON) |
			 (IS_ENABLED(CONFIG_CORTINA_EPON) && hw->set_mode ?
			  XPON_MODE_CAP(XPON_MODE_EPON) : 0),
		.mode = XPON_MODE_GPON,
		.ops = &cx_xpon_ops,
		.netdev = wan,
	};
	struct omci_device *odev;
	struct xpon_device *xpon;
	int rc;

	if (!cx_enable)
		return -EOPNOTSUPP;
	if (cx.odev)
		return -EBUSY;

	INIT_WORK(&cx.event_work, cx_event_fn);
	INIT_WORK(&cx.uni_work, cx_uni_fn);
	cx.dev = dev;
	cx.wan = wan;
	cx.cookie = cookie;
	cx.hw = hw;
	cx.chan_gem = -1;
	cx.state = 0;

	xpon = xpon_device_register(dev, &desc);
	if (IS_ERR(xpon))
		return PTR_ERR(xpon);
	odev = omci_device_register(xpon, OMCI_CAP_TELEMETRY, &cx_ops, NULL);
	if (IS_ERR(odev)) {
		xpon_device_unregister(xpon);
		return PTR_ERR(odev);
	}
	cx.xpon = xpon;
	WRITE_ONCE(cx.odev, odev);

	rc = omci_device_start(odev);
	if (rc) {
		cx_detach(cookie);
		return rc;
	}
	cx_notify(CX_EV_STATE | CX_EV_ONU_ID | CX_EV_IDENTITY | CX_EV_LOS);
	return 0;
}

void cx_detach(void *cookie)
{
	struct omci_device *odev = cx.odev;

	if (!odev || cx.cookie != cookie)
		return;
	WRITE_ONCE(cx.odev, NULL);
	cancel_work_sync(&cx.event_work);
	cancel_work_sync(&cx.uni_work);
	omci_device_stop(odev);
	omci_device_unregister(odev);
	xpon_device_unregister(cx.xpon);
	cx.xpon = NULL;
}
