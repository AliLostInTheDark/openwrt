// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 *
 * USB 2.0 and USB 3.0 PHYs of the Realtek RTL9607DQ (Cortina Access "Taurus").
 *
 * The DWC3 host has two USB 2.0 ports and one USB 3.0 port.  The USB 3.0 port
 * runs on SerDes S2, which is muxed and powered from the global (GLB) block.
 * Sequence and parameter tables follow the vendor taurus-usb2phy/usb3phy
 * drivers for chip revision B, the only revision this board ships with.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

/* GLB block */
#define GLB_BLOCK_RESET			0x098
#define  GLB_RESET_USB			BIT(13)
#define GLB_BLOCK_RESET_EXT		0x09c
#define  GLB_USB2_PORT1_PHY_RESET	BIT(5)
#define GLB_PHY_CONTROL			0x0c4
#define  GLB_S2_USB3_SEL		(BIT(19) | BIT(20))
#define  GLB_S2_PCIE_USB3_SGMII_SEL	(BIT(21) | BIT(22))
#define GLB_PHY_ISO_POWER_CTRL		0x36c
#define  GLB_S2_USB3_POWER_ON		0x30040800	/* POW_USB3, ISO_ANA_B, lane enables */
#define  GLB_S2_USB3_POWER_CLR		0x00001010	/* POW_PCIE2, CPHY_ISOLATE */

/* USBCFG block */
#define USB_U3_CONTROL			0x00
#define  USB_U3_CONTROL_EN		0x80000300
#define USB_U2PHY_PORT_CONFIG(p)	(0x20 + (p) * 0x08)
#define  USB_U2PHY_DATA			GENMASK(31, 24)
#define  USB_U2PHY_ADDR_NIBBLE		GENMASK(23, 20)
#define  USB_U2PHY_STROBE		BIT(19)
#define  USB_U2PHY_IDLE			0x00000064
#define USB_U2PHY_PORT0_FORCE_MODE	0x30
#define USB_U2PHY_PORT1_FORCE_MODE	0x34
#define USB_U3_EXT_CONTROL0		0x4c
#define  USB_U3_EXT_HOLD		BIT(28)

#define USB2_NUM_PORTS			2

enum { RTL9607DQ_PHY_USB2, RTL9607DQ_PHY_USB3, RTL9607DQ_NUM_PHYS };

struct rtl9607dq_reg_val {
	u8 addr;
	u16 val;
};

/* USB 2.0 PHY "revB v1.3 (2024.03.06)"; 0xf4 is the page register. */
static const u8 usb2_addr[] = { 0xe0, 0xe2, 0xe4, 0xe7, 0xf4, 0xe7, 0xf4, 0xe0, 0xe0, 0xe0 };
static const u8 usb2_val[USB2_NUM_PORTS][ARRAY_SIZE(usb2_addr)] = {
	{ 0x1f, 0xf9, 0x3b, 0x71, 0xdb, 0x73, 0xbb, 0x26, 0x22, 0x26 },
	{ 0x1f, 0xf9, 0x3b, 0x71, 0xdb, 0x53, 0xbb, 0x26, 0x22, 0x26 },
};

/* USB 3.0 PHY on SerDes S2, "revB v1.0 (2022.02.24)". */
static const struct rtl9607dq_reg_val usb3_table[] = {
	{ 0x01, 0xaccc }, { 0x04, 0x7000 }, { 0x05, 0x6313 }, { 0x08, 0x30d1 },
	{ 0x09, 0x424c }, { 0x0a, 0xc610 }, { 0x0b, 0xa905 }, { 0x0d, 0xef2e },
	{ 0x0e, 0x1000 }, { 0x20, 0x705e }, { 0x21, 0xf8e8 }, { 0x22, 0x0003 },
	{ 0x23, 0x0b66 }, { 0x27, 0x0241 }, { 0x2b, 0x0038 }, { 0x33, 0x0001 },
	{ 0x09, 0x424c }, { 0x09, 0x404c }, { 0x09, 0x424c },
};

