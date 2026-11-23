// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) AK Sharma <maskachoska@yahoo.com> (github.com/AKoo7)
 *
 * Taken from https://github.com/AKoo7/openwrt/tree/aot5221zy.
 *
 * DSA switch for the Cortina NI on the Realtek RTL9607DQ: exposes the LAN
 * jacks as DSA user ports behind the conduit netdev.
 *
 * The port is carried out of band: on RX in HEADER_A.lspid of the DMA
 * header, on TX in DESC1_DEST of the DMA descriptor. The driver turns it into
 * a 2-byte software tag (tail tag on RX, prefix on TX) that tag_cortina
 * strips. The switch forwards known unicast between the ports of a bridge;
 * lookup misses and frames to our own MAC are trapped to the CPU, never
 * flooded, so received frames carry no offload_fwd_mark and the bridge floods
 * them in software.
 *
 * The NI probe already brings up the GPHYs, per-port MACs and ARB map, so
 * .setup and most phylink MAC ops have little to do.
 */

#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/if_bridge.h>
#include <linux/if_vlan.h>
#include <linux/mdio.h>
#include <linux/phy.h>
#include <linux/phylink.h>
#include <linux/platform_device.h>
#include <net/dsa.h>

#include "cortina-ni.h"
#include "cortina-ni-regs.h"

static enum dsa_tag_protocol
cortina_dsa_get_tag_protocol(struct dsa_switch *ds, int port,
			     enum dsa_tag_protocol mprot)
{
	return DSA_TAG_PROTO_CORTINA;
}

static int cortina_dsa_setup(struct dsa_switch *ds)
{
	/*
	 * Nothing to program: the NI probe already set up the GPHYs, MACs, ARB
	 * map and the ingress-to-CPU classifier.
	 */
	return 0;
}

static void cortina_ni_rmw(void __iomem *reg, u32 clr, u32 set)
{
	writel((readl(reg) & ~clr) | set, reg);
}

/*
 * Port 4 SerDes lane setup, taken from the vendor aal_sds_mode_set: the 2.5G
 * lane block sits at +0x1400.  The PHY rate-matches the slower copper speeds
 * with pause frames, so the lane always runs at 2.5G.
 */
static void cortina_ni_sgmii_lane(void __iomem *lp)
{
	cortina_ni_rmw(lp + CA_NI_SGMII_LANE_2G5 + 0x04, GENMASK(5, 0), 0x10);
	cortina_ni_rmw(lp + CA_NI_SGMII_LANE_2G5 + 0x0c, GENMASK(5, 1), 0x38);
	cortina_ni_rmw(lp + CA_NI_SGMII_LANE_2G5 + 0x18, 0, 0x1f);
	cortina_ni_rmw(lp + CA_NI_SGMII_LANE_2G5 + 0x28, 0, 0x7000);
	writel(0x34aa, lp + CA_NI_SGMII_LANE_2G5 + 0x40);
	writel(0x88aa, lp + CA_NI_SGMII_LANE_2G5 + 0x44);
	writel(0x53, lp + CA_NI_SGMII_LANE_2G5 + 0x48);
	writel(0x8008, lp + CA_NI_SGMII_LANE_2G5 + 0x7c);
	cortina_ni_rmw(lp + CA_NI_SGMII_LANE_2G5 + 0x4c, GENMASK(15, 12), 0);
}

static void cortina_ni_sds_up(struct cortina_ni *ni)
{
	void __iomem *lp = ni->win[CA_NI_WIN_SGMII_PCS];

	/* resetting a running lane would drop the SerDes link of the PHY */
	if (ni->sds_up)
		return;
	ni->sds_up = true;

	regmap_clear_bits(ni->glb, CA_NI_GLB_PHY_CONTROL,
			  CA_NI_GLB_PHY_CONTROL_SDS_MODE);
	regmap_set_bits(ni->glb, CA_NI_GLB_DPHY_RESET, CA_NI_GLB_DPHY_RESET_SDS);
	udelay(10);
	regmap_clear_bits(ni->glb, CA_NI_GLB_DPHY_RESET, CA_NI_GLB_DPHY_RESET_SDS);
	udelay(10);

	cortina_ni_sgmii_lane(lp);
	regmap_set_bits(ni->glb, CA_NI_GLB_PHY_CONTROL,
			CA_NI_GLB_PHY_CONTROL_HSGMII);
	regmap_update_bits(ni->glb, CA_NI_GLB_SGMII_PCS_CONTROL,
			   (u32)~CA_NI_PCS_KEEP, CA_NI_PCS_HSGMII);

	/*
	 * The PCS reset above restores the default AN word. phylink runs the
	 * PHY with in-band AN off, so the PCS must not wait for AN words.
	 */
	writel(CA_NI_SGMII_LP_INBAND_DISABLE, lp + CA_NI_SGMII_LP_AUTONEG);
}

