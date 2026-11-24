// SPDX-License-Identifier: GPL-2.0+
/*
 * Ethernet driver for the Realtek RTL9607DQ (Taurus) SoC.
 *
 * Packets go through the CPU XRAM management FIFO of the NI block, one NI
 * port at a time: the port with link is selected as the CPU port.  The frame
 * and XRAM handling follows the Cortina CAxxxx driver (cortina_ni.c); the
 * register map is the Taurus one.
 *
 * Copyright (C) Cortina Access, Inc.
 */

#include <dm.h>
#include <errno.h>
#include <log.h>
#include <malloc.h>
#include <miiphy.h>
#include <mtd.h>
#include <net.h>
#include <phy.h>
#include <time.h>
#include <asm/io.h>
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <u-boot/crc.h>
#include <linux/mtd/mtd.h>

/* NI window */
#define NI_HV_INIT_DONE		0xa004
#define  NI_HV_INIT_DONE_NI	BIT(1)
#define NI_HV_INTF_RST		0xa008	/* 0 = all interfaces out of reset */
#define NI_HV_STATIC_CFG	0xa01c
#define  NI_HV_PORT_TO_CPU	GENMASK(3, 0)

#define NI_PORT_STRIDE		0x90
#define NI_PORT_STATIC_CFG(p)	(0xa5c0 + (p) * NI_PORT_STRIDE)
#define  NI_PORT_INT_CFG	GENMASK(3, 0)
#define  NI_INT_CFG_GMII	0
#define  NI_INT_CFG_MII		1
#define NI_PORT_GLB_CFG(p)	(0xa5c4 + (p) * NI_PORT_STRIDE)
#define  NI_PORT_GLB_SPEED_10M	BIT(0)
#define  NI_PORT_GLB_HALF	BIT(1)
#define NI_PORT_RXMAC_CFG(p)	(0xa5c8 + (p) * NI_PORT_STRIDE)
#define  NI_PORT_RX_EN		BIT(0)
#define  NI_PORT_RX_FLOW_DISABLE	BIT(8)
#define NI_PORT_TXMAC_CFG(p)	(0xa5d4 + (p) * NI_PORT_STRIDE)
#define  NI_PORT_TX_EN		BIT(0)

/* CPU XRAM management FIFO */
#define NI_XRAM_ADRCFG_RX	0xaa20
#define NI_XRAM_ADRCFG_TX	0xaa24
#define  NI_XRAM_ADRCFG_BASE	GENMASK(9, 0)
#define  NI_XRAM_ADRCFG_TOP	GENMASK(25, 16)
#define NI_XRAM_CFG		0xaa28
#define  NI_XRAM_CFG_RX_DIS	BIT(0)
#define  NI_XRAM_CFG_TX_DIS	BIT(9)
#define  NI_XRAM_CFG_PROMISC	GENMASK(15, 14)
#define NI_XRAM_CPU_CFG_RX	0xaa2c	/* SW read pointer */
#define NI_XRAM_CPU_STA_RX	0xaa30	/* HW write pointer */
#define NI_XRAM_CPU_CFG_TX	0xaa44	/* SW write pointer */
#define NI_XRAM_CPU_STA_TX	0xaa48	/* HW read pointer */
#define  NI_XRAM_PTR_MASK	GENMASK(9, 0)

/* RX/TX regions of the XRAM, in 8-byte units */
#define RX_BASE			0x000
#define RX_TOP			0x2ff
#define TX_BASE			0x300
#define TX_TOP			0x3ff

/* XRAM slot header (Header XR / XT) */
#define XRAM_HDR_NEXT_LINK	GENMASK(9, 0)
#define XRAM_HDR_BYTES_VALID	GENMASK(13, 10)
#define XRAM_HDR_OWN		BIT(31)

/* RX packet status */
#define PKT_STAT_SIZE		GENMASK(13, 0)
#define PKT_STAT_VALID		BIT(19)
#define PKT_STAT_ERR		(GENMASK(23, 20) | BIT(25) | BIT(26))

#define HEADER_A_SIZE		8
#define MAX_TX_LEN		2032

