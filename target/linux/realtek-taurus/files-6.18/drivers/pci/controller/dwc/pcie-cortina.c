// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) Cortina Access, Inc.
 * Copyright (C) BRULE Herman <alpha_one_x86@first-world.info>
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 *
 * PCIe root complex of the Realtek RTL9607DQ (Cortina Access "Taurus") SoC.
 *
 * Each controller is a DesignWare core behind a glue block that holds the
 * address decoder, the link control and one interrupt line that carries
 * INTx, MSI and error events.  The reset, clock gate and SerDes mux/power
 * bits of all controllers sit in the shared global (GLB) register block.
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/irqdomain.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_pci.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include "pcie-designware.h"

/* Glue block */
#define CA_PCIE_GLBL_INT0		0x00
#define CA_PCIE_GLBL_INT_EN0		0x04
#define  CA_INT_INTA			BIT(0)
#define  CA_INT_MSI			BIT(8)
#define  CA_INT_HP			BIT(10)
#define  CA_INT_CORR_ERR		BIT(11)
#define  CA_INT_NONFATAL_ERR		BIT(12)
#define  CA_INT_FATAL_ERR		BIT(13)
#define  CA_INT_PM_TO_ACK		BIT(14)
#define  CA_INT_PM_PME			BIT(15)
#define  CA_INT_QOVERFLOW		BIT(16)
#define  CA_INT_LINK_DOWN		BIT(22)
#define  CA_INT_AER_RC_ERR_MSI		BIT(23)
#define  CA_INT_PME_MSI			BIT(24)
#define  CA_INT_HP_PME			BIT(25)
#define  CA_INT_HP_MSI			BIT(26)
#define  CA_INT_UR_RESP			BIT(27)
#define  CA_INT_INTX_ALL		(BIT(0) | BIT(2) | BIT(4) | BIT(6))
#define  CA_INT_MISC_ALL		(CA_INT_HP | CA_INT_CORR_ERR | \
					 CA_INT_NONFATAL_ERR | CA_INT_FATAL_ERR | \
					 CA_INT_PM_TO_ACK | CA_INT_PM_PME | \
					 CA_INT_QOVERFLOW | CA_INT_LINK_DOWN | \
					 CA_INT_AER_RC_ERR_MSI | CA_INT_PME_MSI | \
					 CA_INT_HP_PME | CA_INT_HP_MSI | \
					 CA_INT_UR_RESP)
#define CA_PCIE_CORE_CONFIG		0x18
#define  CA_CORE_LTSSM_ENABLE		BIT(0)
#define  CA_CORE_LINK_DOWN_RST		BIT(6)
#define CA_PCIE_PM_STATUS		0x1c
#define  CA_PM_LTSSM_STATE		GENMASK(9, 4)
#define  CA_PM_RDLH_LINK_UP		BIT(18)
#define CA_PCIE_CFG_START_ADDR		0x40
#define CA_PCIE_CFG_END_ADDR		0x44
#define CA_PCIE_IATU_BASE_ADDR		0x48
#define  CA_PCIE_IATU_BASE_MASK		GENMASK(31, 19)

/* GLB block */
#define GLB_BLOCK_RESET			0x098
#define GLB_DPHY_RESET			0x0a0
#define GLB_GLOBAL_CONFIG		0x0c0
#define GLB_PHY_CONTROL			0x0c4
#define GLB_PHY_ISO_POWER_CTRL		0x36c

/* SerDes, per lane: two sub-lanes 0x100 apart, lanes 0x1000 apart */
#define SERDES_LANE_STRIDE		0x1000
#define SERDES_SUBLANE_STRIDE		0x100
#define SERDES_RX_CTRL			0x024
#define  SERDES_RX_CTRL_OFF		0x500c
#define  SERDES_RX_CTRL_ON		0x520c
#define SERDES_STATUS			0x07c
#define  SERDES_STATUS_BER_NOTIFY	BIT(0)

#define CA_PCIE_MAX_LANES		2

/* Mux and power bits of one SerDes lane in GLB_PHY_CONTROL/ISO_POWER_CTRL */
struct cortina_pcie_lane {
	u32 ctrl_clr;
	u32 ctrl_set;
	u32 pwr_clr;
	u32 pwr_set;
};

