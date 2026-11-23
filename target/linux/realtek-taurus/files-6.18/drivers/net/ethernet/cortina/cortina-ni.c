// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015 Cortina-Access Limited
 * Copyright (C) BRULE Herman <alpha_one_x86@first-world.info>
 *
 * Cortina NI (network engine) Ethernet driver for the Realtek RTL9607DQ:
 * platform probe, register windows, NE block resets, internal GPHY and MDIO
 * bring-up, debug /proc nodes and module init. The datapath lives in
 * cortina-ni-tx.c and cortina-ni-rx.c.
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/mii.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/phy.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>

#include "cortina-ni.h"

static const char * const cortina_ni_reg_names[CA_NI_WIN_COUNT] = {
	[CA_NI_WIN_NI]		= "ni",
	[CA_NI_WIN_DMA]		= "dma",
	[CA_NI_WIN_AXI_REO]	= "reorder",
	[CA_NI_WIN_MDIO]	= "mdio",
	[CA_NI_WIN_GPHY]	= "gphy",
	[CA_NI_WIN_GPHY_WRAP]	= "gphy-wrap",
	[CA_NI_WIN_SGMII_PCS]	= "pcs",
};

static bool cortina_ni_phy_is_internal(int addr)
{
	return addr >= CA_NI_GPHY_FIRST &&
	       addr < CA_NI_GPHY_FIRST + CA_NI_GPHY_COUNT;
}

/*
 * External MDIO master, clause 22 (vendor aal_mdio_read/write_direct): write
 * the command word, start, poll for done, then for a read fetch RDDATA before
 * acking done (W1C), in the vendor order.
 */
static int cortina_ni_mdio_c22_cmd(struct cortina_ni *ni, u32 cmd, u32 *data)
{
	void __iomem *base = ni->win[CA_NI_WIN_MDIO];
	u32 ctrl;
	int ret;

	writel(cmd, base + CA_NI_MDIO_ADDR);
	/*
	 * Start the frame. Writing DONE too clears a stale done latch, as the
	 * vendor's read-modify-write of CTRL does.
	 */
	writel(CA_NI_MDIO_CTRL_START | CA_NI_MDIO_CTRL_DONE,
	       base + CA_NI_MDIO_CTRL);

	ret = readl_poll_timeout(base + CA_NI_MDIO_CTRL, ctrl,
				 ctrl & CA_NI_MDIO_CTRL_DONE,
				 1, CA_NI_MDIO_TIMEOUT_US);
	if (ret)
		return ret;

	if (data)
		*data = readl(base + CA_NI_MDIO_RDDATA) & 0xffff;

	/* ack: done is write-1-clear */
	writel(ctrl | CA_NI_MDIO_CTRL_DONE, base + CA_NI_MDIO_CTRL);
	return 0;
}

static int cortina_ni_mdio_master_read(struct cortina_ni *ni, int addr,
				       int regnum)
{
	u32 cmd, data;
	int ret;

	cmd = FIELD_PREP(CA_NI_MDIO_ADDR_PHY, addr) |
	      FIELD_PREP(CA_NI_MDIO_ADDR_REG, regnum) |
	      FIELD_PREP(CA_NI_MDIO_ADDR_OP, CA_NI_MDIO_OP_RD) |
	      CA_NI_MDIO_ADDR_RD_WR;

	ret = cortina_ni_mdio_c22_cmd(ni, cmd, &data);
	if (ret)
		return ret;

	return data;
}

static int cortina_ni_mdio_master_write(struct cortina_ni *ni, int addr,
					int regnum, u16 val)
{
	void __iomem *base = ni->win[CA_NI_WIN_MDIO];
	u32 cmd;

	cmd = FIELD_PREP(CA_NI_MDIO_ADDR_PHY, addr) |
	      FIELD_PREP(CA_NI_MDIO_ADDR_REG, regnum) |
	      FIELD_PREP(CA_NI_MDIO_ADDR_OP, CA_NI_MDIO_OP_WR);

	writel(val, base + CA_NI_MDIO_WRDATA);
	return cortina_ni_mdio_c22_cmd(ni, cmd, NULL);
}

/*
 * Internal quad GbE PHYs: registers are memory-mapped in the GPHY window,
 * one 256K bank per PHY.  Regs 0x10..0x17 are banked by the (software)
 * page-select shadow; all other MII regs live at a fixed offset.
 */