/* external MDIO master */
#define MDIO_CFG		0x00
#define MDIO_ADDR		0x04
#define  MDIO_ADDR_PHY		GENMASK(4, 0)
#define  MDIO_ADDR_REG		GENMASK(12, 8)
#define  MDIO_ADDR_READ		BIT(15)
#define MDIO_WRDATA		0x08
#define MDIO_RDDATA		0x0c
#define MDIO_CTRL		0x10
#define  MDIO_CTRL_DONE	BIT(0)
#define  MDIO_CTRL_START	BIT(7)
#define MDIO_TIMEOUT_US		50000

/* internal GbE PHYs: MII registers are memory mapped */
#define GPHY_FIRST		1
#define GPHY_LAST		4

/* port 4 SerDes: global block and the SGMII PCS window */
#define GLB_DPHY_RESET		0x0a0
#define  GLB_DPHY_RESET_SDS	(BIT(5) | BIT(8))
#define GLB_PHY_CONTROL		0x0c4
#define  GLB_PHY_CONTROL_SDS_MODE	GENMASK(26, 25)
#define  GLB_PHY_CONTROL_SGMII	BIT(25)
#define  GLB_PHY_CONTROL_HSGMII	BIT(26)
#define GLB_SDS_CFG		0x0d0
#define GLB_SGMII_PCS_CONTROL	0x0d4
#define  PCS_CONTROL_KEEP	(GENMASK(31, 29) | GENMASK(8, 7) | GENMASK(1, 0))
#define  PCS_CONTROL_HSGMII	0x34200248
#define GPHY_SDS_CTRL		0x01c
#define  GPHY_SDS_CTRL_VAL	0x5359
#define PCS_LP_AUTONEG		0x0008
#define  PCS_LP_INBAND_DISABLE	0x71d0
#define PCS_LANE_2G5		0x1400

/* RTL8221B PHY on the 2.5G port */
#define RTL_VEND1		30
#define RTL_VEND2		31
#define RTL_PAGE_SELECT		0x1f
#define RTL_VEND1_SERDES_OPTION	0x697a
#define  RTL_SERDES_OPTION_MODE	GENMASK(5, 0)
#define  RTL_SERDES_2500BASEX	2	/* 2500base-x, the PHY rate-matches */
#define RTL_VEND1_SERDES_CMD	0x7587
#define  RTL_SERDES_CMD_BUSY	BIT(0)
#define  RTL_SERDES_CMD_WRITE	BIT(1)
#define RTL_VEND1_SERDES_ADDR	0x7588
#define RTL_VEND1_SERDES_DATA	0x7589
#define RTL_SERDES_ADDR_AUTONEG	0x2
#define RTL_VEND2_PHYCR1	0xa430
#define  RTL_PHYCR1_ALDPS_EN	BIT(2)
#define  RTL_PHYCR1_ALDPS_XTAL	BIT(12)
#define  RTL_PHYCR1_PHYAD_0_EN	BIT(13)
#define RTL_PHYSR		0x1a	/* clause 22 register */
#define  RTL_PHYSR_DUPLEX	BIT(3)
#define  RTL_PHYSR_SPEED	(GENMASK(5, 4) | GENMASK(10, 9))
#define RTL_VEND2_AN_10GBT_CTRL	0xa5d4
#define  RTL_AN_ADV_2_5G	BIT(7)
#define  RTL_AN_ADV_5G		BIT(8)
#define  RTL_AN_ADV_10G		BIT(12)

#define MAX_PORTS		6
#define LINK_SCAN_MS		5000

struct ni_port {
	int phy_addr;
	int port;
	bool ext;	/* RTL8221B behind the 2.5G SerDes */
	struct phy_device *phydev;
};

struct ni_priv {
	void __iomem *ni;
	void __iomem *mdio;
	void __iomem *xram;
	void __iomem *gphy;
	void __iomem *glb;
	void __iomem *pcs;
	struct mii_dev *bus;
	struct ni_port ports[MAX_PORTS];
	int num_ports;
	int active;
	u8 rx_buf[2048] __aligned(8);
};

static u32 ni_rd(struct ni_priv *priv, u32 off)
{
	return readl(priv->ni + off);
}

static void ni_wr(struct ni_priv *priv, u32 off, u32 val)
{
	writel(val, priv->ni + off);
}

static void ni_rmw(struct ni_priv *priv, u32 off, u32 clr, u32 set)
{
	ni_wr(priv, off, (ni_rd(priv, off) & ~clr) | set);
}