struct cortina_pcie_port {
	u32 core_reset;		/* GLB_BLOCK_RESET */
	u32 phy_reset;		/* GLB_DPHY_RESET */
	u32 clk_en;		/* GLB_GLOBAL_CONFIG */
	unsigned int perst_ms;
	unsigned int ready_ms;
	struct cortina_pcie_lane lane[CA_PCIE_MAX_LANES];
};

static const struct cortina_pcie_port rtl9607dq_ports[] = {
	{	/* SerDes S0 */
		.core_reset = BIT(6),
		.phy_reset = BIT(0),
		.clk_en = BIT(10) | BIT(22),
		.perst_ms = 150,
		.ready_ms = 150,
		.lane = {
			{ .ctrl_clr = BIT(27), .pwr_clr = BIT(0), .pwr_set = 0x00310080 },
			{ .pwr_clr = BIT(1), .pwr_set = 0x00c00100 },
		},
	}, {	/* SerDes S1 */
		.core_reset = BIT(7),
		.phy_reset = BIT(1),
		.clk_en = BIT(12) | BIT(23),
		.perst_ms = 10,
		.ready_ms = 50,
		.lane = {
			{ .pwr_clr = BIT(2), .pwr_set = 0x03020200 },
			{ .pwr_clr = BIT(3), .pwr_set = 0x0c000400 },
		},
	},
};

struct cortina_serdes_cfg {
	u32 off;
	u32 val;
};

struct cortina_pcie {
	struct dw_pcie pci;
	const struct cortina_pcie_port *port;
	void __iomem *glue;
	void __iomem *serdes;
	resource_size_t serdes_size;
	struct regmap *glb;
	struct gpio_desc *perst;
	struct irq_domain *intx_domain;
	struct cortina_serdes_cfg *cfg;
	int cfg_cnt;
	phys_addr_t dbi_start;
	phys_addr_t dbi_end;
	phys_addr_t iatu_start;
};

#define to_cortina_pcie(x)	container_of((x), struct cortina_pcie, pci)

static u32 cortina_glue_readl(struct cortina_pcie *cp, u32 reg)
{
	return readl(cp->glue + reg);
}

static void cortina_glue_writel(struct cortina_pcie *cp, u32 val, u32 reg)
{
	writel(val, cp->glue + reg);
}

static void cortina_glue_rmw(struct cortina_pcie *cp, u32 reg, u32 clr, u32 set)
{
	cortina_glue_writel(cp, (cortina_glue_readl(cp, reg) & ~clr) | set, reg);
}

static void cortina_pcie_serdes_init(struct cortina_pcie *cp)
{
	int i;

	for (i = 0; i < cp->cfg_cnt; i++)
		if (cp->cfg[i].off < cp->serdes_size)
			writel(cp->cfg[i].val, cp->serdes + cp->cfg[i].off);
}

static void cortina_pcie_phy_power_on(struct cortina_pcie *cp)
{
	int i;

	for (i = 0; i < cp->pci.num_lanes; i++) {
		const struct cortina_pcie_lane *lane = &cp->port->lane[i];

		regmap_update_bits(cp->glb, GLB_PHY_CONTROL,
				   lane->ctrl_clr | lane->ctrl_set, lane->ctrl_set);
		regmap_update_bits(cp->glb, GLB_PHY_ISO_POWER_CTRL,
				   lane->pwr_clr | lane->pwr_set, lane->pwr_set);
	}
}

static void cortina_pcie_serdes_rx_start(struct cortina_pcie *cp)
{
	int i, sub;

	for (i = 0; i < cp->pci.num_lanes; i++) {
		for (sub = 0; sub < 2; sub++) {
			void __iomem *reg = cp->serdes + i * SERDES_LANE_STRIDE +
					    sub * SERDES_SUBLANE_STRIDE + SERDES_RX_CTRL;

			writel(SERDES_RX_CTRL_OFF, reg);
			writel(SERDES_RX_CTRL_ON, reg);
		}
	}
}

static bool cortina_pcie_serdes_ready(struct cortina_pcie *cp)
{
	u32 ready = SERDES_STATUS_BER_NOTIFY;
	int i, sub;

	for (i = 0; i < cp->pci.num_lanes; i++)
		for (sub = 0; sub < 2; sub++)
			ready &= readl(cp->serdes + i * SERDES_LANE_STRIDE +
				       sub * SERDES_SUBLANE_STRIDE + SERDES_STATUS);

	return ready;
}

