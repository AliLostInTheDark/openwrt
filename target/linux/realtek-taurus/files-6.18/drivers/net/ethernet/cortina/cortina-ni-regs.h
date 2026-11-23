/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2015 Cortina-Access Limited
 * Copyright (C) BRULE Herman <alpha_one_x86@first-world.info>
 *
 * Cortina-Access NI (network engine) register map for the Realtek RTL9607DQ
 * GPON SoC.  Offsets are relative to the DT register windows unless noted.
 */

#ifndef _CORTINA_NI_REGS_H
#define _CORTINA_NI_REGS_H

#include <linux/bits.h>

/* DT "reg" windows of the NI node, in reg-names order */
enum cortina_ni_win {
	CA_NI_WIN_NI,		/* NI, L2/L3 forwarding engines, queue manager */
	CA_NI_WIN_DMA,		/* LDMA TX rings */
	CA_NI_WIN_AXI_REO,	/* RMU AXI reorder (RX DMA) */
	CA_NI_WIN_MDIO,		/* MDIO master */
	CA_NI_WIN_GPHY,		/* quad GPHY and SerDes analog */
	CA_NI_WIN_GPHY_WRAP,	/* GPHY wrapper (power, uC control) */
	CA_NI_WIN_SGMII_PCS,	/* port 4 SGMII/HSGMII PCS */
	CA_NI_WIN_COUNT,
};

/*
 * Extra block not in the DT: 4K at 0xf4329000 (peripheral/MDIO/SDS area),
 * mapped through its 32-bit low alias.
 */

/* GLB window (idx 9): +0x00 is a read-only chip/JTAG ID - probe sanity read */
#define CA_NI_GLB_JTAG_ID		0x00

/*
 * GLOBAL_DPHY_RESET (GLB + 0xa0, phys 0xf43200a0).  Each bit holds a
 * digital-PHY / datapath sub-block in reset (1 = asserted), including the
 * internal quad-GPHY <-> NI-MAC datapath.  U-Boot leaves several asserted;
 * they must be released (leaving only bit 28, 0x10000000) early, before the
 * GPHY/MAC bring-up, as a late release does not re-init the sub-block.
 * While held, no frame crosses GPHY <-> MAC on any port even though the
 * GPHY line side links.
 *
 * GLOBAL_BLOCK_RESET (0x098) is a different register from DPHY_RESET.
 */
#define CA_NI_GLB_BLOCK_RESET		0x098
#define CA_NI_GLB_DPHY_RESET		0x0a0
#define CA_NI_GLB_BLOCK_RESET_VAL	0x10000000u	/* DPHY value: all NE sub-blocks released */

/*
 * NE block reset bits.  Each bit holds one NE sub-block in reset
 * (1 = asserted); reset is a plain read-modify-write set, 1 ms, clear (not
 * self-clearing).  Bit IDs: NI=0, L2FE=1, L2TM=2, L3FE=3, SDRAM=4, TQM=5.
 * U-Boot leaves the L3QM uninitialised, so QM_PHY_PORT_STS.qm_init_done
 * stays 0 and the empty-buffer pools never activate until it is reset.
 * Never pulse SDRAM: that would reset DRAM.
 */
#define CA_NI_GLB_BIST_CONTROL4		0x28
#define  CA_NI_GLB_RST_NI		BIT(0)	/* NI_RESET  (U-Boot TX path) */
#define  CA_NI_GLB_RST_L2FE		BIT(1)	/* L2FE_RESET */
#define  CA_NI_GLB_RST_L2TM		BIT(2)	/* L2TM_RESET */
#define  CA_NI_GLB_RST_L3FE		BIT(3)	/* L3FE_RESET */
#define  CA_NI_GLB_RST_TQM		BIT(5)	/* TQM_RESET = the L3QM block */
#define CA_NI_GLB_OPT_MODULE_STATUS	0x98
#define CA_NI_GLB_PON_CNTL	0x9c
#define CA_NI_GLB_BLOCK_RESET_EXT		0x09c
/*
 * GLOBAL_FABRIC_RESET bit 5 is the NI-MCE (multicast expansion) sub-block
 * reset.  While set, the NI-MCE RAM (0xaa6x) is held in reset and a write to
 * the mce_indx DATA register raises an asynchronous SError.  Deassert only
 * (clear, no pulse).
 */
#define  CA_NI_GLB_BLOCK_RESET_EXT_MCE	BIT(5)
/*
 * cortina_ni_qm_reset pulses NI, L2FE, L2TM, L3FE and TQM in sequence (SDRAM
 * skipped).  The QM's init-done depends on the NI<->QM handshake, so the NI
 * must be reset too; cortina_ni_tx_hw_init then rebuilds the FE-bypass TX
 * path the NI reset drops.
 */

/*
 * CapSRAM TrustZone segment-secure control: its own block at 40-bit phys
 * 0x4_f4322000, outside the GLB 4K window.  The CapSRAM backs the NI MCE
 * index table (0xaa6x).  SEGSECURE bits [17:0] (18 segments): 1 = secure,
 * 0 = non-secure.  Firmware may leave segments secure; a non-secure access
 * to a secure segment raises an asynchronous SError, so TZCONTROL is cleared
 * to 0 before the table is used.
 */
#define CA_NI_CAPSRAM_PHYS		0x4f4322000ULL	/* 40-bit: 0x4_f4322000 */
#define CA_NI_CAPSRAM_TZCONTROL		0x00	/* SEGSECURE[17:0]; =0 -> all NS */
#define CA_NI_CAPSRAM_INIT		0x04	/* bit0 INIT (SRAM mem-init)      */
#define CA_NI_CAPSRAM_INITRANGE		0x08	/* dft 0x11ff0000                 */

/*
 * MDIO master, relative to DT window idx 2: CFG/ADDR/WRDATA/RDDATA/CTRL at
 * +0/+4/+8/+0xc/+0x10.
 *
 * This master drives the external MDIO pins (clause 22 "direct" and clause
 * 45 "indirect").  The four internal GbE PHYs are not behind it, see the
 * GPHY window defines below.
 */
#define CA_NI_MDIO_CFG			0x00
#define  CA_NI_MDIO_CFG_PRESCALE	GENMASK(31, 16)	/* MDC divider */
#define  CA_NI_MDIO_CFG_MANUAL		BIT(0)		/* 1 = manual mode */
/* prescale = 125000 kHz / (2 * mdc_speed_khz) */
#define  CA_NI_MDIO_REFCLK_KHZ		125000

#define CA_NI_MDIO_ADDR			0x04
#define  CA_NI_MDIO_ADDR_PHY		GENMASK(4, 0)	/* PHY address     */
#define  CA_NI_MDIO_ADDR_REG		GENMASK(12, 8)	/* register offset */
#define  CA_NI_MDIO_ADDR_RD_WR		BIT(15)		/* 1 = read        */
#define  CA_NI_MDIO_ADDR_ST		BIT(16)		/* 0=direct 1=indir*/
#define  CA_NI_MDIO_ADDR_OP		GENMASK(19, 18)
#define   CA_NI_MDIO_OP_ADDR_WR		0	/* c45 address write  */
#define   CA_NI_MDIO_OP_WR		1	/* write              */
#define   CA_NI_MDIO_OP_RD_INC		2	/* read + post-incr   */
#define   CA_NI_MDIO_OP_RD		3	/* read               */

#define CA_NI_MDIO_WRDATA		0x08	/* [15:0] write data  */
#define CA_NI_MDIO_RDDATA		0x0c	/* [15:0] read data   */

#define CA_NI_MDIO_CTRL			0x10
#define  CA_NI_MDIO_CTRL_START		BIT(7)	/* kick the frame     */
#define  CA_NI_MDIO_CTRL_DONE		BIT(0)	/* done, write-1-clear*/

/* poll done up to 50000 x udelay(1) */
#define CA_NI_MDIO_TIMEOUT_US		50000

/*
 * Internal quad GbE PHYs (MDIO addresses 1..4).  They are not reached
 * through the MDIO master: their registers are memory-mapped in the GPHY
 * window (idx 11), one 256K (1 << 18) bank per PHY:
 *
 *   plain MII reg r        : bank + 0x29000 + 8 * r
 *   paged regs 0x10..0x17  : bank + 0x40 * page + 8 * (r - 0x10)
 *   reg 0x1f (page select) : software shadow only (no HW register), kept
 *                            per PHY; every internal-PHY write, including
 *                            the page write, is followed by a 1 ms delay.
 */
#define CA_NI_GPHY_FIRST		1
#define CA_NI_GPHY_COUNT		4
/*
 * The four RJ45 sockets are the four internal-GPHY NI ports (MDIO address =
 * CA_NI_GPHY_FIRST + port).  For a LAN NI port the L2FE ARB ldpid->pdpid map
 * is the identity, so ldpid == physical port == the value a direct-TX
 * descriptor carries in CA_NI_TX_DESC1_DEST.
 */
#define CA_NI_SGMII_PORT		4	/* external 2.5G PHY over (H)SGMII */
#define CA_NI_EXT_PHY_ADDR		7	/* MDIO address of that PHY */
#define CA_NI_LAN_PORT_COUNT		(CA_NI_GPHY_COUNT + 1)

/*
 * DSA port model.  The RJ45 jacks are DSA user ports whose dp->index is the
 * RX HEADER_A.lspid and the TX DESC1_DEST value (both identity for LAN NI
 * ports), so no translation table is needed.  The last port is the CPU
 * conduit (the "eth0" netdev).
 */
/* NI ports 0..4; DT names the wired ones */
#define CA_NI_DSA_USER_PORTS		CA_NI_LAN_PORT_COUNT
#define CA_NI_DSA_CPU_PORT		CA_NI_DSA_USER_PORTS	/* port 5 = conduit    */
#define CA_NI_DSA_NUM_PORTS		(CA_NI_DSA_USER_PORTS + 1)

/*
 * Cortina software DSA tag, never on the wire.  The egress port is a DMA
 * descriptor field (DESC1_DEST), not a frame byte.  On TX, tag_cortina.c
 * pushes this prefix in front of the DMAC so the port survives the user
 * xmit path and the conduit qdisc; cortina_ni_start_xmit() reads it, folds
 * it into DESC1_DEST and pulls it off before DMA.  RX does not use it.
 * byte[0] = CA_NI_DSA_TAG_MAGIC | (dp->index & PORT_MASK): the magic high
 * nibble lets the conduit tell a tagged frame from one the conduit itself
 * originates, which is dropped instead of mis-stripped.
 * Must stay identical to the copy in net/dsa/tag_cortina.c, which cannot
 * include this header.
 */
#define CA_NI_DSA_TAG_LEN		2
#define CA_NI_DSA_TAG_PORT_OFF		0	/* byte[0] = MAGIC | dp->index  */
#define CA_NI_DSA_TAG_PORT_MASK		0x0f
#define CA_NI_DSA_TAG_MAGIC		0xd0	/* high nibble marks a tagged frame */
#define CA_NI_DSA_TAG_MAGIC_MASK	0xf0

#define CA_NI_GPHY_BANK_STRIDE		0x40000
#define CA_NI_GPHY_MII_BASE		0x29000
#define CA_NI_GPHY_REG_STRIDE		8
#define CA_NI_GPHY_PAGE_STRIDE		0x40
#define CA_NI_GPHY_PAGED_FIRST		0x10
#define CA_NI_GPHY_PAGED_LAST		0x17

/* bank base of the GPHY wired to LAN port p (PHY MDIO address p + 1) */
#define CA_NI_GPHY_BANK(p)		((p) * CA_NI_GPHY_BANK_STRIDE)
/*
 * memory-mapped extension-page register (pages beyond the 0x17 shadow
 * scheme are always accessed by direct offset)
 */
#define CA_NI_GPHY_EXT(page, reg)	((page) * CA_NI_GPHY_PAGE_STRIDE + \
					 ((reg) - CA_NI_GPHY_PAGED_FIRST) * \
					 CA_NI_GPHY_REG_STRIDE)

/*
 * Internal-GPHY fault latch and port reinit.  The quad-GPHY can wedge,
 * typically across a link renegotiation: link reads up and every MAC/QM
 * register looks normal, but the port never delivers an ingress frame.  The
 * latch is polled at 1 Hz on every port and a non-zero read re-initialises
 * the port: INTF_RST pulse -> patch re-enable -> wrapper toggle -> INTF_RST
 * pulse -> 200 ms -> calibration reload -> power-up.  Bank 0 offsets:
 * 0x2e418 / 0x31060 / 0x291a0 / 0x29000.
 */
#define CA_NI_GPHY_FAULT		CA_NI_GPHY_EXT(0xb90, 19)
#define CA_NI_GPHY_PATCH_EN		CA_NI_GPHY_EXT(0xc41, 20)
#define  CA_NI_GPHY_PATCH_EN_BIT	BIT(0)
#define CA_NI_GPHY_HOLD			CA_NI_GPHY_EXT(0xa46, 20)
#define  CA_NI_GPHY_HOLD_BIT		BIT(3)
#define CA_NI_GPHY_BMCR			CA_NI_GPHY_MII_BASE	/* MII reg 0 */
#define  CA_NI_GPHY_BMCR_PDOWN		BIT(11)
/*
 * Analog calibration registers reloaded after a reinit: rc_cal 0xbcd.22/23,
 * r_cal 0xbcf.18-21, amp_cal 0xbca.22.  They are saved at probe (the state
 * U-Boot left working) and restored.  The DSP-SRAM mirror (MII 27/28, addrs
 * 0x824a/0x824c/0x83a9/0x83ab) depends on the uC patch layout and is not
 * rewritten.
 */
#define CA_NI_RX_GPHY_CAL_REGS		7

/*
 * Internal-GPHY firmware (SRAM) patch.
 *
 * The quad-GPHY (PHYID 0x001ccac5) embeds a microcontroller whose DSP
 * scratch SRAM (word range 0x8000..0x83f1) must be loaded with its firmware
 * image (channel estimation, green table, RTCT, AFE) before the PHY passes
 * any frame across its internal GMII to the NI MAC.  Until then the line
 * side still auto-negotiates and reports link, but no frame crosses
 * GPHY <-> MAC on any port.  The image was read out of a running unit
 * (per bank, uC held, via OCP 0xa436 = addr / 0xa438 = data).
 *
 * All registers are internal-PHY OCP addresses, memory-mapped in the GPHY
 * window: OCP register A of bank b lives at b * 0x40000 + A * 4 (e.g. BMCR
 * OCP 0xa400 -> +0x29000, HOLD OCP 0xa468 -> +0x291a0, fault OCP 0xb906 ->
 * +0x2e418).
 */
#define CA_NI_GPHY_OCP(ocp)		((ocp) * 4)	/* OCP addr -> byte offset */

/*
 * SRAM word access: write the word address to SRAM_ADDR, then the 16-bit
 * word to SRAM_DATA (reading SRAM_DATA returns the addressed word).  Same
 * registers as plain-MII page 0xa43 regs 27/28.
 * SRAM_ADDR OCP 0xa436 -> +0x290d8, SRAM_DATA OCP 0xa438 -> +0x290e0.
 */
#define CA_NI_GPHY_SRAM_ADDR		0xa436
#define CA_NI_GPHY_SRAM_DATA	0xa438

/*
 * uC patch-lock handshake.  The uC SRAM is only writable while the uC is
 * held:
 *   GATE  page 0xa42 reg16 = OCP 0xa420 (+0x29080), field [2:0]: value 2
 *         (EXT_INI) = uC not lockable yet -> skip this bank, retry next
 *         link up.
 *   LOCK  page 0xa46 reg21 = OCP 0xa46a (+0x291a8), bit1: set = hold uC,
 *         clear = resume uC (runs the freshly written firmware).
 *   READY page 0xa60 reg16 = OCP 0xa600 (+0x29800), field [7:0]: poll until
 *         == 1 (PCS locked) before writing SRAM, bounded 0xff x 100us.
 * These are byte offsets via CA_NI_GPHY_EXT, used like CA_NI_GPHY_HOLD.
 */
#define CA_NI_GPHY_LOCK_GATE		CA_NI_GPHY_EXT(0xa42, 16)
#define  CA_NI_GPHY_LOCK_GATE_MASK	GENMASK(2, 0)
#define  CA_NI_GPHY_LOCK_GATE_EXT_INI	0x2	/* skip lock when field == this */
#define CA_NI_GPHY_LOCK			CA_NI_GPHY_EXT(0xa46, 21)
#define  CA_NI_GPHY_LOCK_HOLD		BIT(1)	/* 1 = hold uC, 0 = resume */
#define CA_NI_GPHY_LOCK_READY		CA_NI_GPHY_EXT(0xa60, 16)
#define  CA_NI_GPHY_LOCK_READY_MASK	GENMASK(7, 0)
#define  CA_NI_GPHY_LOCK_READY_VAL	0x1
#define  CA_NI_GPHY_LOCK_READY_TRIES	0xff	/* 0xff x 100us bound */

/*
 * GPHY wrapper (idx 15).  EN0 = WRAP_GPHY_CTRL12, EN1 = WRAP_GPHY_CTRL13:
 *   EN0[31:24] cfg_ocp_timeout = 0xFF
 *   EN1[0]     mdio_ocp_sel    = 1
 *   EN1[12]    patch_phy_done  = 1, the last step of integrated-PHY bring-up.
 *              This releases the GPHY -> port-MAC datapath: until it is set
 *              the PHY links and auto-negotiates but no ingress frame
 *              reaches the port MAC.  U-Boot may or may not leave it set,
 *              so it is always forced.
 * Expected values: EN0 = 0xFF000000, EN1 = 0x00001001 at rest.  The per-port
 * EN1[4 + p] bit is pulsed by the fault reinit and reads back clear.
 */
/*
 * WRAP_GPHY_CTRL1 (wrapper + 0x04): cfg_iniphy_pwrup (bits 7:4) is the
 * per-port internal-PHY power-up, one bit per GPHY.  U-Boot only powers the
 * PHY it uses; the others return OCP-timeout data (0xbad0) until all four
 * bits are set.
 */
#define CA_NI_GPHY_WRAP_CTRL1		0x04
#define  CA_NI_GPHY_WRAP_CTRL1_PWRUP	GENMASK(7, 4)
#define CA_NI_GPHY_WRAP_EN0		0x30
#define  CA_NI_GPHY_WRAP_EN0_BITS	GENMASK(31, 24)
#define  CA_NI_GPHY_WRAP_EN0_VAL	0xff000000u
#define CA_NI_GPHY_WRAP_EN1		0x34
#define  CA_NI_GPHY_WRAP_EN1_MDIO_OCP	BIT(0)		/* mdio_ocp_sel  */
#define  CA_NI_GPHY_WRAP_EN1_PATCH_DONE	BIT(12)		/* GPHY->MAC datapath */
#define  CA_NI_GPHY_WRAP_EN1_RST_GPHY	GENMASK(7, 4)	/* rst_gphy_p3_0: 1=run */
#define  CA_NI_GPHY_WRAP_EN1_BITS \
	(CA_NI_GPHY_WRAP_EN1_MDIO_OCP | CA_NI_GPHY_WRAP_EN1_PATCH_DONE)
/*
 * Keep all four GPHY cores released (rst_gphy_p3_0 = 0xF).  A plain EN1
 * write of 0x1001 would clear bits 7:4 and hold every core in reset, leaving
 * PHY2..4 returning 0xbad0.
 */
#define  CA_NI_GPHY_WRAP_EN1_VAL \
	(CA_NI_GPHY_WRAP_EN1_BITS | CA_NI_GPHY_WRAP_EN1_RST_GPHY)	/* 0x10f1 */
/*
 * per-GPHY interface enable in EN1 is a strobe, not a level: the reinit
 * pulses EN1_IF(p) 0->1 between two INTF_RST pulses to connect GPHY p to its
 * MAC; it reads back 0x1001 at rest (bits [7:4] do not stick).
 */
#define  CA_NI_GPHY_WRAP_EN1_IF(p)	BIT(4 + (p))
/* other wrapper regs (analog/datapath, left as U-Boot set them), dumped for debug */
#define CA_NI_GPHY_WRAP_R00		0x00
#define CA_NI_GPHY_WRAP_R04		0x04
#define CA_NI_GPHY_WRAP_R08		0x08
#define CA_NI_GPHY_WRAP_R0C		0x0c

/* Internal GbE PHY identifier (PHYSID1:PHYSID2), low nibble = revision */
#define CA_NI_GPHY_PHY_ID		0x001ccac5
#define CA_NI_GPHY_PHY_ID_MASK		0xfffffff0

/*
 * TX datapath registers.  Two register files are involved:
 *
 *  1. The NI core window (DT idx 0).  The NI_HV block has the CA8277B bit
 *     layouts but sits at +0xaxxx with a per-port stride of 0x90.  The L2TM
 *     block (+0x2xxx) matches CA8277B.
 *
 *  2. The DMA-LSO window (DT idx 4, 4K), which matches the CA8277B register
 *     file; everything there is window-relative (phys - 0xf7001000).
 */

/* --- NI core window (idx 0): NI_HV global block --- */
#define CA_NI_HV_INIT_DONE		0xa004
#define  CA_NI_HV_INIT_DONE_NI		BIT(1)	/* ni_init_done */
#define CA_NI_HV_INTF_RST		0xa008	/* write 0 = deassert all */
/*
 * per-GPHY-port interface reset bit (pulsed set -> 1 ms -> clear, twice,
 * by the internal PHY reinit)
 */
#define  CA_NI_HV_INTF_RST_GPHY(p)	BIT(6 + (p))
#define  CA_NI_HV_INTF_RST_ETH(p)	BIT(p)		/* the MAC interface of NI port p */
#define CA_NI_HV_MAC_AUTOSYNC		0xa010
#define  CA_NI_HV_AUTOSYNC_STS(p)	BIT(p)		/* link/speed/duplex */
#define  CA_NI_HV_AUTOSYNC_FC(p)	BIT(8 + (p))	/* flow control     */
/* 0xa010 = 0x0000000f: status sync for all four ports, no flow-control sync */
#define  CA_NI_HV_AUTOSYNC_STS_ALL	GENMASK(3, 0)
#define  CA_NI_HV_AUTOSYNC_FC_ALL	GENMASK(11, 8)
/*
 * NI_HV_GLB_NITX_MISC_CFG: the NI init sets field [19:14] to 0x21.  This is
 * not the packet-length register (that is 0xa034).
 */
#define CA_NI_HV_CFG_A1B8	0xa1f4
#define  CA_NI_HV_CFG_A1B8_FIELD	GENMASK(19, 14)
#define  CA_NI_HV_CFG_A1B8_VAL		0x21

/*
 * NI_HV_GLB_INTERNAL_PORT_ID_CFG: NI-RX -> L3QM routing.
 * l3qmrx_demux_sel[7:0] selects per port 0 = L3QM, 1 = WAN, 2 = L2FE,
 * 3 = L2TM; all 0 sends every port to the L3QM.  mrr_dsel[9:8] and
 * mrr_ldpid[15:10] are the mirror fields.  Expected value 0x00A87F00.
 */
#define CA_NI_NI_INTERNAL_PORT_ID_CFG	0xa1bc
#define  CA_NI_NI_L3QMRX_DEMUX_SEL_ALL	GENMASK(7, 0)	/* per-port demux, 0 -> L3QM */
#define  CA_NI_NI_MRR_CFG		GENMASK(15, 8)	/* mrr_dsel/mrr_ldpid */
/*
 * NIRX_MISC_CFG.  The NI init read-modify-writes it: clear bit 20, set the
 * l2te_ni_*_rdy_en bits [13:9] and keep everything else.  Do not write a
 * whole word here.
 */
#define CA_NI_NI_NIRX_MISC_CFG	0xa1f8
#define  CA_NI_NI_NIRX_MISC_RDY_EN	0x00003e00u	/* bits[13:9] l2te_ni_*_rdy_en */
#define  CA_NI_NI_NIRX_MISC_CLR		BIT(20)
#define  CA_NI_NI_L2TE_NI_MC_RDY_EN	BIT(9)
/*
 * bit 15 gates an RX-side handshake/drop and must be clear (0x3e80); U-Boot
 * leaves it set (0xbe80)
 */
#define  CA_NI_NI_INTERNAL_BIT15	BIT(15)
#define  CA_NI_NI_L3QMRX_DEMUX_SEL	GENMASK(7, 0)	/* cleared to 0 */

/*
 * NI global static config (NI_HV_GLB_STATIC_CFG, 0xa01c): port_to_cpu[3:0]
 * is the management port whose ingress goes to the CPU path.  The default
 * 0xf disables it; 0 sends LAN port 0 to the CPU.
 */
#define CA_NI_NI_GLB_STATIC_CFG		0xa01c
#define  CA_NI_NI_PORT_TO_CPU		GENMASK(3, 0)
#define  CA_NI_NI_MGMT_PT_TO_FE_ALSO	BIT(4)
/*
 * bits [17:16] of 0xa01c must be set (0x1A07002F) for the L3QM to accept NI
 * ingress; the boot ROM leaves them clear (0x1A04002F).  Set by OR so
 * port_to_cpu[3:0] is kept.
 */
#define  CA_NI_NI_GLB_STATIC_L3QM_EN	GENMASK(17, 16)	/* 0x00030000 */