/* MDIO */

static int ni_ext_mdio_xfer(struct ni_priv *priv, int phy, int reg, u16 data,
			    bool read)
{
	u32 ctrl;
	int ret;

	writel(FIELD_PREP(MDIO_ADDR_PHY, phy) | FIELD_PREP(MDIO_ADDR_REG, reg) |
	       (read ? MDIO_ADDR_READ : 0), priv->mdio + MDIO_ADDR);
	if (!read)
		writel(data, priv->mdio + MDIO_WRDATA);
	writel(MDIO_CTRL_START, priv->mdio + MDIO_CTRL);

	ret = readl_poll_timeout(priv->mdio + MDIO_CTRL, ctrl,
				 ctrl & MDIO_CTRL_DONE, MDIO_TIMEOUT_US);
	if (ret)
		return ret;
	writel(ctrl, priv->mdio + MDIO_CTRL);

	return read ? readl(priv->mdio + MDIO_RDDATA) & 0xffff : 0;
}

static bool ni_gphy_addr(int addr)
{
	return addr >= GPHY_FIRST && addr <= GPHY_LAST;
}

static int ni_raw_read(struct ni_priv *priv, int addr, int reg)
{
	if (ni_gphy_addr(addr))
		return readl(priv->gphy + ((addr << 7) | (reg << 2))) & 0xffff;

	return ni_ext_mdio_xfer(priv, addr, reg, 0, true);
}

static int ni_raw_write(struct ni_priv *priv, int addr, int reg, u16 val)
{
	if (ni_gphy_addr(addr)) {
		writel(val, priv->gphy + ((addr << 7) | (reg << 2)));
		return 0;
	}

	return ni_ext_mdio_xfer(priv, addr, reg, val, false);
}

/* VEND2 of the RTL822x is reached through the paged registers 0x10-0x17 */
static int ni_vend2_xfer(struct ni_priv *priv, int addr, int reg, u16 val,
			 bool read)
{
	int page = reg >> 4;
	int preg = 0x10 + ((reg & 0xf) >> 1);
	int old, ret;

	old = ni_raw_read(priv, addr, RTL_PAGE_SELECT);
	if (old < 0)
		return old;
	ret = ni_raw_write(priv, addr, RTL_PAGE_SELECT, page);
	if (ret)
		return ret;

	ret = read ? ni_raw_read(priv, addr, preg) :
		     ni_raw_write(priv, addr, preg, val);

	ni_raw_write(priv, addr, RTL_PAGE_SELECT, old);
	return ret;
}

/* other MMDs go through the clause 22 indirection registers 13 and 14 */
static int ni_mmd_xfer(struct ni_priv *priv, int addr, int devad, int reg,
		       u16 val, bool read)
{
	ni_raw_write(priv, addr, MII_MMD_CTRL, devad);
	ni_raw_write(priv, addr, MII_MMD_DATA, reg);
	ni_raw_write(priv, addr, MII_MMD_CTRL, 0x4000 | devad);

	return read ? ni_raw_read(priv, addr, MII_MMD_DATA) :
		      ni_raw_write(priv, addr, MII_MMD_DATA, val);
}

static int ni_mdio_read(struct mii_dev *bus, int addr, int devad, int reg)
{
	struct ni_priv *priv = bus->priv;

	if (devad == MDIO_DEVAD_NONE)
		return ni_raw_read(priv, addr, reg);
	if (ni_gphy_addr(addr))
		return -EOPNOTSUPP;
	if (devad == RTL_VEND2)
		return ni_vend2_xfer(priv, addr, reg, 0, true);

	return ni_mmd_xfer(priv, addr, devad, reg, 0, true);
}

static int ni_mdio_write(struct mii_dev *bus, int addr, int devad, int reg,
			 u16 val)
{
	struct ni_priv *priv = bus->priv;

	if (devad == MDIO_DEVAD_NONE)
		return ni_raw_write(priv, addr, reg, val);
	if (ni_gphy_addr(addr))
		return -EOPNOTSUPP;
	if (devad == RTL_VEND2)
		return ni_vend2_xfer(priv, addr, reg, val, false);

	return ni_mmd_xfer(priv, addr, devad, reg, val, false);
}