/*
 * Bring-up order of the vendor driver: hold the endpoint, core and PHY in
 * reset, load the SerDes table, power the lanes, release PHY then core,
 * restart the receivers and only release PERST# once the SerDes reports a
 * good bit error rate.
 */
static void cortina_pcie_host_reset(struct cortina_pcie *cp)
{
	const struct cortina_pcie_port *port = cp->port;
	bool ready;

	regmap_set_bits(cp->glb, GLB_GLOBAL_CONFIG, port->clk_en);

	gpiod_set_value_cansleep(cp->perst, 1);
	regmap_set_bits(cp->glb, GLB_BLOCK_RESET, port->core_reset);
	regmap_set_bits(cp->glb, GLB_DPHY_RESET, port->phy_reset);
	msleep(port->perst_ms);

	cortina_pcie_serdes_init(cp);
	cortina_pcie_phy_power_on(cp);

	regmap_clear_bits(cp->glb, GLB_DPHY_RESET, port->phy_reset);
	regmap_clear_bits(cp->glb, GLB_BLOCK_RESET, port->core_reset);
	cortina_pcie_serdes_rx_start(cp);

	/* The RTL9607DQ never reports it (stock logs "No BER Notify!" too). */
	if (read_poll_timeout(cortina_pcie_serdes_ready, ready, ready,
			      1000, 100000, false, cp))
		dev_dbg(cp->pci.dev, "SerDes BER notify timeout\n");

	gpiod_set_value_cansleep(cp->perst, 0);
	msleep(port->ready_ms);
}

static void cortina_pcie_setup_windows(struct cortina_pcie *cp)
{
	cortina_glue_writel(cp, cp->dbi_start, CA_PCIE_CFG_START_ADDR);
	cortina_glue_writel(cp, cp->dbi_end, CA_PCIE_CFG_END_ADDR);
	cortina_glue_writel(cp, cp->iatu_start & CA_PCIE_IATU_BASE_MASK,
			    CA_PCIE_IATU_BASE_ADDR);
}

static enum dw_pcie_ltssm cortina_pcie_get_ltssm(struct dw_pcie *pci)
{
	struct cortina_pcie *cp = to_cortina_pcie(pci);

	return FIELD_GET(CA_PM_LTSSM_STATE,
			 cortina_glue_readl(cp, CA_PCIE_PM_STATUS));
}

static bool cortina_pcie_link_up(struct dw_pcie *pci)
{
	struct cortina_pcie *cp = to_cortina_pcie(pci);

	return cortina_glue_readl(cp, CA_PCIE_PM_STATUS) & CA_PM_RDLH_LINK_UP;
}

static int cortina_pcie_start_link(struct dw_pcie *pci)
{
	struct cortina_pcie *cp = to_cortina_pcie(pci);

	cortina_glue_writel(cp, CA_CORE_LTSSM_ENABLE, CA_PCIE_CORE_CONFIG);

	return 0;
}

static void cortina_pcie_stop_link(struct dw_pcie *pci)
{
	struct cortina_pcie *cp = to_cortina_pcie(pci);

	cortina_glue_writel(cp, CA_CORE_LINK_DOWN_RST, CA_PCIE_CORE_CONFIG);
	usleep_range(100, 200);
	cortina_glue_writel(cp, 0, CA_PCIE_CORE_CONFIG);
}

static const struct dw_pcie_ops cortina_dw_pcie_ops = {
	.link_up	= cortina_pcie_link_up,
	.get_ltssm	= cortina_pcie_get_ltssm,
	.start_link	= cortina_pcie_start_link,
	.stop_link	= cortina_pcie_stop_link,
};

/*
 * The DesignWare MSI controller has no interrupt line of its own here: it is
 * folded into the glue interrupt, so the glue handler calls
 * dw_handle_msi_irq() and this driver provides the bottom chip that the core
 * would otherwise install.  It follows dw_pci_msi_bottom_irq_chip.
 */