/*
 * CPU RX configuration.  CPU frames are delivered through the CPU-EPP ring
 * in coherent DDR at 0x0bc48000.  PHY_PORT_STS, EQM_PA_REQ, SCH0.voq_en,
 * EPP.cmd_mode, CPU_EPP_CFG and cputag_cfg are all 0 on a working setup.
 */
#define CA_NI_QM_AXIM2_CONFIG		0x6000
#define  CA_NI_QM_AXIM2_STOCK_VAL	0x000002ffu	/* outstanding 0xff + xbus_len 2 */
/*
 * The L3QM RX demux pair: CFG0 = 0xffff7f7f, CFG1 = 0.  (0xa188/0xa18c are
 * the PC_CFG0/PC_CFG1 policer registers on this chip and are not touched.)
 */
#define CA_NI_NIRX_L3FE_DEMUX_CFG1_REAL	0xa1c4
#define  CA_NI_NIRX_L3FE_DEMUX_CFG1_REAL_VAL	0x00000000u
#define CA_NI_NIRX_L3FE_DEMUX_CFG0_REAL	0xa1c8
#define  CA_NI_NIRX_L3FE_DEMUX_CFG0_REAL_VAL	0xffff7f7fu
/* INT_PORT_ID_CFG2 (0xa184 is a policer source-address register here) */
#define CA_NI_NI_INT_PORT_ID_CFG2	0xa1c0
#define  CA_NI_NI_INT_PORT_ID_CFG2_VAL	0x0024009bu
#define CA_NI_NI_INTERNAL_STOCK_VAL	0x00a87f00u
#define  CA_NI_NI_AUTOSYNC_STOCK_VAL	0x0000000fu	/* 0xa010 (U-Boot leaves 0) */
/*
 * CPU-RX dest -> EQ pools.  profile_sel[3:0] indexes EQ_PROFILE.  The
 * deep-queue CPU egress ports (dest 8 and 9) use profile_sel = 0x0C (12)
 * -> EQ_PROFILE(12) -> EQ12; profile 13 would point at the non-deep CPU
 * pool, which the RMU never admits.  dest port 0 -> 0xf8.
 */
#define  CA_NI_NI_DESTPORT0_STOCK_VAL	0x000000f8u
/* deep-queue dest 8/9 -> profile 12 -> EQ12 */
#define  CA_NI_NI_DESTPORT8_STOCK_VAL	0x0000000cu
/*
 * CPU empty-buffer pools: EQ13 = CPU primary pool (0x634c), EQ14 = CPU
 * secondary (0x6360).  CFG0 bit 31 is the pool enable (bit 0 alone does
 * not admit); CFG1 ({bid_start, total_buf}) is built from the
 * CA_NI_RX_EQ*_BID_START/TOTAL_BUF constants; CFG3 =
 * CA_NI_QM_CFG3_CPU_POOL_VAL, CFG4 = 0.
 * The fixed CFG0 addresses below are no longer used: the CPU pools point at
 * a driver-owned region (cfg0 built at runtime in cortina_ni_rx_eq_init).
 */
#define  CA_NI_QM_EQ13_CFG0		0x80020081u
/*
 * EQ13 (pool 0) / EQ14 (pool 1) are the CPU empty-buffer pools RMU0 admits a
 * CPU-dest (PDPID 0x09) frame into, feeding the CPU-EPP ring.
 * CFG2 layout (QM_QM_CFG2_EQ0): buffer_size[2:0], cpu_eq[3],
 * refill_fbm_eqid[6:4], refill_en[7], refill_ths[15:8],
 * buffer_weight[18:16].
 *
 * cpu_eq (bit 3) selects the buffer source:
 *   cpu_eq=1: CPU push (the driver hands each buffer in via CPU_PUSH_PADDR);
 *   cpu_eq=0: self-populating: the QM maps bid n -> CFG0.phy_addr_start +
 *             n * buffer_size over CFG1.total_buf buffers and recycles a bid
 *             when the CPU advances the EPP read pointer.
 * The CPU pools use cpu_eq=0 with refill_en=0 (no FBM refill); refill_ths is
 * then a don't-care.  The RMU picks eqp0 or eqp1 by size and requires
 * eqp0.buffer_size < eqp1.buffer_size, so pool 0 is small and pool 1 large;
 * real CPU frames (> ~56 bytes with the 0x40 header) land in pool 1.
 * buffer_size[2:0]: 128B = 1, 256 = 2, 512 = 3, 1024 = 4, 2048 = 5, 4096 = 6
 * (vendor aal_l3qm_get_buffer_size_index).
 * 0x0000ff05 = cpu_eq=0, refill_en=0, refill_ths=0xff, 2048 bytes.
 */
/* pool 0: 2048B (idx5) */
#define  CA_NI_QM_EQ13_CFG2		0x0000ff05u
#define  CA_NI_QM_EQ8_CFG2		0x0000ff00u
#define  CA_NI_QM_EQ14_CFG0		0x09240001u
/* pool 1: 2048B (idx5) */
#define  CA_NI_QM_EQ14_CFG2		0x0000ff05u
/*
 * EQ_CFG_LOAD commit latch: bit 31 = load trigger, low 16 bits = the set of
 * EQs whose CFG is latched, by bit position (0x370 = {EQ4, 5, 6, 8, 9}).
 * Bits 13/14 are added for the CPU pools, which do not latch otherwise.
 */
#define  CA_NI_QM_EQ_CFG_LOAD_STOCK	(BIT(31) | 0x0370u | BIT(13) | BIT(14))
/*
 * CPU-EPP descriptor ring base: the DT ddr_cache_buffer at 0x0bc48000
 * (0x20000, coherent).  Mapped cached (MEMREMAP_WB) with the coherent
 * CPU_EPP AXI attribute (0x12008060) and dma-coherent on the NE DT node.
 */
#define CA_NI_RX_RING_PHYS		0x0bc48000u
/*
 * CPU-pool buffer region: a fixed address inside the 0x09000000 no-map DDR
 * reserve (size 0x1628800).  The RMU can only DMA a frame into a buffer
 * inside the NE's DDR window; a dma_alloc_coherent buffer lands outside it
 * and is never admitted.  The 2MB pool occupies [0x09400000, 0x09600000).
 */
#define CA_NI_RX_CPU_POOL_PHYS		0x09400000u

/*
 * FBM EXSTACK (pointer spill) region: the FBM DMAs buffer-pointer spills
 * here (POOL + 0x04 = (pbase >> 12) << 4), separate from the packet buffers.
 * Left at 0, spills land at physical address 0 and corrupt kernel memory.
 * Placed in the same no-map reserve, clear of the CPU pool and the ring.
 */
#define CA_NI_RX_FBM_EXSTACK_PHYS	0x0A000000u	/* in the no-map reserve, page-aligned */
/* spill capacity (>= bufs pushed); *4 = 8KB */
#define CA_NI_RX_FBM_EXSTACK_DEPTH	0x800u
/* pool0 buffer count (mult of 64, <=16384) */
#define CA_NI_RX_FBM_POOL0_COUNT	512u
/* CPU pool = FBM pool 0 (POOL+0x40 residue) */
#define CA_NI_RX_FBM_POOL_ID		0
/*
 * FBM_CPU doorbell (window CA_NI_WIN_FBM_CPU), per-pool stride 0x20:
 * +0x00 = cmd (bit31 GO/BUSY, bit30 op: 1 = push, [2:0] flags), +0x04 = addr
 * high, +0x08 = buffer PA low.  Push: poll outstanding (POOL + 0x2c) < depth,
 * poll +0x00 bit31 clear, write +0x04/+0x08, then the GO cmd.
 */
#define CA_NI_QM_FBM_CPU_DOORBELL(id)	((id) << 5)
#define  CA_NI_QM_FBM_CPU_CMD_GO	BIT(31)
#define  CA_NI_QM_FBM_CPU_CMD_PUSH	BIT(30)
#define CA_NI_QM_FBM_POOL(id)		((id) << 7)	/* POOL window: pool desc base = id*0x80 */
#define CA_NI_QM_FBM_POOL_OUTSTND	0x2c		/* +0x2c outstanding-count (gate) */
#define CA_NI_RX_FBM_POOL_BUFSZ		2048u		/* 2048B buffers */
/* FBM soft reset: glb syscon + 0xa0 bit17, pulse 1 -> 0 */
#define CA_NI_GLB_FBM_RESET		0xa0
#define  CA_NI_GLB_FBM_RESET_BIT	BIT(17)

/*
 * SoC reset manager at GLB + 0xa0 ("cortina,rst-mgr", one reset cell =
 * bit): ni = 0, l2fe = 1, l2tm = 2, l3fe = 3, sdram = 4, TQM (QM/L3QM/RMU) =
 * 5, ptp_timer = 30.  Pulsing a bit (set -> 1 ms -> clear) re-runs that
 * engine's internal init.  Without a TQM pulse the QM/RMU admit engine stays
 * in reset: L3QM init-done never sets and nothing is admitted, while every
 * config register still reads back correctly.  Only TQM is pulsed here;
 * pulsing NI breaks the U-Boot TX path.
 */
#define CA_NI_GLB_SOC_RST		0xa0
#define  CA_NI_GLB_SOC_RST_TQM		BIT(5)
/* L3QM init-done: QM_QM_PHY_PORT_STS bit 30 */
#define CA_NI_QM_L3QM_STS	0x6850
#define  CA_NI_QM_L3QM_INIT_DONE	BIT(30)

/*
 * QM AXI-attribute indirect table (QM_QM_AXI_ATTRIBUTE_*).  Each EQ and
 * CPU-EPP port has an entry with the AXI attributes
 * (qos/cache/snoop/domain/prot) the QM's DMA uses when it writes into that
 * pool's DDR buffer or the CPU-EPP ring.  Entries power up invalid, and the
 * first buffer DMA stalls on an unprogrammed entry.  Protocol: write DATA0,
 * then ACCESS = GO | rbw | ADDR, then poll GO (bit 31) clear with a bounded
 * timeout.  ACCESS is a trigger, not a value to restore.
 */
#define CA_NI_QM_AXI_ATTR_ACCESS	0x66a4
#define CA_NI_QM_AXI_ATTR_DATA1		0x67d0	/* [1:0]=axi_top_bit (40-bit addr high) */
#define CA_NI_QM_AXI_ATTR_DATA0	0x66ac
#define  CA_NI_QM_AXI_ATTR_GO		BIT(31)
#define  CA_NI_QM_AXI_ATTR_RBW		BIT(30)	/* 1 = write the entry */
#define  CA_NI_QM_AXI_ATTR_ADDR		GENMASK(5, 0)
#define CA_NI_QM_AXI_ATTR_POLL_MAX	300	/* bounded poll, never spin forever */
#define CA_NI_QM_AXI_ATTR_EQ_BASE	0	/* entries 0-15 = EQ0-15 */
#define CA_NI_QM_AXI_ATTR_CPU_BASE	48	/* entries 48-55 = CPU-EPP port 0-7 */
#define CA_NI_QM_CPU_PORT_COUNT		8	/* CPU-EPP ports 0-7 (all initialised) */
/*
 * Attribute payloads: a DDR EQ pool = 0x04000010 (bufferable, non-coherent;
 * the driver syncs), the CPU-EPP ring = 0x12008060 (domain 2 / ACE coherent,
 * so the CPU sees descriptors without a sync).
 */
#define CA_NI_QM_AXI_ATTR_DDR_POOL	0x04000010u
#define CA_NI_QM_AXI_ATTR_CPU_EPP	0x12008060u

/* QM_PHY_PORT_STS handshake bit groups (diagnostic only) */
#define  CA_NI_QM_STS_NIRX_PORT_RDY	GENMASK(7, 0)
#define  CA_NI_QM_STS_TE_ES_NI_OK	GENMASK(15, 8)
#define  CA_NI_QM_STS_NIRX_QM_RDY	BIT(26)
#define  CA_NI_QM_STS_AXI_WR_RDY	BIT(28)
#define  CA_NI_QM_STS_AXI_RD_RDY	BIT(29)

/*
 * frame-length limits: 0xa034 holds {min[25:16], max[13:0]}, 0xa038 the
 * RX-side max [13:0].  Both are set to min = 0x3c, max = 0x2ee0.
 */
#define CA_NI_HV_PKT_LEN		0xa034
#define  CA_NI_HV_PKT_LEN_MIN		GENMASK(25, 16)
#define  CA_NI_HV_PKT_LEN_MAX		GENMASK(13, 0)
#define CA_NI_HV_PKT_LEN_RX		0xa038
#define  CA_NI_HV_PKT_LEN_RX_MAX	GENMASK(13, 0)
#define  CA_NI_HV_PKT_LEN_MIN_VAL	0x3c
#define  CA_NI_HV_PKT_LEN_MAX_VAL	0x2ee0

/* globals the NI init always read-modify-writes */
#define CA_NI_HV_CFG_A420	0xa428	/* NI_HV_GLB_VENUS_MISC_CFG */
#define  CA_NI_HV_CFG_A420_FIELD	GENMASK(7, 0)
#define  CA_NI_HV_CFG_A420_VAL		0xc0
#define CA_NI_HV_CFG_AAF0	0xaa60	/* NI_HV_MCE_CTL_REG */
#define  CA_NI_HV_CFG_AAF0_FIELD	GENMASK(9, 0)
#define  CA_NI_HV_CFG_AAF0_VAL		0x17

/*
 * NI RX MIB (per-port receive statistics), indirect access.  Global
 * registers with the port selected in the ACCESS word (not a per-port
 * stride): write ACCESS {go, rbw = 0 (read), op_code, port_id, counter_id},
 * poll go == 0, read DATA0 (cnt_lo, valid for every counter).  Used by the
 * /proc RX debug output to see whether ingress frames reach the MAC.
 *
 * RXMIB ACCESS/DATA1/DATA0 = 0xa1a4/0xa1a8/0xa1ac and TXMIB = 0xa1b0/0xa1b4/
 * 0xa1b8.  (0xa168..0xa17c are PG/PC config registers on this chip; an
 * access there corrupts NI configuration.)  The counter ids (UC/MC/BC =
 * 0/1/2) are derived, not measured: check that they move under known
 * traffic before relying on them.
 */
#define CA_NI_HV_RXMIB_ACCESS	0xa1a4
#define  CA_NI_MIB_ACCESS_GO		BIT(31)		/* self-clears when done */
#define  CA_NI_MIB_ACCESS_RBW		BIT(30)		/* 0 = read              */
#define  CA_NI_MIB_ACCESS_OPCODE	GENMASK(29, 28)
#define  CA_NI_MIB_ACCESS_PORT		GENMASK(7, 5)
#define  CA_NI_MIB_ACCESS_CNTID		GENMASK(4, 0)
#define CA_NI_HV_RXMIB_DATA1		0xa1a8	/* counter [63:32] */
#define CA_NI_HV_RXMIB_DATA0	0xa1ac
#define  CA_NI_MIB_OP_READ_ONLY		0		/* read, do not clear    */
/* RX counter ids (subset the debug output reads) */
#define  CA_NI_MIB_RX_UC_PKT		0
#define  CA_NI_MIB_RX_MC_PKT		1
#define  CA_NI_MIB_RX_BC_PKT		2
/* bounded poll for the indirect MIB access */
#define CA_NI_MIB_POLL_US		1
#define CA_NI_MIB_POLL_TIMEOUT_US	5000

/*
 * NI TX MIB (per-port transmit statistics): the TX twin of the RX MIB, same
 * ACCESS word layout.  It is the only per-physical-port egress packet
 * counter, and with CA_NI_MIB_OP_READ_ONLY it is cumulative (unlike the
 * read-and-clear NI_HV_INTPT_* per-stage counters).
 */
#define CA_NI_HV_TXMIB_ACCESS		0xa1b0
#define CA_NI_HV_TXMIB_DATA1		0xa1b4	/* byte-count hi */
#define CA_NI_HV_TXMIB_DATA0		0xa1b8	/* counter [31:0] */
/*
 * TX counter ids, measured against known traffic: UC/MC/BC at 0/1/2 like the
 * RX MIB, the frame-size bins from 0x0e (64 octets) to 0x13 (1024-1518).
 */
#define  CA_NI_MIB_TX_UC_PKT		0
#define  CA_NI_MIB_TX_MC_PKT		1
#define  CA_NI_MIB_TX_BC_PKT		2
#define  CA_NI_MIB_TX_FRM65_127		0x0f

/*
 * Per-port block, ports 0..3 = the four internal GbE MACs, stride 0x90.
 * The block base 0xa5c0 is NI_HV_PT_PORT_STATIC_CFG, the MAC <-> PHY
 * interface config:
 *   int_cfg   [3:0]   interface mode (0 = GE_GMII; the internal quad-GPHY
 *                     runs GMII)
 *   phy_mode  [4]     0 = this NI port is a MAC connecting to a PHY;
 *                     1 = NI acts as a PHY toward an external MAC
 *   lpbk_mode [13:12] MAC loopback: the MAC loops TX -> RX internally and
 *                     never reaches the GPHY
 * U-Boot may leave any of these wrong, so int_cfg = GE_GMII, phy_mode = MAC
 * and loopback off are forced.
 */
#define CA_NI_PORT_STRIDE		0x90
#define CA_NI_PORT_STATIC_CFG(p)	(0xa5c0 + (p) * CA_NI_PORT_STRIDE)
#define  CA_NI_PORT_STATIC_INT_CFG	GENMASK(3, 0)	/* 0 = GE_GMII */
#define  CA_NI_PORT_STATIC_PHY_MODE	BIT(4)		/* 0 = MAC->PHY */
#define  CA_NI_PORT_STATIC_LPBK_MODE	GENMASK(13, 12)	/* 0 = no MAC loopback */
/*
 * per-port CPU-tag trap (NI_HV_PT_PORT_STATIC_CFG.cpu_tag_*): with rx_en set
 * on a port, NI-RX adds a CPU tag (source port id) and traps the frame to
 * the CPU.  The vendor driver only enables this on NI port 4 (external
 * switch uplink).
 */
#define  CA_NI_PORT_STATIC_CPU_TAG_RX_EN	BIT(16)
#define  CA_NI_PORT_STATIC_CPU_TAG_TX_EN	BIT(17)
#define  CA_NI_PORT_STATIC_CPU_TAG_CFG_SEL	BIT(18)
/* NI_HV_GLB_CPU_TAG_CFG (0xa424 is NI_HV_GLB_DEBUG_3 on this chip); expected 0x00048899 */
#define CA_NI_NI_CPU_TAG_CFG		0xa42c
/*
 * full port static config (0xCB000200 on all ports): int_cfg = 0,
 * phy_mode = 0, lpbk = 0, bit 9, and the upper byte 0xCB = the per-port
 * MAC <-> GPHY datapath enable that U-Boot leaves clear.  Written as a whole
 * word.
 */
#define  CA_NI_PORT_STATIC_CFG_VAL	0xCB000200u
#define CA_NI_PORT_GLB_CFG(p)		(0xa5c4 + (p) * CA_NI_PORT_STRIDE)
#define  CA_NI_PORT_GLB_SPEED_10M	BIT(0)	/* 1 = 10M, 0 = 100M/1G */
#define  CA_NI_PORT_GLB_HALF_DUPLEX	BIT(1)
#define  CA_NI_PORT_GLB_LINK_STATUS	BIT(2)	/* read-only */
#define  CA_NI_PORT_GLB_PWR_DWN_RX	BIT(11)
#define  CA_NI_PORT_GLB_PWR_DWN_TX	BIT(12)
#define CA_NI_PORT_RXMAC_CFG(p)		(0xa5c8 + (p) * CA_NI_PORT_STRIDE)
#define  CA_NI_PORT_RXMAC_RX_EN		BIT(0)
/*
 * Bit 8 is rx_flow_disable (the MAC ignores pause frames while set), bit 12
 * rx_flow_to_tx_en (a received pause throttles the transmitter) and bit 13
 * rx_pfc_disable.  The CPU RX port uses {rx_en, bit12, bit13} with bit 8
 * clear (0x00003001), the other ports 0x3101.  U-Boot leaves 0x1100.
 */
#define  CA_NI_PORT_RXMAC_FLOW_DISABLE	BIT(8)
#define  CA_NI_PORT_RXMAC_STOCK_SET	(BIT(12) | BIT(13))
#define  CA_NI_PORT_RXMAC_STOCK_CLR	CA_NI_PORT_RXMAC_FLOW_DISABLE
#define CA_NI_PORT_TXMAC_CFG(p)		(0xa5d4 + (p) * CA_NI_PORT_STRIDE)
#define  CA_NI_PORT_TXMAC_TX_EN		BIT(0)
#define  CA_NI_PORT_TXMAC_CRC_CALC_EN	BIT(8)	/* HW appends FCS */
#define  CA_NI_PORT_TXMAC_TX_DRAIN	BIT(13)

/*
 * All 7 port blocks (0xa5c0 + p * 0x90) are enabled: ports 0-4 are physical
 * GMACs, ports 5-6 are internal (CPU/QM/L3QM facing) and accept the L2TM
 * egress into the L3QM.  Ports 1-6: RXMAC = 0x3101 (rx_en + 0x3100), TXMAC =
 * 0x04055901 (tx_en), RX_CNTRL = 0x08000600 (byp_en bit 28 = 0).  Port 0
 * uses RXMAC = 0x3001 / TXMAC = 0x04054901.
 */
#define CA_NI_PORT_COUNT		7		/* 0-4 GMAC, 5-6 internal */
#define CA_NI_PORT_RXMAC_EN_VAL		0x00003101u	/* p1-6 RXMAC (rx_en) */
#define CA_NI_PORT_TXMAC_EN_VAL		0x04055901u	/* p1-6 TXMAC (tx_en) */
#define CA_NI_PORT_RX_CNTRL_STOCK_VAL	0x08000600u	/* all-port RX_CNTRL (byp_en=0) */

/*
 * --- NI core window (idx 0): L2TM block (QM buffer manager + egress
 *     scheduler), offsets identical to CA8277B ---
 */
#define CA_NI_L2TM_QM_EQ_CFG		0x2208
#define  CA_NI_L2TM_EQ0_BUFNUM		GENMASK(4, 0)
#define  CA_NI_L2TM_EQ0_PRVT		GENMASK(15, 5)
#define  CA_NI_L2TM_EQ1_BUFNUM		GENMASK(19, 16)
#define  CA_NI_L2TM_EQ1_PRVT		GENMASK(29, 20)
#define  CA_NI_L2TM_EQ0_EN		BIT(30)
#define  CA_NI_L2TM_EQ1_EN		BIT(31)
#define CA_NI_L2TM_QM_GLOB_BUF_CFG	0x2210
#define  CA_NI_L2TM_BUF_NODROP		GENMASK(14, 0)
#define  CA_NI_L2TM_BUF_DROP_EN		BIT(15)
#define  CA_NI_L2TM_BUF_NONCONG		GENMASK(30, 16)
#define  CA_NI_L2TM_BUF_FE_BP_EN	BIT(31)
#define CA_NI_L2TM_QM_PORT_PRVT_PROF0	0x2218	/* profiles 0/1 packed 16/16 */
#define CA_NI_L2TM_ES_CTRL		0x2300
#define  CA_NI_L2TM_ES_TX_EN		BIT(31)
/* per-port ES enable = bit(port); valid ports 0..13 and 15 */
#define  CA_NI_L2TM_ES_PORT_EN_ALL	0xbfff
/*
 * per-port VOQ-enable regs: port 0..13 -> 0x2304 + port * 12; there is no
 * port 14, and port 15 maps to 0x23ac = instance 14, so i = 0..14 covers
 * every port.  The vendor init also sets field [23:16] to 6 on instances
 * 8/10/13; not done here.
 */
#define CA_NI_L2TM_ES_SCH_CFG(i)	(0x2304 + (i) * 12)  /* i = 0..14 */
#define  CA_NI_L2TM_ES_VOQ_EN_ALL	GENMASK(7, 0)	/* voq0..7 enable */
#define CA_NI_L2TM_ES_SCH_INSTANCES	15
/*
 * ES egress-port indices: port 8 = L3QM, the deep-queue -> QM/CPU drain that
 * CPU RX needs.
 */
#define CA_NI_L2TM_ES_PORT_L3QM		8

/*
 * Deep-queue / central-buffer scheduler (L2TE_CB + DQSCH).  If left
 * unprogrammed, a deep_q=1 frame is enqueued into the central buffer but the
 * DQSCH never drains it to the RMU.  The per-VOQ threshold tables are
 * indirect (DATA first, then ACCESS = GO | WR | idx).
 */