/* port 4 SerDes lane and the RTL8221B behind it */

static void ni_rmw32(void __iomem *reg, u32 clr, u32 set)
{
	writel((readl(reg) & ~clr) | set, reg);
}

static void ni_sds_power_on(struct ni_priv *priv)
{
	writel(GPHY_SDS_CTRL_VAL, priv->gphy + GPHY_SDS_CTRL);
	ni_rmw32(priv->glb + GLB_PHY_CONTROL, GLB_PHY_CONTROL_SDS_MODE,
		 GLB_PHY_CONTROL_SGMII);
	writel(0x30, priv->glb + GLB_SDS_CFG);
	writel(PCS_CONTROL_HSGMII, priv->glb + GLB_SGMII_PCS_CONTROL);
}

static void ni_sds_up(struct ni_priv *priv)
{
	void __iomem *lane = priv->pcs + PCS_LANE_2G5;

	ni_rmw32(priv->glb + GLB_PHY_CONTROL, GLB_PHY_CONTROL_SDS_MODE, 0);
	ni_rmw32(priv->glb + GLB_DPHY_RESET, 0, GLB_DPHY_RESET_SDS);
	udelay(10);
	ni_rmw32(priv->glb + GLB_DPHY_RESET, GLB_DPHY_RESET_SDS, 0);
	udelay(10);

	/* the PHY rate-matches copper speeds with pause, the lane stays 2.5G */
	ni_rmw32(lane + 0x04, GENMASK(5, 0), 0x10);
	ni_rmw32(lane + 0x0c, GENMASK(5, 1), 0x38);
	ni_rmw32(lane + 0x18, 0, 0x1f);
	ni_rmw32(lane + 0x28, 0, 0x7000);
	writel(0x34aa, lane + 0x40);
	writel(0x88aa, lane + 0x44);
	writel(0x53, lane + 0x48);
	writel(0x8008, lane + 0x7c);
	ni_rmw32(lane + 0x4c, GENMASK(15, 12), 0);

	ni_rmw32(priv->glb + GLB_PHY_CONTROL, 0, GLB_PHY_CONTROL_HSGMII);
	ni_rmw32(priv->glb + GLB_SGMII_PCS_CONTROL, (u32)~PCS_CONTROL_KEEP,
		 PCS_CONTROL_HSGMII & (u32)~PCS_CONTROL_KEEP);

	/* the PHY reports the link, the PCS must not wait for in-band words */
	writel(PCS_LP_INBAND_DISABLE, priv->pcs + PCS_LP_AUTONEG);
}

static int rtl8221b_mmd_modify(struct ni_priv *priv, int addr, int devad,
			       int reg, u16 clr, u16 set)
{
	int val = ni_mdio_read(priv->bus, addr, devad, reg);

	if (val < 0)
		return val;

	return ni_mdio_write(priv->bus, addr, devad, reg, (val & ~clr) | set);
}

static int rtl8221b_serdes_write(struct ni_priv *priv, int addr, u16 reg,
				 u16 val)
{
	struct mii_dev *bus = priv->bus;
	int i, cmd;

	ni_mdio_write(bus, addr, RTL_VEND1, RTL_VEND1_SERDES_ADDR, reg);
	ni_mdio_write(bus, addr, RTL_VEND1, RTL_VEND1_SERDES_DATA, val);
	ni_mdio_write(bus, addr, RTL_VEND1, RTL_VEND1_SERDES_CMD,
		      RTL_SERDES_CMD_WRITE | RTL_SERDES_CMD_BUSY);

	for (i = 0; i < 100; i++) {
		cmd = ni_mdio_read(bus, addr, RTL_VEND1, RTL_VEND1_SERDES_CMD);
		if (cmd >= 0 && !(cmd & RTL_SERDES_CMD_BUSY))
			return 0;
		mdelay(1);
	}

	return -ETIMEDOUT;
}