static void cortina_pcie_msi_compose(struct irq_data *d, struct msi_msg *msg)
{
	struct dw_pcie_rp *pp = irq_data_get_irq_chip_data(d);

	msg->address_lo = lower_32_bits(pp->msi_data);
	msg->address_hi = upper_32_bits(pp->msi_data);
	msg->data = d->hwirq;
}

static void cortina_pcie_msi_update_mask(struct irq_data *d, bool mask)
{
	struct dw_pcie_rp *pp = irq_data_get_irq_chip_data(d);
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	unsigned int ctrl = d->hwirq / MAX_MSI_IRQS_PER_CTRL;
	unsigned int bit = d->hwirq % MAX_MSI_IRQS_PER_CTRL;
	unsigned long flags;

	raw_spin_lock_irqsave(&pp->lock, flags);
	if (mask)
		pp->irq_mask[ctrl] |= BIT(bit);
	else
		pp->irq_mask[ctrl] &= ~BIT(bit);
	dw_pcie_writel_dbi(pci, PCIE_MSI_INTR0_MASK + ctrl * MSI_REG_CTRL_BLOCK_SIZE,
			   pp->irq_mask[ctrl]);
	raw_spin_unlock_irqrestore(&pp->lock, flags);
}

static void cortina_pcie_msi_mask(struct irq_data *d)
{
	cortina_pcie_msi_update_mask(d, true);
}

static void cortina_pcie_msi_unmask(struct irq_data *d)
{
	cortina_pcie_msi_update_mask(d, false);
}

static void cortina_pcie_msi_ack(struct irq_data *d)
{
	struct dw_pcie_rp *pp = irq_data_get_irq_chip_data(d);
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	unsigned int ctrl = d->hwirq / MAX_MSI_IRQS_PER_CTRL;
	unsigned int bit = d->hwirq % MAX_MSI_IRQS_PER_CTRL;

	dw_pcie_writel_dbi(pci, PCIE_MSI_INTR0_STATUS + ctrl * MSI_REG_CTRL_BLOCK_SIZE,
			   BIT(bit));
}

#ifdef CONFIG_SMP
static void cortina_pcie_msi_noop(struct irq_data *d)
{
}
#endif

static struct irq_chip cortina_pcie_msi_bottom_chip = {
	.name			= "CA-PCI-MSI",
	.irq_compose_msi_msg	= cortina_pcie_msi_compose,
	.irq_mask		= cortina_pcie_msi_mask,
	.irq_unmask		= cortina_pcie_msi_unmask,
#ifdef CONFIG_SMP
	.irq_ack		= cortina_pcie_msi_noop,
	.irq_pre_redirect	= cortina_pcie_msi_ack,
	.irq_set_affinity	= irq_chip_redirect_set_affinity,
#else
	.irq_ack		= cortina_pcie_msi_ack,
#endif
};

static int cortina_pcie_msi_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct cortina_pcie *cp = to_cortina_pcie(pci);
	struct device *dev = pci->dev;
	unsigned int ctrl, num_ctrls;
	void *msi_vaddr;
	int ret;

	pp->num_vectors = MSI_DEF_NUM_VECTORS;
	num_ctrls = pp->num_vectors / MAX_MSI_IRQS_PER_CTRL;
	for (ctrl = 0; ctrl < num_ctrls; ctrl++)
		pp->irq_mask[ctrl] = ~0;

	pp->msi_irq_chip = &cortina_pcie_msi_bottom_chip;
	ret = dw_pcie_allocate_domains(pp);
	if (ret)
		return ret;

	ret = dma_set_coherent_mask(dev, DMA_BIT_MASK(32));
	if (ret)
		goto err_remove_domain;

	msi_vaddr = dmam_alloc_coherent(dev, sizeof(u64), &pp->msi_data, GFP_KERNEL);
	if (!msi_vaddr) {
		ret = -ENOMEM;
		goto err_remove_domain;
	}

	for (ctrl = 0; ctrl < num_ctrls; ctrl++) {
		dw_pcie_writel_dbi(pci, PCIE_MSI_INTR0_MASK + ctrl * MSI_REG_CTRL_BLOCK_SIZE,
				   pp->irq_mask[ctrl]);
		dw_pcie_writel_dbi(pci, PCIE_MSI_INTR0_ENABLE + ctrl * MSI_REG_CTRL_BLOCK_SIZE,
				   ~0);
	}
	dw_pcie_writel_dbi(pci, PCIE_MSI_ADDR_LO, lower_32_bits(pp->msi_data));
	dw_pcie_writel_dbi(pci, PCIE_MSI_ADDR_HI, upper_32_bits(pp->msi_data));

	cortina_glue_rmw(cp, CA_PCIE_GLBL_INT_EN0, 0, CA_INT_MSI);

	return 0;