#define CA_NI_L2TM_CB_VOQ_THRSH_ACCESS	0x2da0	/* CB_VOQ_THRSH_PROFILE_MEM_ACCESS */
#define CA_NI_L2TM_CB_VOQ_THRSH_DATA1	0x2da4
#define CA_NI_L2TM_CB_VOQ_THRSH_DATA0	0x2da8
#define CA_NI_L2TM_DQSCH_EQ_THRSH	0x2e58	/* DQSCH_EQ_PROFILE_THRSH0 (dft 0x7fff) */
#define CA_NI_L2TM_DQSCH_PORT_THRSH0	0x2e5c	/* DQSCH_PORT_THRSH_PROFILE0..3 */
#define CA_NI_L2TM_DQSCH_VOQ_THRSH_ACCESS	0x2e70	/* DQSCH_VOQ_THRSH_PROFILE_MEM_ACCESS */
#define CA_NI_L2TM_DQSCH_VOQ_THRSH_DATA		0x2e74
#define CA_NI_L2TM_CB_ABR_CTRL		0x2eec	/* 0x88000200 */
#define  CA_NI_L2TM_CB_ABR_CTRL_STOCK	0x88000200u	/* bit31 = ABR deep-q arbiter enable */
#define  CA_NI_L2TM_DEEPQ_PROFILE_PERMISSIVE 0x7fff7fffu /* permissive VOQ profile threshold */
#define  CA_NI_L2TM_DEEPQ_ACC_POLL	10000u		/* busy-poll cap for the indexed GO bit */
/*
 * DQSCH output static config (0x2f00/04/08), the deep-queue dequeue ->
 * TM-port binding.  0x2f0c and up are live counters and must not be
 * written.  Without this a deep_q frame is dequeued to the wrong TM port
 * (not 8 = L3QM).  0x2f04 is believed to be the DQSCH output -> TM-port map.
 */
#define CA_NI_L2TM_DQSCH_OUT_CFG0	0x2f00
#define  CA_NI_L2TM_DQSCH_OUT_CFG0_VAL	0x00000020u
#define CA_NI_L2TM_DQSCH_OUT_PORT_MAP	0x2f04		/* deep-queue output -> TM-port map */
#define  CA_NI_L2TM_DQSCH_OUT_PORT_MAP_VAL 0x33445550u
#define CA_NI_L2TM_DQSCH_OUT_CFG2	0x2f08
#define  CA_NI_L2TM_DQSCH_OUT_CFG2_VAL	0x00000005u
/*
 * Central-buffer scanner enable (L2TE_CB_CTRL).  scan_enable (bit 31) drives
 * the scanner that drains the deep queues from the central buffer to the
 * QM.  Reset value 0x0003ff03 (disabled); without it a deep_q frame sits in
 * the CB forever.  Enable it last, after all thresholds.
 */
#define CA_NI_L2TM_CB_CTRL		0x2d0c
#define  CA_NI_L2TM_CB_CTRL_STOCK	0x8003ff03u	/* scan_enable=1 + reset scan-cycle */
/*
 * CB occupancy counters (indirect ACCESS/DATA): VOQ_BUFCNT climbing means
 * frames are enqueued in the CB.
 */
#define CA_NI_L2TM_CB_VOQ_BUFCNT_ACCESS	0x2dbc
#define CA_NI_L2TM_CB_VOQ_BUFCNT_DATA	0x2dc0
#define CA_NI_L2TM_CB_PORT_FREECNT_ACCESS 0x2db4
#define CA_NI_L2TM_CB_PORT_FREECNT_DATA	0x2db8
/*
 * number of indirect VoQ/port entries the /proc ES-VoQ debug output reads:
 * 16 covers the CPU/LAN VoQs (0..7) and the data T-CONT deep-queue VoQs
 * (8..15); ports 0..15 cover L3QM (port 8) and its neighbours.
 */
#define CA_NI_ES_VOQ_PROBE		16
/*
 * Deep-queue populate: set the CB per-port free-buffer count for ports
 * 0-47.  Without it the CB has no free deep-queue buffers and drops a deep_q
 * frame at the L2TM -> CB enqueue.  DATA fields = cnt0[14:0], cnt0_msb[15],
 * cnt1[30:16], cnt1_msb[31].  0x8E308000 = {cnt0 = 0x8000, cnt1 = 0x8E30}
 * on every port, the size of the CB SRAM.
 */
#define CA_NI_L2TM_CB_PORT_COUNT	48	/* ports 0..47 */
#define  CA_NI_L2TM_CB_FREECNT_VAL	0x8e308000u	/* all ports */

/*
 * L3 TE_CB per-port free-buffer credit (TE_CB_PORT_FREECNT_MEM, L3 layout
 * cnt0[13:0] cnt0_msb[14] cnt1[29:16] cnt1_msb[30]).  The reset default
 * 0x5fff5fff (cnt0 = cnt1 = 8191 + msb) is already a large credit, so the
 * driver does not write it.
 */
#define CA_NI_L3TECB_PORT_FREECNT_ACCESS	0x95a8	/* reset default 0x5fff5fff */
#define CA_NI_L3TECB_PORT_FREECNT_DATA		0x95ac
/*
 * L3 TE_CB deep-queue VoQ occupancy.  idx = (qm_port << 3) + voq; the
 * L3QM/PON destination is qm_port 8, so the upstream T-CONT VoQs are
 * idx 64..71.  DATA = cnt0[15:0] + cnt1[31:16].
 */
#define CA_NI_L3TECB_VOQ_BUFCNT_ACCESS		0x95c8
#define CA_NI_L3TECB_VOQ_BUFCNT_DATA		0x95cc
#define CA_NI_L3TECB_VOQ_BUFCNT_ENTRIES		128	/* 16 qm_ports x 8 voqs */
#define CA_NI_L3TECB_PORT_FREECNT_ENTRIES	16	/* ports 0..15 (of 0..47) */
/*
 * TE-block deep-queue master enables (the L2TM block at 0x2d0c is a
 * separate copy):
 *   TE_FC_CTRL.rx_en (0x9480 bit 0): TE flow-control receive enable; without
 *     it frames cannot enter the deep-queue central buffer.
 *   TE_CB_CTRL.scan_enable (0x950c bit 31): the CB scanner that drains the
 *     CB toward the QM (reset value 0x0803ff03, disabled).
 */
#define CA_NI_TE_FC_CTRL		0x9480
#define  CA_NI_TE_FC_CTRL_RX_EN		BIT(0)
#define CA_NI_TE_CB_CTRL		0x950c
#define  CA_NI_TE_CB_CTRL_SCAN_EN	BIT(31)

/*
 * TE_TE_GLB_CTRL: the deep-queue scheduler (DQSCH) global dequeue-ready
 * enables, glb_deepq_voq_rdy_en and glb_deepq_port_rdy_en, both set to 0xff
 * by read-modify-write.  At their reset value 0 the DQSCH never drains a
 * deep_q=1 frame from the central buffer, and no drop counter fires.
 */
#define CA_NI_TE_TE_GLB_CTRL			0x9004	/* phys 0xf4309004; reset 0x00000000 */
#define  CA_NI_TE_GLB_DEEPQ_VOQ_RDY		GENMASK(15, 8)	/* glb_deepq_voq_rdy_en  */
#define  CA_NI_TE_GLB_DEEPQ_PORT_RDY		GENMASK(23, 16)	/* glb_deepq_port_rdy_en */

/*
 * TE_CB DQSCH thresholds.  At reset the port high threshold is 0, so the
 * port reads as permanently congested and the DQSCH never dequeues:
 *   PORT (0x963c + profile * 4, hth[29:16], 4 profiles): hth = 1000
 *   VOQ  (indirect ACCESS 0x9654 / DATA 0x9658, lth[13:0], 8 profiles):
 *        lth = 768
 */
#define CA_NI_TE_DQSCH_PORT_THRSH0		0x963c	/* +profile*4 (4 profiles) */
#define CA_NI_TE_DQSCH_PORT_THRSH0_COUNT	4
#define CA_NI_TE_DQSCH_VOQ_THRSH_ACCESS		0x9654	/* indirect, profile 0..7 */
#define CA_NI_TE_DQSCH_VOQ_THRSH_DATA		0x9658
#define  CA_NI_TE_DQSCH_PORT_HTH_VAL		(1000u << 16)	/* hth[29:16]=1000 -> 0x03e80000 */
#define  CA_NI_TE_DQSCH_VOQ_LTH_VAL		768u		/* lth[13:0]=768 -> 0x00000300 */
#define CA_NI_L2TM_DEEPQ_VOQ_ENTRIES	8	/* 8 VOQs per deep-queue port */
#define  CA_NI_L2TM_CB_VOQ_THRSH_D1	0x0fffffffu	/* CB VOQ threshold (last entry) */
#define  CA_NI_L2TM_CB_VOQ_THRSH_D0	0xffffffffu
#define  CA_NI_L2TM_DQSCH_VOQ_THRSH_VAL	0x00700070u	/* DQSCH VOQ threshold (lo=hi=0x70) */
/* QM buffer-manager values */
#define CA_NI_QM_EQ1_BUFNUM_VAL		4	/* 4K x 64-byte buffers */
#define CA_NI_QM_PORT_PRVT_BUFF_NUM	29	/* x64B, one max-size frame */
#define CA_NI_QM_NODROP_THRESHOLD	0x21
#define CA_NI_QM_NONCONG_THRESHOLD	0x800	/* l2tm glob_buf (0x2210) */

/* --- DMA-LSO window (idx 4), window-relative (phys - 0xf7001000) --- */
#define CA_DMA_LSO_CTRL			0x000
#define  CA_DMA_LSO_CTRL_VAL		0x2d	/* tx_dma_enable | burst_len=3
						 * (bits 3:2) | lso_padding_en
						 */
#define  CA_DMA_LSO_CTRL_LSPID_KEEP	BIT(4)
#define  CA_DMA_LSO_CTRL_PAD_EN		BIT(5)
/*
 * 0x2d is written first (with readback), then lspid_keep is set and
 * lso_padding_en cleared, for a final value of 0x1d.  Without lspid_keep
 * the source LSPID from the lspid-map table is not kept on the frame.
 */
/* non-ACE companion: low byte := 0 */
#define CA_DMA_LSO_SRAM_TEST_CTRL0	0x0b8	/* expected 0x02020202 */
#define CA_DMA_LSO_SRAM_TEST_CTRL1	0x0bc	/* expected 0x02020202 */
#define CA_DMA_LSO_SRAM_TEST_CTRL2	0x0c0	/* same register as MISC_C0 */
#define CA_DMA_LSO_SRAM_LS_CTRL		0x0c4	/* same register as MISC_C4; expected 0 */
#define CA_DMA_LSO_CCI_MAP		0x0c8	/* expected 0x2 */
#define CA_DMA_LSO_CFG_LENFIX_EN	0x0cc	/* expected 0x1 */
#define CA_DMA_LSO_INTENABLE		0x008	/* expected 0x3f */
#define CA_DMA_LSO_VLAN_TAG_TYPE0	0x00c
#define  CA_DMA_LSO_VLAN_TAG_TYPE0_VAL	0x800088a8	/* en | TPID 0x88a8 */
#define CA_DMA_LSO_AXI_USER_SEL0	0x014
#define  CA_DMA_LSO_AXI_USER_SEL0_VAL	0x00555555	/* vp0..11 rd_pat=1 */
#define CA_DMA_LSO_AXI_USER_PAT0	0x018	/* ..PAT3 at 0x1c/0x20/0x24 */
#define  CA_DMA_LSO_AXI_USER_PAT_VAL	0x02008080	/* ace_cmd=1 (bit25),
							 * cache=2 (17:14),
							 * domain=2 outer-shareable
							 * (7:6) - coherent CCI path
							 */
/*
 * The coherent/ACE fabric path does not work with this kernel: with the ACE
 * pattern the LSO descriptor fetch never completes and the TXQ read pointer
 * stays 0.  Use a non-coherent read pattern (ace=0, cache=2 normal
 * bufferable, domain=0 non-shareable); the TX ring and buffers are
 * maintained by the DMA API (the NE DT node has no dma-coherent property).
 */
#define  CA_DMA_LSO_AXI_USER_PAT_NOCOH	0x00008000
/*
 * pair written unconditionally by the TX DMA init (the CA8277B names at
 * these offsets do not apply)
 */
#define CA_DMA_LSO_MISC_C0		0x0c0
#define  CA_DMA_LSO_MISC_C0_VAL		0x1
#define CA_DMA_LSO_MISC_C4		0x0c4
#define  CA_DMA_LSO_MISC_C4_VAL		0xc0007777
/*
 * DMA-LSO STREAMID table.  In this indirect-table area only 0x0e0 and 0x0e4
 * decode, as STREAMID ACCESS and STREAMID DATA; 0x0d0/d4/d8/dc/e8/ec/f0 do
 * not read.  The generic LSPID_MAP layout (ACCESS 0x0d8, DATA1 0x0dc, DATA0
 * 0x0e0) therefore aliases STREAMID ACCESS, which is why the VP -> LSPID map
 * init must stay disabled on this chip.
 *
 * Entry layout: deep_q[0] pol_id[8:1] cos[11:9] ldpid[17:12] en_flag[31].
 * Expected contents: 8 live entries, the rest disabled:
 *      sid[v] = en_flag 1, ldpid 0x21, cos v, pol_id 8 + v   for v = 0..7
 *      sid[8..255] = 0
 * (words 0x80021010 / 1212 / 1414 / ... / 1E1E).
 * Reading the table back returns garbage (every index reads the same
 * constant), but writes take effect: verify it by behaviour only.
 */
#define CA_DMA_LSO_STREAMID_ACCESS	0x0e0	/* STREAMID ACCESS (go[31], addr[7:0]) */
#define CA_DMA_LSO_STREAMID_DATA	0x0e4	/* STREAMID DATA */
#define CA_DMA_LSO_STREAMID_ENTRIES	256
#define  CA_DMA_LSO_SID_DEEP_Q		BIT(0)
#define  CA_DMA_LSO_SID_POL_ID		GENMASK(8, 1)
#define  CA_DMA_LSO_SID_COS		GENMASK(11, 9)
#define  CA_DMA_LSO_SID_LDPID		GENMASK(17, 12)
#define  CA_DMA_LSO_SID_EN		BIT(31)
#define  CA_DMA_LSO_SID_STOCK_LDPID	0x21
#define  CA_DMA_LSO_SID_STOCK_LIVE	8	/* entries 0..7 live, 8..255 disabled */

#define CA_DMA_LSO_LSPID_MAP_ACCESS	0x0d8
#define  CA_DMA_LSO_LSPID_MAP_IDX	GENMASK(3, 0)
#define CA_DMA_LSO_LSPID_MAP_DATA0	0x0e0	/* DATA0/DATA1 order is swapped vs CA8277B */
#define  CA_DMA_LSO_LSPID_MAP_VALID	BIT(18)
#define  CA_DMA_LSO_LSPID_MAP_LSPID	GENMASK(17, 12)
#define CA_DMA_LSO_LSPID_MAP_DATA1	0x0dc
#define CA_DMA_LSO_LSPID_MAP_ENTRIES	16
#define CA_DMA_LSO_LSPID_CPU0		0x10	/* AAL_LPORT_CPU_0 */

/*
 * DMA-AFT ("DMA after"): the hardware VLAN/PPPoE egress header edit, used
 * for the WAN VLAN tag.
 *
 * vlan_vld does not mean "valid", it selects the mode:
 *     vlan_vld: 0 = VLAN stacking operation mode, 1 = VLAN set mode
 *     vlan_cnt: redefined as TOP_VLAN_CMD[1:0] in stacking mode
 * In set mode vlan_cnt is the number of tags on the egress frame, so an
 * entry with vlan_vld 1, vlan_cnt 0, top_vid 0 is a VLAN strip (what the
 * downstream leg of a tagged WAN needs), not an empty entry.
 *
 * Two indirect tables, both inside the mapped 4K CA_NI_WIN_DMA window at
 * 0x4_f7001000:
 *   MAP   [lspid]  -> fib_id      (which edit applies to frames from lspid)
 *   L2FIB [fib_id] -> the edit    (top/inner VID, TPID sel, PPPoE session)
 */
#define CA_DMA_AFT_MAP_ACCESS		0x0e4	/* idx[5:0] | WRITE | GO */
#define CA_DMA_AFT_MAP_DATA		0x0e8
#define  CA_DMA_AFT_MAP_FIB_ID		GENMASK(5, 0)
#define  CA_DMA_AFT_MAP_EN		BIT(6)	/* DMAAFT_en */
#define  CA_DMA_AFT_MAP_LSPID		GENMASK(10, 7)	/* lspid - CPU0 */
#define  CA_DMA_AFT_MAP_VLD		BIT(11)
#define CA_DMA_AFT_MAP_COUNT		64
#define CA_DMA_AFT_MAP_DYN_FIRST	2	/* 0-1 are vendor-reserved */

/*
 * the 4 TPID slots the edit selects between, 2 per register, low half
 * first.  A tag whose TPID matches no slot makes the hardware silently skip
 * the whole DMA-AFT edit for that flow.
 */
#define CA_DMA_AFT_TPID01		0xf04
#define CA_DMA_AFT_TPID23		0xf08
#define CA_DMA_AFT_TPID_SLOTS		4
#define CA_DMA_AFT_TPID_8021Q		0x8100	/* slot 0 */

#define CA_DMA_AFT_L2FIB_ACCESS		0xf38	/* idx[5:0] | WRITE | GO */
#define CA_DMA_AFT_L2FIB_DATA2		0xf3c
#define CA_DMA_AFT_L2FIB_DATA1		0xf40
#define CA_DMA_AFT_L2FIB_DATA0		0xf44
#define  CA_DMA_AFT_ACCESS_IDX		GENMASK(5, 0)
#define  CA_DMA_AFT_ACCESS_WRITE	BIT(30)
#define  CA_DMA_AFT_ACCESS_GO		BIT(31)	/* poll until it self-clears */
/*
 * L2FIB entry fields.  The names describe what the field does; the vendor
 * name is given alongside each one.
 */
/* DATA2 */
/*
 * vendor: vlan_vld.  Selects the mode, not validity: 0 = VLAN stacking
 * operation mode, 1 = VLAN set mode.
 */
#define  CA_DMA_AFT_D2_VLAN_SET_MODE	BIT(8)
/*
 * vendor: vlan_cnt.  Meaning depends on CA_DMA_AFT_D2_VLAN_SET_MODE:
 *   set mode (1)      -> the number of tags the frame carries after the
 *                        edit, so 1 = push one tag, 0 = strip to untagged
 *   stacking mode (0) -> TOP_VLAN_CMD[1:0], an opcode
 *                        (0 = no-op, 1 = push, 2 = pop, 3 = swap)
 */
#define  CA_DMA_AFT_D2_EGRESS_TAG_CNT	GENMASK(7, 6)
#define  CA_DMA_AFT_D2_TOP_VLAN_CMD	GENMASK(7, 6)	/* same bits, stacking */
#define  CA_DMA_AFT_D2_UNK_18_17	GENMASK(5, 4)	/* not yet identified */
/*
 * vendor: top_tpid_enc.  A 1-based index into the 4-entry TPID slot table:
 * 0 means "no tag / any other value" and n selects slot n - 1, so 0x8100 in
 * slot 0 is encoded as 1.  3 bits, slots 0..3 use 1..4.
 */
#define  CA_DMA_AFT_D2_TOP_TPID_SLOT_P1	GENMASK(3, 1)
#define  CA_DMA_AFT_D2_TOP_TPID_SRC_HI	BIT(0)		/* src[1] - split */
/* DATA1 */
#define  CA_DMA_AFT_D1_INNER_VID_HI	GENMASK(5, 0)	/* inner_vid[11:6] */
#define  CA_DMA_AFT_D1_INNER_UNK_7_6	GENMASK(7, 6)
#define  CA_DMA_AFT_D1_INNER_TPID_SLOT_P1 GENMASK(10, 8)	/* 1-based, as above */
#define  CA_DMA_AFT_D1_UNK_12_11	GENMASK(12, 11)
#define  CA_DMA_AFT_D1_UNK_15_13	GENMASK(15, 13)
#define  CA_DMA_AFT_D1_UNK_17_16	GENMASK(17, 16)
#define  CA_DMA_AFT_D1_UNK_18		BIT(18)
#define  CA_DMA_AFT_D1_TOP_VID		GENMASK(30, 19)	/* the WAN VLAN */
#define  CA_DMA_AFT_D1_TOP_TPID_SRC_LO	BIT(31)		/* src[0] - split */
/* DATA0 */
#define  CA_DMA_AFT_D0_PPPOE_SID	GENMASK(15, 0)
#define  CA_DMA_AFT_D0_PPPOE_CMD	GENMASK(17, 16)
#define  CA_DMA_AFT_D0_INNER_VID_LO	GENMASK(31, 26)	/* inner_vid[5:0] */
/*
 * vendor: top_tpid_sel.  Where the pushed tag's TPID comes from:
 *   0 = no-op, 1 = top_tpid_enc, 2 = inner_tpid_enc, 3 = fib.top_tpid_enc
 * Split field: bit 0 in DATA1[31], bit 1 in DATA2[0].  inner_vid is split
 * too, the other way round (high half in DATA1[5:0], low half in
 * DATA0[31:26]).
 */
#define  CA_DMA_AFT_TPID_SRC_NOOP	0
#define  CA_DMA_AFT_TPID_SRC_TOP_SLOT	1

/*
 * L3FE packet-parser TPID table, which gates flow action generation: the
 * WAN tag's TPID is looked up here and, without a match, no action is
 * generated and the flow stays in software.
 *
 * Two 16-bit TPID values per register, plus a 4-bit enable bitmap: a slot
 * whose bit is clear does not match even when the value is right.  Offsets
 * are NE-window relative (absolute 0x4_f4303278 / _327c / _3280).  0x8100
 * matches with either PP_TPID_CTRL 0x13 or 0x77; the two only differ for
 * 0x9100.
 */
/*
 * Parse-error forward actions.  Each 2-bit field is the action for one
 * parse-error class: 0 = drop, 1 = trap to CPU, 2 = normal forward.  FWD_0
 * resets to 0 (every class dropped).  Programmed values:
 *   FWD_0 0x8000  -> bits [15:14] pkt_l3_total_len_err = 2 (forward), all
 *                    others drop
 *   FWD_1 0x29008 -> pkt_inner_l3_total_len_err = 2, l2tp_hdr_err = 2,
 *                    rdp_ver_err = 2, more_than_2_vlan_tags_flg = 1 (trap)
 * A frame without an L3 header (e.g. ARP) can raise pkt_l3_total_len_err.
 */
#define CA_NI_L3FE_PP_PARSING_ERR_FWD_0	0x3240
#define  CA_NI_L3FE_PP_PARSING_ERR_FWD_0_STOCK	0x00008000u
#define CA_NI_L3FE_PP_PARSING_ERR_FWD_1	0x3244
#define  CA_NI_L3FE_PP_PARSING_ERR_FWD_1_STOCK	0x00029008u
#define CA_NI_L3FE_PP_TPID01		0x3278
#define CA_NI_L3FE_PP_TPID23		0x327c
#define CA_NI_L3FE_PP_TPID_CTRL		0x3280
#define  CA_NI_L3FE_PP_TPID_TOP_MASK	GENMASK(3, 0)	/* per-slot enable */
#define  CA_NI_L3FE_PP_TPID_INNER_MASK	GENMASK(7, 4)

#define CA_DMA_AFT_FIB_COUNT		64
/*
 * The vendor allocator uses [0x10, 0x40) for runtime entries and
 * [0x00, 0x10) for the init-time pool.
 */
#define CA_DMA_AFT_FIB_DYN_FIRST	0x10

/* per-VP block, stride 0xa0, VP 0..11; CPU n transmits on VP n + 2 */
#define CA_DMA_LSO_VP_STRIDE		0xa0
#define CA_DMA_LSO_VP_COUNT		12
#define CA_DMA_LSO_VP_CONTROL(vp)	(0x100 + (vp) * CA_DMA_LSO_VP_STRIDE)
#define  CA_DMA_LSO_VP_TXQ_ALL_EN	GENMASK(7, 0)	/* txq0..7 enable */
#define CA_DMA_LSO_VP_BD_ACCESS(vp)	(0x104 + (vp) * CA_DMA_LSO_VP_STRIDE)
#define  CA_DMA_LSO_BD_ACCESS_TXQ	GENMASK(2, 0)
#define  CA_DMA_LSO_BD_ACCESS_WRITE	BIT(30)		/* rbw: 1 = write */
#define  CA_DMA_LSO_BD_ACCESS_GO	BIT(31)		/* self-clearing */
/*
 * DATA1 low byte = ring base addr[39:32], written as 0 (the value 2 is only
 * for the ACE test path, which is disabled).
 */
#define CA_DMA_LSO_VP_BD_DATA1(vp)	(0x108 + (vp) * CA_DMA_LSO_VP_STRIDE)
#define CA_DMA_LSO_VP_BD_DATA0(vp)	(0x10c + (vp) * CA_DMA_LSO_VP_STRIDE)
#define  CA_DMA_LSO_BD_DATA0_DEPTH	GENMASK(3, 0)	/* log2(ring size) */
#define  CA_DMA_LSO_BD_DATA0_BASE	GENMASK(31, 4)	/* paddr, 16B aligned */
#define CA_DMA_LSO_VP_TXQ_WPTR(vp, q)	(0x118 + (vp) * CA_DMA_LSO_VP_STRIDE \
					       + (q) * 8)
#define CA_DMA_LSO_VP_TXQ_RPTR(vp, q)	(0x11c + (vp) * CA_DMA_LSO_VP_STRIDE \
					       + (q) * 8)