/* 2500base-x towards the SoC with pause based rate matching */
static void rtl8221b_init(struct ni_priv *priv, int addr)
{
	struct mii_dev *bus = priv->bus;

	ni_mdio_write(bus, addr, RTL_VEND1, 0x75f3, 0);
	rtl8221b_mmd_modify(priv, addr, RTL_VEND1, RTL_VEND1_SERDES_OPTION,
			    RTL_SERDES_OPTION_MODE, RTL_SERDES_2500BASEX);
	ni_mdio_write(bus, addr, RTL_VEND1, 0x6a04, 0x0503);
	ni_mdio_write(bus, addr, RTL_VEND1, 0x6f10, 0xd455);
	ni_mdio_write(bus, addr, RTL_VEND1, 0x6f11, 0x8020);
	rtl8221b_mmd_modify(priv, addr, RTL_VEND2, RTL_VEND2_PHYCR1,
			    RTL_PHYCR1_ALDPS_EN | RTL_PHYCR1_ALDPS_XTAL |
			    RTL_PHYCR1_PHYAD_0_EN, 0);
	rtl8221b_serdes_write(priv, addr, RTL_SERDES_ADDR_AUTONEG,
			      PCS_LP_INBAND_DISABLE);

	rtl8221b_mmd_modify(priv, addr, RTL_VEND2, RTL_VEND2_AN_10GBT_CTRL,
			    RTL_AN_ADV_2_5G | RTL_AN_ADV_5G | RTL_AN_ADV_10G,
			    RTL_AN_ADV_2_5G);

	ni_mdio_write(bus, addr, MDIO_DEVAD_NONE, MII_BMCR,
		      BMCR_ANENABLE | BMCR_ANRESTART);
}

static void rtl8221b_read_speed(struct ni_priv *priv, struct ni_port *port)
{
	int physr = ni_mdio_read(priv->bus, port->phy_addr, MDIO_DEVAD_NONE,
				 RTL_PHYSR);

	if (physr < 0)
		return;

	port->phydev->duplex = physr & RTL_PHYSR_DUPLEX ? DUPLEX_FULL :
							  DUPLEX_HALF;
	switch (physr & RTL_PHYSR_SPEED) {
	case 0x0000:
		port->phydev->speed = SPEED_10;
		break;
	case 0x0010:
		port->phydev->speed = SPEED_100;
		break;
	case 0x0020:
		port->phydev->speed = SPEED_1000;
		break;
	case 0x0210:
		port->phydev->speed = SPEED_2500;
		break;
	}
}

/* port handling */

static bool ni_port_link(struct ni_priv *priv, struct ni_port *port)
{
	/* the link bit latches low, read twice */
	phy_read(port->phydev, MDIO_DEVAD_NONE, MII_BMSR);
	return phy_read(port->phydev, MDIO_DEVAD_NONE, MII_BMSR) & BMSR_LSTATUS;
}

static struct ni_port *ni_find_link(struct ni_priv *priv)
{
	unsigned long start = get_timer(0);
	int i;

	do {
		for (i = 0; i < priv->num_ports; i++) {
			if (ni_port_link(priv, &priv->ports[i]))
				return &priv->ports[i];
		}
	} while (get_timer(start) < LINK_SCAN_MS);

	return NULL;
}

static void ni_select_port(struct ni_priv *priv, struct ni_port *port)
{
	struct phy_device *phydev = port->phydev;
	int p = port->port;

	ni_rmw(priv, NI_HV_STATIC_CFG, NI_HV_PORT_TO_CPU,
	       FIELD_PREP(NI_HV_PORT_TO_CPU, p));

	if (port->ext) {
		/* the SerDes always runs 2.5G, the PHY paces the MAC with pause */
		ni_rmw(priv, NI_PORT_STATIC_CFG(p), NI_PORT_INT_CFG,
		       FIELD_PREP(NI_PORT_INT_CFG, NI_INT_CFG_GMII));
		ni_rmw(priv, NI_PORT_GLB_CFG(p),
		       NI_PORT_GLB_SPEED_10M | NI_PORT_GLB_HALF, 0);
		ni_rmw(priv, NI_PORT_RXMAC_CFG(p), NI_PORT_RX_FLOW_DISABLE, 0);
	} else {
		ni_rmw(priv, NI_PORT_STATIC_CFG(p), NI_PORT_INT_CFG,
		       FIELD_PREP(NI_PORT_INT_CFG, phydev->speed < SPEED_1000 ?
				  NI_INT_CFG_MII : NI_INT_CFG_GMII));
		ni_rmw(priv, NI_PORT_GLB_CFG(p),
		       NI_PORT_GLB_SPEED_10M | NI_PORT_GLB_HALF,
		       (phydev->speed == SPEED_10 ? NI_PORT_GLB_SPEED_10M : 0) |
		       (phydev->duplex == DUPLEX_HALF ? NI_PORT_GLB_HALF : 0));
	}
	ni_rmw(priv, NI_PORT_RXMAC_CFG(p), 0, NI_PORT_RX_EN);
	ni_rmw(priv, NI_PORT_TXMAC_CFG(p), 0, NI_PORT_TX_EN);

	priv->active = p;
}