static u32 cortina_ni_gphy_reg(struct cortina_ni *ni, int addr, int regnum)
{
	u32 off = (addr - CA_NI_GPHY_FIRST) * CA_NI_GPHY_BANK_STRIDE;

	if (regnum >= CA_NI_GPHY_PAGED_FIRST &&
	    regnum <= CA_NI_GPHY_PAGED_LAST)
		off += ni->gphy_page[addr - CA_NI_GPHY_FIRST] *
			       CA_NI_GPHY_PAGE_STRIDE +
		       (regnum - CA_NI_GPHY_PAGED_FIRST) *
			       CA_NI_GPHY_REG_STRIDE;
	else
		off += CA_NI_GPHY_MII_BASE + regnum * CA_NI_GPHY_REG_STRIDE;

	return off;
}

static int cortina_ni_gphy_read(struct cortina_ni *ni, int addr, int regnum)
{
	if (regnum == 0x1f)	/* page select: software shadow only */
		return ni->gphy_page[addr - CA_NI_GPHY_FIRST];

	return readl(ni->win[CA_NI_WIN_GPHY] +
		     cortina_ni_gphy_reg(ni, addr, regnum)) & 0xffff;
}

static int cortina_ni_gphy_write(struct cortina_ni *ni, int addr, int regnum,
				 u16 val)
{
	if (regnum == 0x1f)
		ni->gphy_page[addr - CA_NI_GPHY_FIRST] = val;
	else
		writel(val, ni->win[CA_NI_WIN_GPHY] +
			    cortina_ni_gphy_reg(ni, addr, regnum));

	/* the vendor driver waits 1 ms after every internal PHY write */
	usleep_range(1000, 1500);
	return 0;
}

static int cortina_ni_mdio_read(struct mii_bus *bus, int addr, int regnum)
{
	struct cortina_ni *ni = bus->priv;

	regnum &= 0x1f;
	if (cortina_ni_phy_is_internal(addr) && ni->win[CA_NI_WIN_GPHY])
		return cortina_ni_gphy_read(ni, addr, regnum);

	return cortina_ni_mdio_master_read(ni, addr, regnum);
}

static int cortina_ni_mdio_write(struct mii_bus *bus, int addr, int regnum,
				 u16 val)
{
	struct cortina_ni *ni = bus->priv;

	regnum &= 0x1f;
	if (cortina_ni_phy_is_internal(addr) && ni->win[CA_NI_WIN_GPHY])
		return cortina_ni_gphy_write(ni, addr, regnum, val);

	return cortina_ni_mdio_master_write(ni, addr, regnum, val);
}

/*
 * GPHY wrapper enable (vendor aal_mdio_global_init plus the patch_phy_done bit
 * from aal_internal_phy_init). EN0/EN1 are written with fixed values rather
 * than ORed onto what U-Boot left: EN1 bit 12 (patch_phy_done) releases the
 * GPHY->port MAC datapath and U-Boot does not always set it, which left RX dead
 * on some boots. The internal PHYs are memory-mapped, so these MDIO-OCP bits do
 * not affect register access.
 */
/* MDC at 4 MHz, as the vendor driver sets it (aal_mdio_speed_set) */
#define CA_NI_MDIO_MDC_KHZ	4000

static void cortina_ni_mdio_hw_enable(struct cortina_ni *ni)
{
	void __iomem *wrap = ni->win[CA_NI_WIN_GPHY_WRAP];
	void __iomem *mdio = ni->win[CA_NI_WIN_MDIO];

	if (mdio) {
		u32 cfg = readl(mdio + CA_NI_MDIO_CFG) & ~CA_NI_MDIO_CFG_PRESCALE;

		cfg |= FIELD_PREP(CA_NI_MDIO_CFG_PRESCALE,
				  CA_NI_MDIO_REFCLK_KHZ / (2 * CA_NI_MDIO_MDC_KHZ));
		writel(cfg | CA_NI_MDIO_CFG_MANUAL, mdio + CA_NI_MDIO_CFG);
	}

	if (!wrap)
		return;

	writel(CA_NI_GPHY_WRAP_EN0_VAL, wrap + CA_NI_GPHY_WRAP_EN0);
	writel(CA_NI_GPHY_WRAP_EN1_VAL, wrap + CA_NI_GPHY_WRAP_EN1);
	usleep_range(1000, 1500);
	dev_dbg(ni->dev, "GPHY wrapper: EN0=0x%08x EN1=0x%08x (patch_phy_done set)\n",
		readl(wrap + CA_NI_GPHY_WRAP_EN0),
		readl(wrap + CA_NI_GPHY_WRAP_EN1));
}