#define  CA_DMA_LSO_PTR_MASK		GENMASK(12, 0)
/*
 * per-VP header-A config: febypass (bit 9) is the FE-bypass enable for
 * direct TX, reset default 0; set (ldpid = 0, febypass = 1) on all 12 VPs.
 */
/*
 * DMA_SEC_DMA_LSO_VP_MISC_INTERRUPT (0xf7001168 + vp * 0xa0), write 1 to
 * clear.  Per-txq triples {txqN_empty, txqN_pktcnt_overrun, txqN_des_err} in
 * the low bits, plus fbm_timeout_err[24] and l2te_te_bp[25] (L2-TE
 * back-pressure).  The txqN_empty bit must be cleared right before every
 * doorbell (between the descriptor stores and the write-pointer update);
 * left latched, the queue wedges.  txqN_empty bits: txq0 0x1, txq1 0x8,
 * txq2 0x40, txq3 0x200, txq4 0x1000, txq5/6/7 0x8000.
 */
/*
 * NI CPUXRAM management FIFO, NI-window relative (phys 0x4_f4300000).  The
 * U-Boot NI driver transmits through this engine.
 */
#define CA_NI_XRAM_ADRCFG_RX		0xaa20
#define CA_NI_XRAM_ADRCFG_TX0		0xaa24
#define CA_NI_XRAM_ADRCFG_BASE		GENMASK(9, 0)
#define CA_NI_XRAM_ADRCFG_TOP		GENMASK(25, 16)
#define CA_NI_XRAM_CFG			0xaa28	/* _dft 0x0000c001 */
#define CA_NI_XRAM_CFG_RX0_DIS		BIT(0)
#define CA_NI_XRAM_CFG_TX0_DIS		BIT(9)
#define CA_NI_XRAM_CFG_PROMISC		GENMASK(15, 14)
#define CA_NI_XRAM_CPU_CFG_TX0		0xaa44	/* SW write pointer = doorbell */
#define CA_NI_XRAM_CPU_STAT_TX0		0xaa48	/* HW read pointer = completion */
#define CA_NI_XRAM_PTR_MASK		GENMASK(9, 0)

/* Header-XT, the per-frame descriptor written into the XRAM slot itself */
#define CA_NI_XRAM_XT_OWN		BIT(31)	/* ownership -> HW */
#define CA_NI_XRAM_XT_HDRA		BIT(30)	/* an 8-byte HEADER_A follows */
#define CA_NI_XRAM_XT_BYTES_VALID	GENMASK(13, 10)
#define CA_NI_XRAM_XT_NEXT_LINK		GENMASK(9, 0)

#define CA_NI_XRAM_HDRA_LEN		8
/*
 * lspid 0x11 is what the CPUXRAM OMCI path stamps (header word 0x00182206:
 * cos = 6, lspid = 0x11, pol_id = 6); the DMA-LSO path uses
 * CA_NI_PON_LSPID (0x10).
 */
#define CA_NI_XRAM_LSPID		0x11

#define CA_DMA_LSO_VP_MISC_INT(vp)	(0x168 + (vp) * CA_DMA_LSO_VP_STRIDE)
#define CA_DMA_LSO_VP_MISC_FBM_TIMEOUT	BIT(24)
#define CA_DMA_LSO_VP_MISC_L2TE_TE_BP	BIT(25)

static inline u32 ca_dma_lso_txq_empty_bit(u8 txq)
{
	static const u32 map[8] = { 0x1, 0x8, 0x40, 0x200, 0x1000,
				    0x8000, 0x8000, 0x8000 };

	return map[txq & 7];
}

#define CA_DMA_LSO_VP_HDRA_CFG(vp)	(0x178 + (vp) * CA_DMA_LSO_VP_STRIDE)
/*
 * DMA_SEC_DMA_LSO_VP_HDRA_CFG, reset value 0x00000200.  With hdra_febypass=1
 * the forwarding engine is skipped and the fabric takes the egress
 * destination from hdra_ldpid, so a VP left at the reset default sends to
 * ldpid 0 (CPU0), not the PON.
 */
#define  CA_DMA_LSO_HDRA_COS		GENMASK(2, 0)
#define  CA_DMA_LSO_HDRA_LDPID		GENMASK(8, 3)
#define  CA_DMA_LSO_HDRA_FEBYPASS	BIT(9)
#define  CA_DMA_LSO_HDRA_NODROP		BIT(10)
#define  CA_DMA_LSO_HDRA_MIRROR		BIT(11)
#define  CA_DMA_LSO_HDRA_DEEPQ		BIT(12)

#define CA_DMA_SS_CTRL			0x900
#define  CA_DMA_SS_CTRL_TX_EN		BIT(31)
#define CA_DMA_AXIM2_CONFIG		0xd80
#define  CA_DMA_AXIM2_CONFIG_BITS	0x7aff	/* out-trans 0xf/0xf,
						 * xbus_len=2, rd/wr
						 * cacheline trans en+size
						 */
/*
 * HW short-frame pad, the actual padding once lso_padding_en is off (the
 * driver pads in software as well)
 */
#define CA_DMA_AFT_CTRL			0xf00
#define  CA_DMA_AFT_PAD_EN		BIT(0)
#define  CA_DMA_AFT_PAD_SIZE		GENMASK(17, 4)
#define  CA_DMA_AFT_PAD_SIZE_VAL	0x40

/*
 * --- AXI-reorder window (idx 10): the TX DMA init writes 0xf to five
 *     stride-0x80 slots ---
 */
#define CA_AXI_REO_SLOT(n)		(0x1000 + (n) * 0x80)
#define CA_AXI_REO_SLOT_COUNT		5
#define CA_AXI_REO_SLOT_VAL		0xf
/*
 * RMU AXI read/write reorder channels.  Without them the RMU dequeue DMA
 * never completes and a CPU frame reaches the QM but is never admitted.
 */
#define CA_NI_AXI_REO_RD0		0x000
#define  CA_NI_AXI_REO_RD0_VAL		0x0000000fu
#define CA_NI_AXI_REO_RD1		0x004
#define  CA_NI_AXI_REO_RD1_VAL		0x8000000cu
#define CA_NI_AXI_REO_RD2		0x008
#define  CA_NI_AXI_REO_RD2_VAL		0x10000000u
#define CA_NI_AXI_REO_RD3		0x00c
#define  CA_NI_AXI_REO_RD3_VAL		0x8000000du
#define CA_NI_AXI_REO_WR0		0x400
#define  CA_NI_AXI_REO_WR0_VAL		0x00000004u
#define CA_NI_AXI_REO_WR1		0x404
#define  CA_NI_AXI_REO_WR1_VAL		0x8000000cu
/*
 * L3FE AXI read-reorder channel (AXI ID 2 -> 8) at window + 0x480 (phys
 * 0xf432d480), alongside the NI channels above.  Without it the L3FE cannot
 * fetch frames from memory.  Each channel has 7 registers: ORIG (+0), NEW
 * (+4), TOP (+8), MASK (+c), NEW0 (+10) and 0xFFFFFFFF at +0x18/+0x24.
 * ORIG_ID axi_id[3:0]; NEW_ID axi_id[3:0] + valid[31]; TOP_ADDR/MASK
 * id[31:0].
 */
#define CA_NI_L3FE_AXI_REO_ORIG_ID	0x480
#define  CA_NI_L3FE_AXI_REO_ORIG_ID_VAL	0x00000002u	/* axi_id=2 */
#define CA_NI_L3FE_AXI_REO_NEW_ID	0x484
#define  CA_NI_L3FE_AXI_REO_NEW_ID_VAL	0x80000008u	/* axi_id=8, valid */
#define CA_NI_L3FE_AXI_REO_TOP_ADDR	0x488
#define  CA_NI_L3FE_AXI_REO_TOP_ADDR_VAL	0x10000000u
#define CA_NI_L3FE_AXI_REO_TOP_ADDR_MASK 0x48c
#define  CA_NI_L3FE_AXI_REO_TOP_ADDR_MASK_VAL 0x10000000u
#define CA_NI_L3FE_AXI_REO_NEW_ID0	0x490
#define  CA_NI_L3FE_AXI_REO_NEW_ID0_VAL	0x80000009u	/* axi_id=9, valid */
#define CA_NI_L3FE_AXI_REO_RD18		0x498		/* +0x18 = 0xFFFFFFFF (per channel) */
#define  CA_NI_L3FE_AXI_REO_RD18_VAL	0xFFFFFFFFu
#define CA_NI_L3FE_AXI_REO_RD24		0x4a4		/* +0x24 = 0xFFFFFFFF (per channel) */
#define  CA_NI_L3FE_AXI_REO_RD24_VAL	0xFFFFFFFFu

/*
 * --- TX descriptor (8 bytes, little-endian, in coherent memory) ---
 *
 * word0 = buffer phys addr [31:0].
 * word1 below is the "direct TX to LAN" encoding (mode=1, direct=0): the
 * destination port and CoS are in the descriptor and the buffer holds a
 * plain Ethernet frame, with no 16-byte header-A prefix and no LSO words
 * (HP[1:0] = 11, "no headers in buffer").  word1 = 0x81400000 | len << 8 |
 * cos << 5 | dest << 1 | chk_sel << 19.
 */
#define CA_NI_TX_DESC_WORDS		2
#define CA_NI_TX_DESC1_DEST		GENMASK(4, 1)	/* LAN port 0..3 */
/*
 * number of values the DEST field can hold, for the range check on the
 * per-frame egress port and the force_dest_ldpid debug parameter
 */
#define CA_NI_TX_DEST_LDPID_COUNT	16
#define CA_NI_TX_DESC1_COS		GENMASK(7, 5)
#define CA_NI_TX_DESC1_LEN		GENMASK(18, 8)	/* frame len, no FCS */
#define CA_NI_TX_DESC1_CHK_SEL		GENMASK(21, 19)
#define  CA_NI_TX_CHK_DISABLE		0
#define  CA_NI_TX_CHK_AUTO		1
#define CA_NI_TX_DESC1_MODE_DIRECT	BIT(22)		/* direct-TX format */
#define CA_NI_TX_DESC1_TO_WAN		BIT(23)		/* 0 = LAN */
#define CA_NI_TX_DESC1_HP0		BIT(24)
#define CA_NI_TX_DESC1_HP1		BIT(31)

#define CA_NI_TX_RING_SIZE		1024	/* descriptors per queue */
#define CA_NI_TX_RING_DEPTH		10	/* log2(1024), HW encoding */
#define CA_NI_TX_NUM_VPS		4	/* rings on VP 2..5 (vendor: one per CPU) */
#define CA_NI_TX_VP_BASE		2
/*
 * Each netdev owns exactly one fixed ring: txq[0] (VP2) = PON upstream (OMCI
 * and WAN data), txq[CA_NI_TX_ETH_RING] (VP3) = eth0.  The DMA-LSO engine
 * fetches the VP rings independently, so spreading one flow across rings
 * reorders it.  txq[2]/txq[3] are initialised but unused.
 */
#define CA_NI_TX_ETH_RING		1
#define CA_NI_TX_MIN_DMA_SIZE		34	/* engine minimum */
#define CA_NI_TX_MAX_FRAME		2047	/* 11-bit len field */
/*
 * Frames above CA_NI_TX_MAX_FRAME go out through the header-mode descriptor
 * pair, whose length field is 16 bits wide and whose HEADER_A pkt_size is 14
 * bits (the MACs take 0x2ee0). 9000 is the usual jumbo MTU.
 */
#define CA_NI_MAX_MTU			9000
#define CA_NI_TX_RESERVE_DESC		2	/* descriptors kept in reserve */

/*
 * --- header-mode TX descriptor (upstream PON control frames / OMCI) ---
 *
 * The generic (mode=0) descriptor encoding of the same engine: HP[1:0] = 01
 * marks "HEADER_A present, no HEADER_CPU".  The frame is sent as a
 * two-descriptor chain: descriptor 0 (SOF) points at a 16-byte header block
 * {LSO para0, LSO para1, HEADER_A lo, HEADER_A hi}, descriptor 1 (EOF) at
 * the frame bytes.  buf_len is 16 bits at [23:8] in this mode (not the
 * 11-bit direct-TX field).
 */
#define CA_NI_TX_DESC1_HDR_LEN		GENMASK(23, 8)	/* buf_len, header mode */
#define CA_NI_TX_DESC1_SOF		BIT(30)		/* sop_eop = 10 */
#define CA_NI_TX_DESC1_EOF		BIT(29)		/* sop_eop = 01 */

/*
 * --- upstream PON control-frame (OMCI) injection ---
 *
 * The OMCI response uses the same DMA-LSO ring as Ethernet, as a 16-byte PON
 * control header (DA 00:13:25:00:00:00, SA 00:13:25:00:00:01, link type
 * 0xff 0xf1, flags 0) plus the 48-byte OMCI PDU.  HEADER_A steers it:
 * ldpid = PON (7) + 8 (the "9th queue" high-priority inject), lspid = CPU0
 * logical port 0x10, cos 7, no_drop, fe_bypass, pol_id = (DA[5] & 0x3f) *
 * 8 + 7 = 7 (T-CONT0 VOQ 7, whose OMCC alloc/GEM binding the GPON driver
 * installs), pol_en.
 *
 * HEADER_A bit layout (both 32-bit words little-endian in the header block):
 *   word0 (bits 0..31):  cos[2:0] ldpid[8:3] lspid[14:9] pkt_size[28:15]
 *                        fe_bypass[29] hdr_type[31:30]
 *   word1 (bits 32..63): mcgid[7:0] drop_code[10:8] rx_pkt_type[12:11]
 *                        no_drop[13] mirror[14] mark[15] pol_en[17:16]
 *                        pol_id[26:18] pol_grp[29:27] deep_q[30] cpu_flg[31]
 */
#define CA_NI_PON_HDRA_LO_COS		GENMASK(2, 0)
#define CA_NI_PON_HDRA_LO_LDPID		GENMASK(8, 3)
#define CA_NI_PON_HDRA_LO_LSPID		GENMASK(14, 9)
#define CA_NI_PON_HDRA_LO_PKT_SIZE	GENMASK(28, 15)
#define CA_NI_PON_HDRA_LO_FE_BYPASS	BIT(29)
#define CA_NI_PON_HDRA_HI_NO_DROP	BIT(13)
#define CA_NI_PON_HDRA_HI_POL_EN	GENMASK(17, 16)
#define CA_NI_PON_HDRA_HI_POL_ID	GENMASK(26, 18)
#define CA_NI_PON_LDPID			(7 + 8)	/* PON port 7 + 9th-queue inject */
#define CA_NI_PON_LSPID			0x10	/* CPU0 logical port */
#define CA_NI_PON_COS			7
#define CA_NI_PON_POL_ID		7	/* OMCC VOQ 7 of T-CONT 0 */

/*
 * --- upstream PON data (WAN) injection: the same 2-descriptor HEADER_A
 *     chain with a plain data HEADER_A.  ldpid = 0x20 + tcont (the CPU_MQ
 *     logical ports 0x20..0x3f, whose ARB map entry routes to QM physical
 *     port 0x08), cos = queue.  The 8Q VoQ map (VoQID = {ldpid[3:0],
 *     cos[2:0]}) puts the frame in VoQ tcont * 8 + cos, and pol_id = that
 *     VoQ picks the upstream port-map slot the GPON driver points at the
 *     OLT-assigned GEM.  fe_bypass = 1, as for OMCI; lspid stays the CPU
 *     logical port. ---
 */
/* PON logical port (AAL_LPORT_PON); DS RX lspid key */
#define CA_NI_LSPID_PON			0x07
/*
 * AAL_LPORT_L3_WAN: with HW L3 forwarding enabled the PON PDC stamps
 * downstream data GEMs with ldpid = L3_WAN so they enter the L3FE.  A
 * terminating or not-yet-offloaded frame misses the T2 hash and the CLS
 * default action punts it to CPU_0, but STG0 has already rewritten
 * HDR_I.lspid from PON to L3_WAN.  Such frames (DHCP offer, ICMP to the
 * router, ...) belong on the GPON WAN netdev.  LAN traffic never carries this
 * lspid, so it identifies the WAN side.
 */
/* AAL_LPORT_L3_WAN; DS RX lspid after L3FE miss-punt */
#define CA_NI_LSPID_L3_WAN		0x18
#define CA_NI_PON_DATA_LDPID(tcont)	(0x20 + (tcont))

/*
 * coherent TX scratch: N slots of {16B header block at 0, frame at 32}.  32
 * slots (tracked by a u32 bitmap) absorb the OLT's MIB-Upload-Next reply
 * burst, so no reply is dropped with -EBUSY (a drop desyncs the stateful
 * upload).
 */
#define CA_NI_PON_TX_SLOTS		32
#define CA_NI_PON_TX_SLOT_SZ		128
#define CA_NI_PON_TX_FRAME_OFF		32
#define CA_NI_PON_HDR_BLK_LEN		16	/* lso0 + lso1 + HEADER_A */
#define CA_NI_PON_HDR_LEN		16	/* DA + SA + 0xfff1 + flags */
#define CA_NI_PON_TX_PDU_MAX		(CA_NI_PON_TX_SLOT_SZ - \
					 CA_NI_PON_TX_FRAME_OFF - \
					 CA_NI_PON_HDR_LEN)

#define CA_NI_TX_POLL_US		1
#define CA_NI_TX_POLL_TIMEOUT_US	10000

/*
 * RX datapath registers, all in the NI core window (DT idx 0).
 *
 * The RX engine is the L3QM CPU-EPP path: ingress frames land in DMA buffers
 * taken from an empty-buffer pool (QM block, +0x6xxx), and for each filled
 * buffer the HW appends one 8-byte descriptor to a per-(cpu-port, voq) FIFO
 * ring in coherent DDR whose pointers live at +0x7xxx.  Bit layouts match
 * the CA8277B register file; several blocks sit at different offsets.
 */

/* --- QM block: CPU-EPP configuration (one-time init) --- */
/* per-cpu-port EPP config; only map_mode used (0 = linear voq map) */
#define CA_NI_QM_CPU_EPP_CFG(p)	(0x6558 + (p) * 4)	/* QM_QM_CPU_EPP0_CFG */
#define  CA_NI_QM_EPP_MAP_MODE		GENMASK(2, 0)
/* descriptor-coalescing timer bases, written as 0 */
#define CA_NI_QM_CPU_EPP_CT_CFG	0x6578	/* QM_QM_CPU_EPP_CT_CFG */
/*
 * EPP FIFO profiles 0..7 (stride 4); the CPU RX path uses profile 4.  (0x66a4
 * is QM_QM_AXI_ATTRIBUTE_ACCESS on this chip, an indirect command register.)
 */
#define CA_NI_QM_CPU_EPP_FIFO_PROF(n)	(0x657c + (n) * 4)
#define  CA_NI_QM_EPP_PROF_HIGH_THS	GENMASK(3, 0)	/* IRQ at >= N entries */
#define  CA_NI_QM_EPP_PROF_TIMER_THS	GENMASK(7, 4)	/* aging force-IRQ    */
#define  CA_NI_QM_EPP_PROF_SIZE		GENMASK(28, 8)	/* in 4-entry units   */
/* per-(port,voq) profile select */
/* QM_QM_CPU_EPP_FIFO0_0_CFG */
#define CA_NI_QM_CPU_EPP_FIFO_CFG(p, q)	(0x65a4 + (p) * 0x20 + (q) * 4)
#define  CA_NI_QM_EPP_PROFILE_SEL	GENMASK(2, 0)
#define CA_NI_RX_PROFILE_ID		4	/* CPU EPP profile */
/*
 * Profile SIZE field: in 64-bit EPP mode the vendor uses 256 4-byte units
 * per VoQ (1024 bytes) and size = 256 / 4 = 0x40.  The driver counts the same
 * ring as 128 x 8-byte descriptors, so the field is derived from bytes:
 *     size = bytes_per_voq / 16
 */
#define CA_NI_RX_EPP_PROF_SIZE		(CA_NI_RX_RING_BYTES / 16)
/* high_ths = (ca_ni_napi_budget / 16) + 1, with ca_ni_napi_budget = 64. */
#define CA_NI_RX_EPP_HIGH_THS		5

/*
 * --- QM block: CPU-EPP interrupt enable (also the mask/ack: the interrupt
 *     is level-triggered by FIFO occupancy vs high_ths, no separate W1C).
 *     One byte per cpu_port: port0 -> bits[7:0] of INT_EN0 (voq0..7). ---
 */
#define CA_NI_QM_EPP64_INT_EN0		0x6110	/* cpu ports 0..3 */
#define CA_NI_QM_EPP64_INT_EN1		0x6114	/* cpu ports 4..7 */
#define  CA_NI_QM_EPP64_INT_PORT0	GENMASK(7, 0)
/*
 * 0x6110 is written as 0xFFFFFFFF by the runtime enable path.  Bits [15:8]
 * are the writeback-completion / write-pointer latch enable, a functional
 * engine enable and not just an interrupt mask: clearing them stops RX
 * descriptor writeback.  MASKED therefore clears only the port-0 interrupt
 * byte this driver owns.  0x6118 = 0x00000100 (per-EQ-pool refill threshold
 * IRQ enable).
 */
#define  CA_NI_QM_EPP64_INT_EN0_STOCK	0xFFFFFFFFu
#define  CA_NI_QM_EPP64_INT_EN0_MASKED	(CA_NI_QM_EPP64_INT_EN0_STOCK & ~CA_NI_QM_EPP64_INT_PORT0)
#define CA_NI_QM_EPP64_INT_EN2		0x6118
#define  CA_NI_QM_EPP64_INT_EN2_STOCK	0x00000100u

/*
 * QM interrupt sources (W1C) and their enables.  They drive network-engine
 * interrupt line 4, which shares GIC SPI 1 with the PON MAC.  Stock writes the
 * enable as 0 or 0x100 only; the reset value enables many sources, and one
 * that latches holds the shared line up with no handler to clear it.
 */
#define CA_NI_QM_INT_SRC		0x611c
#define CA_NI_QM_INT_SRCE		0x6120

/* --- QM block: empty-buffer pool --- */
/* push one 128-byte-aligned buffer phys addr into EQ pool <eqid> */
#define CA_NI_QM_CPU_PUSH_PADDR(p)	(0x636c + (p) * 8)	/* QM_QM_CPU_PUSH_PADDR0 */
#define  CA_NI_QM_PUSH_ADDR		GENMASK(31, 7)	/* pa >> 7 */
#define  CA_NI_QM_PUSH_EQID		GENMASK(3, 0)
/*
 * per-cpu-port push status: bit 31 = the (shallow) CPU-push stage has a free
 * slot.  Must be checked before every CPU_PUSH_PADDR write: a write while
 * the stage is full back-pressures the AXI write and hangs the CPU.  The
 * stage only drains once the EQ config is committed (CA_NI_QM_EQ_CFG_LOAD)
 * and the RMU0 RX master runs; until then it holds ~4 entries, so the poll
 * must be bounded.
 */
#define CA_NI_QM_CPU_PUSH_READY(p)	(0x6368 + (p) * 8)	/* QM_QM_CPU_PUSH_RDY0 */
#define  CA_NI_QM_PUSH_READY		BIT(31)
/* per-push ready timeout (us); bounds the seed loop */
#define CA_NI_RX_PUSH_TIMEOUT_US	1000
/*
 * Per-pool empty-buffer status (QM_QM_EQM_PA_REQn): bit 31 = count valid,
 * [13:0] = buffer count.  Same register as CA_NI_QM_EQM_INACTIVE_BID below.
 */
#define CA_NI_QM_EQM_PA_REQ(eqid)	(0x6328 + (eqid) * 4)	/* QM_QM_EQM_PA_REQ0 */
#define  CA_NI_QM_PA_REQ_READY		BIT(31)		/* req: the pool wants buffers */
#define  CA_NI_QM_PA_INACTIVE_CNT	GENMASK(13, 0)	/* buffer count */
/*
 * Per-packet-engine inactive-bid (free-buffer shortfall) count: bits [13:0]
 * = number of buffers the pool is short (0 when fully populated); bit 31 =
 * valid/err.  Indexed by PE, not eqid.
 */
#define CA_NI_QM_EQM_INACTIVE_BID(pe)	(0x6328 + (pe) * 4)	/* QM_QM_EQM_PA_REQ0 */
#define  CA_NI_QM_INACTIVE_BID_CNT	GENMASK(13, 0)
/*
 * One pool per CPU port: CPU port 0 -> EQ profile 4 = 0x000000F4 (eqp0 =
 * EQ4, eqp1 = 0xF, single pool); CPU ports 1 and 2 use EQ5 and EQ6.
 */
#define CA_NI_RX_EQ_ID			4	/* CPU_0 pool 0 = EQ4 */

/*
 * --- QM block: RMU and empty-buffer-pool configuration.  Same bit layouts
 *     as CA8277B with relocated offsets (per-EQ CFG 0x61b8 -> 0x6248, EQ
 *     profile 0x612c -> 0x6128, dest-port EQ cfg 0x614c -> 0x6168, pkt-buf
 *     cfg 0x6190 -> 0x6228). ---
 */