static int ni_start(struct udevice *dev)
{
	struct ni_priv *priv = dev_get_priv(dev);
	struct ni_port *port;

	port = ni_find_link(priv);
	if (!port) {
		printf("%s: no link on any LAN port\n", dev->name);
		return -ENETDOWN;
	}

	phy_startup(port->phydev);
	if (port->ext)
		rtl8221b_read_speed(priv, port);
	printf("%s: lan port %d, %dMbps %s duplex\n", dev->name, port->port,
	       port->phydev->speed,
	       port->phydev->duplex == DUPLEX_HALF ? "half" : "full");

	ni_wr(priv, NI_XRAM_ADRCFG_RX,
	      FIELD_PREP(NI_XRAM_ADRCFG_BASE, RX_BASE) |
	      FIELD_PREP(NI_XRAM_ADRCFG_TOP, RX_TOP));
	ni_wr(priv, NI_XRAM_ADRCFG_TX,
	      FIELD_PREP(NI_XRAM_ADRCFG_BASE, TX_BASE) |
	      FIELD_PREP(NI_XRAM_ADRCFG_TOP, TX_TOP));

	/* the packet disable bits are toggled after changing the addresses */
	ni_rmw(priv, NI_XRAM_CFG, NI_XRAM_CFG_PROMISC,
	       NI_XRAM_CFG_RX_DIS | NI_XRAM_CFG_TX_DIS |
	       FIELD_PREP(NI_XRAM_CFG_PROMISC, 3));
	udelay(100);
	ni_rmw(priv, NI_XRAM_CFG, NI_XRAM_CFG_RX_DIS | NI_XRAM_CFG_TX_DIS, 0);

	ni_select_port(priv, port);

	return 0;
}

static void ni_stop(struct udevice *dev)
{
}

/* XRAM access, in 32-bit words; each region wraps inside itself */

static u32 xram_next(u32 word, u32 base, u32 top)
{
	return word + 1 > top * 2 + 1 ? base * 2 : word + 1;
}

static u32 xram_rd(struct ni_priv *priv, u32 word)
{
	return readl(priv->xram + word * 4);
}

static void xram_wr(struct ni_priv *priv, u32 word, u32 val)
{
	writel(val, priv->xram + word * 4);
}

static int ni_recv(struct udevice *dev, int flags, uchar **packetp)
{
	struct ni_priv *priv = dev_get_priv(dev);
	u32 rd, wr, word, hdr, status, next_link, len, i;
	u32 *buf = (u32 *)priv->rx_buf;

	rd = ni_rd(priv, NI_XRAM_CPU_CFG_RX) & NI_XRAM_PTR_MASK;
	wr = ni_rd(priv, NI_XRAM_CPU_STA_RX) & NI_XRAM_PTR_MASK;
	if (rd == wr)
		return -EAGAIN;

	/* word 0 of a slot is unused */
	word = xram_next(rd * 2, RX_BASE, RX_TOP);
	hdr = xram_rd(priv, word);
	if (hdr & XRAM_HDR_OWN)
		return -EAGAIN;
	next_link = FIELD_GET(XRAM_HDR_NEXT_LINK, hdr);

	word = xram_next(word, RX_BASE, RX_TOP);
	status = xram_rd(priv, word);
	len = FIELD_GET(PKT_STAT_SIZE, status);

	if (!(status & PKT_STAT_VALID) || (status & PKT_STAT_ERR) ||
	    len > 1518 || len < 4) {
		ni_wr(priv, NI_XRAM_CPU_CFG_RX, next_link);
		return -EAGAIN;
	}

	/* skip the word after the status, the data starts after it */
	word = xram_next(word, RX_BASE, RX_TOP);
	word = xram_next(word, RX_BASE, RX_TOP);

	for (i = 0; i <= len / 4; i++) {
		buf[i] = xram_rd(priv, word);
		word = xram_next(word, RX_BASE, RX_TOP);
	}

	ni_wr(priv, NI_XRAM_CPU_CFG_RX, next_link);

	*packetp = priv->rx_buf;
	return len - 4;	/* without the FCS */
}