static struct cortina_ni *pcs_to_ni(struct phylink_pcs *pcs)
{
	return container_of(pcs, struct cortina_ni, sgmii_pcs);
}

/*
 * The SerDes runs 2500BASE-X with in-band signalling off; the PHY reports
 * the link, so phylink never asks the PCS for its state. No PCS status
 * register is known.
 */
static unsigned int cortina_pcs_inband_caps(struct phylink_pcs *pcs,
					    phy_interface_t interface)
{
	return LINK_INBAND_DISABLE;
}

static void cortina_pcs_get_state(struct phylink_pcs *pcs,
				  unsigned int neg_mode,
				  struct phylink_link_state *state)
{
	state->link = false;
}

static int cortina_pcs_config(struct phylink_pcs *pcs, unsigned int neg_mode,
			      phy_interface_t interface,
			      const unsigned long *advertising,
			      bool permit_pause_to_mac)
{
	/* the PHY waits for its SerDes partner as soon as it starts */
	cortina_ni_sds_up(pcs_to_ni(pcs));
	return 0;
}

static const struct phylink_pcs_ops cortina_pcs_ops = {
	.pcs_inband_caps	= cortina_pcs_inband_caps,
	.pcs_get_state		= cortina_pcs_get_state,
	.pcs_config		= cortina_pcs_config,
};

static void cortina_dsa_phylink_get_caps(struct dsa_switch *ds, int port,
					 struct phylink_config *config)
{
	config->mac_capabilities = MAC_SYM_PAUSE | MAC_ASYM_PAUSE |
				   MAC_10 | MAC_100 | MAC_1000FD;

	/*
	 * The 2.5G jack: an external PHY behind a SerDes that always runs at
	 * 2500BASE-X; the PHY rate-matches the slower copper speeds.
	 */
	if (port == CA_NI_SGMII_PORT) {
		config->mac_capabilities |= MAC_2500FD;
		__set_bit(PHY_INTERFACE_MODE_2500BASEX,
			  config->supported_interfaces);
		return;
	}

	/*
	 * The other jacks are internal GPHYs and the CPU port is an internal
	 * MAC. GMII must also be advertised or phylib's default interface makes
	 * phylink reject the internal PHYs.
	 */
	__set_bit(PHY_INTERFACE_MODE_INTERNAL, config->supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_GMII, config->supported_interfaces);
}

/*
 * Per-port MAC MIB counters, read through the indirect ACCESS/DATA pair; ~0
 * means the access never completed.
 */
static const struct {
	char	name[ETH_GSTRING_LEN];
	bool	tx;
	u32	id;
} cortina_dsa_mib[] = {
	{ "rx_unicast",		false,	CA_NI_MIB_RX_UC_PKT },
	{ "rx_multicast",	false,	CA_NI_MIB_RX_MC_PKT },
	{ "rx_broadcast",	false,	CA_NI_MIB_RX_BC_PKT },
	{ "tx_unicast",		true,	CA_NI_MIB_TX_UC_PKT },
	{ "tx_multicast",	true,	CA_NI_MIB_TX_MC_PKT },
	{ "tx_broadcast",	true,	CA_NI_MIB_TX_BC_PKT },
	{ "tx_frames_65_127",	true,	CA_NI_MIB_TX_FRM65_127 },
};

static void cortina_dsa_get_strings(struct dsa_switch *ds, int port,
				    u32 stringset, u8 *data)
{
	int i;

	if (stringset != ETH_SS_STATS)
		return;
	for (i = 0; i < ARRAY_SIZE(cortina_dsa_mib); i++)
		ethtool_puts(&data, cortina_dsa_mib[i].name);
}

static int cortina_dsa_get_sset_count(struct dsa_switch *ds, int port, int sset)
{
	return sset == ETH_SS_STATS ? ARRAY_SIZE(cortina_dsa_mib) : 0;
}