/* master L3QM RX enable: cleared around pool init, set last */
#define CA_NI_QM_RMU0_CTRL		0x6104
#define  CA_NI_QM_RMU0_RX_EN		BIT(31)
/*
 * QM_QM_ES_CTRL, the L3QM egress scheduler: the drain side of the RX path.
 * RMU0 receives frames into pool buffers and enqueues a per-dest VOQ; the
 * egress scheduler drains that VOQ and writes the 8-byte descriptor into
 * the CPU-EPP FIFO ring.  With it disabled, frames are received (MAC RX MIB
 * climbs) but no descriptor is appended.  Enabled in two steps: {tx_en,
 * ni_en = 0xff, inccfg_for_pkt_cntrs = 2, inccfg_for_err_cntrs = 6} at init,
 * then the per-CPU-port cpu_en bits on open.  (0x7108 is a ring read pointer,
 * EPP64_RDPTR(0, 2), not a second ES_CTRL.)
 */
#define CA_NI_QM_ES_CTRL		0x6108
#define  CA_NI_QM_ES_CPU_EN		GENMASK(7, 0)	/* per-CPU-port egress   */
#define  CA_NI_QM_ES_NI_EN		GENMASK(15, 8)	/* per-NI-port egress    */
#define  CA_NI_QM_ES_INCCFG_PKT		GENMASK(18, 16)	/* value 2               */
#define  CA_NI_QM_ES_INCCFG_ERR		GENMASK(22, 20)	/* value 6               */
#define  CA_NI_QM_ES_RSVD25		BIT(25)		/* cleared               */
#define  CA_NI_QM_ES_TX_EN		BIT(31)		/* master egress enable  */
#define  CA_NI_QM_ES_INCCFG_PKT_VAL	2
#define  CA_NI_QM_ES_INCCFG_ERR_VAL	6
/*
 * Expected ES_CTRL: tx_en + cpu_en = 0xff (all CPU ports) + ni_en = 0xff +
 * inccfg {pkt = 2, err = 6} + bit 26, bit 25 clear = 0x8462FFFF.
 */
#define  CA_NI_QM_ES_CPU_EN_ALL		0xff	/* ES_CTRL=0x8462ffff (cpu_en=0xff) */

/*
 * L3-TM egress-scheduler port enables, the second egress enable layer: the
 * CPU byte lives in 0x6108, the NI ports in 0x610c (all enabled,
 * 0xffffffff).
 */
#define CA_NI_QM_L3TM_NI_PORT_ENA	0x610c
#define  CA_NI_QM_L3TM_NI_PORT_ENA_ALL	0xffffffffu

/*
 * QM block-init-done latch: the L3QM delivery init must wait for it before
 * touching the EQ/EPP config.
 */
#define CA_NI_QM_PHY_PORT_STS	0x6850	/* QM_QM_PHY_PORT_STS */
#define  CA_NI_QM_INIT_DONE		BIT(30)	/* qm_init_done */
#define  CA_NI_QM_INIT_DONE_TIMEOUT_US	100000	/* bounded wait, non-fatal */

/*
 * QM_QM_ES_CTRL2: ni_qm_hol_pkt_ctrl (bit 1) gates the NI -> QM
 * head-of-line handoff and is set by read-modify-write.  Left at 0, a
 * deep_q frame drained to ES port 8 never reaches RMU0.
 */
#define CA_NI_QM_ES_CTRL2	0x6968	/* QM_QM_ES_CTRL2 */
#define  CA_NI_QM_ES_CTRL2_NI_QM_HOL	BIT(1)	/* ni_qm_hol_pkt_ctrl */
#define  CA_NI_QM_ES_CTRL2_STOCK_VAL	0x00000300u
/*
 * Alias of ES_CTRL2.  (0x6a30 does not decode on this chip; reading it is a
 * synchronous external abort.)
 */
#define CA_NI_QM_ES_CTRL2_REAL	0x6968

/*
 * CPU-EPP FIFO command mode: 0 = 32-bit descriptor, 1 = 64-bit.  The NAPI
 * poll parses 64-bit descriptors, so this must be 1.
 */
#define CA_NI_QM_EPP	0x68f4	/* QM_QM_EPP */
#define  CA_NI_QM_EPP_CMD_MODE_64	BIT(2)	/* cmd_mode: 1 = 64-bit */
/*
 * Egress-scheduler egress-enable pair: TX-path and CPU-path egress enable,
 * each an 8-bit port mask (0xff00), expected 0x0000FF00 in both.  Arming the
 * EPP engine clears the CPU-path enable, so both are re-asserted after the
 * EPP GO.  Without the CPU-path enable the ES never services CPU_0 egress and
 * the CPU-EPP writeback starves.
 */
#define CA_NI_QM_EPP_TX_EGR_EN	0x68d8	/* QM_QM_BURST_BUF_DEBUG_STATUS */
#define CA_NI_QM_EPP_CPU_EGR_EN	0x68c8	/* QM_QM_VOQ_OVER_THS_STATUS4 */
#define  CA_NI_QM_EPP_EGR_EN_ALL	0x0000FF00u
/*
 * EQ config commit: after programming the per-EQ CFG0-4, unlock HDM write
 * protection, pulse EQ_CFG_LOAD and relock.  This latches the bid range into
 * the empty-buffer manager; without it pushed buffers never leave the
 * shallow push stage.
 */
/*
 * L3QM RX-side status and moved-frame counters (read-only).  The RX counter
 * climbs as the RMU moves ingress frames into pool buffers; if it stays 0,
 * frames never reached the RMU.
 */
#define CA_NI_QM_RX_STATUS0	0x659c	/* QM_QM_CPU_EPP_STATUS0 */
#define CA_NI_QM_RX_STATUS1	0x65a0	/* QM_QM_CPU_EPP_STATUS1 */
#define CA_NI_QM_RX_CNTR	0x67d8	/* QM_QM_RMU0_RX_PKT_CNTR */
/*
 * the last admitted frame's header: dest ldpid [7:0] + deep_q flag (bit 30)
 * + valid (bit 31).  A normal CPU frame reads 0x80000010 (dest 0x10 = CPU0,
 * deep_q clear -> CPU-EPP64 ring).
 */
#define CA_NI_QM_RMU0_RX_HDR_INFO0	0x67dc	/* QM_QM_RMU0_RX_PACKET_HEADER_INFO1 */
#define  CA_NI_QM_RMU0_RX_DEST_LDPID	GENMASK(7, 0)
#define  CA_NI_QM_RMU0_RX_DEEP_Q	BIT(30)
#define  CA_NI_QM_RMU0_RX_VALID		BIT(31)
#define CA_NI_QM_TX_CNTR	0x67e4	/* QM_QM_TX_PKT_CNTR */
/*
 * RMU front-end drop counters.  FE_DROP increments when a frame reaches RMU0
 * but its size-selected pool has no free buffer: RX counter 0 with this
 * climbing means "reached the RMU, pool empty"; both 0 means the frame never
 * reached the RMU.
 */
#define CA_NI_QM_RMU_FE_DROP	0x681c	/* QM_QM_RMU0_FE_DROP_PKT_CNTR */
#define CA_NI_QM_RMU_NO_BUF_DROP	0x6818	/* QM_QM_RMU0_NO_BUF_DROP_PKT_CNTR */
/*
 * Further QM/RMU0 debug registers.  The BM -> L3QM accept has no enable bit:
 * the credit is an available empty buffer in the EQ pool.
 */
#define CA_NI_QM_RMU0_RX_PKT_CNTR_RE	0x66b0	/* QM_QM_BID_PADDR_LKUP_ACCESS */
#define CA_NI_QM_RMU0_NO_BUF_DROP_RE	0x6818	/* RMU0_NO_BUF_DROP (pool empty) */
#define CA_NI_QM_EQ13_BUF_USG	0x6834	/* QM_QM_VOQ_STATUS1 */
#define CA_NI_QM_CPU_PUSH_RDY0_RE	0x6368	/* CPU_PUSH_RDY0 */
#define CA_NI_QM_EQ_STACK_UNFILL	0x63c0	/* EQ_STACK_UNFILL */
/*
 * Per-stage cumulative packet counters, NI core window.  Stage 1 (L2TM
 * egress) is CA_NI_L2TM_BM_TX_PCNT (0x2140).
 */
#define CA_NI_NI_L3QM_RX_PKT_CNT	0xa9fc	/* stage2 QM ingress: NI_HV RX_PKT_CNT_L3QM */
/* stage2 QM ingress: NI_HV TX_PKT_CNT_L3QM (NI->QM) */
#define CA_NI_NI_L3QM_TX_PKT_CNT	0xaa10
/*
 * NI_HV L3QM interface error counters (hi16/lo16 = two sub-reasons each).
 * Note: 0xa9fc, 0xaa10, 0xa9f4 and 0xa9f8 are not named registers on this
 * chip and their readings are not meaningful; the internal-port counters
 * are the CA_NI_NI_INTPT_* set below.
 */
#define CA_NI_NI_L3QM_RX_MISS_SOP_EOP	0xa9f4	/* RX_MISSING_SOP_EOP (hi16=SOP, lo16=EOP) */
#define CA_NI_NI_L3QM_RX_SHORT_ERR	0xa9f8	/* RX_SHORT_ERR (hi16=short, lo16=err) */
#define CA_NI_NI_INTPT_RX_PKT_CNT	0xa92c	/* NI_HV_INTPT_RX_PKT_CNT */
#define CA_NI_NI_INTPT_RX_SHORT_ERR	0xa928	/* NI_HV_INTPT_RX_SHORT_ERR_CNT */
#define CA_NI_NI_INTPT_RX_MISS_SOP_EOP	0xa924	/* NI_HV_INTPT_RX_MISSING_SOP_EOP_CNT */
#define CA_NI_NI_INTPT_TX_PKT_CNT	0xa940	/* NI_HV_INTPT_TX_PKT_CNT */
#define CA_NI_NI_XRAM_DMA_PKT_CNT	0xaa4c	/* NI_HV_XRAM_DMA_PKT_CNT */
/*
 * The NI internal-port RX packet count (not an L3FE counter).  0xa9bc does
 * not decode on this chip.
 */
#define CA_NI_NI_INTPT_RX_PKT_CNT	0xa92c	/* NI_HV_INTPT_RX_PKT_CNT */
/*
 * These two are not packet counters despite their names:
 *   0xaa3c is NI_HV_XRAM_CPUXRAM_BYT_CNT_0, a byte count on the CPUXRAM path;
 *   0xaa7c is NI_HV_MCE_LAST_IN_HDR2, a header latch.
 */
#define CA_NI_NI_MCE_RX_PKT_CNT		0xaa3c	/* really XRAM_CPUXRAM_BYT_CNT_0 (bytes) */
#define CA_NI_NI_DMA_RX_PKT_CNT		0xaa7c	/* really MCE_LAST_IN_HDR2 (a latch) */
/* stage 3 = RMU0 RX / TX counters; stage 4 = the EPP write pointer (0x7000) */

#define CA_NI_QM_HDM_WRITE_PROT	0x66d4	/* QM_QM_HDM_WRITE_PROTECTION */
#define  CA_NI_QM_HDM_UNLOCK		0x05102013u
#define CA_NI_QM_EQ_CFG_LOAD	0x63a8	/* QM_QM_EQ_CFG_LOAD */
#define  CA_NI_QM_EQ_CFG_LOAD_ALL	0xffffu
/* EQ profile n (0..7): pool0/pool1 EQ ids + fill rule (0 = SW push) */
#define CA_NI_QM_EQ_PROFILE(n)		(0x6128 + (n) * 4)
#define  CA_NI_QM_EQ_PROF_EQP0		GENMASK(3, 0)
#define  CA_NI_QM_EQ_PROF_EQP1		GENMASK(7, 4)
#define  CA_NI_QM_EQ_PROF_RULE		BIT(31)
/*
 * CPU EQ profile {eqp0 = pool 0, eqp1 = pool 1, rule = 0}.  The CPU dest (9)
 * selects EQ profile 13 (DESTPORT map = 0x0D).  The EPP-FIFO profile is a
 * separate 3-bit space (CA_NI_RX_PROFILE_ID = 4).
 */
#define CA_NI_RX_EQ_PROFILE		0x0d	/* CPU dest-port profile 13 */
/*
 * The RMU picks the pool by size: a frame that fits eqp0 goes there, else
 * to eqp1.  eqp0.size < eqp1.size is required; with equal sizes RMU0
 * silently refuses to admit.
 */
#define CA_NI_RX_EQ_PROFILE_VAL \
	(FIELD_PREP(CA_NI_QM_EQ_PROF_EQP0, CA_NI_RX_EQ_ID) | \
	 FIELD_PREP(CA_NI_QM_EQ_PROF_EQP1, CA_NI_RX_EQ_ID2))
/*
 * DEST_PORT_EQ_CFG.profile_sel is only 3 bits [2:0], so the DEST_PORT value
 * 0x0d selects profile 5, not 13.  EQ_PROFILE(5) is programmed as well.
 */
/* = 5, the reachable profile */
#define CA_NI_RX_EQ_PROFILE_SEL		(CA_NI_RX_EQ_PROFILE & 0x7)
/*
 * The CPU physical destination (AAL_PPORT_CPU, 0x09) sits at DEST_PORT
 * index 15 of the QM DEST_PORT_EQ_CFG table; it is pointed at the CPU EQ
 * profile.
 */
#define CA_NI_RX_CPU_DEST_PORT		15
/*
 * deep-queue frames use DEST_PORT_EQ_CFG indices 8..15 (8 + deep-queue
 * index); all are pointed at the CPU EQ profile.
 */
#define CA_NI_RX_DEEPQ_DEST_PORT_LO	8
#define CA_NI_RX_DEEPQ_DEST_PORT_HI	15

/*
 * EQ0, the SRAM-backed deep-queue pool:
 *     CFG0_EQ0 = 0x80000081  eq_en=1, phy_addr_start = 0x80000080
 *     CFG1_EQ0 = 0x00f30e00  bid_start = 3584, total_buffer_num = 243
 *     CFG2_EQ0 = 0x0000ff03  buffer_size idx 3 = 512 B, cpu_eq=0,
 *                            refill_en=0, ths=0xff
 *     CFG3_EQ0 = 0x00000010  CFG4_EQ0 = 0
 * 0x80000080 is on-chip SRAM, so this pool cannot collide with any DDR
 * allocation.  Dest ports 8/9 (the deep-queue CPU egress) select EQ
 * profile 0, whose pool is EQ0; without it a deep-queued CPU-bound frame
 * (such as the GPON downstream punt) has no buffer and is not admitted.
 * eqp1 is left empty (0xF): the vendor's second pool is a large DDR region at
 * 0x09000000 that would overlap the CPU pools and the CPU-EPP ring.
 */
#define CA_NI_RX_EQ0_ID			0
#define  CA_NI_QM_EQ0_CFG0		0x80000081u	/* eq_en + SRAM base 0x80000080 */
#define  CA_NI_QM_EQ0_CFG1		0x00f30e00u	/* bid_start 3584, total_buf 243 */
#define  CA_NI_QM_EQ0_CFG2		0x0000ff03u	/* 512 B, cpu_eq=0 (HW self-populating) */
#define  CA_NI_QM_EQ0_CFG3		0x00000010u
#define  CA_NI_RX_EQ0_PROFILE		0		/* EQ_PROFILE(0), selected by destp8/9 */
/*
 * DEST_PORT_EQ_CFG has 32 entries, so the last valid index is 31.
 * CA_NI_QM_DEST_PORT_MAX (0x2f) is the dest-port number space, not the
 * table length: indices 38..40 would land on EQ_PROFILE_GLOBAL(0..2).  Use
 * CA_NI_QM_DEST_PORT_LAST to bound any loop over the table.
 */
/*
 * Debug-only bound check for an indexed register write.
 *
 * Nothing stops `writel(v, base + REG(i))` from going past the end of a
 * register array, and the hardware accepts a write to the wrong address
 * silently.  With CONFIG_CORTINA_NI_REG_GUARD, CA_NI_IDX() warns once,
 * naming the array and index, when the index is past the array's declared
 * entry count; otherwise it is the plain macro expansion.
 */
#ifdef CONFIG_CORTINA_NI_REG_GUARD
#define CA_NI_IDX(_arr, _i, _cnt)					\
	({								\
		unsigned int __ni_i = (unsigned int)(_i);		\
		unsigned int __ni_c = (unsigned int)(_cnt);		\
		WARN_ONCE(__ni_i >= __ni_c,				\
			  "cortina-ni: " #_arr "[%u] is past its %u "	\
			  "declared entries -- this write lands on "	\
			  "another register\n", __ni_i, __ni_c);	\
		_arr(__ni_i);						\
	})
#else
#define CA_NI_IDX(_arr, _i, _cnt)	({ (void)(_cnt); _arr(_i); })
#endif

/*
 * Declared entry counts, so a loop is bounded by the table size rather than
 * by a port-number maximum or another nearby constant.
 */
#define CA_NI_QM_EQ_PROFILE_COUNT	16
#define CA_NI_NI_RXMUX_FC_THR_COUNT	8

#define CA_NI_QM_DEST_PORT_MAX		0x2f
#define CA_NI_QM_DEST_PORT_ENTRIES	32
#define CA_NI_QM_DEST_PORT_LAST		(CA_NI_QM_DEST_PORT_ENTRIES - 1)
/*
 * per-dest-port EQ-profile select (QM_QM_DEST_PORT0_EQ_CFG), stride 4,
 * 32 entries
 */
#define CA_NI_QM_DEST_PORT_EQ_CFG(p)	(0x6148 + (p) * 4)	/* QM_QM_DEST_PORT0_EQ_CFG */
#define  CA_NI_QM_DEST_PORT_PROF_SEL	GENMASK(3, 0)
/*
 * Global default EQ profile (3 entries at 0x6200/0x6204/0x6208): the profile
 * a destination without its own DEST_PORT_EQ_CFG entry uses.  0x0D (profile
 * 13) in each steers a CPU-dest (physical 0x09) frame to the CPU pools.
 */
#define CA_NI_QM_EQ_PROFILE_GLOBAL(n)	(0x6200 + (n) * 4)
#define CA_NI_QM_EQ_PROFILE_GLOBAL_COUNT 3
/* CPU EQ-profile selector value = the CPU pool's EQ_PROFILE index (13) */
#define CA_NI_RX_CPU_PROFILE_VAL	CA_NI_RX_EQ_PROFILE	/* 0x0D -> EQ_PROFILE(13) */
/*
 * dest-port head/tail room in 16-byte units; head_room_first = 4 is what
 * puts HEADER_A at buffer + 0x40 (it is programmed, not fixed).
 * QM_QM_DEST_PORT0_PKT_BUF_CFG, stride 4, 8 entries indexed by CPU port
 * number (not the 0..31 dest-port number); every entry is programmed.
 */
/* QM_QM_DEST_PORT0_PKT_BUF_CFG */
#define CA_NI_QM_DEST_PORT_PKT_BUF_CFG(p)	(0x61c8 + (p) * 4)
#define  CA_NI_QM_PKT_BUF_PORTS		8
#define  CA_NI_QM_PKT_BUF_HEAD_FIRST	GENMASK(5, 0)
#define  CA_NI_QM_PKT_BUF_TAIL_FIRST	GENMASK(13, 8)
#define  CA_NI_QM_PKT_BUF_HEAD_REST	GENMASK(21, 16)
#define  CA_NI_QM_PKT_BUF_TAIL_REST	GENMASK(29, 24)
/*
 * pkt_buf value 0x18041804: head_first = head_rest = 4 (16-byte units,
 * HEADER_A at +0x40) and tail_first = tail_rest = 0x18 (384 bytes of tail
 * room).
 */
#define  CA_NI_QM_PKT_BUF_HEAD_UNITS	(CA_NI_RX_BUF_HEADROOM / 16)	/* 4 */
#define  CA_NI_QM_PKT_BUF_TAIL_UNITS	0x18				/* 24 */
/* per-EQ config CFG0..4 (QM_QM_CFG0_EQ0), 16 EQs, stride 0x14 */
#define CA_NI_QM_CFG0_EQ(e)	(0x61e8 + (e) * 0x14)	/* QM_QM_CFG0_EQ0 */
#define  CA_NI_QM_CFG0_EQ_EN		BIT(0)
#define  CA_NI_QM_CFG0_PHY_ADDR_START	GENMASK(31, 7)	/* 0 for SW-push pools */
#define CA_NI_QM_CFG1_EQ(e)	(0x61ec + (e) * 0x14)	/* QM_QM_CFG1_EQ0 */
#define  CA_NI_QM_CFG1_BID_START	GENMASK(13, 0)
#define  CA_NI_QM_CFG1_TOTAL_BUF_NUM	GENMASK(29, 16)
#define CA_NI_QM_CFG2_EQ(e)	(0x61f0 + (e) * 0x14)	/* QM_QM_CFG2_EQ0 */
#define  CA_NI_QM_CFG2_BUF_SIZE	GENMASK(2, 0)	/* size index, see below */
#define  CA_NI_QM_CFG2_CPU_EQ		BIT(3)
#define  CA_NI_QM_CFG2_REFILL_EQID	GENMASK(6, 4)
#define  CA_NI_QM_CFG2_REFILL_EN	BIT(7)
#define  CA_NI_QM_CFG2_REFILL_THS	GENMASK(15, 8)
/*
 * CFG3 = per-EQ AXI cache/snoop attributes (cache_eos/domain/snoop/cache/
 * qos); 0x00000010 for the CPU pools.
 */
#define CA_NI_QM_CFG3_EQ(e)	(0x61f4 + (e) * 0x14)	/* QM_QM_CFG3_EQ0 */
#define  CA_NI_QM_CFG3_CPU_POOL_VAL	0x00000010u	/* CPU pool CFG3 */
#define CA_NI_QM_CFG4_EQ(e)	(0x61f8 + (e) * 0x14)	/* QM_QM_CFG4_EQ0 */
#define CA_NI_QM_EQ_COUNT		16
/*
 * buffer-size index (vendor aal_l3qm_get_buffer_size_index):
 * 0x80=1 0x100=2 0x200=3 0x400=4 0x800=5 0x1000=6
 */
/*
 * Two CPU empty-buffer pools: pool 0 (primary, profile eqp0) and pool 1
 * (secondary, eqp1, overflow reserve).  Both are cpu_eq=0 self-populating
 * (refill_en=0): the QM maps bid n -> CFG0.phy_addr_start + n * buffer_size
 * over CFG1.total_buf buffers and recycles a bid when the CPU-EPP read
 * pointer moves past its frame; no software push and no FBM.  The bid
 * windows must not overlap.
 */
#define CA_NI_RX_EQ_ID2			5	/* pool1 = EQ5, immediately after EQ4 */
#define CA_NI_RX_EQ_BID_START		0	/* EQ4 bid_start = 0 */
#define CA_NI_RX_EQ_TOTAL_BUF		512	/* pool0 buffer count (CFG1) */
#define CA_NI_RX_EQ2_TOTAL_BUF		512	/* pool1 buffer count (CFG1) */
#define CA_NI_RX_EQ2_BID_START		CA_NI_RX_EQ_TOTAL_BUF	/* contiguous after EQ4 */
/*
 * CPU-pool DRAM layout: one reserved region holds both CPU pools back to
 * back, pool 0 at offset 0 and pool 1 right after.  Each pool's
 * CFG0.phy_addr_start points at its sub-region, walked at a 2048-byte
 * (CFG2 idx 5) stride.  BUFSZ must match the CFG2 buffer_size index or the
 * NAPI offset -> buffer calculation breaks.
 */
#define CA_NI_RX_CPU_POOL0_BUFSZ	2048u	/* pool0 buffer_size idx5 (2048B) */
#define CA_NI_RX_CPU_POOL1_BUFSZ	2048u	/* pool1 buffer_size idx5 (2048B) */
#define CA_NI_RX_CPU_POOL0_BYTES \
	(CA_NI_RX_CPU_POOL0_BUFSZ * CA_NI_RX_EQ_TOTAL_BUF)	/* 512*2048 = 1MB */
#define CA_NI_RX_CPU_POOL1_BYTES \
	(CA_NI_RX_CPU_POOL1_BUFSZ * CA_NI_RX_EQ2_TOTAL_BUF)	/* 512*2048 = 1MB */
#define CA_NI_RX_CPU_DRAM_SIZE \
	(CA_NI_RX_CPU_POOL0_BYTES + CA_NI_RX_CPU_POOL1_BYTES)	/* 2MB */
/*
 * EQ8, the RMU empty-buffer allocation pool: a CPU-push pool reusing the
 * EQ13 CFG0 (enable) with CFG2 = 0xff00 (refill_en=0, no FBM), bid window
 * after pool 1.
 */
#define CA_NI_RX_EQ8_ID			8	/* RMU empty-buffer alloc pool */
#define CA_NI_RX_EQ8_TOTAL_BUF		383	/* seeded free buffers */
#define CA_NI_RX_EQ8_BID_START \
	(CA_NI_RX_EQ2_BID_START + CA_NI_RX_EQ2_TOTAL_BUF)	/* 0x960, after EQ14 */