struct rtl9607dq_usb_phy {
	void __iomem *usb;
	void __iomem *serdes;
	struct regmap *glb;
	struct regulator *vbus;
	struct phy *phys[RTL9607DQ_NUM_PHYS];
};

static void rtl9607dq_rmw(void __iomem *reg, u32 clr, u32 set)
{
	writel((readl(reg) & ~clr) | set, reg);
}

static void rtl9607dq_usb2_write_nibble(void __iomem *reg, u8 val, u8 nibble)
{
	u32 cmd = FIELD_PREP(USB_U2PHY_DATA, val) |
		  FIELD_PREP(USB_U2PHY_ADDR_NIBBLE, nibble) | USB_U2PHY_IDLE;

	writel(cmd | USB_U2PHY_STROBE, reg);
	writel(cmd, reg);
	writel(cmd | USB_U2PHY_STROBE, reg);
}

static void rtl9607dq_usb2_write(struct rtl9607dq_usb_phy *priv, int port,
				 u8 addr, u8 val)
{
	void __iomem *reg = priv->usb + USB_U2PHY_PORT_CONFIG(port);

	rtl9607dq_usb2_write_nibble(reg, val, addr & 0xf);
	rtl9607dq_usb2_write_nibble(reg, val, addr >> 4);
}

static int rtl9607dq_usb2_init(struct phy *phy)
{
	struct rtl9607dq_usb_phy *priv = phy_get_drvdata(phy);
	int port, i;

	for (port = 0; port < USB2_NUM_PORTS; port++)
		for (i = 0; i < ARRAY_SIZE(usb2_addr); i++)
			rtl9607dq_usb2_write(priv, port, usb2_addr[i],
					     usb2_val[port][i]);

	rtl9607dq_rmw(priv->usb + USB_U2PHY_PORT0_FORCE_MODE, BIT(9), BIT(7));
	rtl9607dq_rmw(priv->usb + USB_U2PHY_PORT1_FORCE_MODE, BIT(25), 0);

	return 0;
}

static int rtl9607dq_usb3_init(struct phy *phy)
{
	struct rtl9607dq_usb_phy *priv = phy_get_drvdata(phy);
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(usb3_table); i++)
		writel(usb3_table[i].val, priv->serdes + (usb3_table[i].addr << 2));

	rtl9607dq_rmw(priv->usb + USB_U3_EXT_CONTROL0, USB_U3_EXT_HOLD, 0);

	ret = regulator_enable(priv->vbus);
	if (ret)
		return ret;

	msleep(100);
	return 0;
}

static int rtl9607dq_usb3_exit(struct phy *phy)
{
	struct rtl9607dq_usb_phy *priv = phy_get_drvdata(phy);

	return regulator_disable(priv->vbus);
}

static const struct phy_ops rtl9607dq_usb2_ops = {
	.init	= rtl9607dq_usb2_init,
	.owner	= THIS_MODULE,
};

static const struct phy_ops rtl9607dq_usb3_ops = {
	.init	= rtl9607dq_usb3_init,
	.exit	= rtl9607dq_usb3_exit,
	.owner	= THIS_MODULE,
};

/*
 * Take the USB block out of reset, enable both USB 2.0 ports and hand SerDes
 * S2 to USB 3.0.  This has to happen before the DWC3 core probes, since it
 * reads its ID register straight away; the phys link in the device tree makes
 * the core wait for this driver.
 */