static void cortina_dsa_get_ethtool_stats(struct dsa_switch *ds, int port,
					  u64 *data)
{
	struct cortina_ni *ni = ds->priv;
	int i;

	for (i = 0; i < ARRAY_SIZE(cortina_dsa_mib); i++)
		data[i] = cortina_dsa_mib[i].tx ?
			  cortina_ni_tx_mib_read(ni, port, cortina_dsa_mib[i].id) :
			  cortina_ni_rx_mib_read(ni, port, cortina_dsa_mib[i].id);
}

/*
 * The MACs accept frames up to CA_NI_HV_PKT_LEN_MAX_VAL, set once at probe,
 * so there is nothing to program per port: the TX descriptor is the limit.
 */
static int cortina_dsa_port_change_mtu(struct dsa_switch *ds, int port,
				       int new_mtu)
{
	return 0;
}

static int cortina_dsa_port_max_mtu(struct dsa_switch *ds, int port)
{
	return CA_NI_MAX_MTU;
}

/*
 * Hardware forwarding between LAN ports follows the software bridge: a port
 * forwards only to the other ports of its bridge, and only while both are in
 * the forwarding state.  Everything else (standalone ports, other bridges,
 * blocked ports, lookup misses) reaches the CPU and is bridged in software.
 * The L2 engine's VLAN tables are not documented, so the ports of a
 * VLAN-filtering bridge also go through the CPU, where the bridge applies
 * the VLANs.
 */
static void cortina_dsa_update_fwd(struct dsa_switch *ds)
{
	struct cortina_ni *ni = ds->priv;
	struct dsa_port *dp, *other;

	dsa_switch_for_each_user_port(dp, ds) {
		u8 mask = 0;

		if (dp->bridge && (ni->stp_fwd & BIT(dp->index)) &&
		    !(ni->vlan_aware & BIT(dp->index)))
			dsa_switch_for_each_user_port(other, ds)
				if (other != dp &&
				    dsa_port_bridge_same(dp, other) &&
				    (ni->stp_fwd & BIT(other->index)) &&
				    !(ni->vlan_aware & BIT(other->index)))
					mask |= BIT(other->index);
		ni->lan_fwd[dp->index] = mask;
		cortina_ni_l2fe_mmshp_set(ni, dp->index);
	}
}

static int cortina_dsa_port_bridge_join(struct dsa_switch *ds, int port,
					struct dsa_bridge bridge,
					bool *tx_fwd_offload,
					struct netlink_ext_ack *extack)
{
	cortina_dsa_update_fwd(ds);
	return 0;
}

static void cortina_dsa_port_bridge_leave(struct dsa_switch *ds, int port,
					  struct dsa_bridge bridge)
{
	cortina_dsa_update_fwd(ds);
}

static void cortina_dsa_port_stp_state_set(struct dsa_switch *ds, int port,
					   u8 state)
{
	struct cortina_ni *ni = ds->priv;

	if (state == BR_STATE_FORWARDING)
		ni->stp_fwd |= BIT(port);
	else
		ni->stp_fwd &= ~BIT(port);
	cortina_dsa_update_fwd(ds);
}

static int cortina_dsa_port_vlan_filtering(struct dsa_switch *ds, int port,
					   bool vlan_filtering,
					   struct netlink_ext_ack *extack)
{
	struct cortina_ni *ni = ds->priv;

	if (vlan_filtering)
		ni->vlan_aware |= BIT(port);
	else
		ni->vlan_aware &= ~BIT(port);
	cortina_dsa_update_fwd(ds);
	return 0;
}

/* The bridge keeps the VLAN table; frames keep their tags through the CPU. */
static int cortina_dsa_port_vlan_add(struct dsa_switch *ds, int port,
				     const struct switchdev_obj_port_vlan *vlan,
				     struct netlink_ext_ack *extack)
{
	return 0;
}

static int cortina_dsa_port_vlan_del(struct dsa_switch *ds, int port,
				     const struct switchdev_obj_port_vlan *vlan)
{
	return 0;
}

/*
 * Static entries of a plain bridge (VLAN 0) are programmed, replacing what
 * the port learned. Entries on the CPU port are not needed (lookup misses and
 * the own MAC already reach the CPU), and entries with a VLAN belong to a
 * VLAN-filtering bridge, which the CPU switches.
 */