err_remove_domain:
	irq_domain_remove(pp->irq_domain);
	return ret;
}

static void cortina_pcie_intx_mask(struct irq_data *d)
{
	struct cortina_pcie *cp = irq_data_get_irq_chip_data(d);

	cortina_glue_rmw(cp, CA_PCIE_GLBL_INT_EN0, CA_INT_INTA << (2 * d->hwirq), 0);
}

static void cortina_pcie_intx_unmask(struct irq_data *d)
{
	struct cortina_pcie *cp = irq_data_get_irq_chip_data(d);

	cortina_glue_rmw(cp, CA_PCIE_GLBL_INT_EN0, 0, CA_INT_INTA << (2 * d->hwirq));
}

static struct irq_chip cortina_pcie_intx_chip = {
	.name		= "CA-PCI-INTx",
	.irq_mask	= cortina_pcie_intx_mask,
	.irq_unmask	= cortina_pcie_intx_unmask,
};

static int cortina_pcie_intx_map(struct irq_domain *domain, unsigned int irq,
				 irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler(irq, &cortina_pcie_intx_chip, handle_simple_irq);
	irq_set_chip_data(irq, domain->host_data);

	return 0;
}

static const struct irq_domain_ops cortina_pcie_intx_domain_ops = {
	.map	= cortina_pcie_intx_map,
	.xlate	= pci_irqd_intx_xlate,
};

static int cortina_pcie_init_intx(struct cortina_pcie *cp)
{
	struct device *dev = cp->pci.dev;
	struct device_node *node;

	node = of_get_child_by_name(dev->of_node, "interrupt-controller");
	if (!node) {
		dev_err(dev, "missing INTx interrupt-controller node\n");
		return -EINVAL;
	}

	cp->intx_domain = irq_domain_create_linear(of_fwnode_handle(node),
						   PCI_NUM_INTX,
						   &cortina_pcie_intx_domain_ops, cp);
	of_node_put(node);

	return cp->intx_domain ? 0 : -ENOMEM;
}

static irqreturn_t cortina_pcie_irq_handler(int irq, void *arg)
{
	struct cortina_pcie *cp = arg;
	unsigned int i;
	u32 val;

	val = cortina_glue_readl(cp, CA_PCIE_GLBL_INT0);
	if (!val)
		return IRQ_NONE;

	if (val & CA_INT_MSI)
		dw_handle_msi_irq(&cp->pci.pp);

	for (i = 0; i < PCI_NUM_INTX; i++)
		if (val & (CA_INT_INTA << (2 * i)))
			generic_handle_domain_irq(cp->intx_domain, i);

	if (val & CA_INT_LINK_DOWN) {
		dev_err(cp->pci.dev, "link down, LTSSM 0x%x\n",
			cortina_pcie_get_ltssm(&cp->pci));
		cortina_glue_writel(cp, CA_CORE_LINK_DOWN_RST, CA_PCIE_CORE_CONFIG);
	}

	cortina_glue_writel(cp, val, CA_PCIE_GLBL_INT0);

	return IRQ_HANDLED;
}

static int cortina_pcie_host_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct cortina_pcie *cp = to_cortina_pcie(pci);
	struct platform_device *pdev = to_platform_device(pci->dev);
	int irq, ret;

	cortina_glue_writel(cp, 0, CA_PCIE_GLBL_INT_EN0);

	cortina_pcie_host_reset(cp);
	cortina_pcie_setup_windows(cp);

	ret = cortina_pcie_init_intx(cp);
	if (ret)
		return ret;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	ret = devm_request_irq(pci->dev, irq, cortina_pcie_irq_handler,
			       IRQF_SHARED, dev_name(pci->dev), cp);
	if (ret)
		return ret;

	cortina_glue_rmw(cp, CA_PCIE_GLBL_INT_EN0, 0, CA_INT_MISC_ALL);

	return 0;
}