/*
 * Internal quad GPHY bring-up, after the vendor aal_internal_phy_init, done
 * through the memory-mapped GPHY registers only: power up the PHY cores (CTRL1
 * PWRUP) and enable the GPHY wrapper (EN0/EN1), which releases the GPHY->MAC
 * datapath of all ports.
 *
 * The SRAM patch, uC resume and BMCR power-up are left to link-up
 * (cortina_ni_rx_link_up), after cortina_ni_rx_gphy_cal_save() has saved the
 * analog calibration U-Boot left on the port it used. Neither patching here nor
 * pulsing the per-port INTF_RST is safe: the patch is one-shot per boot, and an
 * INTF_RST of a calibrated port destroys its calibration so it never links
 * again.
 */
static void cortina_ni_internal_phy_init(struct cortina_ni *ni)
{
	void __iomem *wrap = ni->win[CA_NI_WIN_GPHY_WRAP];
	int p;

	if (wrap) {
		u32 c1 = readl(wrap + CA_NI_GPHY_WRAP_CTRL1);

		writel((c1 & ~CA_NI_GPHY_WRAP_CTRL1_PWRUP) |
		       CA_NI_GPHY_WRAP_CTRL1_PWRUP, wrap + CA_NI_GPHY_WRAP_CTRL1);
		usleep_range(1000, 1500);
	}

	cortina_ni_mdio_hw_enable(ni);

	/* log the PHY IDs after the reset release; 0x001c is a valid core */
	if (wrap) {
		dev_dbg(ni->dev, "after reset release: CTRL13=0x%08x\n",
			readl(wrap + CA_NI_GPHY_WRAP_EN1));
		for (p = CA_NI_GPHY_FIRST;
		     p < CA_NI_GPHY_FIRST + CA_NI_GPHY_COUNT; p++) {
			int id1 = cortina_ni_gphy_read(ni, p, 2);
			int id2 = cortina_ni_gphy_read(ni, p, 3);

			dev_dbg(ni->dev, "PHY%d ID=0x%04x:0x%04x %s\n",
				p, id1, id2,
				(id1 == 0x001c) ? "VALID" : "POISON");
		}
	}

	dev_dbg(ni->dev, "internal PHY cores powered, patch deferred to link-up\n");
}

static int cortina_ni_map_windows(struct cortina_ni *ni)
{
	struct platform_device *pdev = to_platform_device(ni->dev);
	int i;

	for (i = 0; i < CA_NI_WIN_COUNT; i++) {
		ni->win[i] = devm_platform_ioremap_resource_byname(pdev,
						cortina_ni_reg_names[i]);
		if (IS_ERR(ni->win[i]))
			return PTR_ERR(ni->win[i]);
	}

	ni->glb = syscon_regmap_lookup_by_phandle(ni->dev->of_node,
						  "realtek,glb");
	if (IS_ERR(ni->glb))
		return dev_err_probe(ni->dev, PTR_ERR(ni->glb),
				     "cannot get global registers\n");
	return 0;
}

static int cortina_ni_mdio_init(struct cortina_ni *ni)
{
	struct device *dev = ni->dev;
	struct device_node *mdio_np;
	struct mii_bus *bus;
	int ret;

	bus = devm_mdiobus_alloc(dev);
	if (!bus)
		return -ENOMEM;

	bus->name = CA_NI_DRV_NAME "-mdio";
	snprintf(bus->id, MII_BUS_ID_SIZE, "%s", dev_name(dev));
	bus->parent = dev;
	bus->priv = ni;
	bus->read = cortina_ni_mdio_read;
	bus->write = cortina_ni_mdio_write;

	cortina_ni_mdio_hw_enable(ni);

	/*
	 * Do not reset the PHYs here: U-Boot leaves the link of the port it
	 * used up and calibrated.
	 */
	mdio_np = of_get_child_by_name(dev->of_node, "mdio");
	ret = devm_of_mdiobus_register(dev, bus, mdio_np);
	of_node_put(mdio_np);
	if (ret)
		return dev_err_probe(dev, ret, "cannot register MDIO bus\n");

	ni->mii = bus;
	return 0;
}