static int cortina_dsa_port_fdb_add(struct dsa_switch *ds, int port,
				    const unsigned char *addr, u16 vid,
				    struct dsa_db db)
{
	struct cortina_ni *ni = ds->priv;
	int ret;

	if (dsa_is_cpu_port(ds, port) || vid)
		return 0;

	mutex_lock(&ni->l2_lock);
	ret = cortina_ni_l2fe_fdb_replace(ni->win[CA_NI_WIN_NI], addr, port);
	mutex_unlock(&ni->l2_lock);
	return ret;
}

static int cortina_dsa_port_fdb_del(struct dsa_switch *ds, int port,
				    const unsigned char *addr, u16 vid,
				    struct dsa_db db)
{
	struct cortina_ni *ni = ds->priv;
	int ret;

	if (dsa_is_cpu_port(ds, port) || vid)
		return 0;

	mutex_lock(&ni->l2_lock);
	ret = cortina_ni_l2fe_fdb_del(ni->win[CA_NI_WIN_NI], addr);
	mutex_unlock(&ni->l2_lock);
	return ret;
}

/* Delete the dynamic entries learned on @port. */
static void cortina_dsa_port_fast_age(struct dsa_switch *ds, int port)
{
	struct cortina_ni *ni = ds->priv;
	void __iomem *base = ni->win[CA_NI_WIN_NI];
	u8 mac[ETH_ALEN];
	u32 idx, act;
	u16 vid;
	int ret;

	mutex_lock(&ni->l2_lock);
	for (idx = 0; idx < CA_NI_L2FE_FDB_ENTRIES; idx++) {
		u32 from = idx;

		ret = cortina_ni_l2fe_fdb_scan(base, &idx, mac, &vid, &act);
		if (ret || idx < from)
			break;
		if ((act & CA_NI_L2FE_FDB_VALID) &&
		    !(act & CA_NI_L2FE_FDB_STATIC) &&
		    FIELD_GET(CA_NI_L2FE_FDB_LPID, act) == port)
			cortina_ni_l2fe_fdb_del(base, mac);
	}
	mutex_unlock(&ni->l2_lock);
}

static int cortina_dsa_set_ageing_time(struct dsa_switch *ds,
				       unsigned int msecs)
{
	struct cortina_ni *ni = ds->priv;
	void __iomem *reg = ni->win[CA_NI_WIN_NI] + CA_NI_L2FE_FDB_CTRL_1;
	u32 val = readl(reg);

	val &= ~CA_NI_L2FE_FDB_AGE;
	val |= FIELD_PREP(CA_NI_L2FE_FDB_AGE,
			  msecs / CA_NI_L2FE_FDB_AGE_UNIT_MS);
	writel(val, reg);
	return 0;
}

static int cortina_dsa_port_fdb_dump(struct dsa_switch *ds, int port,
				     dsa_fdb_dump_cb_t *cb, void *data)
{
	struct cortina_ni *ni = ds->priv;
	void __iomem *base = ni->win[CA_NI_WIN_NI];
	u8 mac[ETH_ALEN];
	u32 idx, act;
	u16 vid;
	int ret;

	ret = 0;
	mutex_lock(&ni->l2_lock);
	for (idx = 0; idx < CA_NI_L2FE_FDB_ENTRIES; idx++) {
		u32 from = idx;

		ret = cortina_ni_l2fe_fdb_scan(base, &idx, mac, &vid, &act);
		if (ret == -ENOENT || (!ret && idx < from)) {
			ret = 0;
			break;
		}
		if (ret)
			break;
		if (!(act & CA_NI_L2FE_FDB_VALID) ||
		    FIELD_GET(CA_NI_L2FE_FDB_LPID, act) != port)
			continue;
		ret = cb(mac, vid, act & CA_NI_L2FE_FDB_STATIC, data);
		if (ret)
			break;
	}
	mutex_unlock(&ni->l2_lock);
	return ret;
}