/*
 * EQ12, the deep-queue CPU pool: a deep_q=1 frame egresses ES port 8/9 ->
 * EQ_PROFILE(12) = 0xEC {eqp0 = 12, eqp1 = 14} -> EQ12 buffer -> RMU -> CPU.
 * cfg1 = 0x010004b0 (256 buffers, bid 0x4b0, just below 0x5b0).
 */
#define CA_NI_RX_EQ12_ID		12
#define CA_NI_RX_EQ12_TOTAL_BUF		256	/* cfg1 total_buf = 0x100 */
#define CA_NI_RX_EQ12_BID_START		0x4b0	/* 0x4b0 + 256 = 0x5b0 */
/*
 * EQ12 is a cpu_eq=0 DDR-backed pool: the central-buffer deep-queue enqueue
 * needs a DDR buffer.  CFG0.phy_addr_start is computed from the DMA handle
 * at init and the HW populates 256 buffers over the region.
 */
/* cpu_eq=0, buffer_size=2, refill_ths=0xff */
#define  CA_NI_QM_EQ12_CFG2		0x0000ff02u
#define CA_NI_RX_DQ_DRAM_SIZE		(256u * 2048u)	/* 256 buf * 2048B (over-reserve) = 512KB */
/* deep-queue EQ profile 12 = {eqp0=EQ12, eqp1=EQ14} = 0xEC */
#define CA_NI_RX_EQ12_PROFILE		12
#define CA_NI_RX_EQ12_PROFILE_VAL \
	(FIELD_PREP(CA_NI_QM_EQ_PROF_EQP0, CA_NI_RX_EQ12_ID) | \
	 FIELD_PREP(CA_NI_QM_EQ_PROF_EQP1, CA_NI_RX_EQ_ID2))
/*
 * per-scheduler VOQ enable = QM_QM_SCH_CFG0.voq_en[7:0], stride 4; 0xff is
 * ORed into all of them.
 */
#define CA_NI_QM_VOQ_EN(p)	(0x63c4 + (p) * 4)	/* QM_QM_SCH_CFG0 */
#define  CA_NI_QM_VOQ_EN_ALL		GENMASK(7, 0)
#define CA_NI_QM_VOQ_EN_COUNT		32

/*
 * --- QM block: CPU-EPP 64-bit descriptor FIFO ring pointers.
 *     Index = 4 * (8 * cpu_port + voq); pointers are byte offsets into the
 *     ring, 24 bits meaningful, wrapping at the ring size (0x400). ---
 */
#define CA_NI_QM_EPP64_WRPTR(p, q)	(0x7000 + 4 * (8 * (p) + (q)))
#define CA_NI_QM_EPP64_RDPTR(p, q)	(0x7100 + 4 * (8 * (p) + (q)))
#define CA_NI_QM_EPP64_PADDR_START(p, q) (0x7200 + 4 * (8 * (p) + (q)))
/*
 * The second per-VoQ CPU-EPP64 ring buffer (PADDR_HI at 0x7220), at PADDR +
 * 0x2000.  Without it the writeback engine advances the write pointer but
 * writes zero descriptors.  Per VoQ with the same 0x400 stride: voq q ->
 * ring_base + 0x2000 + q * 0x400.
 */
#define CA_NI_QM_EPP64_PADDR_HI(p, q)	(0x7220 + 4 * (8 * (p) + (q)))
#define CA_NI_RX_RING_HI_OFFSET		0x2000u
#define  CA_NI_QM_EPP64_PTR		GENMASK(23, 0)

/* ring geometry: 128 descriptors x 8 bytes, byte pointers wrap at 0x400 */
#define CA_NI_RX_EPP_PER_VOQ		128
#define CA_NI_RX_DESC_SIZE		8
#define CA_NI_RX_RING_BYTES		(CA_NI_RX_EPP_PER_VOQ * \
					 CA_NI_RX_DESC_SIZE)	/* 0x400 per voq */
/*
 * Each of the 8 VoQs of the CPU port gets its own CPU-EPP FIFO ring: a frame
 * on a VoQ without an armed ring is not admitted.  The 8 rings are
 * contiguous in the reserved DDR region, ring[voq * 128 + slot], and NAPI
 * drains all of them.
 */
#define CA_NI_RX_VOQ_COUNT		8
/*
 * Per port 0x4000 = the low PADDR rings (0x2000) + the high PADDR_HI rings
 * (0x2000); the mapping and poison fill cover both ports.  A hash-engine
 * punt can land on CPU port 1, so it has its own rings at +0x4000.
 */
#define CA_NI_RX_PORTS			2	/* CPU ports 0 and 1 are drained */
#define CA_NI_RX_RING_COUNT		(CA_NI_RX_PORTS * CA_NI_RX_VOQ_COUNT)
#define CA_NI_RX_PORT_STRIDE		0x4000u	/* low + high rings of one port */
#define CA_NI_RX_RING_TOTAL_BYTES	(CA_NI_RX_PORTS * CA_NI_RX_PORT_STRIDE)
#define CA_NI_RX_RING_SLOTS_PER_VOQ	CA_NI_RX_EPP_PER_VOQ	/* __le64 slots per voq */
/*
 * RMU0 drop counters: climbing means the frame reached RMU0 but found no
 * buffer for its VoQ; 0 means the frame never arrived.
 */
#define CA_NI_QM_RMU0_NO_BUF_DROP	0x6818	/* QM_QM_RMU0_NO_BUF_DROP_PKT_CNTR */
#define CA_NI_QM_RMU0_FE_DROP	0x681c	/* QM_QM_RMU0_FE_DROP_PKT_CNTR */

/*
 * --- 64-bit EPP RX descriptor (little-endian, in the coherent ring):
 *     {eqid[3:0], sop bit4, l4_csum_err bit5, eop bit6, phy_addr[31:7],
 *     pkt_size[45:32], sw_id[51:46]}.  sw_id == 0 is the normal frame
 *     (HEADER_A at +0x40); sw_id != 0 is the header-less format (WiFi fast
 *     forward): frame at +0x10, length = pkt_size - 0x10.  A descriptor
 *     without SOP is invalid. ---
 */
#define CA_NI_RX_DESC_PA		0xffffff80u	/* of the low word */
#define CA_NI_RX_DESC_EQID		GENMASK(3, 0)
#define CA_NI_RX_DESC_SOP		BIT(4)
#define CA_NI_RX_DESC_CSUM_ERR		BIT(5)
#define CA_NI_RX_DESC_EOP		BIT(6)
#define CA_NI_RX_DESC_LEN		GENMASK_ULL(45, 32)
#define CA_NI_RX_DESC_SWID		GENMASK_ULL(51, 46)
#define CA_NI_RX_DESC_HDR_LEN		0x10	/* sw_id != 0: data off/len adj */

/*
 * --- RX buffer layout (buffer base = the 128B-aligned buffer PA) ---
 *   [0x00..0x3f] 64B SW headroom (QM pkt-buf-cfg head_room_first = 4 x 16B)
 *   [0x40..0x47] HEADER_A - two 32-bit big-endian words
 *   [0x48......] HEADER_CPU (8B, only if HEADER_A.cpu_flg) then the frame
 */
#define CA_NI_RX_BUF_HEADROOM		64
#define CA_NI_RX_HDRA_OFF		0x40
#define CA_NI_RX_FRAME_OFF		0x48
/* word 0 = get_unaligned_be32(buf + 0x40): the pkt_info word */
#define  CA_NI_HDRA_W0_CPU_FLG		BIT(31)	/* HEADER_CPU present */
#define  CA_NI_HDRA_W0_DEEP_Q		BIT(30)
/* word 1 = get_unaligned_be32(buf + 0x44): size/port word */
#define  CA_NI_HDRA_W1_PKT_SIZE		GENMASK(28, 15)
#define  CA_NI_HDRA_W1_LSPID		GENMASK(14, 9)
#define  CA_NI_HDRA_W1_LDPID		GENMASK(8, 3)
#define  CA_NI_HDRA_W1_COS		GENMASK(2, 0)
#define CA_NI_RX_HDR_CPU_LEN		8

/*
 * buffer size pushed to the pool (2048 frame + 64 headroom + 16 headers) and
 * the required PA alignment
 */
#define CA_NI_RX_BUF_SIZE		2128
#define CA_NI_RX_BUF_ALIGN		128

/*
 * --- NI_HV per-port RX control: drop policy and the FE-bypass RX steer.
 *     rxctrl_byp_en + byp_dpid: ingress frames of this port bypass the
 *     forwarding engine and go straight to CPU port <dpid> (the RX twin of
 *     the TX CA_DMA_LSO_VP_HDRA_CFG febypass).  Reset value 0x08000400. ---
 */
#define CA_NI_PORT_RX_CNTRL_CFG(p)	(0xa5f8 + (p) * CA_NI_PORT_STRIDE)
#define  CA_NI_RX_CNTRL_FF_OVR_DROP_DIS	BIT(2)
#define  CA_NI_RX_CNTRL_CRC_DROP_DIS	BIT(4)
#define  CA_NI_RX_CNTRL_OVR_DROP_DIS	BIT(5)
#define  CA_NI_RX_CNTRL_RUNT_DROP_DIS	BIT(6)
#define  CA_NI_RX_CNTRL_FLOW_DROP_DIS	BIT(7)
#define  CA_NI_RX_CNTRL_UKOP_DROP_DIS	BIT(8)
#define  CA_NI_RX_CNTRL_OAM_DROP_DIS	BIT(9)
#define  CA_NI_RX_CNTRL_LNKSTAT_DROP_DIS BIT(10)
#define  CA_NI_RX_CNTRL_PFC_DROP_DIS	BIT(11)
#define  CA_NI_RX_CNTRL_BYP_DPID	GENMASK(21, 16)
#define  CA_NI_RX_CNTRL_BYP_COS		GENMASK(26, 24)
#define  CA_NI_RX_CNTRL_BYP_DPQ		BIT(27)
#define  CA_NI_RX_CNTRL_BYP_EN		BIT(28)
#define  CA_NI_RX_CNTRL_BYP_MIR		BIT(30)
#define  CA_NI_RX_CNTRL_BYP_NDRP	BIT(31)

/* CPU port 0 logical destination port id (AAL_LPORT_CPU_0) */
#define CA_NI_LDPID_CPU0		0x10

/*
 * --- L2FE ARB ldpid->pdpid map: selects the physical egress port for a
 *     logical destination port.  A CPU-injected upstream OMCI frame carries
 *     HEADER_A ldpid = PON (7) + 8 = 0x0f (the "9th queue"); without a map
 *     entry it has no route to the PON-OAM egress and never reaches the
 *     PUC.  All 9th-queue ldpids 0x08..0x0f (both dbuf, both my_mac) map to
 *     PPORT_OAM.  Indirect: DATA = pdpid, ACCESS = go[31] | write[30] |
 *     addr[7:0], addr = (my_mac << 7) | (dbuf << 6) | ldpid. ---
 */
#define CA_NI_L2FE_ARB_PDPID_ACCESS	0x1654	/* L2FE_ARB_PDPID_MAP_TBL_ACCESS */
#define CA_NI_L2FE_ARB_PDPID_DATA	0x1658	/* L2FE_ARB_PDPID_MAP_TBL_DATA */
#define CA_NI_PPORT_OAM			0x0c	/* AAL_PPORT_OAM */
#define CA_NI_PPORT_QM			0x08	/* AAL_PPORT_QM (US PON data path) */
#define CA_NI_PPORT_BLACKHOLE		0x0f	/* AAL_PPORT_BLACKHOLE (drop) */
#define CA_NI_LDPID_9QUEUE_LO		0x08	/* AAL_LPORT_9QUEUE_NI0 */
#define CA_NI_LDPID_9QUEUE_HI		0x0f	/* AAL_LPORT_9QUEUE_NI7 (PON 7 + 8) */
#define CA_NI_LDPID_CPU_MQ_LO		0x20	/* AAL_LPORT_CPU_MQ_0 / LLID_GEM_INDEX_0 */
#define CA_NI_LDPID_CPU_MQ_HI		0x3f	/* all of 0x20..0x3f -> PPORT_QM */

/*
 * --- NI-RX L3FE demux routing map: maps the forwarding engine's
 *     per-{ingress, traffic class} demux output to the L3QM CPU/NI egress
 *     slots.  Without it the FE forwards a frame to CPU0 logically but the
 *     L3QM never enqueues it to the CPU-EPP ring.  0xa1a0 = 0x22AA0000 is a
 *     2-bit value of 2 at bits 16, 18, 20, 22, 24 and 28. ---
 */
#define CA_NI_NIRX_L3FE_DEMUX0	0xa1cc	/* NI_HV_GLB_RX_PORT_ID_CFG */
#define  CA_NI_NIRX_L3FE_DEMUX0_VAL	0x040c2040u
#define CA_NI_NIRX_L3FE_DEMUX1	0xa1d0	/* NI_HV_GLB_RX_PORT_ID_CFG2 */
#define  CA_NI_NIRX_L3FE_DEMUX1_VAL	0x00007185u
#define CA_NI_NIRX_L3FE_DEMUX2	0xa198
#define  CA_NI_NIRX_L3FE_DEMUX2_VAL	0x00000000u
#define CA_NI_NIRX_L3FE_DEMUX3	0xa19c
#define  CA_NI_NIRX_L3FE_DEMUX3_VAL	0x00000002u
#define CA_NI_NIRX_L3FE_DEMUX4	0xa1a0
#define  CA_NI_NIRX_L3FE_DEMUX4_VAL	0x22aa0000u
#define CA_NI_NIRX_L3FE_DEMUX5	0xa1a4
/* this value lands at 0xa1e0 (NIRX_L3FE_DEMUX_CFG0), expected 0xFFFFFFFF */
#define  CA_NI_NIRX_L3FE_DEMUX5_VAL	0xffffffffu

/*
 * Deep-queue (deep_q=1) per-ldpid L3FE demux table, a parallel copy of the
 * normal per-ldpid table at +0x10; a deep_q=1 frame is routed by this table.
 * Each ldpid has a 2-bit demux_id (0 = L3QM, 1 = WAN, 2 = L2FE, 3 = L2TM):
 * reg = 0xa1b4 - 4 * (ldpid >> 4), field = bits [2 * (ldpid & 15) + 1 :
 * 2 * (ldpid & 15)].
 */
#define CA_NI_NIRX_L3FE_DPQ_DEMUX_48_63	0xa1a8
#define CA_NI_NIRX_L3FE_DPQ_DEMUX_32_47	0xa1ac
#define CA_NI_NIRX_L3FE_DPQ_DEMUX_16_31	0xa1b0
#define CA_NI_NIRX_L3FE_DPQ_DEMUX_0_15	0xa1b4
/*
 * DPQ demux values.  This table is not a copy of the normal one: ldpid 8-15
 * map to L2FE (0xAAAA0000).
 */
/* ldpid 0x32 -> demux_id 0 = L3QM */
#define  CA_NI_NIRX_L3FE_DPQ_DEMUX_48_63_VAL	0x00000000u
#define CA_NI_NIRX_L3FE_DPQ_DEMUX_32_47_VAL	0x00000000u
#define  CA_NI_NIRX_L3FE_DPQ_DEMUX_16_31_VAL	0x22AA0000u	/* ldpid 0x19 -> L2FE */
#define  CA_NI_NIRX_L3FE_DPQ_DEMUX_0_15_VAL	0xAAAA0000u
/*
 * NIRX-L3FE demux per-ldpid table (normal at 0xa1d4-0xa1e0, deep queue at
 * 0xa1e4-0xa1f0), separate from the tables above.  The destination of a
 * deep_q=1 CPU frame is selected here; left at reset it egresses a physical
 * ES port instead of ES8 (L3QM).  Values are the known-good register
 * contents rather than decoded fields.
 */
#define CA_NI_NIRX_L3FE_DEMUX_NORM0	0xa1d4
#define  CA_NI_NIRX_L3FE_DEMUX_NORM0_VAL 0x00000000u
#define CA_NI_NIRX_L3FE_DEMUX_NORM1	0xa1d8
#define  CA_NI_NIRX_L3FE_DEMUX_NORM1_VAL 0x00000000u
#define CA_NI_NIRX_L3FE_DEMUX_NORM2	0xa1dc
#define  CA_NI_NIRX_L3FE_DEMUX_NORM2_VAL 0x00000000u
#define CA_NI_NIRX_L3FE_DEMUX_NORM3	0xa1e0
#define  CA_NI_NIRX_L3FE_DEMUX_NORM3_VAL 0x64503C28u
#define CA_NI_NIRX_L3FE_DPQ0	0xa1e4
#define  CA_NI_NIRX_L3FE_DPQ0_VAL	0x780C7864u
#define CA_NI_NIRX_L3FE_DPQ1	0xa1e8
#define  CA_NI_NIRX_L3FE_DPQ1_VAL	0x78A3E8C8u
#define CA_NI_NIRX_L3FE_DPQ2	0xa1ec
#define  CA_NI_NIRX_L3FE_DPQ2_VAL	0x60506050u
#define CA_NI_NIRX_L3FE_DPQ3	0xa1f0
#define  CA_NI_NIRX_L3FE_DPQ3_VAL	0x00006050u
/*
 * 0xa1c0 is INT_PORT_ID_CFG2, owned by CA_NI_NI_INT_PORT_ID_CFG2; it is not
 * a port-order map.
 */

/*
 * --- L2FE block: PLE default-forward indirect table (the DLF trap-to-CPU
 *     fallback path).  Entry address = lspid << 2 | type; the ACCESS
 *     register kicks a read (go) or a write (go | write) of the DATA
 *     register from/to the table; go self-clears. ---
 */
/*
 * 0x1560 is TX_DOT1P_MAP_DATA and 0x156c the MMSHP_CHK_ID_MAP ACCESS (not a
 * DFT_FWD control block).  The membership check-id map (lport -> check-id,
 * which gates LAN <-> CPU forwarding via PLE_CTL b0/b1) is programmed by
 * cortina_ni_rx_port_profiles_init.
 */
#define CA_NI_L2FE_CHKID_MAP_ACCESS	0x156c	/* MMSHP_CHK_ID_MAP indirect ACCESS */
#define CA_NI_L2FE_CHKID_MAP_DATA	0x1570	/* ... DATA = check-id for the lport */
/* PLE control registers */
#define CA_NI_L2FE_PLE_TRUNK0		0x1514
#define CA_NI_L2FE_PLE_TRUNK1		0x1520
#define  CA_NI_L2FE_PLE_TRUNK_STOCK	0x00000012u
#define CA_NI_L2FE_PLE_HD_FF_CTL	0x152c
#define  CA_NI_L2FE_PLE_HD_FF_STOCK	0x00000610u
#define CA_NI_PLE_DFT_FWD_ACCESS	0x1564
#define  CA_NI_PLE_ACCESS_GO		BIT(31)
#define  CA_NI_PLE_ACCESS_WRITE		BIT(30)
#define CA_NI_PLE_DFT_FWD_DATA		0x1568
#define  CA_NI_PLE_DFT_DENY		BIT(0)
#define  CA_NI_PLE_DFT_MC_GROUP_ID	GENMASK(10, 1)
#define  CA_NI_PLE_DFT_REDIR_EN		BIT(11)
#define  CA_NI_PLE_DFT_MC_GROUP_VALID	BIT(12)
/* non-known traffic types (entry address low 2 bits) */
#define CA_NI_PLE_TYPE_BC		0
#define CA_NI_PLE_TYPE_UUC		1
#define CA_NI_PLE_TYPE_UL2MC		2
#define CA_NI_PLE_TYPE_UL3MC		3

/*
 * L2FE forwarding to CPU.  For a LAN-port DLF frame PLE_DFT_FWD redirects it
 * to a redirect LDPID and REDIR_LDPID_CONFIG[that] maps it to the real
 * destination LDPID, the CPU port (0x10).  Without REDIR_LDPID_CONFIG and
 * the ARB/PLC forwarding control registers, DLF frames have no CPU
 * destination and are dropped before the QM.
 */
#define CA_NI_L2FE_PLC_DPID_FWD_CTRL	0x1404
#define  CA_NI_L2FE_DPID_FWD_CTRL_VAL	0x00000003u
#define CA_NI_L2FE_PLC_LRN_FWD_CTRL_0	0x1408
#define  CA_NI_L2FE_LRN_FWD_CTRL_0_VAL	0x22000290u
#define CA_NI_L2FE_PLC_LRN_FWD_CTRL_1	0x140c
#define  CA_NI_L2FE_LRN_FWD_CTRL_1_VAL	0x0c100c10u
/* FDB global control: per-type forward actions (unknown DA/BC -> the LRN_FWD_CTRL dest) */
#define CA_NI_L2FE_FDB_CTRL_0		0x1c00
#define  CA_NI_L2FE_FDB_CTRL_0_VAL	0x82BFEFF9u
#define CA_NI_L2FE_FDB_CTRL_1		0x1c04	/* dest_ldpid[21:16]=0x10 (CPU_0) */
#define  CA_NI_L2FE_FDB_AGE		GENMASK(15, 0)	/* seconds; entries go in 1-2x */
#define  CA_NI_L2FE_FDB_AGE_UNIT_MS	1000
#define  CA_NI_L2FE_FDB_CTRL_1_VAL	0x9F90012Cu	/* 300 s */
#define CA_NI_L2FE_PLE_DEFAULT_REG	0x1504
#define  CA_NI_L2FE_PLE_DEFAULT_VAL	0x001b0000u
#define CA_NI_L2FE_ARB_CTRL		0x1600
#define  CA_NI_L2FE_ARB_CTRL_VAL	0x89c71c82u
#define  CA_NI_L2FE_ARB_MC_IDX_COPY_EN	BIT(3)	/* keep mc_idx on MC replication */
/*
 * ARB register 0x1608, written as 0x00000280.  Its purpose is unknown; it
 * does not control the header CPU flag.
 */
#define CA_NI_L2FE_ARB_REG_1608		0x1608
#define  CA_NI_L2FE_ARB_REG_1608_VAL	0x00000280u
#define CA_NI_L2FE_ARB_CTRL_EXT		0x160c
#define  CA_NI_L2FE_ARB_CTRL_EXT_VAL	0x00000001u
/*
 * 0x1614-0x1620 are two indirect ACCESS/DATA pairs (redirect LDPID config and
 * redirect drop-source config), not a mask; both read 0 on a working setup.
 */
/* L2FE_ARB_REDIR_DROP_SRC_CONFIG_TBL_ACCESS */
#define CA_NI_L2FE_REDIR_DROP_SRC_ACCESS	0x161c
/* L2FE_ARB_REDIR_DROP_SRC_CONFIG_TBL_DATA */
#define CA_NI_L2FE_REDIR_DROP_SRC_DATA		0x1620
/*
 * redirect-LDPID config table (indirect: ACCESS = GO | (rbw << 30) | idx,
 * poll GO clear).  DATA: rdir_cos_vld[0], rdir_cos[3:1], rdir_ldpid[9:4],
 * rdir_en[10], rdir_wan_dst[11].
 */
#define CA_NI_L2FE_REDIR_LDPID_ACCESS	0x1614	/* L2FE_ARB_REDIR_LDPID_CONFIG_TBL_ACCESS */
#define CA_NI_L2FE_REDIR_LDPID_DATA	0x1618	/* L2FE_ARB_REDIR_LDPID_CONFIG_TBL_DATA */
#define  CA_NI_L2FE_REDIR_ACCESS_GO	BIT(31)
#define  CA_NI_L2FE_REDIR_ACCESS_WR	BIT(30)
#define  CA_NI_L2FE_REDIR_RDIR_LDPID	GENMASK(9, 4)
#define  CA_NI_L2FE_REDIR_RDIR_EN	BIT(10)
#define CA_NI_RX_BLACKHOLE_LDPID	0x1f	/* AAL_LPORT_BLACKHOLE: the DLF default dest */

/*
 * L2FE per-port profile tables.  The L2FE consults three per-logical-port
 * profile tables and one membership table around the FDB and DFT_FWD
 * forwarding decision:
 *   IPPB  (0x11dc/0x11e0)  physical -> logical source-port map (1 word)
 *   ILPB  (0x11e4/0x11e8..0x11f8, DATA4..DATA0)  ingress port profile
 *   ELPB  (0x1420/0x1424+0x1428, DATA1+DATA0)    egress  port profile
 *   MMSHP (0x1574/0x1578+0x157c, DATA1+DATA0)    per-port allowed-ldpid
 *                                                64-bit isolation bitmap
 * All use the usual indirect protocol (DATA registers first, then ACCESS =
 * GO | WR | index, poll GO clear).
 *
 * The reset contents drop everything:
 *   - ILPB.stp_mode[1:0] = 0 is the STP drop state (3 = forward + learn), so
 *     every ingress frame is discarded at the PLC stage and resolves to the
 *     blackhole ldpid 0x1f;
 *   - MMSHP[lspid] = 0 allows no egress port (isolation filter, active via
 *     PLE_CTL igr_fltr_chk_ldpid, set at reset);
 *   - ELPB.egr_port_stp_status (bit 1) = 0 blocks egress.
 * All of them must be programmed at init.
 */