static int ni_send(struct udevice *dev, void *packet, int length)
{
	struct ni_priv *priv = dev_get_priv(dev);
	static u8 pkt_buf[2048] __aligned(8);
	u32 hw_rd, sw_wr, word, hdr, i, fcs;
	u32 *data = (u32 *)pkt_buf;
	u32 len, valid_bytes, next_link;

	if (!packet || length > MAX_TX_LEN)
		return -EINVAL;

	hw_rd = ni_rd(priv, NI_XRAM_CPU_STA_TX) & NI_XRAM_PTR_MASK;
	sw_wr = ni_rd(priv, NI_XRAM_CPU_CFG_TX) & NI_XRAM_PTR_MASK;
	if (hw_rd != sw_wr)
		return -EAGAIN;

	/* the frame plus its FCS is padded to 8 bytes and at least 64 bytes */
	len = length;
	if ((len + 4) % 8)
		len += 8 - (len + 4) % 8;
	memset(pkt_buf, 0, sizeof(pkt_buf));
	memcpy(&pkt_buf[HEADER_A_SIZE], packet, length);
	if (len + 4 < 64)
		len = 60;

	/* the hardware recalculates the FCS */
	fcs = crc32(0, &pkt_buf[HEADER_A_SIZE], len);
	memcpy(&pkt_buf[HEADER_A_SIZE + len], &fcs, sizeof(fcs));
	len += 4;

	valid_bytes = len % 8;
	next_link = sw_wr + (len + 7 + HEADER_A_SIZE) / 8 + 1;
	if (next_link > TX_TOP)
		next_link = TX_BASE + (next_link - (TX_TOP + 1));

	hdr = XRAM_HDR_OWN | FIELD_PREP(XRAM_HDR_BYTES_VALID, valid_bytes) |
	      FIELD_PREP(XRAM_HDR_NEXT_LINK, next_link);

	word = xram_next(sw_wr * 2, TX_BASE, TX_TOP);
	xram_wr(priv, word, hdr);
	word = xram_next(word, TX_BASE, TX_TOP);

	for (i = 0; i <= (len + HEADER_A_SIZE) / 4; i++) {
		xram_wr(priv, word, data[i]);
		word = xram_next(word, TX_BASE, TX_TOP);
	}

	ni_wr(priv, NI_XRAM_CPU_CFG_TX, next_link);

	return 0;
}

/* The factory MAC sits in the raw static_conf area, with a second copy */
static bool ni_read_flash_mac(struct udevice *dev, u8 *mac)
{
	struct mtd_info *mtd;
	u32 offset[2];
	size_t retlen;
	int i;

	if (dev_read_u32_array(dev, "mac-flash-offsets", offset, 2))
		return false;

	mtd_probe_devices();
	mtd = get_mtd_device_nm("spi-nand0");
	if (IS_ERR_OR_NULL(mtd))
		return false;

	for (i = 0; i < 2; i++) {
		if (!mtd_read(mtd, offset[i], ARP_HLEN, &retlen, mac) &&
		    is_valid_ethaddr(mac))
			break;
	}
	put_mtd_device(mtd);

	return i < 2;
}

static int ni_read_rom_hwaddr(struct udevice *dev)
{
	struct eth_pdata *pdata = dev_get_plat(dev);

	if (!ni_read_flash_mac(dev, pdata->enetaddr))
		net_random_ethaddr(pdata->enetaddr);

	return 0;
}