static const struct dsa_switch_ops cortina_dsa_switch_ops = {
	.get_tag_protocol	= cortina_dsa_get_tag_protocol,
	.setup			= cortina_dsa_setup,
	.phylink_get_caps	= cortina_dsa_phylink_get_caps,
	.get_strings		= cortina_dsa_get_strings,
	.get_sset_count		= cortina_dsa_get_sset_count,
	.get_ethtool_stats	= cortina_dsa_get_ethtool_stats,
	.port_change_mtu	= cortina_dsa_port_change_mtu,
	.port_max_mtu		= cortina_dsa_port_max_mtu,
	.port_bridge_join	= cortina_dsa_port_bridge_join,
	.port_bridge_leave	= cortina_dsa_port_bridge_leave,
	.port_stp_state_set	= cortina_dsa_port_stp_state_set,
	.set_ageing_time	= cortina_dsa_set_ageing_time,
	.port_vlan_filtering	= cortina_dsa_port_vlan_filtering,
	.port_vlan_add		= cortina_dsa_port_vlan_add,
	.port_vlan_del		= cortina_dsa_port_vlan_del,
	.port_fdb_add		= cortina_dsa_port_fdb_add,
	.port_fdb_del		= cortina_dsa_port_fdb_del,
	.port_fast_age		= cortina_dsa_port_fast_age,
	.port_fdb_dump		= cortina_dsa_port_fdb_dump,
};

/*
 * phylink calls these for every port. The NI probe already enabled the
 * per-port MACs, and the 2.5G port's SerDes is the PCS above, so mac_config
 * has nothing to do.
 */
static void cortina_dsa_mac_config(struct phylink_config *config,
				   unsigned int mode,
				   const struct phylink_link_state *state)
{
}

static struct phylink_pcs *
cortina_dsa_mac_select_pcs(struct phylink_config *config,
			   phy_interface_t interface)
{
	struct dsa_port *dp = dsa_phylink_to_port(config);
	struct cortina_ni *ni = dp->ds->priv;

	if (dp->index == CA_NI_SGMII_PORT &&
	    interface == PHY_INTERFACE_MODE_2500BASEX)
		return &ni->sgmii_pcs;
	return NULL;
}

static void cortina_dsa_mac_link_up(struct phylink_config *config,
				    struct phy_device *phy, unsigned int mode,
				    phy_interface_t interface, int speed,
				    int duplex, bool tx_pause, bool rx_pause)
{
	struct dsa_port *dp = dsa_phylink_to_port(config);
	struct cortina_ni *ni = dp->ds->priv;
	u32 clr = CA_NI_PORT_GLB_PWR_DWN_TX, set = 0;
	void __iomem *reg;

	if (dp->index >= CA_NI_DSA_USER_PORTS)
		return;

	/*
	 * The pause-paced copper speed, not the SerDes speed phylink passes
	 * while the PHY rate-matches, is what the wire carries.
	 */
	if (ni->tx) {
		int wire = phy && phy->speed > 0 ? phy->speed : speed;

		ni->tx->wire_free[dp->index] = 0;
		WRITE_ONCE(ni->tx->wire_ps_per_byte[dp->index],
			   8000000 / wire);
	}

	/*
	 * MAC autosync follows the PHY in hardware; mirror speed and duplex
	 * into the port config like the vendor driver does.
	 */
	if (speed == SPEED_10)
		set |= CA_NI_PORT_GLB_SPEED_10M;
	else
		clr |= CA_NI_PORT_GLB_SPEED_10M;
	if (duplex == DUPLEX_HALF)
		set |= CA_NI_PORT_GLB_HALF_DUPLEX;
	else
		clr |= CA_NI_PORT_GLB_HALF_DUPLEX;

	reg = ni->win[CA_NI_WIN_NI] + CA_NI_PORT_GLB_CFG(dp->index);
	writel((readl(reg) & ~clr) | set, reg);

	/*
	 * The PHY of the 2.5G port paces the MAC with pause frames while it
	 * rate-matches the slower copper speeds.
	 */
	if (dp->index == CA_NI_SGMII_PORT) {
		reg = ni->win[CA_NI_WIN_NI] + CA_NI_PORT_RXMAC_CFG(dp->index);
		writel((readl(reg) & ~CA_NI_PORT_RXMAC_FLOW_DISABLE) |
		       (rx_pause ? 0 : CA_NI_PORT_RXMAC_FLOW_DISABLE), reg);
	}
}