/* ------------------------------------------------------------------ */
static int cortina_ni_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct cortina_ni *ni;
	int ret;

	ni = devm_kzalloc(dev, sizeof(*ni), GFP_KERNEL);
	if (!ni)
		return -ENOMEM;
	ni->dev = dev;
	/*
	 * must be ready before anything samples the read-and-clear NI_HV
	 * counters
	 */
	spin_lock_init(&ni->nihv_lock);
	mutex_init(&ni->l2_lock);
	platform_set_drvdata(pdev, ni);

	/* may defer (nvmem on flash): look it up before touching the hardware */
	ret = of_get_mac_address(dev->of_node, ni->mac);
	if (ret == -EPROBE_DEFER)
		return ret;

	ret = cortina_ni_map_windows(ni);
	if (ret)
		return ret;

	/*
	 * Release the GPHY <-> NI datapath sub-blocks from reset before any
	 * GPHY/MAC init. U-Boot leaves GLOBAL_DPHY_RESET with them held, and
	 * then no frame crosses the internal GMII even though the line side
	 * links. A release at link-up comes too late to re-init them.
	 */
	{
		/*
		 * The vendor ca_ni_global_reset sequence: reset ni, l2fe, l2tm,
		 * l3fe and tqm one at a time, in that order, 1 ms each, tqm
		 * last. sdram (bit 4) is skipped since it resets the DRAM
		 * controller. Resetting them together leaves the engine dead.
		 */
		static const int order[] = { 0, 1, 2, 3, 5 };
		int j;

		/*
		 * The DPHY sub-block release goes to DPHY_RESET (0x0a0), the
		 * block resets to BLOCK_RESET (0x098).
		 */
		regmap_write(ni->glb, CA_NI_GLB_DPHY_RESET,
			     CA_NI_GLB_BLOCK_RESET_VAL);
		for (j = 0; j < ARRAY_SIZE(order); j++) {
			regmap_set_bits(ni->glb, CA_NI_GLB_BLOCK_RESET,
					BIT(order[j]));
			mdelay(1);
			regmap_clear_bits(ni->glb, CA_NI_GLB_BLOCK_RESET,
					  BIT(order[j]));
		}
		mdelay(100);
	}

	/*
	 * The internal GPHY firmware patch and uC resume are done at link-up
	 * (cortina_ni_rx_link_up): at probe the uC is still running and its
	 * SRAM is not writable.
	 */

	/*
	 * bring up the quad GPHY through the memory-mapped path first, as the
	 * vendor does; the MDIO bus below only carries register access
	 */
	cortina_ni_internal_phy_init(ni);

	ret = cortina_ni_mdio_init(ni);
	if (ret)
		return ret;

	ret = cortina_ni_tx_probe(ni);
	if (ret)
		return ret;

	ret = cortina_ni_rx_probe(ni);
	if (ret)
		return ret;

	/*
	 * Register the DSA switch now that the conduit netdev and the MDIO bus
	 * are up. Not fatal: on error eth0 keeps working on its own.
	 */
	ret = cortina_ni_dsa_register(ni);
	if (ret)
		return dev_err_probe(dev, ret, "cannot add switch device\n");

	/* L3FE flow offload. Not fatal: on error offload stays disabled. */
	ret = cortina_ppe_probe(ni);
	if (ret)
		dev_warn(dev, "L3FE flow offload disabled (%d)\n", ret);

	/* the `ethtool -d` decode map (cortina-ni/regdump_map) */
	cortina_ni_debugfs_init(ni);

	dev_dbg(dev, "probe complete\n");
	return 0;
}

static const struct of_device_id cortina_ni_of_match[] = {
	{ .compatible = "realtek,rtl9607dq-ni" },
	{ }
};
MODULE_DEVICE_TABLE(of, cortina_ni_of_match);

static struct platform_driver cortina_ni_driver = {
	.probe = cortina_ni_probe,
	.driver = {
		.name = CA_NI_DRV_NAME,
		.of_match_table = cortina_ni_of_match,
	},
};

static struct platform_driver * const cortina_ni_drivers[] = {
	&cortina_ni_driver,
#if IS_ENABLED(CONFIG_CORTINA_NI_DSA)
	&cortina_ni_switch_driver,
#endif
};

static int __init cortina_ni_module_init(void)
{
	return platform_register_drivers(cortina_ni_drivers,
					 ARRAY_SIZE(cortina_ni_drivers));
}
module_init(cortina_ni_module_init);

static void __exit cortina_ni_module_exit(void)
{
	platform_unregister_drivers(cortina_ni_drivers,
				    ARRAY_SIZE(cortina_ni_drivers));
}
module_exit(cortina_ni_module_exit);

MODULE_DESCRIPTION("Cortina NI Ethernet driver for the Realtek RTL9607DQ");
MODULE_LICENSE("GPL");