#define CA_NI_L2FE_IPPB_ACCESS		0x11dc
#define CA_NI_L2FE_IPPB_DATA		0x11e0	/* lspid[5:0] */
#define CA_NI_L2FE_ILPB_ACCESS		0x11e4
#define CA_NI_L2FE_ILPB_DATA4		0x11e8
#define CA_NI_L2FE_ILPB_DATA3		0x11ec
#define CA_NI_L2FE_ILPB_DATA2		0x11f0
#define CA_NI_L2FE_ILPB_DATA1		0x11f4
#define CA_NI_L2FE_ILPB_DATA0		0x11f8
#define CA_NI_L2FE_ELPB_ACCESS		0x1420
#define CA_NI_L2FE_ELPB_DATA1		0x1424
#define CA_NI_L2FE_ELPB_DATA0		0x1428
#define CA_NI_L2FE_MMSHP_ACCESS		0x1574
#define CA_NI_L2FE_MMSHP_DATA1		0x1578	/* allowed-ldpid bitmap [63:32] */
#define CA_NI_L2FE_MMSHP_DATA0		0x157c	/* allowed-ldpid bitmap [31:0]  */

/* logical-port map facts (lspid/ldpid space, 64 entries) */
#define CA_NI_L2FE_LPORT_COUNT		64
#define CA_NI_LPORT_ETH_NI6		0x06
#define CA_NI_LPORT_CPU_0		0x10
#define CA_NI_LPORT_CPU_7		0x17
#define CA_NI_LPORT_L3_WAN		0x18
#define CA_NI_LPORT_L3_LAN		0x19
#define CA_NI_LPORT_MC			0x1b
#define CA_NI_LPORT_GEM_FIRST		0x20	/* 0x20-0x3f = PON LLID/GEM + CPU-MQ */

/*
 * Composed ILPB profile words, every field named:
 * DATA4: wan_ind (b10), only for the PON-side logical ports.
 * DATA3: vlan_cmd_sel_bm[1:0] = 3, sa_miss_fwd (b20) = 1 (DLF forwards, no
 *        drop).
 * DATA2: stp_mode[1:0] = 3 (forward + learn; 0 would drop everything), tag
 *        drops all 0, c/s_tpid_match = compare entry 3 (0x8100),
 *        other_tpid_match = entry 3, top/inner C-TPID compare enabled (LAN
 *        ports) or S-TPID (GEM ports), inner_vid_sel_bm[28:27] = 3.  The MC
 *        port compares all 4 TPID entries.
 * DATA1: 802.1p_mark_bm hi[2:0] = 3, change_1p_if_pop (b3) = 1,
 *        flowid_sel_bm[8:6] = 7, station_move_en (b31) on ETH-NI0-6 and
 *        the CPU ports.
 * DATA0: 802.1p_mark_bm lo (b31) = 1, inner_1p_mode[30:29] = 2,
 *        dscp_mode[24:23] = 2 (from the RX DSCP map), ing_fltr_ena (b3) = 0.
 */
#define CA_NI_L2FE_ILPB_D4_WAN		0x00000400u
#define CA_NI_L2FE_ILPB_D3_INIT		0x00100003u
#define CA_NI_L2FE_ILPB_D2_PORT		0x18022163u	/* LAN/CPU/L3/other <0x20 */
#define CA_NI_L2FE_ILPB_D2_MC		0x1803fd7fu	/* MC port 0x1b */
#define CA_NI_L2FE_ILPB_D2_GEM		0x180222a3u	/* GEM/LLID >=0x20 (S-TPID) */
#define CA_NI_L2FE_ILPB_D1_INIT		0x000001cbu
#define CA_NI_L2FE_ILPB_D1_STAMOVE	BIT(31)
/*
 * Per-ingress-port classifier window: a LAN port only consults the CLE_IGR
 * ingress classifier if its ILPB profile enables igr_cls_lookup_en and sets
 * how many entries to walk from cls_start.  On the eth lports D1 =
 * 0x80800dcb = INIT | STAMOVE | CLS_EN | (6 << 9).  Never set CLS_EN on the
 * CPU lports 0x10-0x17: it drops BM_RX and diverts upstream OMCI.
 */
#define CA_NI_L2FE_ILPB_D1_CLS_EN	BIT(23)		/* igr_cls_lookup_en */
#define CA_NI_L2FE_ILPB_D1_CLS_LEN	GENMASK(14, 9)	/* entries to walk */
#define CA_NI_L2FE_ILPB_D1_CLS_START	GENMASK(22, 15)	/* first entry (0 here) */
#define CA_NI_L2FE_ILPB_D1_CLS_LEN_VAL	6

/* per-drop-reason counters (0x1788 starts a different table) */
#define CA_NI_L2FE_PE_DROP_STTS_ACCESS	0x1780
#define CA_NI_L2FE_PE_DROP_STTS_DATA	0x1784
#define CA_NI_L2FE_PE_DROP_STTS_COUNT	32
#define CA_NI_L2FE_ILPB_D0_INIT		0xc1000000u
#define  CA_NI_L2FE_ILPB_STP_MODE	GENMASK(1, 0)	/* in DATA2; 0=DROP 3=fwd+learn */

/*
 * ELPB DATA0: bit0 egr_vlan_aware, bit1 egr_port_stp forward (0 = blocked),
 * bit2 egr_ve_srch (off), bit3 egr_dest_wan (PON-side + L3_LAN dests).
 */
#define CA_NI_L2FE_ELPB_D0_LAN		0x00000003u
#define CA_NI_L2FE_ELPB_D0_WAN		0x0000000bu

/* L2FE direct config registers (reset values in the comments) */
#define CA_NI_L2FE_GLB_CTRL		0x1004	/* dft 0x14 */
#define  CA_NI_L2FE_GLB_CTRL_STOCK	0x00000054u	/* + change_1p_if_pop_en(b6) */
#define CA_NI_L2FE_PP_TPID_CMP_S	0x1108	/* dft 0x8100 */
#define  CA_NI_L2FE_TPID_S_STOCK	0x000088a8u
#define CA_NI_L2FE_PP_TPID_CMP_O	0x1110	/* dft 0x8100 */
#define  CA_NI_L2FE_TPID_O_STOCK	0x00009100u
#define CA_NI_L2FE_PP_PARSER_CTRL	0x11d0	/* dft 0xa03b0400 */
#define  CA_NI_L2FE_PARSER_CTRL_STOCK	0xa03b2400u	/* + arp_op_filter_dis(b13) */
#define CA_NI_L2FE_PLC_L2_LEARNING	0x1410	/* dft 0x46 */
#define  CA_NI_L2FE_L2_LEARNING_STOCK	0x00000001u	/* unkvlan_learn only */
#define CA_NI_L2FE_PLC_VLAN_MODE	0x1414	/* dft 0x2 */
#define  CA_NI_L2FE_VLAN_MODE_STOCK	0x00000003u	/* + glb_vlan_mode(b0) */
#define CA_NI_L2FE_PLE_CTL		0x1500	/* dft 0x260 */
#define  CA_NI_L2FE_PLE_CTL_STOCK	0x0000027bu	/* + membshp_chk lan/wan(b0/1),
							 * skip_port_lpbk_chk(b3),
							 * pon_mode(b4)
							 */
#define CA_NI_L2FE_PLE_UNKWN_VLAN_DFT1	0x150c	/* dft 0x0 */
#define  CA_NI_L2FE_UNKWN_VLAN_DFT1_STOCK 0x00000001u
/*
 * L2 FDB (MAC table): a hash table, the HW computes the bucket on APPEND.
 * A static entry {our MAC -> DeepQ_0} makes an own-MAC frame a known unicast
 * forwarded to DeepQ_0 -> PDPID = QM -> RMU -> CPU, bypassing the DLF flood
 * and the L3FE.  Commit: write DATA0-3, ACCESS = GO | op, poll GO clear;
 * CMD_RETURN.status[3:0] == 0x5 means HIT (appended).
 */
#define CA_NI_L2FE_FDB_ACCESS		0x1ca0
#define  CA_NI_L2FE_FDB_GO		BIT(31)		/* access / busy */
#define  CA_NI_L2FE_FDB_OP_INIT		0x00		/* one-time hash-table init */
#define  CA_NI_L2FE_FDB_OP_READ		0x04		/* look up (status=HIT on found) */
#define  CA_NI_L2FE_FDB_OP_SCAN		0x01		/* read the entry at INDEX */
#define  CA_NI_L2FE_FDB_INDEX		GENMASK(20, 8)
#define CA_NI_L2FE_FDB_ENTRIES		0x1020		/* 8K hash + 32 overflow */
#define  CA_NI_L2FE_FDB_VID		GENMASK(18, 7)	/* in DATA1 */
#define  CA_NI_L2FE_FDB_OP_APPEND	0x45		/* add a static entry */
#define  CA_NI_L2FE_FDB_OP_REPLACE	0x65		/* add a static entry, replacing the key's */
#define  CA_NI_L2FE_FDB_OP_DELETE	0x85		/* delete the entry with the key */
#define CA_NI_L2FE_FDB_DATA3		0x1ca4
#define CA_NI_L2FE_FDB_DATA2		0x1ca8
#define CA_NI_L2FE_FDB_DATA1		0x1cac
#define CA_NI_L2FE_FDB_DATA0		0x1cb0
#define  CA_NI_L2FE_FDB_LPID		GENMASK(5, 0)	/* action: forward-to ldpid */
#define  CA_NI_L2FE_FDB_VALID		BIT(9)
#define  CA_NI_L2FE_FDB_STATIC		BIT(19)		/* no-age */
#define  CA_NI_L2FE_FDB_DA_PERMIT	BIT(20)		/* mandatory or DA-hit won't fwd */
#define  CA_NI_L2FE_FDB_SA_PERMIT	BIT(21)
#define CA_NI_L2FE_FDB_CMD_RETURN	0x1c2c		/* status[3:0]; 0x5 = HIT (appended) */
#define  CA_NI_L2FE_FDB_STATUS_HIT	0x5
#define  CA_NI_L2FE_FDB_RET_INDEX	GENMASK(16, 4)	/* entry a scan found */
/*
 * My-MAC detection.  L2FE 0x1150 is a mask, and 0x3440 (STG0-SPB on other
 * chips) is an unmapped hole here: STG0 ends at LPB_IDX 0x343c and STG1
 * starts at 0x3480, so a write to 0x3440 raises an asynchronous SError.  The
 * active my-MAC is comparator A (NI global 0xa024/a028/a5c0); comparator B
 * (0x3294/98) is unused; chk_mymac_for_lan (0x3400 b21) = 0.  The my-MAC
 * hit route lives in the STG0 LPB profile vector (0x3408-3434).
 */
/* (A) NI-global my-MAC (the active one): CFG0 = bytes 0-3, CFG1 = byte 4, PT[31:24] = byte 5 */
#define CA_NI_L3FE_NI_MAC_CFG0		0xa024
#define CA_NI_L3FE_NI_MAC_CFG1		0xa028		/* [7:0] = byte4 */
#define  CA_NI_L3FE_NI_MAC_BYTE4	GENMASK(7, 0)
#define CA_NI_L3FE_PT_PORT_STATIC_CFG	0xa5c0		/* [31:24] = MAC byte5 (port 0) */
#define  CA_NI_L3FE_PT_MAC_BYTE5	GENMASK(31, 24)
/* enable (0x3400 bit21 chk_mymac_for_lan stays 0) */
#define CA_NI_L3FE_SPCL_PKT_DET_CFG	0x3218
#define  CA_NI_L3FE_MY_MAC_EN		BIT(2)
#define CA_NI_L3FE_STG0_CTRL		0x3400
#define  CA_NI_L3FE_CHK_MYMAC_LAN	BIT(21)
/*
 * STG0 LPB profiles: the my-MAC hit route and spcl_pkt_en live in this
 * per-profile 96-bit vector (LOW/MID/HIGH).  Direct registers.
 */
#define CA_NI_L3FE_STG0_LDPID_MAP	0x3404
#define  CA_NI_L3FE_STG0_LDPID_MAP_VAL	0x03985907u
#define CA_NI_L3FE_STG0_LPB_LOW0	0x3408
#define CA_NI_L3FE_STG0_LPB_MID0	0x340c
#define CA_NI_L3FE_STG0_LPB_HIGH0	0x3410
#define CA_NI_L3FE_STG0_LPB_LOW1	0x3414
#define CA_NI_L3FE_STG0_LPB_MID1	0x3418
#define CA_NI_L3FE_STG0_LPB_HIGH1	0x341c
#define CA_NI_L3FE_STG0_LPB_LOW2	0x3420
#define CA_NI_L3FE_STG0_LPB_MID2	0x3424
#define CA_NI_L3FE_STG0_LPB_HIGH2	0x3428
#define CA_NI_L3FE_STG0_LPB_LOW3	0x342c
#define CA_NI_L3FE_STG0_LPB_MID3	0x3430
#define CA_NI_L3FE_STG0_LPB_HIGH3	0x3434
#define  CA_NI_L3FE_LPB_MID_SEL0	0x000003C0u	/* prof0/2 (spcl_pkt_sel=0) */
#define  CA_NI_L3FE_LPB_MID_SEL1	0x000007C0u	/* prof1/3 (spcl_pkt_sel=1) */
/*
 * LPB HIGH profile values.  The register is an opaque 96-bit vector with a
 * reset value of 0x18000190.  Known-good values:
 *     3410 (P0) 18100192   341c (P1) 19180182
 *     3428 (P2) 18100192   3434 (P3) 1a1bfd92
 * i.e. reset | BIT(20) | BIT(1) on every profile.  The meaning of BIT(1) is
 * not documented; it matters for frames entering the L3FE through the
 * L3_LAN loopback path (LAN ingress), not for PON frames.  The
 * cortina_ni_rx.lpb_stock_bit1 module parameter can clear it.
 */
#define  CA_NI_L3FE_LPB_HIGH_BIT1	BIT(1)
#define  CA_NI_L3FE_LPB_HIGH_P0		0x18100192u	/* WAN prof (spcl_pkt_en=1) */
#define  CA_NI_L3FE_LPB_HIGH_P1		0x19180182u	/* LAN prof */
#define  CA_NI_L3FE_LPB_HIGH_P3		0x1A1BFD92u	/* OAM prof */
#define  CA_NI_L3FE_LPB_SPCL_PKT_EN	BIT(20)
#define  CA_NI_L3FE_LPB_SPCL_PKT_SEL	BIT(19)
#define CA_NI_RX_CPU_LDPID		0x10	/* CPU_0 LDPID (direct-CPU; has NO ES port) */
/*
 * DLF redirect target = DeepQ_0 (ldpid 0x00).  The BM-header deep_q (word0
 * bit 30) that steers a frame to L2TM ES port 7 (L3QM -> QM RMU -> CPU) is
 * the L2FE ARB dbuf flag, and the ARB resolves PDPID = QM (0x08) only for
 * {dbuf = 1, dest ldpid in DeepQ 0x0..0x6}.  A redirect to CPU_0 (0x10) or
 * CPU_Q (0x1d) resolves to PDPID = CPU (0x09), which has no ES port.  So the
 * redirect goes to a DeepQ ldpid with dbuf forced to 1
 * (cortina_ni_rx_arb_deepq_init).
 */
#define CA_NI_RX_REDIR_LDPID		0x00	/* DeepQ_0: {dbuf=1,ldpid}->PDPID=QM->ES port 7 */
/*
 * PLE default-forward (lookup-miss) entry, per lspid and traffic type
 * (vendor aal_ple_dft_fwd_set): redirect the miss to a logical destination
 * port.  Stock's miss actions are 0 (flood), 0x1000 (drop), trap to CPU
 * (redirect to CPU_0, 0x1820) and forward to L3 (redirect to L3_LAN/L3_WAN).
 */
#define CA_NI_PLE_DFT_FWD_VALID		BIT(12)
#define CA_NI_PLE_DFT_FWD_REDIR		BIT(11)
#define CA_NI_PLE_DFT_FWD_LDPID		GENMASK(10, 1)
#define CA_NI_PLE_DFT_FWD_TO(ldpid)	(CA_NI_PLE_DFT_FWD_VALID |	\
					 CA_NI_PLE_DFT_FWD_REDIR |	\
					 FIELD_PREP(CA_NI_PLE_DFT_FWD_LDPID, ldpid))
#define CA_NI_RX_QM_REDIR_LDPID		0x32	/* legacy redir target (unused after CPU redir) */
/*
 * L3_LAN ldpid (0x19): the L2FE resolves an own-MAC unicast / ARP / L3-hit
 * frame to ldpid 0x19 via the my-MAC comparator and the ARP parser
 * (PARSER_CTRL arp_op_filter_dis), not via DFT_FWD.  PDPID_MAP[0x19] = 0x0d
 * (L3_LAN -> ES port 8 -> L3FE -> CPU) is a second CPU path the RX driver
 * does not drain, so PDPID_MAP[0x19] is set to QM (0x08) like 0x32, and both
 * classifier outputs reach the CPU through QM -> DeepQ -> ES7 -> RMU.
 */
#define CA_NI_RX_L3LAN_LDPID		0x19
/* AAL_LPORT_L3_WAN: PON-DS routed-frame classifier output (HW L3-fwd) */
#define CA_NI_RX_L3WAN_LDPID		0x18
/*
 * PDPID_MAP: logical destination LDPID -> physical destination port
 * (indirect).  Expected: [0x10-0x17] = 0x09 (CPU), [0x1d] = 0x09, [0x19] =
 * 0x0d (L3_LAN), [0-6] = 0x08 (QM).  Indexed by {my_mac << 7 | dbuf << 6 |
 * ldpid[5:0]}; DATA pdpid[3:0].
 */
#define CA_NI_L2FE_PDPID_MAP_ACCESS	0x1654	/* L2FE_ARB_PDPID_MAP_TBL_ACCESS */
#define CA_NI_L2FE_PDPID_MAP_DATA	0x1658	/* L2FE_ARB_PDPID_MAP_TBL_DATA */
#define  CA_NI_L2FE_PDPID_MAP_PDPID	GENMASK(3, 0)
/* PDPID_MAP index = {my_mac[7], dbuf[6], ldpid[5:0]} */
#define  CA_NI_L2FE_PDPID_IDX_DBUF	BIT(6)
#define  CA_NI_L2FE_PDPID_IDX_MYMAC	BIT(7)
#define CA_NI_RX_CPU_PDPID		0x09	/* AAL_PPORT_CPU physical dest (no ES port) */
#define CA_NI_RX_QM_PDPID		0x08	/* AAL PDPID of the QM deep-queue (-> ES port 7) */
/*
 * L2FE ARB deep-buffer (dbuf) table, the source of the BM-header deep_q
 * without the L3FE.  ARB_CTRL.dbuf_sel picks the flow table (1) or the
 * PORT_DBUF table (0); the PORT_DBUF entry matches the resolved dest ldpid
 * and outputs dbuf_flg.
 */
#define CA_NI_L2FE_ARB_CTRL		0x1600
#define  CA_NI_L2FE_ARB_DBUF_SEL	BIT(1)		/* 0 = use PORT_DBUF table */
/* resolved PDPID==this => Deep Queue (dft 8=QM) */
#define  CA_NI_L2FE_ARB_DBUF_DPID	GENMASK(7, 4)
#define  CA_NI_L2FE_ARB_USE_HDR_A_DBUF	BIT(8)		/* 1=take dbuf from Header-A (FIB/L3FE) */
#define CA_NI_L2FE_ARB_PORT_DBUF_ACCESS	0x164c	/* L2FE_ARB_PORT_DBUF_TBL_ACCESS */
#define CA_NI_L2FE_ARB_PORT_DBUF_DATA	0x1650	/* L2FE_ARB_PORT_DBUF_TBL_DATA */
/*
 * ARB FLOW_DBUF table: per-flow deep-buffer flags, used as the deep_q source
 * when ARB_CTRL.dbuf_sel = 1.  It must stay all zero: marking flows forces
 * deep_q=1 on LAN traffic and diverts it before it can enter the L3FE.
 * Indirect (GO = bit31 / WR = bit30, idx = flow_id / 4); DATA bits [3:0] =
 * dbuf_flg_0..3, one per flow_id % 4.
 */
#define CA_NI_L2FE_ARB_FLOW_DBUF_ACCESS	0x1644	/* L2FE_ARB_FLOW_DBUF_TBL_ACCESS */
#define CA_NI_L2FE_ARB_FLOW_DBUF_DATA	0x1648	/* L2FE_ARB_FLOW_DBUF_TBL_DATA */
/* dbuf_flg_0..3 (all flows marked; normally 0) */
#define  CA_NI_L2FE_ARB_FLOW_DBUF_FLG_ALL 0x0000000Fu
#define  CA_NI_L2FE_ARB_FLOW_DBUF_ENTRIES 16u		/* entries 0..15 cover flows 0..63 */
#define  CA_NI_L2FE_ARB_DBUF_FLG	BIT(0)		/* output: deep-buffer => deep_q */
#define  CA_NI_L2FE_ARB_DBUF_LDPID_VLD	BIT(1)
#define  CA_NI_L2FE_ARB_DBUF_LDPID	GENMASK(7, 2)	/* match on resolved dest-ldpid */
/*
 * L2TM BM packet-memory read-back (buffer header): ACCESS selects the buffer
 * index, DATA7 = Header-A word0 (bit30 = deep_q, bit31 = cpu_flag).
 */
#define CA_NI_L2TM_BM_PKT_MEM_ACCESS	0x218c
#define CA_NI_L2TM_BM_PKT_MEM_DATA7	0x2190
/*
 * BM latches of the last frame's headers: FE = the L2FE-resolved forwarding
 * header (deep_q b30 / cpu b31 / resolved ldpid), NI = the raw received
 * header, TX-NI = the dequeued frame's header.
 */
#define CA_NI_L2TM_BM_RX_FE_HDR_LO	0x2170
#define CA_NI_L2TM_BM_RX_FE_HDR_HI	0x2174
#define CA_NI_L2TM_BM_RX_NI_HDR_LO	0x2178
#define CA_NI_L2TM_BM_RX_NI_HDR_HI	0x217c
#define CA_NI_L2TM_BM_TX_NI_HDR_LO	0x2180
#define CA_NI_L2TM_BM_TX_NI_HDR_HI	0x2184
#define CA_NI_L2TM_BM_STS		0x2188
/*
 * L2TM_L2TM_BM_INT (0x2100), the BM interrupt/error latch, write 1 to clear.
 * These flags latch a per-frame error without incrementing any *_DPCNT drop
 * counter.  Cleared once at probe and read from /proc/net/cortina_ni_rx.
 * (CA_NI_L2TM_TM_CFG below is another name for the same register.)
 */
#define CA_NI_L2TM_BM_INT		0x2100
#define  CA_NI_BM_INT_SB_DROP		BIT(0)
#define  CA_NI_BM_INT_HDR_DROP		BIT(1)
#define  CA_NI_BM_INT_TE_DROP		BIT(2)
#define  CA_NI_BM_INT_WVOQID		BIT(3)	/* wrong VoQ id: dest-port -> VoQ resolve fail */
#define  CA_NI_BM_INT_SZERR		BIT(4)
#define  CA_NI_BM_INT_SEGLENERR		BIT(5)
#define  CA_NI_BM_INT_SEQ_ERR		BIT(6)
#define  CA_NI_BM_INT_DQ_WVOQID		BIT(10)	/* dequeue-side wrong VoQ id */
#define  CA_NI_BM_INT_DQ_SZERR		BIT(11)
#define  CA_NI_BM_INT_DQ_PACK_FF_OVRFLO	BIT(17)
/* BM dequeue packet count (not the 0x213c enqueue cnt) */
#define CA_NI_L2TM_BM_RX_DQ_PCNT	0x2130
/*
 * Port-0 DLF can also be redirected to L3-LAN (LDPID 0x19 -> PDPID 0x0D)
 * rather than CPU-direct (0x10 -> 0x09): dest 0x09 does not enter the QM
 * while dest 0x0D does.  Both map to EQ profile 13.  The dest-port index for
 * physical dest 0x0D is 0x0D + 6 = 0x13 (0x61b4).
 */
#define CA_NI_RX_L3LAN_PDPID		0x0d	/* L3-LAN physical dest (redir 0x19 -> this) */
#define CA_NI_RX_L3LAN_DEST_PORT	19	/* dest-port idx for phys dest 0x0D (0x61b4) */

/*
 * MC flood to CPU: a port-0 broadcast/DLF frame -> DFT_FWD -> mcgid G ->
 * ni_mce_indx[G].mc_vec -> the replication set including the CPU.  Both the
 * mc_vec bit 16 and MC_FIB[16].ldpid = 16 are set, so it works whether
 * mc_vec is a port bitmap or an MC_FIB entry bitmap.  Indirect writes: DATA
 * registers, then ACCESS = GO | WR | idx, poll GO clear.
 */
#define  CA_NI_IND_ACCESS_GO		BIT(31)
#define  CA_NI_IND_ACCESS_WR		BIT(30)		/* rbw = 1 for a store */
/*
 * L2FE ARB MC_FIB.  It is normally empty: CPU delivery uses the DFT_FWD
 * 0x1832 redirect, not an L2 flood to the CPU.
 */