static void rtl9607dq_usb_hw_init(struct rtl9607dq_usb_phy *priv)
{
	regmap_set_bits(priv->glb, GLB_BLOCK_RESET, GLB_RESET_USB);
	msleep(20);
	regmap_clear_bits(priv->glb, GLB_BLOCK_RESET, GLB_RESET_USB);
	msleep(20);
	regmap_clear_bits(priv->glb, GLB_BLOCK_RESET_EXT, GLB_USB2_PORT1_PHY_RESET);

	rtl9607dq_rmw(priv->usb + USB_U2PHY_PORT_CONFIG(0), 0,
		      USB_U2PHY_STROBE | USB_U2PHY_IDLE);
	rtl9607dq_rmw(priv->usb + USB_U2PHY_PORT_CONFIG(1), 0,
		      USB_U2PHY_STROBE | USB_U2PHY_IDLE);

	regmap_update_bits(priv->glb, GLB_PHY_CONTROL,
			   GLB_S2_PCIE_USB3_SGMII_SEL | GLB_S2_USB3_SEL, GLB_S2_USB3_SEL);
	regmap_update_bits(priv->glb, GLB_PHY_ISO_POWER_CTRL,
			   GLB_S2_USB3_POWER_CLR | GLB_S2_USB3_POWER_ON,
			   GLB_S2_USB3_POWER_ON);
	msleep(20);

	rtl9607dq_rmw(priv->usb + USB_U3_CONTROL, 0, USB_U3_CONTROL_EN);
}

static struct phy *rtl9607dq_usb_phy_xlate(struct device *dev,
					   const struct of_phandle_args *args)
{
	struct rtl9607dq_usb_phy *priv = dev_get_drvdata(dev);

	if (args->args[0] >= RTL9607DQ_NUM_PHYS)
		return ERR_PTR(-EINVAL);

	return priv->phys[args->args[0]];
}

static int rtl9607dq_usb_phy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rtl9607dq_usb_phy *priv;
	struct phy_provider *provider;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->usb = devm_platform_ioremap_resource_byname(pdev, "usbcfg");
	if (IS_ERR(priv->usb))
		return PTR_ERR(priv->usb);

	priv->serdes = devm_platform_ioremap_resource_byname(pdev, "serdes");
	if (IS_ERR(priv->serdes))
		return PTR_ERR(priv->serdes);

	priv->glb = syscon_regmap_lookup_by_phandle(dev->of_node, "realtek,glb");
	if (IS_ERR(priv->glb))
		return dev_err_probe(dev, PTR_ERR(priv->glb), "missing GLB syscon\n");

	priv->vbus = devm_regulator_get(dev, "vbus");
	if (IS_ERR(priv->vbus))
		return dev_err_probe(dev, PTR_ERR(priv->vbus), "failed to get VBUS\n");

	priv->phys[RTL9607DQ_PHY_USB2] = devm_phy_create(dev, NULL, &rtl9607dq_usb2_ops);
	if (IS_ERR(priv->phys[RTL9607DQ_PHY_USB2]))
		return PTR_ERR(priv->phys[RTL9607DQ_PHY_USB2]);

	priv->phys[RTL9607DQ_PHY_USB3] = devm_phy_create(dev, NULL, &rtl9607dq_usb3_ops);
	if (IS_ERR(priv->phys[RTL9607DQ_PHY_USB3]))
		return PTR_ERR(priv->phys[RTL9607DQ_PHY_USB3]);

	phy_set_drvdata(priv->phys[RTL9607DQ_PHY_USB2], priv);
	phy_set_drvdata(priv->phys[RTL9607DQ_PHY_USB3], priv);
	dev_set_drvdata(dev, priv);

	rtl9607dq_usb_hw_init(priv);

	provider = devm_of_phy_provider_register(dev, rtl9607dq_usb_phy_xlate);
	return PTR_ERR_OR_ZERO(provider);
}

static const struct of_device_id rtl9607dq_usb_phy_of_match[] = {
	{ .compatible = "realtek,rtl9607dq-usb-phy" },
	{ }
};
MODULE_DEVICE_TABLE(of, rtl9607dq_usb_phy_of_match);

static struct platform_driver rtl9607dq_usb_phy_driver = {
	.probe = rtl9607dq_usb_phy_probe,
	.driver = {
		.name = "rtl9607dq-usb-phy",
		.of_match_table = rtl9607dq_usb_phy_of_match,
	},
};
module_platform_driver(rtl9607dq_usb_phy_driver);

MODULE_DESCRIPTION("Realtek RTL9607DQ USB 2.0/3.0 PHY driver");
MODULE_LICENSE("GPL");