static int ni_connect_ports(struct udevice *dev)
{
	struct ni_priv *priv = dev_get_priv(dev);
	int i, val;

	for (i = 0; i < priv->num_ports; i++) {
		struct ni_port *port = &priv->ports[i];

		port->phydev = phy_connect(priv->bus, port->phy_addr, dev,
					   PHY_INTERFACE_MODE_GMII);
		if (!port->phydev) {
			printf("%s: no PHY at address %d\n", dev->name,
			       port->phy_addr);
			return -ENODEV;
		}

		phy_config(port->phydev);
		val = phy_read(port->phydev, MDIO_DEVAD_NONE, MII_BMCR);
		phy_write(port->phydev, MDIO_DEVAD_NONE, MII_BMCR,
			  val & ~BMCR_PDOWN);

		if (port->ext) {
			ni_sds_power_on(priv);
			ni_sds_up(priv);
			rtl8221b_init(priv, port->phy_addr);
		}
	}

	return 0;
}

static int ni_probe(struct udevice *dev)
{
	struct ni_priv *priv = dev_get_priv(dev);
	u32 val;
	int ret;

	priv->bus = mdio_alloc();
	if (!priv->bus)
		return -ENOMEM;

	priv->bus->read = ni_mdio_read;
	priv->bus->write = ni_mdio_write;
	priv->bus->priv = priv;
	snprintf(priv->bus->name, sizeof(priv->bus->name), "%s", dev->name);

	ret = mdio_register(priv->bus);
	if (ret)
		return ret;

	/* MDC divider of the external MDIO master */
	val = readl(priv->mdio + MDIO_CFG);
	writel(val | 0x00280000, priv->mdio + MDIO_CFG);

	ret = readl_poll_timeout(priv->ni + NI_HV_INIT_DONE, val,
				 val & NI_HV_INIT_DONE_NI, 500000);
	if (ret)
		printf("%s: NI is not ready\n", dev->name);

	ni_wr(priv, NI_HV_INTF_RST, 0);

	return ni_connect_ports(dev);
}

static int ni_of_to_plat(struct udevice *dev)
{
	struct ni_priv *priv = dev_get_priv(dev);
	int i, len;

	priv->ni = dev_remap_addr_index(dev, 0);
	priv->mdio = dev_remap_addr_index(dev, 1);
	priv->xram = dev_remap_addr_index(dev, 2);
	priv->gphy = dev_remap_addr_index(dev, 3);
	priv->glb = dev_remap_addr_index(dev, 4);
	priv->pcs = dev_remap_addr_index(dev, 5);
	if (!priv->ni || !priv->mdio || !priv->xram || !priv->gphy ||
	    !priv->glb || !priv->pcs)
		return -EINVAL;

	/* <phy-address ni-port> pairs */
	if (!dev_read_prop(dev, "ports", &len))
		return -EINVAL;
	priv->num_ports = min(len / 8, MAX_PORTS);

	for (i = 0; i < priv->num_ports; i++) {
		u32 phy, port;

		if (dev_read_u32_index(dev, "ports", i * 2, &phy) ||
		    dev_read_u32_index(dev, "ports", i * 2 + 1, &port))
			return -EINVAL;
		priv->ports[i].phy_addr = phy;
		priv->ports[i].port = port;
		priv->ports[i].ext = false;
	}

	/* NI ports whose PHY is an RTL8221B behind the 2.5G SerDes */
	len = 0;
	if (dev_read_prop(dev, "realtek,rtl8221b-ports", &len)) {
		for (i = 0; i < len / 4; i++) {
			u32 port;
			int j;

			if (dev_read_u32_index(dev, "realtek,rtl8221b-ports",
					       i, &port))
				return -EINVAL;
			for (j = 0; j < priv->num_ports; j++)
				if (priv->ports[j].port == port)
					priv->ports[j].ext = true;
		}
	}

	return 0;
}

static const struct eth_ops ni_ops = {
	.start		= ni_start,
	.send		= ni_send,
	.recv		= ni_recv,
	.stop		= ni_stop,
	.read_rom_hwaddr = ni_read_rom_hwaddr,
};

static const struct udevice_id ni_ids[] = {
	{ .compatible = "realtek,rtl9607dq-ni" },
	{ }
};

U_BOOT_DRIVER(rtl9607dq_eth) = {
	.name		= "rtl9607dq_eth",
	.id		= UCLASS_ETH,
	.of_match	= ni_ids,
	.of_to_plat	= ni_of_to_plat,
	.probe		= ni_probe,
	.ops		= &ni_ops,
	.priv_auto	= sizeof(struct ni_priv),
	.plat_auto	= sizeof(struct eth_pdata),
};