#define CA_NI_L2FE_MC_FIB_ACCESS	0x1634	/* L2FE_ARB_MC_FIB_TBL_ACCESS */
#define CA_NI_L2FE_MC_FIB_DATA4		0x1648
#define CA_NI_L2FE_MC_FIB_DATA3		0x164c
#define CA_NI_L2FE_MC_FIB_DATA2	0x1638	/* L2FE_ARB_MC_FIB_TBL_DATA2 */
#define CA_NI_L2FE_MC_FIB_DATA1	0x163c	/* L2FE_ARB_MC_FIB_TBL_DATA1 */
#define CA_NI_L2FE_MC_FIB_DATA0	0x1640	/* L2FE_ARB_MC_FIB_TBL_DATA0 */
#define  CA_NI_L2FE_MC_FIB_LDPID	GENMASK(29, 24)	/* per-copy dest ldpid */
/* L2FE ARB non-known policer map and DSCP TE mark tables */
#define CA_NI_L2FE_NKPOL_MAP_ACCESS	0x1624	/* L2FE_ARB_NON_KNOWN_POL_MAP_TBL_ACCESS */
#define CA_NI_L2FE_NKPOL_MAP_DATA	0x1628	/* L2FE_ARB_NON_KNOWN_POL_MAP_TBL_DATA */
#define CA_NI_L2FE_DSCP_TE_ACCESS	0x162c	/* L2FE_ARB_DSCP_TE_MARK_TBL_ACCESS */
#define CA_NI_L2FE_DSCP_TE_DATA	0x1630	/* L2FE_ARB_DSCP_TE_MARK_TBL_DATA */
/*
 * Guaranteed CPU trap: DFT_FWD redirects UUC/DLF to a real MCE group
 * (mc_group_id 0 is the reserved null group and blackholes to 0x1f).  The
 * group has one member: mc_vec bit b selects ARB-FIB[b], whose copy ldpid
 * is DeepQ_0 -> PDPID = QM -> ES port 7 -> RMU -> CPU.
 */
#define CA_NI_RX_MCGID			0x20	/* our MCE group (non-empty; != reserved group 0) */
/* mc_vec bit = ARB-FIB member index (the one copy) */
#define CA_NI_RX_MC_FIB_BIT		0x10
#define CA_NI_NI_MCE_INDX_ACCESS	0xaa64	/* NI_HV_MCE_INDX_LKUP_ACCESS */
#define CA_NI_NI_MCE_INDX_DATA1	0xaa68	/* NI_HV_MCE_INDX_LKUP_DATA1 */
#define CA_NI_NI_MCE_INDX_DATA0	0xaa6c	/* NI_HV_MCE_INDX_LKUP_DATA0 */
#define CA_NI_RX_FLOOD_MCGID		1		/* the mcgid our DFT_FWD floods to */
/*
 * The LAN -> CPU chain with the multicast-group decode: the DFT_FWD 0x1832
 * target bits [10:1] = 0x19 are a 10-bit multicast group index.  A
 * broadcast/DLF frame replicates via MC group 0x19's member vector; a CPU
 * copy is only produced if the vector includes the CPU deep-buffer ldpid
 * 0x32 (PDPID_MAP[0x32] = 0x08 = QM/dbuf -> TM port 8 -> deep buffer -> RMU0
 * -> CPU).  The member vector is 64 bits with ldpid = bit position: 0x32 =
 * 50 -> high word bit 18 = 0x00040000.
 */
#define CA_NI_RX_DFT_FWD_MCGID		0x19	/* the MC group DFT_FWD 0x1832 replicates to */
/* CPU deep-buffer member ldpid (PDPID_MAP[0x32]=0x08) */
#define CA_NI_RX_MC_CPU_LDPID		0x32

/*
 * --- L3FE special-packet classifier.  ARP to CPU is an L3-CLS ethertype
 *     trap: a TCAM rule matches ethertype ARP -> action dest CPU_0 (ldpid
 *     0x10) with dpid_pri = 1 (wins over the FIB/flood destination).  The
 *     priority-7 rule beats the priority-0 broadcast -> L3_LAN (0x19) rule,
 *     so a broadcast ARP is trapped to the CPU.  Two indirect tables: KEY
 *     (11 words = match key + mask) and FIB (7 words = action). ---
 */
/*
 * L3FE stage-0 control: lpb_idx_mode = 0 (index the LPB/CLS by HDR_A.ldpid,
 * not lspid), use_lspid_ovwr_o_lspid = 0, mac_fltr_fail_pkt_mru_chk_en = 0.
 * At the reset value 0x001c7c7e the wrong lpb_idx_mode mis-indexes the STG0
 * LPB and CLS rules never match.
 */
#define CA_NI_L3FE_STG0_CTRL		0x3400
#define  CA_NI_L3FE_STG0_CTRL_VAL	0x001c787cu	/* reset value is 0x001c7c7e */
/* my-MAC/FIELD_CAM: LO = valid (bit16) | mac[0] << 8 | mac[1] */
#define CA_NI_L3FE_MY_MAC_LO		0x3210
#define  CA_NI_L3FE_MY_MAC_VALID	BIT(16)	/* 0x3210 bit16 = my-MAC entry valid */
#define CA_NI_L3FE_MY_MAC_HI		0x3214	/* HI = mac[2]<<24|mac[3]<<16|mac[4]<<8|mac[5] */
#define CA_NI_L3FE_SPCL_PKT_DET_CFG	0x3218	/* expected 0x0739DC24 (left as is) */
/*
 * L3FE ingress-loopback port-validity table (L3FE_GLB_ILPB_LDPID).  Without
 * it, a frame forwarded to the L3_LAN loopback ingress (ldpid 0x19) has no
 * valid L3FE ingress and never reaches the classifier.  Layout: valid0[0],
 * ldpid0[6:1], valid1[8], ldpid1[14:9], valid2[16], ldpid2[22:17],
 * valid3[23].  ldpid0 = L3_LAN (0x19), ldpid1 = L3_WAN (0x18) and entry 3
 * (ldpid3 = 0 = NI port 0) are marked valid.
 */
#define CA_NI_L3FE_GLB_ILPB_LDPID	0x3134
/*
 * L3FE_GLB config block.  Left at 0, the L3FE stage is uninitialised and
 * never ingests frames.  FWD_CTRL = forwarding control bitfields, LF_CFG =
 * ingress FIFO thresholds, ILPB = per-port VLAN loopback config.
 */
#define CA_NI_L3FE_GLB_FWD_CTRL_1	0x30a4
#define  CA_NI_L3FE_GLB_FWD_CTRL_1_VAL	0x80020000u	/* register name GLB_CFG */
#define CA_NI_L3FE_GLB_FWD_CTRL_2	0x30a8
#define  CA_NI_L3FE_GLB_FWD_CTRL_2_VAL	0x004641F4u
#define CA_NI_L3FE_GLB_LF_CFG		0x30b4	/* see the CLS monitor note below */
#define  CA_NI_L3FE_GLB_LF_CFG_VAL	0x4855CFE4u
#define CA_NI_L3FE_GLB_ILPB_00		0x30bc	/* see the CLS monitor note below */
#define  CA_NI_L3FE_GLB_ILPB_00_VAL	0x4856CF7Cu
#define CA_NI_L3FE_GLB_FWD_CTRL_3	0x30ac
#define  CA_NI_L3FE_GLB_FWD_CTRL_3_VAL	0x00000300u
#define CA_NI_L3FE_GLB_CFG_30CC		0x30cc	/* unnamed; expected 0xE21 */
#define  CA_NI_L3FE_GLB_CFG_30CC_VAL	0x00000E21u
/*
 * L3FE LDPID-remap family: 313c CTRL | 3140 SMAC_PRO | 3144 DMAC_PRO |
 * 3148 SIP_PRO | 314c DIP_PRO | 3150 SPORT_PRO | 3154 DPORT_PRO | 3158 VAL0 |
 * 315c VAL1.  The *_PRO registers select which fields take part in the
 * remap match, CTRL enables it and VAL0/VAL1 carry the values.  The macro
 * names below follow an older ELPB/DEEPQ naming.
 */
#define CA_NI_L3FE_GLB_ELPB0	0x313c
#define  CA_NI_L3FE_GLB_ELPB0_VAL	0x00007F03u
#define CA_NI_L3FE_GLB_ELPB_DEEPQ_VLD1	0x3140
#define  CA_NI_L3FE_GLB_ELPB_DEEPQ_VLD1_VAL 0x00C00000u
#define CA_NI_L3FE_GLB_ELPB_DEEPQ_VLD0	0x3144
#define  CA_NI_L3FE_GLB_ELPB_DEEPQ_VLD0_VAL 0x00400000u
#define CA_NI_L3FE_GLB_ELPB_DEEPQ1	0x3148
#define  CA_NI_L3FE_GLB_ELPB_DEEPQ1_VAL	0x00F00000u
#define CA_NI_L3FE_GLB_ELPB_DEEPQ0	0x314c
#define  CA_NI_L3FE_GLB_ELPB_DEEPQ0_VAL	0x00F00000u
#define CA_NI_L3FE_GLB_L3FE_L2FE_LDPID	0x3150
#define  CA_NI_L3FE_GLB_L3FE_L2FE_LDPID_VAL 0x000C0000u
#define CA_NI_L3FE_GLB_VE	0x3154
#define  CA_NI_L3FE_GLB_VE_VAL		0x00040000u
/* L3FE_GLB_LDPID_REMAP_VAL0/VAL1 */
#define CA_NI_L3FE_GLB_LDPID_REMAP_VAL0	0x3158
#define  CA_NI_L3FE_GLB_LDPID_REMAP_VAL0_VAL 0x04421088u
#define CA_NI_L3FE_GLB_LDPID_REMAP_VAL1	0x315c
#define  CA_NI_L3FE_GLB_LDPID_REMAP_VAL1_VAL 0x00002211u
/*
 * L3FE global debug / monitor block: two indirect read ports {index, data}
 * and a one-shot descriptor latch:
 *   0x30b0 CLS-monitor index: BIT(8) ENABLE | (vector << 5) | word; up to 32
 *          words
 *   0x30b4 CLS-monitor DATA (read-only)
 *   0x30b8 DBG index: (vector << 5) | word, vector 0..31; no enable bit
 *   0x30bc DBG DATA (read-only).  Vector 15 = four 10-bit per-stage packet
 *          counters {L3FE_IN, L3FE_OUT, T1_T2, STG3_PE}, which show whether
 *          a frame entered the engine.
 *   0x30c0 latch trigger: BIT(1) mode, BIT(0) an arm toggle (one capture per
 *          flip).  0x30c4 latch index (vector << 5 | word), 0x30c8 latch
 *          DATA.  Vector 2 = HDR_I before the packet editor, with all lookups
 *          resolved: the engine's key CRC32/CRC16, hash_idx, hash_profile,
 *          the CLS hit class and hash_dbl_chk_fail.
 * The unlatched taps read live pipeline shadow registers, so words of one
 * dump can come from different packets; use the latch to compare fields of
 * one frame.
 *
 * CA_NI_L3FE_GLB_LF_CFG (0x30b4) and CA_NI_L3FE_GLB_ILPB_00 (0x30bc) name
 * the same two addresses.  As read-data ports, the writes the RX init does
 * through those names have no effect.
 */
#define CA_NI_L3FE_CLS_MON_CTRL		0x30b0
#define  CA_NI_L3FE_CLS_MON_ENABLE	BIT(8)	/* not bit0 */
#define CA_NI_L3FE_MON_CLS_RESULT	3	/* l3fe_glb_monitor_vector_e: the
						 * CLS result, split across
						 * vectors 3 (123 B) + 4 (8 B)
						 */
/* GO|WR|idx; poll GO clear (CA_NI_IND_ACCESS_GO/WR) */
#define CA_NI_L3FE_CLS_KEY_ACCESS	0x3380
#define CA_NI_L3FE_CLS_KEY_DATA_BASE	0x3384	/* 11 words 0x3384..0x33ac */
#define CA_NI_L3FE_CLS_KEY_WORDS	11
#define CA_NI_L3FE_CLS_FIB_ACCESS	0x33b0
#define CA_NI_L3FE_CLS_FIB_DATA_BASE	0x33b4	/* 7 words 0x33b4..0x33cc */
#define CA_NI_L3FE_CLS_FIB_WORDS	7
#define CA_NI_RX_CLS_ENTRIES		16	/* KEY/FIB indices [0..15] */

/*
 * Datapath stage counters.  Read before and after traffic to find the first
 * stage that does not move: L2FE ingress -> L2FE drop -> L2FE to TM -> QM
 * RMU ingest -> QM drop -> CPU-EPP write pointer.
 */
#define CA_NI_L2FE_NI_INTF_DROP_CNT	0x11bc	/* frames NI->L2FE dropped   */
#define CA_NI_L2FE_NI_INTF_PKT_CNT	0x11c0	/* frames NI->L2FE (ingress) */
/*
 * L2FE post-parse HEADER_A of the last parsed frame
 * (L2FE_PP_HEADER_A_{LOW,MID,HI}):
 *   HI : cpu_flag b31, deep_q b30, policy_group_id[29:27], policy_id[26:18],
 *        policy_ena[17:16], marked b15, mirror b14, no_drop b13,
 *        rx_packet_type[12:11], drop_code[10:8], mc_group_id[7:0]
 *   MID: header_type[31:30], fe_bypass b29, packet_size[28:15],
 *        logic_spid[14:9], logic_dpid[8:3] (the resolved dest ldpid),
 *        cos[2:0]
 */
#define CA_NI_L2FE_PP_HEADER_A_LOW	0x11c4
#define CA_NI_L2FE_PP_HEADER_A_MID	0x11c8
#define CA_NI_L2FE_PP_HEADER_A_HI	0x11cc
#define CA_NI_L2FE_DOS_FLOOD_CNT	0x1234	/* DoS/flood drop            */
#define CA_NI_L2FE_PE_TM_PKT_CNT	0x1760	/* frames L2FE->TM (forwarded)*/
#define CA_NI_QM_RMU0_RX_PKT_CNTR	0x6900	/* frames into QM RMU (see CA_NI_QM_RX_CNTR) */
#define CA_NI_QM_RX_EOP_DROP_CNTR	0x6820	/* QM_QM_RX_EOP_DROP_CNTR */
#define CA_NI_QM_RX_LEN_ERR_CNTR	0x6824	/* QM_QM_RX_LEN_CHK_ERROR_CNTR */
#define CA_NI_QM_RX_L2TE_DROP_CNTR	0x6828	/* QM_QM_RX_L2TE_TAIL_DROP_CNTR */

/*
 * L2TM buffer-manager counters (0x21xx): RX = frames into the TM BM, TX =
 * frames out to the QM, NOBUF = dropped for lack of a buffer.
 */
#define CA_NI_L2TM_BM_RX_PCNT		0x213c	/* frames into L2TM BM (enqueue)  */
#define CA_NI_L2TM_BM_TX_PCNT		0x2140	/* frames out of L2TM BM->QM (egress) */
#define CA_NI_L2TM_BM_SB_DPCNT		0x2144	/* shared-buffer-full drop */
#define CA_NI_L2TM_BM_HDR_DPCNT		0x2148	/* header drop */
/* threshold-engine (deep-queue/VOQ/port thr) drop */
#define CA_NI_L2TM_BM_TE_DPCNT		0x214c
#define CA_NI_L2TM_BM_ERR_DPCNT		0x2150	/* error drop */
#define CA_NI_L2TM_BM_RX_DPCNT		0x2164	/* enqueue drop */
#define CA_NI_L2TM_BM_NOBUF_DPCNT	0x216c	/* no-free-buffer drop */
/*
 * QM per-VoQ non-empty status (VOQ_STATUS0..7): a set bit means that VoQ has
 * a queued descriptor.
 */
#define CA_NI_QM_VOQ_STATUS(n)	(0x6830 + (n) * 4)	/* QM_QM_VOQ_STATUS0 */
/*
 * destination routing maps: L2TM TM output -> CPU queue (0x76543210) and the
 * QM upper-LDPID map
 */
#define CA_NI_L2TM_TM_TO_CPUQ_MAP	0x2118
/*
 * L2TM TM egress map block (0x2100..0x2120): routes a BM-dequeued frame from
 * its TM port into the CPU queue RMU0 services.  U-Boot does not set these;
 * without them a non-deep CPU frame leaves the L2TM but never reaches RMU0.
 * Values: 0x2100 = 0x2, 0x2110 = 0xC0000000, 0x2114 = 0xFEDCBA98, 0x2118 =
 * 0x76543210, 0x211c = 0x76, 0x2120 = 0x76543210.
 */
#define CA_NI_L2TM_TM_CFG		0x2100
#define  CA_NI_L2TM_TM_CFG_VAL		0x00000002u
#define CA_NI_L2TM_TM_MAP_A		0x2110
#define  CA_NI_L2TM_TM_MAP_A_VAL	0xc0000000u
#define CA_NI_L2TM_TM_MAP_B		0x2114
#define  CA_NI_L2TM_TM_MAP_B_VAL	0xfedcba98u
#define  CA_NI_L2TM_TM_TO_CPUQ_VAL	0x76543210u	/* 0x2118 */
#define CA_NI_L2TM_TM_MAP_C		0x211c
#define  CA_NI_L2TM_TM_MAP_C_VAL	0x00000076u
#define CA_NI_L2TM_TM_MAP_D		0x2120
#define  CA_NI_L2TM_TM_MAP_D_VAL	0x76543210u
#define CA_NI_QM_UPPER_LDPID_MAP	0x68f8
#define CA_NI_PLE_TYPE_COUNT		4

/*
 * NI flow-control init: the L2TM BM dequeue -> TM-port map and per-port
 * flow-control thresholds.  U-Boot does not run it, so the BM dequeue map
 * (0x2124) is unset and a CPU-dest frame is dequeued to the wrong TM port
 * instead of the QM, with no drop counted.  0x88888888 sends every BM
 * dequeue to TM port 8 (QM).
 */
#define CA_NI_NI_FLOWCTRL_EN		0x3400		/* RMW |= bit19 (flow-ctrl enable) */
#define  CA_NI_NI_FLOWCTRL_EN_BIT	0x00080000u
/* BM dequeue LANL3FE-queue flag map (->L3QM/ES8) */
#define CA_NI_L2TM_BM_DQ_PORT_MAP	0x2124
#define  CA_NI_L2TM_BM_DQ_PORT_MAP_VAL	0x88888888u	/* all dequeues -> TM-port 8 = QM */
/*
 * The physical DQ -> TM-port number map, separate from the 0x2124 flag map:
 * tm_dpid_sel, 4 bits per entry, default identity 0x76543210 (DQ N -> TM
 * port N).  0x88888888 forces every DQ to TM port 8 (ES8/L3QM).  Set through
 * the dq_tmport_map module parameter.
 */
#define CA_NI_L2TM_BM_DQ_TO_TM_PORT_MAP	0x212c
#define CA_NI_NI_FLOWCTRL_THRESH	0x96b4	/* TE_CB_SP_BASE_BP_THRSH_P0 */
#define CA_NI_NI_FLOWCTRL_THRESH_CNT	7
#define  CA_NI_NI_FLOWCTRL_THRESH_CLR	0x3fff3fffu	/* fields [13:0] and [29:16] */
#define  CA_NI_NI_FLOWCTRL_THRESH_VAL	0x01a80178u	/* [0:13]=0x178 [16:29]=0x1a8     */

/* driver-side RX geometry */
#define CA_NI_RX_CPU_PORT		0	/* CPU port 0, voq 0 */
#define CA_NI_RX_VOQ			0
#define CA_NI_RX_PORT			0	/* ingress LAN port */
/*
 * total CPU-push pool = pool 0 + pool 1 + EQ8 RMU allocation pool.  EQ12 is
 * DDR-backed and populated by HW, so it is not part of this count.
 */
#define CA_NI_RX_POOL_SIZE		(CA_NI_RX_EQ_TOTAL_BUF + CA_NI_RX_EQ2_TOTAL_BUF + \
					 CA_NI_RX_EQ8_TOTAL_BUF)
#define CA_NI_RX_POOL_MIN		32	/* below this: warn (still usable) */
#define CA_NI_RX_SEED_BUDGET_MS		600	/* total seed wall-clock cap (1327 bufs) */
#define CA_NI_RX_NUM_IRQS		8	/* DT idx 0..7 = SPI 0x54..0x5b */

/*
 * NI -> L3QM ingress handoff and flow control.  If these are left unset,
 * the NIRX -> L3QM presentation is back-pressured and frames leave the L2TM
 * but never enter the QM.
 */
#define CA_NI_NI_FC_2914		0x2914		/* RMW |= bit31 */
#define  CA_NI_NI_FC_2914_EN		BIT(31)
/* full value: bit 31 plus the NI -> L3QM ingress flow-control thresholds 0x73FF */
#define  CA_NI_NI_FC_2914_STOCK		0x800073FFu
/*
 * RXMUX flow-control thresholds: 12 entries at 0xa098 + idx * 4, ending at
 * 0xa0c4 just before RX_P11_SEG_BUF_TOP_BOTTOM_ADDR_CFG (0xa0c8);
 * [9:0] = thr_lo, [19:10] = thr_hi.  idx 0-3 = 0x0002080A, idx 4-11 =
 * 0x0000A014.  The internal-port RX-mux calendar (INTPT_CALENDAR_CFG0/1)
 * and RXMUX_CTRL_CFG sit just below and must keep their own values.
 */
#define CA_NI_NI_RXMUX_FC_THR(idx)	(0xa098 + (idx) * 4)
#define CA_NI_NI_RXMUX_INTPT_CAL_CFG1	0xa07c
#define CA_NI_NI_RXMUX_INTPT_CAL_CFG0	0xa080
#define  CA_NI_NI_RXMUX_INTPT_CAL_VAL	0xe4e4e4e4u
#define CA_NI_NI_RXMUX_CTRL_CFG		0xa084
#define  CA_NI_NI_RXMUX_CTRL_CFG_VAL	0x700a09bau
#define  CA_NI_NI_RXMUX_FC_THR_LO_VAL	0x0002080Au	/* idx 0-3 */
#define  CA_NI_NI_RXMUX_FC_THR_HI_VAL	0x0000A014u	/* idx 4-11 */
#define  CA_NI_NI_RXMUX_FC_THR_LO_CNT	4
#define  CA_NI_NI_RXMUX_FC_THR_CNT	12
/*
 * NI-HV RXFIFO threshold block (RXFIFO_THRESHOLD_MISC_CFG, including
 * l3qm_rxfifo_hi[6:0]): 0xa1b0 = 0x22AA0000, 0xa1b4 = 0xAAAA0000, 0xa1b8 =
 * 0x00086FFC.  l3qm_rxfifo_hi = 0 back-pressures NIRX -> L3QM.
 */
#define CA_NI_NI_RXFIFO_THR_B0	0xa1b0
#define  CA_NI_NI_RXFIFO_THR_B0_VAL	0x22AA0000u
#define CA_NI_NI_RXFIFO_THR_B4	0xa1b4
#define  CA_NI_NI_RXFIFO_THR_B4_VAL	0xAAAA0000u
#define CA_NI_NI_RXFIFO_THR_B8	0xa1b8
#define  CA_NI_NI_RXFIFO_THR_B8_VAL	0x00086FFCu	/* [6:0]=0x7C = l3qm_rxfifo_hi */

#endif /* _CORTINA_NI_REGS_H */

/*
 * Port 4 SerDes: pon-glb registers (glb syscon) and the
 * SGMII LP block (CA_NI_WIN_SGMII_PCS).
 */
#define CA_NI_GLB_DPHY_RESET		0x0a0
#define  CA_NI_GLB_DPHY_RESET_SDS	(BIT(5) | BIT(8))
#define CA_NI_GLB_PHY_CONTROL		0x0c4
#define  CA_NI_GLB_PHY_CONTROL_SDS_MODE	GENMASK(26, 25)
#define  CA_NI_GLB_PHY_CONTROL_SGMII	BIT(25)
#define  CA_NI_GLB_PHY_CONTROL_HSGMII	BIT(26)
#define CA_NI_GLB_SGMII_PCS_CONTROL	0x0d4
#define  CA_NI_PCS_KEEP			(GENMASK(31, 29) | GENMASK(8, 7) | GENMASK(1, 0))
#define  CA_NI_PCS_HSGMII		(0x34200248u)
#define  CA_NI_PCS_SGMII		(0x0a800408u)
#define  CA_NI_PCS_SGMII_100M		(0x06800208u)
#define  CA_NI_PCS_SGMII_10M		(0x02800008u)
/*
 * SerDes analog control in the GPHY window, written once at power-on for the
 * SGMII PHY (PHY 7)
 */
#define CA_NI_GPHY_SDS_CTRL		0x01c
#define  CA_NI_GPHY_SDS_CTRL_VAL	0x5359
/*
 * SGMII LP registers: page 0 reg 2 (+0x0008) is in-band auto-negotiation,
 * the 1G lane block sits at 0x1200 and the 2.5G one at 0x1400
 */
#define CA_NI_SGMII_LP_AUTONEG		0x0008
#define  CA_NI_SGMII_LP_INBAND_DISABLE	0x71d0
#define CA_NI_SGMII_LANE_1G		0x1200
#define CA_NI_SGMII_LANE_2G5		0x1400