static void cortina_dsa_mac_link_down(struct phylink_config *config,
				      unsigned int mode,
				      phy_interface_t interface)
{
	struct dsa_port *dp = dsa_phylink_to_port(config);
	struct cortina_ni *ni = dp->ds->priv;

	if (ni->tx && dp->index < CA_NI_DSA_USER_PORTS)
		WRITE_ONCE(ni->tx->wire_ps_per_byte[dp->index], 0);
}

static const struct phylink_mac_ops cortina_dsa_phylink_mac_ops = {
	.mac_select_pcs	= cortina_dsa_mac_select_pcs,
	.mac_config	= cortina_dsa_mac_config,
	.mac_link_up	= cortina_dsa_mac_link_up,
	.mac_link_down	= cortina_dsa_mac_link_down,
};

/*
 * What the vendor "realtek,taurus-phy" driver does when ca-ne powers on its
 * sgmii-phy (PHY 7): SerDes analog control, SGMII mode and 2.5G PCS defaults.
 */
static void cortina_ni_sgmii_power_on(struct cortina_ni *ni)
{
	writel(CA_NI_GPHY_SDS_CTRL_VAL,
	       ni->win[CA_NI_WIN_GPHY] + CA_NI_GPHY_SDS_CTRL);
	regmap_update_bits(ni->glb, CA_NI_GLB_PHY_CONTROL,
			   CA_NI_GLB_PHY_CONTROL_SDS_MODE,
			   CA_NI_GLB_PHY_CONTROL_SGMII);
	regmap_write(ni->glb, 0x0d0, 0x30);
	regmap_write(ni->glb, CA_NI_GLB_SGMII_PCS_CONTROL, CA_NI_PCS_HSGMII);
}

/*
 * The switch is its own platform device on the NI node, so a probe deferral
 * (the tagger registers after this driver when both are built in) only
 * retries the switch, not the NI hardware bring-up.
 */
static int cortina_ni_switch_probe(struct platform_device *pdev)
{
	struct cortina_ni *ni = dev_get_drvdata(pdev->dev.parent);
	struct device *dev = &pdev->dev;
	struct dsa_switch *ds;
	int ret;

	ds = devm_kzalloc(dev, sizeof(*ds), GFP_KERNEL);
	if (!ds)
		return -ENOMEM;
	ds->dev = dev;
	ds->num_ports = CA_NI_DSA_NUM_PORTS;
	ds->ops = &cortina_dsa_switch_ops;
	ds->ageing_time_min = CA_NI_L2FE_FDB_AGE_UNIT_MS;
	ds->ageing_time_max = FIELD_MAX(CA_NI_L2FE_FDB_AGE) *
			      CA_NI_L2FE_FDB_AGE_UNIT_MS;
	ds->phylink_mac_ops = &cortina_dsa_phylink_mac_ops;
	ds->priv = ni;

	cortina_ni_sgmii_power_on(ni);
	ni->sgmii_pcs.ops = &cortina_pcs_ops;
	__set_bit(PHY_INTERFACE_MODE_2500BASEX, ni->sgmii_pcs.supported_interfaces);

	ni->ds = ds;
	ret = dsa_register_switch(ds);
	if (ret) {
		ni->ds = NULL;
		return dev_err_probe(dev, ret, "cannot register switch\n");
	}

	platform_set_drvdata(pdev, ni);
	return 0;
}

static void cortina_ni_switch_remove(struct platform_device *pdev)
{
	struct cortina_ni *ni = platform_get_drvdata(pdev);

	dsa_unregister_switch(ni->ds);
	ni->ds = NULL;
}

struct platform_driver cortina_ni_switch_driver = {
	.probe = cortina_ni_switch_probe,
	.remove = cortina_ni_switch_remove,
	.driver = {
		.name = CA_NI_DRV_NAME "-switch",
	},
};

static void cortina_ni_switch_unregister(void *data)
{
	platform_device_unregister(data);
}

int cortina_ni_dsa_register(struct cortina_ni *ni)
{
	struct platform_device_info info = {
		.parent = ni->dev,
		.name = CA_NI_DRV_NAME "-switch",
		.id = PLATFORM_DEVID_AUTO,
		.fwnode = dev_fwnode(ni->dev),
		.of_node_reused = true,
	};
	struct platform_device *pdev;

	pdev = platform_device_register_full(&info);
	if (IS_ERR(pdev))
		return PTR_ERR(pdev);

	return devm_add_action_or_reset(ni->dev, cortina_ni_switch_unregister,
					pdev);
}