static const struct dw_pcie_host_ops cortina_pcie_host_ops = {
	.init		= cortina_pcie_host_init,
	.msi_init	= cortina_pcie_msi_init,
};

static int cortina_pcie_parse_dt(struct platform_device *pdev,
				 struct cortina_pcie *cp)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct dw_pcie *pci = &cp->pci;
	struct resource *res;
	int domain, n;

	cp->glue = devm_platform_ioremap_resource_byname(pdev, "glbl_regs");
	if (IS_ERR(cp->glue))
		return PTR_ERR(cp->glue);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "rc_dbi");
	pci->dbi_base = devm_ioremap_resource(dev, res);
	if (IS_ERR(pci->dbi_base))
		return PTR_ERR(pci->dbi_base);
	pci->dbi_phys_addr = res->start;
	cp->dbi_start = res->start;
	cp->dbi_end = res->end;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "iatu");
	pci->atu_base = devm_ioremap_resource(dev, res);
	if (IS_ERR(pci->atu_base))
		return PTR_ERR(pci->atu_base);
	pci->atu_phys_addr = res->start;
	pci->atu_size = resource_size(res);
	cp->iatu_start = res->start;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "serdes_phy");
	cp->serdes = devm_ioremap_resource(dev, res);
	if (IS_ERR(cp->serdes))
		return PTR_ERR(cp->serdes);
	cp->serdes_size = resource_size(res);

	cp->glb = syscon_regmap_lookup_by_phandle(np, "realtek,glb");
	if (IS_ERR(cp->glb))
		return dev_err_probe(dev, PTR_ERR(cp->glb), "missing GLB syscon\n");

	domain = of_get_pci_domain_nr(np);
	if (domain < 0 || domain >= ARRAY_SIZE(rtl9607dq_ports))
		return dev_err_probe(dev, -EINVAL, "invalid linux,pci-domain\n");
	cp->port = &rtl9607dq_ports[domain];

	if (pci->num_lanes < 1 || pci->num_lanes > CA_PCIE_MAX_LANES)
		pci->num_lanes = 1;

	n = of_property_count_elems_of_size(np, "realtek,serdes-cfg",
					    sizeof(*cp->cfg));
	if (n < 1)
		return dev_err_probe(dev, -EINVAL, "missing realtek,serdes-cfg\n");

	cp->cfg = devm_kcalloc(dev, n, sizeof(*cp->cfg), GFP_KERNEL);
	if (!cp->cfg)
		return -ENOMEM;
	cp->cfg_cnt = n;

	of_property_read_u32_array(np, "realtek,serdes-cfg", (u32 *)cp->cfg, n * 2);

	cp->perst = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(cp->perst))
		return dev_err_probe(dev, PTR_ERR(cp->perst), "missing reset GPIO\n");

	return 0;
}

static int cortina_pcie_probe(struct platform_device *pdev)
{
	struct cortina_pcie *cp;
	int ret;

	cp = devm_kzalloc(&pdev->dev, sizeof(*cp), GFP_KERNEL);
	if (!cp)
		return -ENOMEM;

	cp->pci.dev = &pdev->dev;
	cp->pci.ops = &cortina_dw_pcie_ops;
	cp->pci.pp.ops = &cortina_pcie_host_ops;
	platform_set_drvdata(pdev, cp);

	of_property_read_u32(pdev->dev.of_node, "num-lanes", &cp->pci.num_lanes);

	ret = cortina_pcie_parse_dt(pdev, cp);
	if (ret)
		return ret;

	return dw_pcie_host_init(&cp->pci.pp);
}

static const struct of_device_id cortina_pcie_of_match[] = {
	{ .compatible = "realtek,rtl9607dq-pcie" },
	{ }
};
MODULE_DEVICE_TABLE(of, cortina_pcie_of_match);

static struct platform_driver cortina_pcie_driver = {
	.probe	= cortina_pcie_probe,
	.driver = {
		.name			= "cortina-pcie",
		.of_match_table		= cortina_pcie_of_match,
		.suppress_bind_attrs	= true,
	},
};
builtin_platform_driver(cortina_pcie_driver);

MODULE_DESCRIPTION("Realtek RTL9607DQ PCIe host controller driver");
MODULE_LICENSE("GPL");
