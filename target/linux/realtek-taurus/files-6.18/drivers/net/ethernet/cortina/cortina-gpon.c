// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015 Cortina-Access Limited
 * Copyright (C) BRULE Herman <alpha_one_x86@first-world.info>
 *
 * Cortina-Access GPON MAC driver for the Realtek RTL9607DQ ONU.
 *
 * The GPON MAC is a Cortina-Access IP block in the PON register window at
 * 0x4_f5500000 + 0x6000; its vendor-id register reads the ASCII serial
 * prefix. The G.984.3 activation state machine (O1..O5) runs in hardware;
 * this driver brings up the PON SerDes, optics and laser, binds the OMCC and
 * data GEM/T-CONT paths, carries OMCI between the MAC and the shared G.988
 * responder (drivers/net/gpon) or the xPON framework, and exposes status
 * and controls through /proc/gpon.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/crc32.h>
#include <linux/ctype.h>
#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/ethtool.h>
#include <linux/hwmon.h>
#include <linux/leds.h>
#include <linux/linkmode.h>
#include <linux/etherdevice.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_net.h>
#include <linux/platform_device.h>
#include <linux/ratelimit.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#include <linux/gpio/consumer.h>
#include <net/xpon.h>
#include "cortina-epon.h"
#include "cortina-gpon-serdes.h"
#include "cortina-gpon-bosa.h"
#include "cortina-gpon-ddm.h"
#include "cortina-ni.h"
#include "cortina-xpon.h"

#include "gpon_omci_core.h"
#include "gpon_omci_me.h"
#include "gpon_gem_us.h"

#define DRV_NAME		"cortina-gpon"

/* PON register window: phys 0x4_f5500000, 48 KiB. */
#define CG_PON_WINDOW_PHYS	0x4f5500000ULL
#define CG_PON_WINDOW_SIZE	0xc000

/* The GPON MAC register block sits at window + 0x6000. */
#define CG_GPON_MAC_OFF		0x6000

/*
 * PON SerDes (PSDS) registers, direct within the PON window.
 *   PSDS_MODE (+0xa02c): SerDes rate/mode. GPON = 0x408 (sd_s0=1, sds_mode_s0=0x8).
 *   PSDS_RGB8 (+0xa060): SerDes status. bit10 CKRDY_RX, bit11 CKRDY_TX (TX PLL locked to the reference clock; asserts without fiber), bit0 RX_LOS.
 */
#define CG_PSDS_MODE		0xa02c
/* DS lock status; locked = (val & 0x9c01) == 0x9c00 */
#define CG_PSDS_RGB8		0xa060
/* RX/TX bit ordering [7:4]; vendor value 0x454 */
#define CG_PSDS_GBOX_CTRL	0xa064
#define CG_PON_EPON_SPARE	0x01c8	/* EPON_GLB_SPARE_CFG (PON window); bit31 for GPON los-rst */
/*
 * PSDS internal analog register indirect interface, used to load the
 * CMU/PLL/CDR/TX profile. Command word: bit31 = strobe, bit30 = write (else
 * read), bits[11:0] = internal register index. The CMU/PLL relock
 * (cg_psds_relock) strobes CG_PSDS_CMU_IDX bits[7:4].
 */
#define CG_PSDS_IND_CMD		0xa0cc
#define CG_PSDS_IND_WDATA	0xa0d0
#define CG_PSDS_IND_RDATA	0xa0d4
#define CG_PSDS_IND_READ	0x80000000u	/* command: strobe, read */
#define CG_PSDS_IND_WRITE	0xc0000000u	/* command: strobe, write */
#define CG_PSDS_CMU_IDX		0x400		/* analog CMU reg; [7:4] = re-lock strobe */

/*
 * GLB (global) PON/GPON reset and clock control window: phys 0x4_f4320000, 4
 * KiB. The GPON MAC reads garbage until released from reset here. Released
 * values: EPON_CNTL = 0x00030000, GPON_CNTL = 0x00000003, PON_CNTL =
 * 0x0000030e.
 * GPON_CNTL bits: ani_rst_n[0], gpon_rst_n[1]. PON_CNTL bits:
 * pon_serdes_rst_n[1], psds_reg_rst_n[2], ptp_rst_n[3], puc_reset[8],
 * pdc_reset[9].
 */
#define CG_GLB_WINDOW_PHYS	0x4f4320000ULL
#define CG_GLB_WINDOW_SIZE	0x1000
/*
 * PON reset/mode register offsets on the RTL9607DQ. With wrong offsets the MAC
 * is never released from reset and its whole window reads one constant.
 */
#define CG_GLB_EPON_CNTL	0x074
#define CG_GLB_GPON_CNTL	0x07c
#define CG_GLB_PON_CNTL		0x094
/*
 * PON interrupt aggregation, level 1 of 2 (GLB window): the GPON MAC int_top
 * output feeds GLOBAL_PON_INTERRUPT_0.PON_MACi (bit 0), enabled by
 * GLOBAL_PON_INTENABLE_0.PON_MACe. The ISR masks this bit while servicing.
 */
#define CG_GLB_PON_INT0		0x180	/* GLOBAL_PON_INTERRUPT_0 */
#define CG_GLB_PON_INTEN0	0x184	/* GLOBAL_PON_INTENABLE_0 */
#define CG_PON_INT0_PON_MAC	BIT(0)	/* PON_MACi/e */
/*
 * PON interrupt aggregation, level 2 of 2: the NE sub-interrupt controller
 * "ne_ictl" at GLB+0x164 (+0 STATUS, +4 ENABLE), whose output is GIC SPI 1. The
 * PON aggregate is line 5. There is no irqchip driver for it, so this driver
 * enables/acks line 5 itself and requests SPI 1 directly (IRQF_SHARED).
 */
#define CG_GLB_NE_ICTL_STS	0x164	/* GLOBAL_NETWORK_ENGINE_INTERRUPT_0 */
#define CG_GLB_NE_ICTL_EN	0x168	/* GLOBAL_NETWORK_ENGINE_INTENABLE_0 */
#define CG_NE_ICTL_PON_LINE	BIT(5)	/* PON = ne_ictl line 5 */
/*
 * GLOBAL_PSDS_INIT_CNTL: bit5 POW_PCIX powers the PON SerDes analog and digital
 * logic, which clocks the PON APB bus the GPON MAC is on. bit4 ben_oen is the
 * laser burst enable (left 0 during bring-up). Writing to a wrong offset here
 * hangs the bus.
 */
#define CG_GLB_PSDS_INIT	0x22c
#define CG_PSDS_POW_PCIX	BIT(5)
#define CG_PSDS_BEN_OEN		BIT(4)

/*
 * Laser TX-disable GPIO. The GN25L95 TX_DIS input is on a net that GPIO pin 34
 * (group 1, bit 2) only mirrors as an input; the net is pulled low (laser
 * enabled) by a GPIO group 0 pin route and drive, see CG_PERGPIO_CFG0 below.
 * Read-modify-write only, never whole-register writes on the mux:
 *   GLOBAL_GPIO_MUX_1 (GLB  +0x134): set the bit -> pin is a GPIO
 *   PER_GPIO1_CFG    (PERI +0x324): 1 = input
 *   PER_GPIO1_IN     (PERI +0x32c): bit2 = net level, 0 = laser on
 * The PER_GPIO block is a separate MMIO window (0x4_f4329000).
 */
#define CG_PERGPIO_PHYS		0x4f4329000ULL
#define CG_PERGPIO_SIZE		0x1000
#define CG_GLB_GPIO_MUX1	0x134
#define CG_PERGPIO_CFG1		0x324
#define CG_PERGPIO_OUT1		0x328
#define CG_PERGPIO_IN1		0x32c
#define CG_LASER_PIN34		BIT(2)
/*
 * Laser enable path: a GPIO group 0 pin route and drive pulls the TX-disable
 * net low. Vendor values at O5:
 *   GLB +0x42c (pin route) = 0x01101101, bit20 routes the laser enable net;
 *   PER_GPIO0_CFG (+0x300) = 0xffffe7bf: pins 6, 11, 12 are outputs;
 *   PER_GPIO0_OUT (+0x304) = 0x00000040: pin 6 high, pins 11 and 12 low;
 *   PER_GPIO1_IN (+0x32c) bit2 (pin 34) then reads 0 = TX disable released. Otherwise GN25L95 register 0x6e bit7 stays set and the laser is dark.
 */
#define CG_PERGPIO_CFG0		0x300
#define CG_PERGPIO_OUT0		0x304
#define CG_GLB_GPIO_MUX0	0x130	/* vendor 0x00001fff: pins 0-12 are GPIO */
#define CG_GLB_GPIO_MUX3	0x13c	/* vendor 0x000390ff: pins 96-103,108,111-113 */
#define CG_GLB_GPIO_MUX4	0x140	/* vendor 0x00003b00: pins 136,137,139-141 */
#define CG_GLB_PINROUTE		0x42c	/* vendor 0x01101101; bit20 = laser net */
#define CG_PINROUTE_LASER	BIT(20)
#define CG_GPIO0_LASER_PINS	(BIT(6) | BIT(11) | BIT(12))
#define CG_GPIO0_PIN6		BIT(6)
/*
 * GPIO groups 3 and 4 (PERI +0x36c/+0x370, +0x390/+0x394), driven as the vendor
 * firmware drives them at O5; the laser enable / BOSA control also depends on
 * one of these pins:
 *   grp3 cfg 0xfffdef00 (outputs: pins 96-103, 108, 113), out 0x00021010 (pins 100, 108, 113 high);
 *   grp4 cfg 0xffffc5ff (outputs: pins 137, 139-141), out 0x00003200 (pins 137, 140, 141 high).
 */
#define CG_PERGPIO_CFG3		0x36c
#define CG_PERGPIO_OUT3		0x370
#define CG_PERGPIO_CFG4		0x390
#define CG_PERGPIO_OUT4		0x394
#define CG_GPIO3_CFG_STOCK	0xfffdef00
#define CG_GPIO3_OUT_STOCK	0x00021010
#define CG_GPIO4_CFG_STOCK	0xffffc5ff
#define CG_GPIO4_OUT_STOCK	0x00003200

/*
 * GPON MAC register offsets within the block.
 *
 * The silicon has an extra 0x20-byte AES key bank after offset 0x4c (two 8-word
 * key banks at 0x30-0x6c), so offsets below are the silicon ones, not those of
 * the older Cortina register header.
 */
#define CG_REG_GPON_DS		0x000	/* DS framer thresholds; max_packet_size low */
#define CG_REG_US		0x00c	/* us: frame_var[8:0], eqd_select[16] */
#define CG_REG_SIGNAL		0x010	/* SF/SD BER alarm thresholds */
#define CG_REG_VENDOR		0x014	/* vendor-id (ASCII "XPON") */
#define CG_REG_VENDOR_SPEC	0x018	/* vendor-specific serial number */
/*
 * The 10-byte GPON registration password, in wire order, split 2 + 4 + 4 over
 * three registers (PFRAG0 holds the first two bytes in its low half). Some OLTs
 * drop a ranging ONU that presents no password.
 */
#define CG_REG_PFRAG0		0x01c
#define CG_REG_PFRAG1		0x020
#define CG_REG_PFRAG2		0x024
#define CG_REG_ALARM		0x07c	/* LOS/LOF alarm bits (live levels) */

/*
 * Interrupt block. int_top is read-to-clear; the four per-group status
 * registers are W1C. int_top bits[3:0] = {INTERRUPT, INTERRUPT2, INTERRUPT3,
 * INTERRUPT4}.
 */
#define CG_REG_INT_TOP		0x084	/* interrupt_top (read-clear) */
#define CG_REG_INT_TOP_EN	0x088	/* int_top_en */
#define CG_REG_INT		0x08c	/* INTERRUPT  status (W1C) - operational */
#define CG_REG_INT_EN		0x090	/* INTERRUPT  enable */
#define CG_REG_INT2		0x094	/* INTERRUPT2 status (W1C) - TC/parse err */
#define CG_REG_INT2_EN		0x098	/* INTERRUPT2 enable */
#define CG_REG_INT3		0x09c	/* INTERRUPT3 status (W1C) - negedge/MSB */
#define CG_REG_INT3_EN		0x0a0	/* INTERRUPT3 enable */
#define CG_REG_INT4		0x0a4	/* INTERRUPT4 status (W1C) - FEC MSB */
#define CG_REG_INT4_EN		0x0a8	/* INTERRUPT4 enable */

#define CG_INT_TOP_EN_ALL	0xF		/* vendor GPON_MAC_GPON_INT_TOP_ENA_DEF */
/*
 * Vendor GPON_MAC_GPON_INT_ENA_DEF 0xC00AFFFF plus bit 27, the downstream PLOAM
 * received interrupt used to recompute us.frame_var after
 * Extended_Burst_Length.
 */
#define CG_INT_EN_DEFAULT	0xC80AFFFF	/* int2/3/4 enables = 0 */

/* INTERRUPT source bits we service (alarm bits 0..15 are event/diag) */
#define CG_INT_ONU_ST_CHG	BIT(31)	/* ONU activation-FSM state changed */
#define CG_INT_ONU_ID		BIT(30)	/* Assign_ONU-ID accepted -> bind OMCC T-CONT */
#define CG_INT_PLOAMD		BIT(27)	/* DS PLOAM msg received (recheck frame_var) */
#define CG_INT_KSW		BIT(19)	/* Key_Switching_Time (AES rekey; next phase) */
#define CG_INT_PORTID		BIT(17)	/* Configure_Port-ID -> omci_port valid, bind OMCC GEM */
#define CG_INT_DS_FEC_OFF	BIT(15)	/* the OLT stopped downstream FEC */
#define CG_INT_DS_FEC_ON	BIT(14)	/* the OLT started downstream FEC */
#define CG_INT_DACT		BIT(8)	/* Deactivate_ONU-ID */

#define CG_REG_GPON_ONU		0x0bc	/* ONU id[7:0], state[18:16]; dft id=0xff */
#define CG_ONU_ID(v)		((v) & 0xff)
#define CG_ONU_STATE(v)		(((v) >> 16) & 0x7)
#define CG_ONU_ID_NONE		0xff	/* reset default = unassigned */
/*
 * onu.state encoding: 0=O1 Initial, 1=O2 Standby, 2=O3 SerialNumber, 3=O4
 * Ranging, 4=O5 Operation, 5=O6 POPUP, 6=O7 EmrgStop.
 */
#define CG_STATE_INITIAL	0
#define CG_STATE_RANGING	3
#define CG_STATE_OPERATION	4
#define CG_STATE_POPUP		5
#define CG_STATE_ESTOP		6

#define CG_REG_GPON_MAIN	0x0c0	/* equalization delay (EqD) */
#define CG_REG_OMCI_PORT	0x0c8	/* omci_port id[11:0], en[12]; HW-filled */
#define CG_OMCI_PORT_ID(v)	((v) & 0xfff)
#define CG_OMCI_PORT_EN		BIT(12)
/*
 * extend[16], ranged[15:8], pre_range[7:0]; latched by hardware from the
 * OLT's Extended_Burst_Length PLOAM
 */
#define CG_REG_T3_PREAMBLE	0x0d8

/*
 * Indirect table access pairs. Protocol: write ACCESS = go(bit31) | rbw(bit30,
 * 1 = write) | index/alloc-id, then poll for bit31 to self-clear (<= 10000
 * reads). Data goes through the DATA register.
 */
#define CG_REG_TCONT_ACCESS	0x12c	/* alloc_id[11:0], sw_plm_en[16], rbw[30], go[31] */
#define CG_REG_TCONT_DATA	0x130	/* ploam_en[0], omci_en[1], index[6:2] (hw T-CONT 0-31) */
#define CG_REG_DS_GEM_ACCESS	0x134	/* id[11:0] (GEM port-id), sw_aes[16], rbw[30], go[31] */
#define CG_REG_DS_GEM_DATA	0x138	/* vld[0], aes[1], tdm[2], index[10:3] (intern gem) */
#define CG_REG_US_PORT_ACCESS	0x170	/* index[7:0] (us hw gem 0-255), rbw[30], go[31] */
#define CG_REG_US_PORT_DATA	0x174	/* id[11:0] (GEM port-id) */
/*
 * Last slot of the upstream port-map array (index[7:0]); checked by the
 * GPON_GEM_US_RANGE_OK() assertions on the slot ranges below.
 */
#define CG_US_PORT_IDX_MAX	255

/*
 * Per upstream GEM MIB behind an indirect ACCESS/DATA pair, counting what the
 * framer actually transmitted. It must be driven through ACCESS; the DATA
 * registers only hold whatever the last access latched.
 */
#define CG_REG_US_MIB_ACCESS	0x150	/* sel[7:0], op_code[29:28], rbw[30], go[31] */
/*
 * op_code[29:28] selects the read operation; with 0 the counters read back as
 * zero. A read-clear op returns the count since the last read.
 */
#define CG_US_MIB_OP_CODE	GENMASK(29, 28)

/*
 * Downstream MIB, same access shape (DS_PORT_ACCESS/DATA3..0 at 0x13c-0x14c).
 * Since downstream traffic always flows, it shows whether the MIB interface is
 * being driven correctly when the upstream one reads zero.
 */
/*
 * Per T-CONT upstream buffer occupancy, maintained by hardware (bufocc_mode =
 * 0) even with DBRu disabled. It is a level, not a total: non-zero means frames
 * reached the PUC and wait for a grant; zero while sending means they never
 * arrived.
 */
#define CG_REG_DBRU_BUFOCC_ACCESS	0x1bc	/* Addr[4:0], Select[16], rw[30], access[31] */
#define CG_REG_DBRU_BUFOCC_DATA		0x1c0	/* BufOcc[15:0] */

#define CG_REG_DS_MIB_ACCESS	0x13c
#define CG_REG_DS_MIB_DATA3	0x140
#define CG_REG_DS_MIB_DATA2	0x144
#define CG_REG_DS_MIB_DATA1	0x148
#define CG_REG_DS_MIB_DATA0	0x14c
#define CG_REG_US_MIB_DATA3	0x154	/* bcnt hi */
#define CG_REG_US_MIB_DATA2	0x158	/* bcnt lo */
#define CG_REG_US_MIB_DATA1	0x15c	/* fcnt - GEM frames transmitted */
#define CG_REG_US_MIB_DATA0	0x160	/* pcnt - packets transmitted */

#define CG_DS_GEM_VLD		BIT(0)
#define CG_DS_GEM_INDEX(x)	(((x) & 0xff) << 3)

#define CG_TBL_GO		BIT(31)
#define CG_TBL_WR		BIT(30)

#define CG_TCONT_PLOAM_EN	BIT(0)
#define CG_TCONT_OMCI_EN	BIT(1)
#define CG_TCONT_INDEX(x)	(((x) & 0x1f) << 2)
#define CG_TCONT_INDEX_MASK	(0x1f << 2)

#define CG_OMCC_US_GEM_IDX_NUM	8	/* vendor AAL_GPON_OMCI_RSV_PORT_MAX: us hw gems 0..7 = OMCC */

/*
 * WAN data path: up to CG_DATA_TCONT_MAX data T-CONTs and CG_DATA_GEM_MAX GEM
 * ports, plus the downstream broadcast GEM.
 *
 * hw T-CONT 0 = OMCC, data T-CONT t = hw T-CONT t + 1. In the PUC's 8-queue
 * VoQ map (VoQID = {HdrA.ldpid[3:0], HdrA.cos[2:0]}) hw T-CONT h owns VoQ
 * h * 8 .. h * 8 + 7. An upstream GEM on queue q of hw T-CONT h uses VoQ,
 * upstream port-map slot and HEADER_A pol_id h * 8 + q: the CPU injects with
 * ldpid = 0x20 + h, cos = q and pol_id = that slot (see cortina-ni-regs.h),
 * and the upstream engine stamps US_PORT_ID[slot] onto the burst. A
 * bidirectional GEM's downstream index is the same number; downstream-only
 * GEMs and the broadcast GEM sit above the upstream range, as the vendor's
 * dsFlow indices (flow + 8) do.
 */
#define CG_DATA_TCONT_MAX	CX_DATA_TCONT_MAX
#define CG_DATA_GEM_MAX		CX_DATA_GEM_MAX
#define CG_DATA_GEM_IDX		8	/* first data slot = hw T-CONT 1, queue 0 */
#define CG_MCAST_GEM_IDX	127	/* DS-only broadcast GEM */
#define CG_DS_ONLY_IDX(n)	(128 + (n))
#define CG_GEM_DIR_US		1	/* ME 268 attribute 3 */
#define CG_GEM_DIR_DS		2
#define CG_GEM_DIR_BIDIR	3
#define CG_MCAST_GEM_ID		4095	/* G.984 broadcast GEM port-id */

/*
 * PDC (packet downstream classifier) sub-block at PON window + 0x9000. It maps
 * each downstream GEM (by internal index) to a logical destination port;
 * without it downstream OMCI never reaches the CPU. Map entries 0..7 (OMCC
 * GEMs) go to CPU port 0 with fe_bypass, no_drop and cos 6, entries 8..255
 * (data GEMs) to L3_WAN; PDC_CTRL then enables the map memory and the OMCI
 * high-priority override (cos 7 -> CPU_0).
 *
 * PDC_MAP uses the same go/rbw/poll indirect protocol; data through
 * DATA0/DATA1.
 */
#define CG_PDC_CTRL		0x9014	/* dft 0x2 (pdc_map_mem_en) */
#define CG_PDC_CTRL_MAP_MEM_EN	BIT(1)
#define CG_PDC_CTRL_HP_COS_SH	16	/* omci_hp_cos[18:16] */
#define CG_PDC_CTRL_HP_LDPID_SH	19	/* omci_hp_ldpid[24:19] */
#define CG_PDC_CTRL_HP_EN	BIT(25)	/* omci_hp_en */
#define CG_PDC_CTRL_HP_MASK	GENMASK(25, 16)
#define CG_PDC_MAP_ACCESS	0x9020	/* address[7:0], rbw[30], go[31] */
#define CG_PDC_MAP_DATA1	0x9024	/* pol_en[3:2], pol_id[12:4], pol_grp_id[15:13], deepq[16] */
#define CG_PDC_MAP_DATA0	0x9028	/* cos[2:0], ldpid[8:3], lspid[14:9], fe_bypass[15], no_drop[31] */
#define CG_PDC_MAP_ENTRIES	256	/* vendor AAL_PDC_MAP_ENTRY_NUM */
#define CG_PDC_D1_POL_ID(x)	(((x) & 0x1ff) << 4)
#define CG_PDC_D0_COS(x)	((x) & 0x7)
#define CG_PDC_D0_LDPID(x)	(((x) & 0x3f) << 3)
#define CG_PDC_D0_LSPID(x)	(((x) & 0x3f) << 9)
#define CG_PDC_D0_FE_BYPASS	BIT(15)
#define CG_PDC_D0_NO_DROP	BIT(31)
/* logical port IDs */
#define CG_LPORT_CPU_0		0x10	/* CPU port 0 = the NI CPU-RX EPP port we drain */
#define CG_LPORT_L3_WAN		0x18
#define CG_LPORT_PON		0x07

/*
 * PUC (PON upstream classifier) sub-block at PON window + 0x8000, the admission
 * stage between the NI DMA-LSO egress and the GEM upstream engine. Upstream
 * OMCI from the CPU arrives with HEADER_A ldpid = PON(7) + 8, i.e. 8Q VoQID
 * 127. At reset defaults the OMCC VoQs are not mapped, valid or GEM/cos
 * stamped, so frames are silently dropped; this mirrors the vendor PUC init for
 * GPON.
 *
 * Indirect tables use the go/rbw/poll protocol: PVTBL (per T-CONT VoQ map, 5
 * data words) and VOQBPREMAP (per VoQ back-pressure remap).
 */
#define CG_PUC_BASE		0x8000	/* PON window + 0x8000 */
#define CG_PUC_PVTBL_ACCESS	(CG_PUC_BASE + 0x000)	/* addr[5:0]=T-CONT, rbw[30], go[31] */
#define CG_PUC_PVTBL_DATA4	(CG_PUC_BASE + 0x004)
#define CG_PUC_PVTBL_DATA3	(CG_PUC_BASE + 0x008)
#define CG_PUC_PVTBL_DATA2	(CG_PUC_BASE + 0x00c)	/* voq7[7:0], schmode[8], entryvld[12], wrr0/1 */
#define CG_PUC_PVTBL_DATA1	(CG_PUC_BASE + 0x010)	/* voq3[3:0],voq4,voq5,voq6,voq7[31] */
#define CG_PUC_PVTBL_DATA0	(CG_PUC_BASE + 0x014)	/* voq0,voq1,voq2,voq3[31:27] */
#define CG_PUC_VOQSCHECFG	(CG_PUC_BASE + 0x048)	/* sche_mode[1:0]: WRR credit scale */
#define CG_PUC_VOQMAPCFG	(CG_PUC_BASE + 0x04c)	/* voqmapsel[1:0]: 0 = 8Q mode */
/*
 * Per T-CONT WRR credit table (vendor port_voq_sche_info_tbl): eight 20-bit
 * credits, VoQ 0 in the low bits of DATA0, packed through DATA4.
 */
#define CG_PUC_SCHEINFO_ACCESS	(CG_PUC_BASE + 0x060)	/* addr[4:0]=T-CONT */
#define CG_PUC_SCHEINFO_DATA(n)	(CG_PUC_BASE + 0x074 - (n) * 4)
#define CG_PUC_BTCCFG		(CG_PUC_BASE + 0x050)
#define CG_PUC_PUCCFG		(CG_PUC_BASE + 0x08c)	/* dft 0x84040001 */
#define CG_PUC_VOQBUFLIMSEL0	(CG_PUC_BASE + 0x090)	/* 16 regs, stride 4 (0x090..0x0cc) */
#define CG_PUC_VOQBUFLIMSEL_N	16
#define CG_PUC_VOQBUFLIMIT_A	(CG_PUC_BASE + 0x0d0)
#define CG_PUC_VOQBUFLIMIT_B	(CG_PUC_BASE + 0x0d4)
#define CG_PUC_VOQBUFLIMIT_C	(CG_PUC_BASE + 0x0d8)
#define CG_PUC_BPCNTL		(CG_PUC_BASE + 0x0e4)	/* bpen[0], dropen[4], bpth[30:16] */
#define CG_PUC_VOQBPREMAP_ACCESS (CG_PUC_BASE + 0x0e8)	/* addr[7:0]=VoQ, rbw[30], go[31] */
#define CG_PUC_VOQBPREMAP_DATA	(CG_PUC_BASE + 0x0ec)	/* tqmvoqid[7:0] */
#define CG_PUC_PONCNTL_INTEN	(CG_PUC_BASE + 0x0f4)
#define CG_PUC_CTRL		(CG_PUC_BASE + 0x13c)	/* dft 0x3300007c; shp_en[30], rl_en[26] */
#define CG_PUC_CTRL1		(CG_PUC_BASE + 0x140)	/* rlovhd[4:0], shpovhd[9:5], agrshpovhd[14:10] */
#define CG_PUC_CTRL2		(CG_PUC_BASE + 0x144)	/* dft 0x03000000; pirovhd[4:0], pir_en[26] */
/*
 * PUC QM packet length memory control. bit3 cfg_qmplmem_en is the global PUC QM
 * enable, set once before any queue is added. With the QM disabled, reading
 * REPORT_ENABLE0 aborts and enabling report causes an SError.
 */
#define CG_PUC_QM_PLEN_MEM_CTL	(CG_PUC_BASE + 0x274)	/* cfg_qmplmem_sel[2:0] en[3] lch[4] */
/* voqid[7:0], tcontid[12:8], openpktflushen[16], start[31] */
#define CG_PUC_VOQFLUSH		(CG_PUC_BASE + 0x0dc)
#define CG_PUC_VALID_VOQ0	(CG_PUC_BASE + 0x1bc)	/* valid_voqN = VALID_VOQ0 - (voq/32)*4 */
/*
 * Per VoQ token-bucket indirect memory. DATA1: bs[5:0] enb[7:6] mode[8]
 * tbc[29:9] pkt_mode_class_sel[30]; DATA0: rate_k[9:0] rate_m[25:10] bs[31:26].
 * Both are written per VoQ before REPORT_ENABLE.
 */
#define CG_PUC_VOQ_TBC_ACCESS	(CG_PUC_BASE + 0x1c4)	/* CIR bucket: addr[7:0]=VoQ */
#define CG_PUC_VOQ_TBC_DATA1	(CG_PUC_BASE + 0x1c8)
#define CG_PUC_VOQ_TBC_DATA0	(CG_PUC_BASE + 0x1cc)
#define CG_PUC_RL_VOQ_TBC_ACCESS (CG_PUC_BASE + 0x1d0)	/* PIR (rate-limit) bucket */
#define CG_PUC_RL_VOQ_TBC_DATA1	(CG_PUC_BASE + 0x1d4)
#define CG_PUC_RL_VOQ_TBC_DATA0	(CG_PUC_BASE + 0x1d8)
#define CG_PUC_Q2PQSRCFG01	(CG_PUC_BASE + 0x230)	/* qm_rpt_lv0[15:0], lv1[31:16] */
#define CG_PUC_Q2PQSRCFG23	(CG_PUC_BASE + 0x234)	/* qm_rpt_lv2[15:0], lv3[31:16] */
#define CG_PUC_BMC_RX_PKT	(CG_PUC_BASE + 0x17c)	/* US frames received by the PUC */
#define CG_PUC_BMC_RX_PKT_ENQ	(CG_PUC_BASE + 0x180)	/* US frames enqueued to a VoQ */
#define CG_PUC_BMC_FORCE_DROP	(CG_PUC_BASE + 0x184)	/* US frames dropped (invalid VoQ) */
/* gemid[7:0], cos[10:8], tcont[21:16], datapkt[30], en[31] */
#define CG_PUC_US_OMCI_HDR_A	(CG_PUC_BASE + 0x160)
#define CG_PUC_US_OMCI_HP_HDR_A	(CG_PUC_BASE + 0x164)	/* gemid[7:0],cos[10:8],tcont[21:16] */
#define CG_US_OMCI_HDR_A_VAL		0x80000606
#define CG_US_OMCI_HP_HDR_A_VAL		0x00000707	/* vendor value */
#define CG_PUC_GLOBAL_PLOAM_CFG	(CG_PUC_BASE + 0x168)	/* us_hdr_min_size[21:16], us_ext_omci_en[31] */

/*
 * The PUC control packet classifier and its two counters, which unlike the
 * other PUC counters do not count upstream user data.
 *
 * Every upstream control frame carries the 16-byte PON control header the NI TX
 * path adds (cortina_ni_pon_hdr): a fixed DA/SA pair, then a 16-bit type. The
 * PUC matches against:
 *
 *   GLOBAL_DA_SA2/1/0 = 00:13:25:00:00:00 / 00:13:25:00:00:01 (DA then SA, big-endian across the three words)
 *   GLOBAL_LNK_TYPE   = 0xfff1, the OMCI type the NI puts on upstream OMCI
 *   GLOBAL_MAC_TYPE   = 0xfff0, the companion MAC control type
 *   BMC_CONTROL_PKT_CNTR_lnk / _mac = the matching frame counts
 *   BMC_LENGTH_ERROR                = upstream frames rejected on length
 *
 * So _lnk counts upstream OMCI frames, while BMC_RX_PKT counts everything.
 * BMC_RX_PKT/_ENQ are 32-bit; FORCE_DROP, LENGTH_ERROR and both control
 * counters are 16-bit with a reserved upper half that is masked off.
 *
 * All are clear-on-read (PUCCFG.inccfg = 2), so only cg_puc_ctrl_sample() reads
 * the control packet counters. Whether the type match also covers the two
 * header bytes after the type is not known; a zero _lnk count is therefore not
 * conclusive.
 */
#define CG_PUC_GLOBAL_DA_SA2	(CG_PUC_BASE + 0x14c)	/* DA[0..3] */
#define CG_PUC_GLOBAL_DA_SA1	(CG_PUC_BASE + 0x150)	/* DA[4..5], SA[0..1] */
#define CG_PUC_GLOBAL_DA_SA0	(CG_PUC_BASE + 0x154)	/* SA[2..5] */
#define CG_PUC_GLOBAL_MAC_TYPE	(CG_PUC_BASE + 0x158)	/* type:32, dft 0xfff00000 */
#define CG_PUC_GLOBAL_LNK_TYPE	(CG_PUC_BASE + 0x15c)	/* type:32, dft 0xfff10000 (OMCI) */
#define CG_PUC_BMC_CTRL_PKT_MAC	(CG_PUC_BASE + 0x174)	/* cntr:16, MAC-type control frames */
#define CG_PUC_BMC_CTRL_PKT_LNK	(CG_PUC_BASE + 0x178)	/* cntr:16, OMCI-type control frames */
#define CG_PUC_BMC_LENGTH_ERROR	(CG_PUC_BASE + 0x188)	/* cntr:16, US length-check rejects */
#define CG_PUC_BMC_CNTR_MASK	0xffff	/* the cntr:16 fields' reserved upper half */
/*
 * The PUC's view of the last upstream control frame header. For OMCI the vendor
 * firmware shows hdr0 = 0x00203108 and hdr1 = 0x40200000; hdr_type 3 in hdr0
 * means the L2FE re-stamped the frame.
 */
#define CG_PUC_BMC_PKT_HDR1	(CG_PUC_BASE + 0x194)	/* expected 0x40200000 */
#define CG_PUC_BMC_PKT_HDR0	(CG_PUC_BASE + 0x198)	/* expected 0x00203108 */
#define  CG_PUC_HDR0_GOLDEN	0x00203108u
#define CG_PUC_LNK_TYPE_OMCI	0xfff1

/*
 * PUC to upstream scheduler interface control at PON + 0x6e00 (PON + 0x4fe0
 * does not decode on this chip). Only read, for /proc.
 *
 * Fields: cntr_tconid[4:0], cntr_tconid_en[5], cntr0/1/2_event_sel (3 bits
 * each), single_thread, sch_to_threshold[27:16], cntr_inccfg[31:29].
 * cntr_inccfg is 0 at reset, so the three counters at +0x04/08/0c are
 * free-running. sch_to_threshold stays at its reset value 64; the vendor
 * firmware sets 1000.
 */
#define CG_GPON_MAC_PUCIF_CTRL	0x6e00	/* dft 0x0040a100 */

/*
 * GPON MAC statistics counters (MAC block relative). These accumulate and are
 * cleared by software writing zero, not by reading, so plain reads are safe
 * from any number of readers. They are never written here: writes in this area
 * corrupt the upstream PLOAM engine.
 */
/*
 * Downstream MIB block, in vendor statistics order: BIP accumulator, BIP
 * frames, DS OMCI GEM, DS OMCI packets, DS CRC errors, DS undersize, DS
 * oversize.
 */
#define CG_REG_BIP_ERR		0x058	/* BIP-8 errors of last superframe */
#define CG_REG_BIP_ERR_ACCUM	0x05c	/* accumulated BIP-8 errors */
#define CG_REG_BIP_ERR_FRAMES	0x060	/* frames BIP accumulated over */
#define CG_REG_DS_OMCI_GEM	0x064	/* DS OMCI GEM frames (hw count) */
#define CG_REG_DS_OMCI_PKT	0x068	/* DS OMCI packets (hw count) */
#define CG_REG_DS_PKT_CRC	0x06c	/* DS packets failing CRC */
#define CG_REG_DS_UNDERSIZE	0x070	/* DS undersized packets */
#define CG_REG_DS_OVERSIZE	0x074	/* DS oversized packets */
#define CG_REG_SUPERFRAME	0x0fc	/* 125 us superframe counter */
/* upstream OMCC frames, GPON MAC side; not validated against vendor firmware */
#define CG_REG_US_OMCC_CNT	0x1e0
/*
 * b0 = PUCIF hang latched, b5:1 = T-CONT id that hung. Read only: clearing it
 * (by writing 0) would modify upstream state.
 */
#define CG_REG_PUCIF_PROTECT	0xe14
#define CG_REG_O5		0x1a8	/* O5-related count (semantics unknown) */
#define CG_REG_GEM_FRAG_DROP	0x1ac	/* DS GEM fragments dropped */
#define CG_REG_GEM_1BITERR	0x1b0	/* GEM header 1-bit errors (corrected) */
#define CG_REG_GEM_2BITERR	0x1b4	/* GEM header 2-bit errors */
#define CG_REG_GEM_UNCORR	0x1b8	/* GEM header uncorrectable errors */
#define CG_REG_BWMAP_DROP	0x1bc	/* upstream BWmap entries dropped */
#define CG_REG_OMCI_CRC		0x1c0	/* DS OMCI CRC failures */
#define CG_REG_PLEND_ERR	0x1c4	/* PLend field errors */
#define CG_REG_PLEND_BITERR	0x1c8	/* PLend bit errors */
#define CG_REG_DS_ASMBL_DROP	0x1cc	/* DS reassembly-FIFO drops */
#define CG_REG_BWMAP_UNCORR	0x1f8	/* BWmap uncorrectable bit errors */
#define CG_REG_BWMAP_CORR	0x1fc	/* BWmap corrected bit errors */
/*
 * FEC block. Whether these counters clear on read is not known, so they are
 * reported raw.
 */
#define CG_REG_FEC_CTRL		0x800
#define CG_REG_FEC_MISC_STATUS	0x804
#define CG_REG_FEC_CORR_BLK	0x808	/* correctable FEC blocks   */
#define CG_REG_FEC_UNCORR_BLK	0x80c	/* uncorrectable FEC blocks */
#define CG_REG_FEC_CLEAN_BLK	0x810	/* error-free FEC blocks    */
#define CG_REG_FEC_BLK_TOTAL	0x814	/* total FEC blocks         */
#define CG_REG_FEC_CORR_BYTES	0x818	/* bytes corrected by FEC   */

/*
 * BWmap capture (GPON_bwmap_ctl/_data1), as the vendor driver reads the
 * upstream FEC flag: start a capture, wait, then pop the captured
 * allocation structures; bit 9 is the FEC flag of that allocation's burst.
 */
#define CG_REG_BWMAP_CTL	0x178
#define CG_BWMAP_CAPTURE	BIT(0)
#define CG_BWMAP_DONE		BIT(2)
#define CG_BWMAP_COUNT		GENMASK(14, 8)
#define CG_REG_BWMAP_DATA1	0x180
#define CG_BWMAP_ALLOC		GENMASK(23, 12)
#define CG_BWMAP_FEC		BIT(9)
#define CG_BWMAP_TRIES		5
#define CG_US_FEC_CACHE		(60 * HZ)

/*
 * Without frame sync the GTC never syncs on its own, so restart after
 * CG_COLD_LOF_CHECKS checks; with sync, give the OLT CG_COLD_PLOAM_WAIT to
 * send its serial number request.
 */
#define CG_COLD_CHECK		(2 * HZ)
#define CG_COLD_LOF_CHECKS	3
#define CG_COLD_PLOAM_WAIT	(15 * HZ)

#define CG_PUC_TCONT_NUM	32	/* AAL_GPON_SYSTEM_MAX_TCONT_NUM */
#define CG_PUC_QUEUE_PER_TCONT	8	/* 8Q mode */
#define CG_PUC_9TH_QUEUE_VOQ	127	/* the CPU high-prio inject VoQ (ldpid 0xf, cos 7) */

/*
 * The upstream port-map slots carrying the OMCC and data GEM Port-IDs. On this
 * chip the slot number is the VoQ, so a T-CONT uses slots tcont *
 * CG_PUC_QUEUE_PER_TCONT .. +7. The static_asserts check both ranges fit the
 * port-map array.
 */
static const struct gpon_gem_us_range cg_us_omcc_slots = {
	.base		= 0,				/* us hw gems 0..7 */
	.count		= CG_OMCC_US_GEM_IDX_NUM,
	.index_max	= CG_US_PORT_IDX_MAX,
};
static const struct gpon_gem_us_range cg_us_data_slots = {
	.base		= CG_DATA_GEM_IDX,		/* = VoQ 8..15 */
	.count		= CG_PUC_QUEUE_PER_TCONT,
	.index_max	= CG_US_PORT_IDX_MAX,
};
static_assert(GPON_GEM_US_RANGE_OK(0, CG_OMCC_US_GEM_IDX_NUM,
				   CG_US_PORT_IDX_MAX),
	      "OMCC upstream slot range runs past the US port-map array");
static_assert(GPON_GEM_US_RANGE_OK(CG_DATA_GEM_IDX, CG_PUC_QUEUE_PER_TCONT,
				   CG_US_PORT_IDX_MAX),
	      "data upstream slot range runs past the US port-map array");
/* CG_DATA_GEM_IDX uses a literal 8 that must match CG_PUC_QUEUE_PER_TCONT. */
/*
 * onu_cfg. laser_on_align = 0x12 in the top byte aligns the upstream burst to
 * the OLT grant window; at the reset default (0) the OLT cannot decode the
 * serial number and ranging stalls. laser_pre_bias[11:7] = 18 (reset default
 * 15) as in the vendor firmware.
 */
#define CG_REG_ONU_CFG_REAL	0x118
#define CG_ONU_CFG_VAL		0x12100900
/*
 * Activation control register. Bit 1 = en: the MAC ranges O1 -> O5 by itself.
 * The vendor O5 value is written whole.
 */
#define CG_REG_ONU_CTL		0x114
#define CG_ONU_CTL_VAL		0x00460262	/* en (bit1) + defaults */
/*
 * GPON_MAC_GPON_CTRL (default 0x00430000).
 *   sw_random_en(16): serial number random delay engine. Left on, the SN response delay changes every frame and the OLT cannot decode the SN burst; the vendor clears it right after enabling the MAC.
 *   pti_omci(17): cleared by the vendor common init.
 */
#define CG_REG_GPON_MAC_CTRL	0x1c4
#define CG_MAC_CTRL_SW_RANDOM_EN BIT(16)
#define CG_MAC_CTRL_PTI_OMCI	BIT(17)

/* one post-O5 servicing event, snapshotted in hardirq, handled in the work */
struct cg_evt {
	u32 intr;			/* INTERRUPT (0x8c) sources, already enable-masked */
	u8 state;			/* onu.state at IRQ time */
	u8 id;				/* onu.id at IRQ time */
};

#define CG_EVT_RING_SZ		16	/* power of 2 */

/*
 * Source of the ONU serial number, strongest first. The serial number is the
 * ONU identity on the PON, so it is read from the board (see the cg_sn_* code),
 * never compiled in.
 */
enum cg_sn_src {
	CG_SN_NONE = 0,		/* not provisioned yet: ranging is held off */
	CG_SN_PARAM,		/* cortina_gpon.sn= module parameter */
	CG_SN_BOARD,		/* the xPON framework (omci-agent's UCI identity) */
	CG_SN_USER,		/* written to /proc/gpon */
	CG_SN_FALLBACK,		/* nothing readable: a placeholder, NOT an identity */
};

static const char *const cg_sn_src_name[] = {
	"NONE", "module-param", "board", "user", "FALLBACK",
};

/* the OLT's view of one T-CONT (ME 262) and one GEM port (ME 268) */
struct cg_tcont {
	u16 inst;
	u16 alloc;
	bool used;
};

struct cg_gem {
	u16 inst;
	u16 port;
	u16 tcont_inst;
	u8 dir;
	bool used;
};

/*
 * T-CONT scheduling from the OMCI agent: strict priority, or WRR with a
 * weight per priority (0 highest).  Keyed by ME 262 instance.
 */
struct cg_tcont_qos {
	u16 inst;
	bool used;
	bool wrr;
	u8 weight[CG_PUC_QUEUE_PER_TCONT];
};

/* the priority the OMCI agent gave the services on an upstream GEM */
struct cg_gem_prio {
	u16 port;
	u8 prio;
	bool used;
};

struct cg_hw_gem {
	u16 port;
	u16 idx;			/* VoQ/slot or DS index, 0 = not installed */
	u8 dir;
	s8 tcont;			/* data T-CONT, -1 = downstream only */
};

/*
 * An LED trigger with a state: on and off times in ms (blinking when both
 * are set, steady on with off 0, dark with on 0), applied to every LED on
 * the trigger and to one that attaches later.
 */
struct cg_led {
	struct led_trigger trig;
	unsigned long on, off;
};

struct cortina_gpon {
	struct device *dev;
	void __iomem *pon;		/* ioremap of the whole PON window */
	void __iomem *mac;		/* pon + CG_GPON_MAC_OFF, the GPON MAC block */
	void __iomem *glb;		/* ioremap of the GLB reset/clock window */
	void __iomem *gpio;		/* ioremap of the PER_GPIO window */
	struct dentry *debugfs;
	u8 wan_addr[ETH_ALEN];		/* pon0 MAC from the DT, zero if none */
	u8 password[10];		/* registration password, all zero if none */
	u8 *bosa_cal;			/* "calibration" nvmem cell, NULL if none */

	/* post-O5 servicing (ISR top half -> event ring -> work bottom half) */
	int irq;			/* GIC SPI 1, shared NE global line */
	spinlock_t evt_lock;		/* protects the ring, taken in hardirq */
	struct cg_evt evt[CG_EVT_RING_SZ];
	unsigned int evt_head, evt_tail;
	struct work_struct isr_work;
	u32 irq_count;			/* ISR entries that found PON work */
	u32 evt_drop;			/* events lost to a full ring */
	u8 last_state;			/* FSM tracker (0=O1 .. 6=O7) */
	bool omcc_up;			/* OMCC channel bound + link signalled */
	void __iomem *dg;		/* PER_DYING_GASP block */
	void __iomem *per_ictl;		/* PER_PERIPHERAL_INTERRUPT_0/INTENABLE_0 */
	u8 dg_pdu[OMCI_LEN];		/* the alarm sent when power fails */
	u16 omcc_alloc;			/* last alloc-id bound to T-CONT[0] */
	/* omcc_alloc is valid (ONU-ID 0 is legal, so 0 cannot mean "none") */
	bool omcc_alloc_valid;
	u16 omcc_gem;			/* last omci_port.id bound to us-gem 0..7 */

	/* downstream OMCI receive */
	u32 omci_rx;			/* DS OMCI PDUs delivered by the NI CPU-RX hook */
	u32 omci_rx_short;		/* runt PDUs (< 8 bytes, not decodable) */
	bool pdc_ready;			/* PDC map + CTRL programmed */
	bool puc_ready;			/* PUC US-VoQ admission programmed */

	/*
	 * The PUC control packet counters, accumulated in software because the
	 * hardware counters clear on read. Only cg_puc_ctrl_sample() reads the
	 * registers, so /proc readers add to these totals rather than consuming
	 * them.
	 */
	spinlock_t puc_cnt_lock;	/* serializes the read-and-add */
	struct delayed_work puc_cnt_work;	/* samples shortly after a US OMCI TX */
	u32 puc_omci_us;		/* upstream OMCI (link-type 0xfff1) frames */
	u32 puc_ctrl_mac;		/* upstream MAC-type control frames */
	u32 puc_len_err;		/* upstream frames failing the length check */
	u32 puc_cnt_samples;		/* reads folded in (0 = never sampled) */

	/* G.988 OMCI responder and upstream OMCI TX */
	struct omci_onu *omci;		/* responder context (kzalloc'd at probe) */
	spinlock_t omci_lock;		/* RX hook (softirq) vs isr_work/AVC work */
	bool omci_active;		/* ctx armed (OMCC up) */
	struct delayed_work veip_avc_work;	/* the ~31s post-O5 VEIP oper-up AVC */
	struct delayed_work optic_log_work;	/* one optical sample after O5 */
	struct delayed_work stats_work;		/* periodic OMCI counter dump */
	unsigned int veip_avc_retry_ms;	/* backoff after a failed AVC TX; 0 = none pending */
	struct delayed_work coldstart_work;	/* stuck-O1 US-lock-miss recovery */
	struct delayed_work carrier_work;	/* WAN carrier off once an outage outlasts the hold */
	int coldstart_tries;		/* re-rolls THIS stuck episode (reset on leaving O1) */
	u32 coldstart_rolls;		/* total re-rolls this power-on (/proc visibility) */
	u8 coldstart_lof;		/* checks in a row with SerDes lock but LOS/LOF */
	unsigned long coldstart_sync;	/* jiffies of downstream frame sync at O1, 0 = none */
	u32 omci_tx;			/* US OMCI responses enqueued to the NI */
	u32 omci_tx_fail;		/* NI TX rejected (ring/scratch busy) */
	u32 omci_ds_crc_ok;		/* DS MIC self-check (first PDUs only) */
	u32 omci_ds_crc_bad;

	/*
	 * OLT-provisioned WAN data path, as snooped from OMCI (ME 262 by
	 * instance, ME 268 by instance); data_lock guards it against isr_work.
	 * It survives leaving O5, so a LOS re-range without re-provisioning
	 * re-installs it; an OMCI MIB-Reset clears it.
	 */
	spinlock_t data_lock;
	struct cg_tcont tc[CG_DATA_TCONT_MAX];
	struct cg_gem gm[CG_DATA_GEM_MAX];
	struct cg_tcont_qos tq[CG_DATA_TCONT_MAX];
	struct cg_gem_prio gp[CG_DATA_GEM_MAX];
	bool qos_dirty;			/* tq/gp changed since the last sync */
	/*
	 * What cg_data_sync() programmed in the T-CONT, DS GEM and US port
	 * tables; isr_work only. The OMCC alloc is tracked in omcc_alloc.
	 */
	struct {
		u16 alloc;		/* bound alloc-id, 0 = none */
		u8 queues;		/* VoQs taken by upstream GEMs */
	} hw_tc[CG_DATA_TCONT_MAX];
	struct cg_hw_gem hw_gm[CG_DATA_GEM_MAX];
	bool bcast_bound;
	bool data_flush;		/* flush the VoQs on the next O5 */
	bool data_installed;		/* a default upstream GEM is up */
	u16 us_idx;			/* its VoQ/slot, 0 = none */
	u32 omci_cfg_log;		/* config-ME body log budget used */
	struct net_device *wan_ndev;	/* pon0 */
	bool tx_allowed;		/* the laser driver has been programmed */
	struct gpio_desc *tx_disable;	/* asserted keeps the laser off */
	struct gpio_desc *tx_power;	/* asserted powers the transmitter */
	bool tx_on;			/* the laser pins are driven on */
	unsigned long activate_jiffies;	/* activation, or signal detected */
	unsigned long los_jiffies;	/* downstream signal lost, 0 if not */
	s8 ds_fec;			/* downstream FEC: 1 on, 0 off, -1 not seen */
	struct cg_led led_link;		/* "gpon-link": O5 on, ranging blinks */
	struct cg_led led_los;		/* "gpon-los": boot blink, then LOS */
	struct delayed_work los_work;	/* LOS shown once it lasts */
	s8 us_fec;			/* upstream FEC on our bursts, -1 unknown */
	unsigned long us_fec_jiffies;	/* when us_fec was sampled */
	struct mutex fec_lock;		/* the FEC totals below */
	u32 fec_last[4];		/* last hardware counts, to add deltas */
	u64 fec_sum[4];			/* corrected bytes/blocks, uncorrectable, total */

	/*
	 * The board's PON identity, used for both the MAC
	 * vendor-id/vendor-specific registers and the OMCI ME 256 serial
	 * number.
	 */
	struct mutex sn_lock;		/* serializes sn/sn_src/activated + activation */
	u8 sn[8];			/* wire order: 4 ASCII vendor-id + 4 VSSN bytes */
	enum cg_sn_src sn_src;
	bool activated;			/* cg_mac_activate() has run at least once */
	struct delayed_work sn_wait_work;	/* bounded wait for the board's serial */
	struct work_struct activate_work;	/* (re)ranging, off the caller's path */
	struct cortina_epon *epon;		/* 1G EPON mode controller */
};

static struct cortina_gpon *cg_singleton;

/*
 * Put the whole PON domain into a known reset state so the SerDes can be
 * brought up first. The release (PON_CNTL = 0x30e, GPON_CNTL = 0x3) happens at
 * the end of cg_psds_init(), once the SerDes clock runs, so the GTC reset edge
 * propagates. cortina-ni does not touch these registers.
 */
static void cg_glb_reset(struct cortina_gpon *cg)
{
	void __iomem *glb = cg->glb;

	/*
	 * SerDes power off first, mode select, then assert every PON-domain
	 * reset and release only psds_reg_rst_n (the SerDes CSR bus) so the
	 * analog profile can be loaded.
	 *
	 * The GTC and ANI resets (GPON_CNTL) stay asserted through the SerDes
	 * bring-up: released while the PON clock is dead, the reset edge does
	 * not propagate and the downstream framer may come up wedged (stuck at
	 * O1/LOF, surviving warm reboots).
	 */
	/* clear only POW_PCIX (bit5) */
	writel(readl(glb + CG_GLB_PSDS_INIT) & ~CG_PSDS_POW_PCIX, glb + CG_GLB_PSDS_INIT);
	writel(0x00030000, glb + CG_GLB_EPON_CNTL);	/* select PON/ONU mode */
	writel(0x00000000, glb + CG_GLB_PON_CNTL);	/* assert all PON-domain resets */
	writel(0x00000000, glb + CG_GLB_GPON_CNTL);	/* GTC+ANI held until SerDes lock */
	writel(0x00000004, glb + CG_GLB_PON_CNTL);	/* psds_reg_rst_n 0->1 only */
	mdelay(1);
}

/*
 * Bring the PON SerDes CMU/PLL up so it clocks the PON APB bus the GPON MAC is
 * on. The PLL needs its rate (PSDS_MODE) and analog profile before it is
 * powered (POW_PCIX). It locks to the reference clock, so this works without
 * fiber; only the downstream RX lock needs light.
 */
static void cg_psds_init(struct cortina_gpon *cg)
{
	void __iomem *pon = cg->pon;
	u32 v;
	int i;

	/*
	 * Raise PON_CNTL bit0 and keep it set through the SerDes bring-up (the
	 * final PON_CNTL = 0x30e clears it), then toggle psds_reg_rst_n (bit2)
	 * 1 -> 0 -> 1.
	 */
	v = readl(cg->glb + CG_GLB_PON_CNTL) | BIT(0);
	writel(v, cg->glb + CG_GLB_PON_CNTL);
	writel(v & ~BIT(2), cg->glb + CG_GLB_PON_CNTL);
	mdelay(1);
	writel(v | BIT(2), cg->glb + CG_GLB_PON_CNTL);
	mdelay(1);

	/* GPON rate: sd_s0 = 1, sds_mode_s0 = 0x8, usx = 0 */
	writel(0x00000408, pon + CG_PSDS_MODE);
	udelay(10);

	/*
	 * CMU/PLL/CDR/TX driver analog profile, written through the
	 * DATAIN/ACCESS pair.
	 */
	for (i = 0; i < ARRAY_SIZE(cg_serdes_gpon); i++) {
		writel(cg_serdes_gpon[i].val, pon + cg_serdes_gpon[i].off);
		udelay(cg_serdes_gpon[i].delay_us ? cg_serdes_gpon[i].delay_us : 10);
	}

	/*
	 * Hold EPON in reset and set the spare config bit around the lock wait
	 * (GPON only).
	 */
	v = readl(cg->glb + CG_GLB_EPON_CNTL);
	writel(v | BIT(0), cg->glb + CG_GLB_EPON_CNTL);		/* epon_rst_n = 1 */
	v = readl(pon + CG_PON_EPON_SPARE);
	writel(v | 0x80000000, pon + CG_PON_EPON_SPARE);

	/*
	 * Power the SerDes, which starts the PON APB clock. The laser burst
	 * enable (ben_oen) stays off until the SerDes is stable, at the end of
	 * this function.
	 */
	v = readl(cg->glb + CG_GLB_PSDS_INIT);
	writel((v | CG_PSDS_POW_PCIX) & ~CG_PSDS_BEN_OEN, cg->glb + CG_GLB_PSDS_INIT);
	mdelay(1);

	/*
	 * Bounded wait (1001 x 1 ms, as the vendor) for the RX clock lock,
	 * continuing on timeout. Cold boot lock takes ~400-500 ms. Order: lock,
	 * then los-reset release, then gearbox reset release.
	 */
	for (i = 0; i < 1001; i++) {
		if ((readl(pon + CG_PSDS_RGB8) & 0x9c01) == 0x9c00)
			break;
		mdelay(1);
	}
	dev_dbg(cg->dev, "psds: __psds_sync RX-lock wait done at %dms, rgb8=0x%08x\n",
		 i, readl(pon + CG_PSDS_RGB8));

	/* release GPON los-reset */
	v = readl(cg->glb + CG_GLB_EPON_CNTL);
	writel(v & ~BIT(0), cg->glb + CG_GLB_EPON_CNTL);	/* epon_rst_n = 0 */

	/*
	 * Toggle PON_CNTL.pon_serdes_rst_n (bit1) 0 -> 1 now that the SerDes is
	 * powered and its RX clock locked. The gearbox connects the serial
	 * stream to the MAC's parallel downstream input; released on an
	 * unlocked RX its FIFO may come up misaligned, and without it no
	 * downstream frames reach the framer.
	 */
	v = readl(cg->glb + CG_GLB_PON_CNTL);
	writel(v & ~BIT(1), cg->glb + CG_GLB_PON_CNTL);
	mdelay(1);
	writel(v | BIT(1), cg->glb + CG_GLB_PON_CNTL);
	mdelay(1);

	/* RX/TX bit ordering = 1 */
	v = readl(pon + CG_PSDS_GBOX_CTRL);
	v = (v & ~((0x3u << 4) | (0x3u << 6))) | (0x1u << 4) | (0x1u << 6);
	writel(v, pon + CG_PSDS_GBOX_CTRL);
	mdelay(1);

	/*
	 * Release the remaining PON-domain resets and give the GTC and ANI
	 * their 0 -> 0x3 reset edge now, with the SerDes clock locked, so the
	 * downstream framer starts in a defined state.
	 *
	 * The RX lock (RGB8 bit15) can arrive ~300 ms after the gearbox
	 * release, so wait for it again (bounded) before the edge.
	 */
	for (i = 0; i < 2000; i++) {
		if ((readl(pon + CG_PSDS_RGB8) & 0x9c01) == 0x9c00)
			break;
		mdelay(1);
	}
	dev_dbg(cg->dev, "psds: pre-edge DS-lock wait done at %dms, rgb8=0x%08x\n",
		 i, readl(pon + CG_PSDS_RGB8));
	writel(0x0000030e, cg->glb + CG_GLB_PON_CNTL);	/* pon_serdes/psds/ptp + puc/pdc */
	writel(0x00000003, cg->glb + CG_GLB_GPON_CNTL);	/* ani_rst_n + gpon_rst_n */
	mdelay(100);

	/*
	 * Drive the laser burst enable output now, while the MAC is still
	 * disabled (BEN idles low), before the GN25L95 is programmed. A
	 * floating burst enable during BOSA bring-up can latch the laser
	 * driver's TX fault (0x6e bit7) and keep the laser dark.
	 */
	v = readl(cg->glb + CG_GLB_PSDS_INIT);
	writel(v | CG_PSDS_BEN_OEN, cg->glb + CG_GLB_PSDS_INIT);
}

/*
 * The per-board GPON serial number
 *
 * The serial number is 8 bytes on the wire: 4 ASCII vendor-id characters and 4
 * binary vendor-specific bytes, written as "VVVVHHHHHHHH". It is the ONU's
 * identity on the PON, so it must come from the board: a compiled-in value
 * would make every unit announce the same identity.
 *
 It is configuration: omci-agent hands it over from UCI (omci.main.serial)
 * through the xPON framework. Ranging is held off until it arrives, or until
 * `echo "sn VVVVHHHHHHHH" > /proc/gpon`. If none arrives within
 * CG_SN_WAIT_SECS the driver logs an error and ranges with a placeholder so the
 * box is not silently dark; a real serial number arriving later re-activates
 * with it.
 */
#define CG_SN_WAIT_SECS		60

/*
 * Placeholder used when the board's serial number cannot be read: vendor-id
 * "XPON" with an all-ones VSSN (blank flash value), which cannot collide with a
 * provisioned unit. Always reported by dev_err() and "sn-source = FALLBACK" in
 * /proc/gpon.
 */
static const u8 cg_sn_unprovisioned[8] = { 'X', 'P', 'O', 'N', 0xff, 0xff, 0xff, 0xff };



/* One 32-bit register value from 4 wire-order bytes (endianness-agnostic). */
static u32 cg_sn_word(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static int cg_hex_nibble(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/*
 * "VVVVHHHHHHHH" -> the 8 wire bytes.  Rejects anything that is not exactly 4
 * printable non-space vendor-id characters followed by 8 hex digits, so a
 * truncated/garbled provisioning read can never be programmed as an identity.
 */
static int cg_sn_parse(const char *s, u8 out[8])
{
	int i, hi, lo;

	if (!s || strlen(s) != 12)
		return -EINVAL;
	for (i = 0; i < 4; i++) {
		if (!isprint(s[i]) || isspace(s[i]))
			return -EINVAL;
		out[i] = s[i];
	}
	for (i = 0; i < 4; i++) {
		hi = cg_hex_nibble(s[4 + 2 * i]);
		lo = cg_hex_nibble(s[5 + 2 * i]);
		if (hi < 0 || lo < 0)
			return -EINVAL;
		out[4 + i] = (hi << 4) | lo;
	}
	return 0;
}

/* The 8 wire bytes -> the printable "VVVVHHHHHHHH" form. */
static void cg_sn_format(const u8 sn[8], char out[13])
{
	snprintf(out, 13, "%c%c%c%c%02X%02X%02X%02X",
		 sn[0], sn[1], sn[2], sn[3], sn[4], sn[5], sn[6], sn[7]);
}

/*
 * The laser pins are GPIO 96 (TX power) and GPIO 97 (TX disable) of GPIO group
 * 3. Both are driven low at idle and at O5 by the vendor firmware. Out of reset
 * the pads are pulled high, which keeps the laser off, so they are only driven
 * low once the transmitter is allowed.
 */
static void cg_tx_pins(struct cortina_gpon *cg, bool on)
{
	if (!cg->tx_power && !cg->tx_disable) {
		dev_dbg(cg->dev, "laser GPIOs not described - pads left untouched\n");
		return;
	}
	if (cg->tx_power)
		gpiod_set_value(cg->tx_power, on);
	if (cg->tx_disable)
		gpiod_set_value(cg->tx_disable, !on);
	if (cg->tx_on != on)
		dev_dbg(cg->dev, "laser transmitter %s\n", on ? "enabled" : "disabled");
	cg->tx_on = on;
}

/* One PDC map-memory entry write: DATA0/DATA1, then kick ACCESS, poll go. */
static int cg_pdc_map_write(struct cortina_gpon *cg, u32 idx, u32 d0, u32 d1)
{
	int i;

	writel(d0, cg->pon + CG_PDC_MAP_DATA0);
	writel(d1, cg->pon + CG_PDC_MAP_DATA1);
	writel(CG_TBL_GO | CG_TBL_WR | (idx & 0xff), cg->pon + CG_PDC_MAP_ACCESS);
	for (i = 0; i < 10000; i++) {
		if (!(readl(cg->pon + CG_PDC_MAP_ACCESS) & CG_TBL_GO))
			return 0;
	}
	dev_warn(cg->dev, "PDC map[%u] write timed out\n", idx);
	return -ETIMEDOUT;
}

/*
 * PDC init: route the downstream GEMs. Without it the OMCC downstream GEM is
 * de-encapsulated but has no destination and downstream OMCI never reaches the
 * CPU.
 * Entries 0..7 (OMCC internal GEMs) -> CPU port 0, forwarding-engine bypass,
 * no-drop, cos 6, pol_id 0x80 + idx; entries 8..255 (data GEMs) -> L3_WAN,
 * pol_id idx - 8. Then PDC_CTRL: map memory enable plus the OMCI high-priority
 * override (cos 7, ldpid CPU_0), expected readback 0x02870002. Runs once after
 * the puc/pdc reset release and before the MAC is enabled (order:
 * ds_frame_thrsd, pdc, puc).
 */
static void cg_pdc_init(struct cortina_gpon *cg)
{
	u32 idx, d0, d1, ctrl;
	unsigned int dead = 0;		/* entries that could not be written */

	for (idx = 0; idx < CG_PDC_MAP_ENTRIES; idx++) {
		if (idx < CG_OMCC_US_GEM_IDX_NUM) {
			d0 = CG_PDC_D0_COS(6) | CG_PDC_D0_LDPID(CG_LPORT_CPU_0) |
			     CG_PDC_D0_LSPID(CG_LPORT_PON) |
			     CG_PDC_D0_FE_BYPASS | CG_PDC_D0_NO_DROP;
			d1 = CG_PDC_D1_POL_ID(idx + 0x80);
		} else {
			d0 = CG_PDC_D0_LDPID(CG_LPORT_L3_WAN) |
			     CG_PDC_D0_LSPID(CG_LPORT_PON);
			d1 = CG_PDC_D1_POL_ID(idx - 8);
		}
		if (cg_pdc_map_write(cg, idx, d0, d1))
			dead++;		/* keep going, still program PDC_CTRL */
	}

	ctrl = readl(cg->pon + CG_PDC_CTRL);
	ctrl &= ~CG_PDC_CTRL_HP_MASK;
	ctrl |= CG_PDC_CTRL_MAP_MEM_EN | CG_PDC_CTRL_HP_EN |
		(7 << CG_PDC_CTRL_HP_COS_SH) |
		(CG_LPORT_CPU_0 << CG_PDC_CTRL_HP_LDPID_SH);
	writel(ctrl, cg->pon + CG_PDC_CTRL);

	cg->pdc_ready = (dead == 0);
	if (dead)
		dev_warn(cg->dev, "PDC: %u map entr%s failed - NOT ready, O5 supervisor re-runs\n",
			 dead, dead == 1 ? "y" : "ies");
	dev_dbg(cg->dev, "PDC: OMCC DS GEMs 0-7 -> CPU_0, ctrl=0x%08x\n",
		 readl(cg->pon + CG_PDC_CTRL));
}

/* PUC indirect-table op: kick ACCESS (go[31] + rbw[30]=write + index), poll go. */
static int cg_puc_ind_write(struct cortina_gpon *cg, u32 access_off, u32 index)
{
	int i;

	writel(CG_TBL_GO | CG_TBL_WR | index, cg->pon + access_off);
	for (i = 0; i < 10000; i++) {
		if (!(readl(cg->pon + access_off) & CG_TBL_GO))
			return 0;
	}
	dev_warn(cg->dev, "PUC indirect +0x%04x[%u] timed out\n", access_off, index);
	return -ETIMEDOUT;
}

/*
 * Initialise a data VoQ's PUC token-bucket memory (CIR and PIR), as the vendor
 * queue_add does per VoQ before enabling DBA report; enabling report over
 * uninitialised bucket memory causes an asynchronous SError. Values are the
 * vendor firmware's for an unshaped data T-CONT (CIR = PIR = 0).
 */
static void cg_puc_voq_tbc_init(struct cortina_gpon *cg, u32 voq)
{
	/* CIR bucket (PUC_VOQ_TBC_MEM) */
	writel(0x200a0080, cg->pon + CG_PUC_VOQ_TBC_DATA1);
	writel(0x14000000, cg->pon + CG_PUC_VOQ_TBC_DATA0);
	cg_puc_ind_write(cg, CG_PUC_VOQ_TBC_ACCESS, voq);
	/* PIR bucket (PUC_RL_VOQ_TBC_MEM), wide open */
	writel(0x3ffe00bf, cg->pon + CG_PUC_RL_VOQ_TBC_DATA1);
	writel(0xffffffff, cg->pon + CG_PUC_RL_VOQ_TBC_DATA0);
	cg_puc_ind_write(cg, CG_PUC_RL_VOQ_TBC_ACCESS, voq);
}

/* One PUC per-VoQ valid bit (PUC_valid_voqN, 256-bit mask across 8 regs). */
static void cg_puc_voq_valid(struct cortina_gpon *cg, u32 voq, bool valid)
{
	u32 off = CG_PUC_VALID_VOQ0 - (voq / 32) * 4;
	u32 v = readl(cg->pon + off);

	if (valid)
		v |= BIT(voq % 32);
	else
		v &= ~BIT(voq % 32);
	writel(v, cg->pon + off);
}

/*
 * Program one PUC pvtbl entry (per-T-CONT VoQ map) + its 8 VoQs' back-
 * pressure remap and valid bits.  @ena gates the per-queue enable bit (bit 8
 * of each 9-bit voqN field) and the valid-VoQ mask; the entry itself is
 * always marked entryvld so the scheduler walks it.  voqN 9-bit fields are
 * bit-split across the 5 DATA words exactly as the vendor packs them.
 *
 * @weight holds one WRR weight per VoQ, or is NULL for strict priority.  As
 * in the vendor's port_voq_map_set, a weight of 0 makes a VoQ strict and the
 * strict VoQs rank above the WRR ones by index, so the weighted VoQs must be
 * the low ones; schmode stays 0 in 8Q mode.
 */
static int cg_puc_pvtbl_program(struct cortina_gpon *cg, u32 tcont, bool ena,
				const u8 *weight)
{
	static const u8 none[CG_PUC_QUEUE_PER_TCONT];
	void __iomem *pon = cg->pon;
	u32 voq[CG_PUC_QUEUE_PER_TCONT];
	u32 credit[5] = { };
	u32 d0, d1, d2, d3, d4, q, shift;
	u64 c;

	if (!weight)
		weight = none;

	for (q = 0; q < CG_PUC_QUEUE_PER_TCONT; q++)
		voq[q] = (q + tcont * CG_PUC_QUEUE_PER_TCONT) | ((u32)ena << 8);

	d0 = voq[0] | (voq[1] << 9) | (voq[2] << 18) |
	     ((voq[3] & 0x1f) << 27);
	d1 = ((voq[3] >> 5) & 0xf) | (voq[4] << 4) | (voq[5] << 13) |
	     (voq[6] << 22) | ((voq[7] & 1) << 31);
	d2 = ((voq[7] >> 1) & 0xff) | BIT(12) |	/* schmode=0, entryvld=1 */
	     ((u32)weight[0] << 16) | ((u32)weight[1] << 24);
	d3 = weight[2] | (weight[3] << 8) | (weight[4] << 16) |
	     ((u32)weight[5] << 24);
	d4 = weight[6] | (weight[7] << 8);

	writel(d4, pon + CG_PUC_PVTBL_DATA4);
	writel(d3, pon + CG_PUC_PVTBL_DATA3);
	writel(d2, pon + CG_PUC_PVTBL_DATA2);
	writel(d1, pon + CG_PUC_PVTBL_DATA1);
	writel(d0, pon + CG_PUC_PVTBL_DATA0);
	if (cg_puc_ind_write(cg, CG_PUC_PVTBL_ACCESS, tcont))
		return -ETIMEDOUT;

	/* the credit per round, scaled as the vendor's restore does */
	shift = 7 + (readl(pon + CG_PUC_VOQSCHECFG) & 3);
	for (q = 0; q < CG_PUC_QUEUE_PER_TCONT; q++) {
		c = (u64)((weight[q] << shift) & GENMASK(19, 0)) << (q * 20 % 32);
		credit[q * 20 / 32] |= lower_32_bits(c);
		if (q * 20 / 32 + 1 < ARRAY_SIZE(credit))
			credit[q * 20 / 32 + 1] |= upper_32_bits(c);
	}
	for (q = 0; q < ARRAY_SIZE(credit); q++)
		writel(credit[q], pon + CG_PUC_SCHEINFO_DATA(q));
	if (cg_puc_ind_write(cg, CG_PUC_SCHEINFO_ACCESS, tcont))
		return -ETIMEDOUT;

	for (q = 0; q < CG_PUC_QUEUE_PER_TCONT; q++) {
		u32 qid = q + tcont * CG_PUC_QUEUE_PER_TCONT;

		if (qid <= 63) {
			writel(qid & 0x7, pon + CG_PUC_VOQBPREMAP_DATA);
			if (cg_puc_ind_write(cg, CG_PUC_VOQBPREMAP_ACCESS, qid))
				return -ETIMEDOUT;
		}
		cg_puc_voq_valid(cg, qid, ena);
	}
	return 0;
}

/*
 * Flush one T-CONT's 8 VoQs (start + openpktflushen + tcontid + voqid, poll for
 * start to clear), as the vendor does after every CAM re-install so a re-range
 * does not burst frames queued before the link drop. The vendor also brackets
 * each flush with VoQ drop-enable and pvtbl disable; here the WAN carrier is
 * off during the flush, so nothing is enqueued.
 */
static void cg_puc_voq_flush(struct cortina_gpon *cg, u32 tcont)
{
	u32 q, v;
	int i;

	for (q = 0; q < CG_PUC_QUEUE_PER_TCONT; q++) {
		v = BIT(31) | BIT(16) | ((tcont & 0x1f) << 8) |
		    ((tcont * CG_PUC_QUEUE_PER_TCONT + q) & 0xff);
		writel(v, cg->pon + CG_PUC_VOQFLUSH);
		for (i = 0; i < 10000; i++) {
			if (!(readl(cg->pon + CG_PUC_VOQFLUSH) & BIT(31)))
				break;
			udelay(1);
		}
		if (i == 10000)
			dev_warn(cg->dev, "VoQ %u flush timed out\n",
				 tcont * CG_PUC_QUEUE_PER_TCONT + q);
	}
}

/*
 * PUC init: the upstream admission setup that connects CPU-injected frames to
 * the OMCC T-CONT / GEM upstream burst. Without it frames land in an unmapped,
 * invalid VoQ and are dropped. Runs once, right after the PDC, only in the PON
 * + 0x8000 sub-block. 8Q VoQ mode: VoQID = {HdrA.ldpid[3:0], HdrA.cos[2:0]}.
 * Only T-CONT 0 (OMCC) has its VoQs enabled; the CPU high-priority OMCI inject
 * also uses the "9th queue" VoQ 127 (ldpid 0xf, cos 7).
 */
static void cg_puc_init(struct cortina_gpon *cg)
{
	void __iomem *pon = cg->pon;
	u32 tcont, q, v;
	unsigned int dead = 0;		/* T-CONT entries that failed */

	/* clear the PUC interrupt enable */
	writel(0, pon + CG_PUC_PONCNTL_INTEN);

	/*
	 * PUCCFG: inccfg = 2 (clear-on-read), crccntl = 2 (regenerate upstream
	 * CRC), invalid_voqdrop_enable = 1
	 */
	v = readl(pon + CG_PUC_PUCCFG);
	v = (v & ~(GENMASK(18, 16) | GENMASK(1, 0))) | (2u << 16) | 2u;
	v |= BIT(30);
	writel(v, pon + CG_PUC_PUCCFG);

	/*
	 * VoQ buffer limits A/B/C and per-VoQ limit select (all 256 VoQs use A)
	 */
	writel(0x7a0, pon + CG_PUC_VOQBUFLIMIT_A);
	writel(0x3b0, pon + CG_PUC_VOQBUFLIMIT_B);
	writel(0x200, pon + CG_PUC_VOQBUFLIMIT_C);
	for (q = 0; q < CG_PUC_VOQBUFLIMSEL_N; q++)
		writel(0x55555555, pon + CG_PUC_VOQBUFLIMSEL0 + q * 4);

	/* VoQ map mode = 8Q (voqmapsel = 0) */
	writel(0, pon + CG_PUC_VOQMAPCFG);

	/*
	 * pvtbl: per T-CONT VoQ map, with only T-CONT 0 (OMCC) enabled; every
	 * entry is marked valid so the scheduler walks it. Also programs the
	 * back-pressure remap and the per-VoQ valid bits.
	 */
	/*
	 * On a pvtbl failure carry on with the rest of the setup; the O5
	 * supervisor re-runs this while puc_ready is false.
	 */
	for (tcont = 0; tcont < CG_PUC_TCONT_NUM; tcont++)
		if (cg_puc_pvtbl_program(cg, tcont, tcont == 0, NULL))
			dead++;
	/* the CPU high-priority OMCI inject rides the 9th queue (VoQ 127) */
	cg_puc_voq_valid(cg, CG_PUC_9TH_QUEUE_VOQ, true);

	/*
	 * Upstream OMCI HEADER_A replacement: for an OMCI control frame
	 * (GLOBAL_LNK_TYPE 0xfff1) the PUC stamps the OMCC GEM index and CoS
	 * onto the frame. us_ext_omci_en with us_hdr_min_size = 30 accepts
	 * extended OMCI.
	 */
	/*
	 * The CPU inject uses the high-priority queue, so its header comes from
	 * US_OMCI_HP_HDR_A. cos 7 there is the PLOAM code point
	 * (PUC_GLOBAL_PLOAM_CFG: OMCI field 6, PLOAM field 7).
	 */
	writel(CG_US_OMCI_HDR_A_VAL, pon + CG_PUC_US_OMCI_HDR_A);
	writel(CG_US_OMCI_HP_HDR_A_VAL, pon + CG_PUC_US_OMCI_HP_HDR_A);
	dev_dbg(cg->dev,
		"PUC US_OMCI_HDR_A=%08x HP_HDR_A=%08x (readback %08x / %08x)\n",
		CG_US_OMCI_HDR_A_VAL, CG_US_OMCI_HP_HDR_A_VAL,
		readl(pon + CG_PUC_US_OMCI_HDR_A),
		readl(pon + CG_PUC_US_OMCI_HP_HDR_A));
	v = readl(pon + CG_PUC_GLOBAL_PLOAM_CFG);
	v = (v & ~GENMASK(21, 16)) | (30u << 16) | BIT(31);
	writel(v, pon + CG_PUC_GLOBAL_PLOAM_CFG);

	/*
	 * The OMCI link type (a hardware default) is what makes the control
	 * packet counter count OMCI (see cg_puc_ctrl_sample()), so check it.
	 */
	v = readl(pon + CG_PUC_GLOBAL_LNK_TYPE) >> 16;
	if (v != CG_PUC_LNK_TYPE_OMCI)
		dev_warn(cg->dev,
			 "PUC control-packet link type is 0x%04x, expected 0x%04x: the upstream OMCI frame count will not match\n",
			 v, CG_PUC_LNK_TYPE_OMCI);

	/* back-pressure: drop off, bp on, threshold 0x100 */
	v = readl(pon + CG_PUC_BPCNTL);
	v = (v & ~(BIT(4) | GENMASK(30, 16))) | BIT(0) | (0x100u << 16);
	writel(v, pon + CG_PUC_BPCNTL);

	/*
	 * BTC: pfovrhd = 5, schmode = FRAGMENT(0), wdaligned = 0,
	 * minrmnwindowsz = 5, sch2en = 1, lrgfrmfragen = 1 (segment frames >
	 * 4095 bytes)
	 */
	v = readl(pon + CG_PUC_BTCCFG);
	v = (v & ~GENMASK(5, 0)) | 5u;
	v &= ~(BIT(8) | BIT(12));
	v |= BIT(16) | BIT(25);
	v = (v & ~GENMASK(31, 27)) | (5u << 27);
	writel(v, pon + CG_PUC_BTCCFG);

	/* QM to PUC report adjust levels */
	writel(0x00c80000, pon + CG_PUC_Q2PQSRCFG01);	/* lv0=0, lv1=0xc8 */
	writel(0x05c201b8, pon + CG_PUC_Q2PQSRCFG23);	/* lv2=0x1b8, lv3=0x5c2 */

	/* aggregate shaper + PIR (rate limiter off) */
	v = readl(pon + CG_PUC_CTRL);
	v = (v | BIT(30)) & ~BIT(26);	/* shp_en=1, rl_en=0 */
	writel(v, pon + CG_PUC_CTRL);
	writel(20u | (20u << 5) | (20u << 10), pon + CG_PUC_CTRL1);
	v = readl(pon + CG_PUC_CTRL2);
	v = (v & ~GENMASK(4, 0)) | 20u | BIT(26);	/* pirovhd=20, pir_en=1 */
	writel(v, pon + CG_PUC_CTRL2);

	/*
	 * Global PUC QM enable: set bit 3 (cfg_qmplmem_en) of PLEN_MEM_CTL, as
	 * the vendor does once before adding any queue. It enables the QM
	 * packet length memory the DBA report engine reads; without it
	 * REPORT_ENABLE0 reads abort and writes cause an SError.
	 */

	/*
	 * Set the PUC configuration registers to the vendor firmware values.
	 * Notably VALID_VOQ: the vendor validates VoQ 0..15 and leaves the
	 * 9th-queue range (96..127) invalid.
	 */
	static const struct { u16 off; u32 val; } stockcfg[] = {
		{ 0x000, 0x0000003f },	/* PUC control */
		{ 0x00c, 0x00000000 },
		{ 0x010, 0x00000000 },
		{ 0x014, 0x00000000 },
		{ 0x090, 0xaaa9aaa9 },	/* per-queue 2-bit map */
		{ 0x0d0, 0x00000c00 },
		{ 0x0d4, 0x00000700 },
		{ 0x0e8, 0x400000ff },	/* 8 enables, not 6 */
		{ 0x0ec, 0x000000ff },	/* 8 enables, not 3 */
		{ 0x1bc, 0x0000ffff },	/* VALID_VOQ 0..15, no 9th queue */
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(stockcfg); i++)
		writel(stockcfg[i].val,
		       pon + CG_PUC_BASE + stockcfg[i].off);
	dev_dbg(cg->dev,
		"PUC set to vendor config (%u regs); voq0=%08x e8=%08x ec=%08x ctrl=%08x\n",
		(unsigned int)ARRAY_SIZE(stockcfg),
		readl(pon + CG_PUC_BASE + 0x1bc),
		readl(pon + CG_PUC_BASE + 0x0e8),
		readl(pon + CG_PUC_BASE + 0x0ec),
		readl(pon + CG_PUC_BASE + 0x000));

	cg->puc_ready = (dead == 0);
	if (dead)
		dev_warn(cg->dev, "PUC: %u T-CONT pvtbl entr%s failed - NOT ready, O5 supervisor re-runs\n",
			 dead, dead == 1 ? "y" : "ies");
	dev_dbg(cg->dev,
		"PUC: OMCC T-CONT0 VoQs + 9th-queue enabled, puccfg=0x%08x lnk_type=0x%04x\n",
		readl(pon + CG_PUC_PUCCFG),
		readl(pon + CG_PUC_GLOBAL_LNK_TYPE) >> 16);
}

/*
 * Fold one read of the PUC control packet counters into the cumulative totals.
 *
 * The counters clear on read (PUCCFG.inccfg = 2, as the vendor sets it) and
 * also reset after a short idle window, so every read is a delta and the sum of
 * reads is the exact total. This must be the only reader of
 * +0x174/+0x178/+0x188; /proc/gpon calls this instead of reading them, so
 * concurrent readers add counts rather than consume them.
 * BMC_RX_PKT/_ENQ/FORCE_DROP are read raw by the /proc show function and are
 * not sampled here.
 *
 * inccfg is global to the PUC and no "accumulate" encoding is documented, so it
 * is left at the vendor value. The 16-bit fields cannot overflow between
 * samples, as a sample follows every OMCI transmit.
 */
static void cg_puc_ctrl_sample(struct cortina_gpon *cg)
{
	void __iomem *pon = cg->pon;

	if (!cg->puc_ready)
		return;
	spin_lock(&cg->puc_cnt_lock);
	cg->puc_omci_us += readl(pon + CG_PUC_BMC_CTRL_PKT_LNK) &
			   CG_PUC_BMC_CNTR_MASK;
	cg->puc_ctrl_mac += readl(pon + CG_PUC_BMC_CTRL_PKT_MAC) &
			    CG_PUC_BMC_CNTR_MASK;
	cg->puc_len_err += readl(pon + CG_PUC_BMC_LENGTH_ERROR) &
			   CG_PUC_BMC_CNTR_MASK;
	cg->puc_cnt_samples++;
	spin_unlock(&cg->puc_cnt_lock);
}

/*
 * Sample shortly after an upstream OMCI frame was handed to the NI, when the
 * counter is expected to move. A burst of replies coalesces into one sample;
 * the hardware keeps counting meanwhile.
 */
#define CG_PUC_CNT_TX_DELAY_MS	20
/*
 * Backoff for a failed VEIP oper-up AVC transmit. Rate limited rather than
 * count limited, as the OLT never asks for this AVC again.
 */
#define CG_VEIP_AVC_RETRY_MIN_MS	500
#define CG_VEIP_AVC_RETRY_MAX_MS	30000

static void cg_puc_cnt_work(struct work_struct *work)
{
	struct cortina_gpon *cg = container_of(to_delayed_work(work),
					       struct cortina_gpon, puc_cnt_work);

	cg_puc_ctrl_sample(cg);
}

static void cg_led_set(struct cg_led *led, unsigned long on, unsigned long off);

/*
 * Program the GPON MAC identity and datapath, then start the activation FSM.
 * G.984.3 ranging runs in hardware: once the serial number is programmed and
 * onu_ctl.en is set, the MAC sends its Serial_Number PLOAM, answers
 * Assign_ONU-ID / Ranging_Time and advances onu.state to O5. Serial number and
 * config must be written while en = 0.
 */
static void cg_mac_activate(struct cortina_gpon *cg)
{
	void __iomem *mac = cg->mac;
	u32 v;
	int i;

	/* the boot blink ends here: from now on the LEDs show the PON */
	cg_led_set(&cg->led_los, 0, 0);

	/*
	 * The PON/GPON reset release already happened at the end of
	 * cg_psds_init(); writing it again here could disturb a running FSM.
	 */

	/*
	 * Release the laser TX-disable net before programming the BOSA, so the
	 * GN25L95 safe-mode start runs with TX_DIS released. Programming it
	 * with TX_DIS asserted leaves 0x6e bit7 set (TX fault) and the laser
	 * dark.
	 */
	if (cg->tx_allowed) {
		cg_tx_pins(cg, true);
		mdelay(10);	/* let the TX_DIS net settle before the i2c stream */
	}

	/*
	 * Program the GN25L95 BOSA laser driver over I2C (bias/mod/APD DAC
	 * tables, alarm thresholds, TX gate) before ranging starts: out of
	 * power-on reset it is unprogrammed and the laser never bursts.
	 */
	if (cg->tx_allowed) {
		if (cg_bosa_init(cg->dev, cg->bosa_cal))
			dev_warn(cg->dev, "BOSA init failed - upstream laser will not burst\n");
		else
			dev_dbg(cg->dev, "GN25L95 BOSA programmed from calibration data\n");
	} else {
		dev_dbg(cg->dev, "BOSA init SKIPPED (bosa_init=0) - upstream laser will not burst\n");
	}

	/* --- config while en=0 (serial number is range-critical) --- */
	writel(CG_ONU_CFG_VAL, mac + CG_REG_ONU_CFG_REAL);	/* laser_on_align=0x12, pre_bias=18 */
	/*
	 * The PON identity from cg->sn; the OMCI responder uses the same bytes.
	 */
	writel(cg_sn_word(cg->sn), mac + CG_REG_VENDOR);	/* 4 ASCII vendor-id chars */
	writel(cg_sn_word(cg->sn + 4), mac + CG_REG_VENDOR_SPEC);	/* 4 VSSN bytes */
	/* datapath: gpon_ds.max_packet_size (bits 29:16) = 0x3FFF */
	v = readl(mac + CG_REG_GPON_DS);
	v = (v & ~(0x3fffu << 16)) | (0x3fffu << 16);
	writel(v, mac + CG_REG_GPON_DS);
	/*
	 * PDC: route the OMCC downstream GEMs to the CPU. Only touches the PDC
	 * sub-block, so it is safe before ranging.
	 */
	cg_pdc_init(cg);
	/*
	 * PUC: upstream admission of CPU-injected OMCI to the OMCC T-CONT/GEM,
	 * right after the PDC.
	 */
	cg_puc_init(cg);

	/*
	 * Set GPON MAC configuration registers to the vendor firmware values.
	 * Ranging state (onu_ctl/onu_cfg), the registration password, indirect
	 * access latches and counters are not included.
	 */
	{
		static const struct { u16 off; u32 val; } stockmac[] = {
			{ 0x00c, 0x00000040 },
			{ 0x078, 0x00000060 },
			{ 0x084, 0x00000000 },
			{ 0x08c, 0x00000000 },
			{ 0x09c, 0x00000103 },
			{ 0x0fc, 0x12409c00 },
			{ 0x170, 0x0000000f },
			{ 0x174, 0x000000e1 },
			{ 0x1c4, 0x1f400000 },
			{ 0xe14, 0x00000002 },
		};
		unsigned int i;

		for (i = 0; i < ARRAY_SIZE(stockmac); i++)
			writel(stockmac[i].val, mac + stockmac[i].off);
		dev_dbg(cg->dev,
			"GPON MAC set to vendor config; 170=%08x 174=%08x 09c=%08x 0fc=%08x\n",
			readl(mac + 0x170), readl(mac + 0x174),
			readl(mac + 0x09c), readl(mac + 0x0fc));
	}
	/*
	 * Registration password, written with the serial number before
	 * onu_ctl.en, since the OLT may request it during ranging. Skipped when
	 * unset.
	 */
	if (memchr_inv(cg->password, 0, sizeof(cg->password))) {
		writel((cg->password[0] << 8) | cg->password[1],
		       mac + CG_REG_PFRAG0);
		writel(cg_sn_word(cg->password + 2), mac + CG_REG_PFRAG1);
		writel(cg_sn_word(cg->password + 6), mac + CG_REG_PFRAG2);
		dev_dbg(cg->dev, "registration password programmed\n");
	}

	/* Wait for downstream lock (RGB8 bit15) before enabling ranging. */
	for (i = 0; i < 8000; i++) {
		if ((readl(cg->pon + CG_PSDS_RGB8) & 0x9c01) == 0x9c00)
			break;
		mdelay(1);
	}
	dev_dbg(cg->dev, "activate: DS-lock wait done at %dms, rgb8=0x%08x\n",
		i, readl(cg->pon + CG_PSDS_RGB8));

	/* start ranging */
	/*
	 * ben_oen was set in cg_psds_init(); hardware gates the actual burst
	 * per grant.
	 */
	/*
	 * onu_ctl.en: hardware starts ranging O1 -> O5. Nothing else may be
	 * written near here: the neighbouring registers are the downstream
	 * PLOAM RX FIFO.
	 */
	writel(CG_ONU_CTL_VAL, mac + CG_REG_ONU_CTL);
	/*
	 * Stop the SN random delay engine right after enabling the MAC, as the
	 * vendor does; left running, the Serial_Number burst delay changes
	 * every frame and the OLT cannot decode it. Clear pti_omci too.
	 */
	v = readl(mac + CG_REG_GPON_MAC_CTRL);
	writel(v & ~(CG_MAC_CTRL_SW_RANDOM_EN | CG_MAC_CTRL_PTI_OMCI),
	       mac + CG_REG_GPON_MAC_CTRL);
}

static void cg_led_update(struct cortina_gpon *cg, u8 state);

/*
 * Program the identity, start ranging, and check the vendor-id readback against
 * what was written (catches a wrong window base or a gated MAC). Runs only from
 * cg_activate_work, after a valid serial number was put in cg->sn.
 */
static void cg_activate_start(struct cortina_gpon *cg)
{
	char sn_str[13];
	u32 vid;

	cg->activate_jiffies = jiffies;
	cg_led_update(cg, CG_ONU_STATE(readl(cg->mac + CG_REG_GPON_ONU)));
	cg_sn_format(cg->sn, sn_str);
	dev_dbg(cg->dev, "activating with serial number %s (source: %s)\n",
		sn_str, cg_sn_src_name[cg->sn_src]);

	cg_mac_activate(cg);
	cg->activated = true;

	vid = readl(cg->mac + CG_REG_VENDOR);
	if (vid != cg_sn_word(cg->sn))
		dev_warn(cg->dev,
			 "vendor-id readback 0x%08x != programmed 0x%08x - PON window base wrong, or the MAC is still gated\n",
			 vid, cg_sn_word(cg->sn));

	/* Post-activation register snapshot. */
	dev_dbg(cg->dev,
		"activate: vendor-id=0x%08x vendor-spec=0x%08x onu_cfg=0x%08x onu_ctl=0x%08x gpon_ds=0x%08x onu=0x%08x rgb8=0x%08x\n",
		vid, readl(cg->mac + CG_REG_VENDOR_SPEC),
		readl(cg->mac + CG_REG_ONU_CFG_REAL),
		readl(cg->mac + CG_REG_ONU_CTL),
		readl(cg->mac + CG_REG_GPON_DS),
		readl(cg->mac + CG_REG_GPON_ONU),
		readl(cg->pon + CG_PSDS_RGB8));

	/*
	 * Arm the cold-start recovery watchdog: if the hardware FSM is still at
	 * O1 after a grace period (cold TX PLL lock problem), relock the SerDes
	 * CMU and re-arm ranging until it advances. mod_delayed_work() so a
	 * re-activation on a live link starts a fresh check.
	 */
	mod_delayed_work(system_wq, &cg->coldstart_work, CG_COLD_CHECK);
}

/*
 * Latch a serial number and (re)start ranging with it. The /proc write, the
 * module parameter and the timeout fallback all go through here, so the MAC
 * registers and the OMCI responder always use the same 8 bytes. A late identity
 * re-runs the activation, but only when it differs, so a duplicate write never
 * disturbs a working link.
 */
static int cg_sn_set(struct cortina_gpon *cg, const char *s, enum cg_sn_src src)
{
	u8 sn[8];
	char sn_str[13];
	bool changed;
	int ret;

	ret = cg_sn_parse(s, sn);
	if (ret) {
		dev_err(cg->dev, "rejected GPON serial number \"%s\": expected 4 vendor-id characters + 8 hex digits\n",
			s ? s : "");
		return ret;
	}

	mutex_lock(&cg->sn_lock);
	changed = cg->sn_src == CG_SN_NONE || memcmp(cg->sn, sn, sizeof(sn));
	memcpy(cg->sn, sn, sizeof(sn));
	cg->sn_src = src;
	cg_sn_format(cg->sn, sn_str);

	if (cg->activated && !changed)
		dev_dbg(cg->dev, "serial number %s re-confirmed (source: %s) - link untouched\n",
			sn_str, cg_sn_src_name[src]);
	else {
		if (cg->activated)
			dev_warn(cg->dev, "serial number CHANGED to %s (source: %s) - re-ranging\n",
				sn_str, cg_sn_src_name[src]);
		cancel_delayed_work(&cg->sn_wait_work);
		/* ranging takes seconds; the xPON identity call must not wait */
		schedule_work(&cg->activate_work);
	}
	mutex_unlock(&cg->sn_lock);
	cx_notify(CX_EV_IDENTITY);
	return 0;
}

/*
 * The only caller of cg_activate_start(), so activations never overlap. It
 * runs without sn_lock: ranging takes seconds and identity writes must not
 * wait for it. A write during a run queues this work again, so the last
 * identity always ends up programmed.
 */
static void cg_activate_work(struct work_struct *work)
{
	cg_activate_start(container_of(work, struct cortina_gpon, activate_work));
}

/*
 * No serial number arrived in time. Never guess the identity: log an error,
 * range with the placeholder so the failure is visible at the OLT too, and
 * accept the real serial number later.
 */
static void cg_sn_wait_work(struct work_struct *work)
{
	struct cortina_gpon *cg = container_of(to_delayed_work(work),
					       struct cortina_gpon, sn_wait_work);
	char sn_str[13];

	mutex_lock(&cg->sn_lock);
	if (cg->sn_src != CG_SN_NONE) {		/* raced with a provisioning write */
		mutex_unlock(&cg->sn_lock);
		return;
	}
	memcpy(cg->sn, cg_sn_unprovisioned, sizeof(cg->sn));
	cg->sn_src = CG_SN_FALLBACK;
	cg_sn_format(cg->sn, sn_str);
	dev_err(cg->dev,
		"no GPON serial number after %ds (is omci-agent running?); ranging with placeholder %s. Set it with: echo \"sn <VVVVHHHHHHHH>\" > /proc/gpon\n",
		CG_SN_WAIT_SECS, sn_str);
	schedule_work(&cg->activate_work);
	mutex_unlock(&cg->sn_lock);
}

/*
 * us.frame_var: compensate the upstream burst position for the OLT's
 * Extended_Burst_Length (the MAC latches the Type-3 preamble lengths into
 * t3_preamble). Recomputed on every downstream PLOAM, as the vendor does:
 *     frame_var = 0x200 - ((pre_range + ranged + 0x20) & 0xff)
 * At the reset value (0) the burst is misaligned in the grant window and the
 * OLT cannot hear the ONU. eqd_select (bit 16) stays 0.
 */
static void cg_frame_var_update(struct cortina_gpon *cg)
{
	u32 t3 = readl(cg->mac + CG_REG_T3_PREAMBLE);
	u32 pre = t3 & 0xff, ranged = (t3 >> 8) & 0xff;
	u32 us, fv;

	/*
	 * Recompute even when the latch is empty, or a previous OLT's
	 * compensation stays in place after a fibre swap. Empty gives the
	 * standard default: (0x200 - 0x20) & 0x1ff = 0x1e0.
	 */
	fv = (0x200 - ((pre + ranged + 0x20) & 0xff)) & 0x1ff;
	us = readl(cg->mac + CG_REG_US);
	if ((us & 0x1ff) == fv)
		return;
	writel((us & ~0x1ffu) | fv, cg->mac + CG_REG_US);
	dev_dbg(cg->dev, "us.frame_var = 0x%03x (t3_preamble 0x%08x)\n", fv, t3);
}

static inline u32 cg_mac_rd(struct cortina_gpon *cg, u32 off)
{
	return readl(cg->mac + off);
}

/*
 * Re-arm the GPON MAC interrupt enables (the four W1C groups and int_top). The
 * GLB aggregation gates and the requested IRQ survive a GTC reset, so a
 * cold-start retry only needs this. Shared by cg_intr_setup() and the watchdog.
 */
/* The OLT's downstream FEC setting, known only from its on/off edges. */
static void cg_ds_fec_event(struct cortina_gpon *cg, u32 intr)
{
	s8 fec;

	if (!(intr & (CG_INT_DS_FEC_ON | CG_INT_DS_FEC_OFF)))
		return;
	/* a lock after a reset shows on; both edges latched end the same way */
	fec = !!(intr & CG_INT_DS_FEC_ON);
	if (fec != cg->ds_fec)
		dev_dbg(cg->dev, "downstream FEC %s by OLT\n",
			 fec ? "enabled" : "disabled");
	cg->ds_fec = fec;
}

static void cg_mac_intr_arm(struct cortina_gpon *cg)
{
	static const struct { u32 sts, en, mask; } grp[4] = {
		{ CG_REG_INT,  CG_REG_INT_EN,  CG_INT_EN_DEFAULT },
		{ CG_REG_INT2, CG_REG_INT2_EN, 0 },
		{ CG_REG_INT3, CG_REG_INT3_EN, 0 },
		{ CG_REG_INT4, CG_REG_INT4_EN, 0 },
	};
	int i;

	writel(0, cg->mac + CG_REG_INT_TOP_EN);
	(void)readl(cg->mac + CG_REG_INT_TOP);		/* read-clear stale */
	/*
	 * The downstream may have locked before this arm (at probe, or after
	 * the cold-start reset): keep its FEC edge before clearing it.
	 */
	cg_ds_fec_event(cg, readl(cg->mac + CG_REG_INT));
	for (i = 0; i < 4; i++) {
		writel(0, cg->mac + grp[i].en);
		writel(grp[i].mask, cg->mac + grp[i].sts);	/* W1C stale */
		writel(grp[i].mask, cg->mac + grp[i].en);
	}
	writel(CG_INT_TOP_EN_ALL, cg->mac + CG_REG_INT_TOP_EN);
}

/*
 * Cold-start recovery watchdog. Ranging runs in hardware and servicing is
 * interrupt driven, so a boot whose bring-up comes up wedged sits at O1 forever
 * with no event to recover it.
 *
 * Stuck signature: state O1, ONU-ID 0xff, t3_preamble 0 and no MAC interrupts,
 * while the downstream is locked. The framer syncs but decodes no downstream
 * PLOAM, and a CMU strobe alone does not clear it. Recovery re-runs the whole
 * bring-up: cg_glb_reset() power-cycles the SerDes and re-asserts the
 * GTC/gearbox resets, cg_psds_init() brings it up again, then the MAC
 * interrupts and ranging are re-armed. Each retry is an independent attempt.
 *
 * Only the stuck signature (O1 with downstream locked) triggers a retry. After
 * O5 the same work acts as the post-O5 supervisor; cg_datapath_reset() re-arms
 * it on every O5 exit.
 */
/*
 * Fast retries per stuck-O1 episode (reset when the FSM leaves O1). After that
 * the watchdog warns once and retries every 60 s, forever: a retry at O1 is
 * invisible to the OLT, and transient causes (OLT still settling, ONU
 * administratively disabled) must still recover once they clear.
 */
#define CG_COLD_FAST_TRIES	12
/*
 * Post-O5 supervisor period. At O5 this work only re-kicks cg_isr_work, whose
 * tail reconciles soft state against the live FSM register, so a lost interrupt
 * costs at most one period of OMCI silence.
 */
static void cg_reinit_link(struct cortina_gpon *cg)
{
	cg_glb_reset(cg);
	cg_psds_init(cg);
	cg_mac_intr_arm(cg);	/* the GTC reset cleared the MAC int enables */
	/* sn_lock: cg_mac_activate() programs the identity from cg->sn */
	mutex_lock(&cg->sn_lock);
	cg_mac_activate(cg);	/* re-config and re-assert onu_ctl.en */
	mutex_unlock(&cg->sn_lock);
}

#define CG_O5_SUPERVISOR_SECS	30
static void cg_coldstart_work(struct work_struct *work)
{
	struct cortina_gpon *cg = container_of(to_delayed_work(work),
					       struct cortina_gpon, coldstart_work);
	u32 onu = cg_mac_rd(cg, CG_REG_GPON_ONU);
	u32 rgb8 = readl(cg->pon + CG_PSDS_RGB8);
	u8 state = CG_ONU_STATE(onu);
	bool ds_locked = (rgb8 & 0x9c01) == 0x9c00;

	if (state != 0) {			/* left O1: ranging is progressing */
		cg->coldstart_tries = 0;	/* fresh episode = fresh fast budget */
		if (state != CG_STATE_OPERATION) {
			schedule_delayed_work(&cg->coldstart_work, 5 * HZ);
			return;
		}
		/*
		 * At O5 this work becomes the slow post-O5 supervisor, so a
		 * lost edge (an event dropped by a full ring, or a table op
		 * that timed out in cg_data_sync()) cannot leave soft
		 * state out of step with hardware until the OLT deactivates the
		 * ONU.
		 *
		 * It only re-kicks isr_work, the single-threaded bottom half
		 * that is the only context allowed to run cg_tbl_op(). The
		 * SerDes retry below is gated on O1, so it is unreachable from
		 * here. On a converged link the reconcile writes nothing.
		 */
		/*
		 * Finish a PDC or PUC init that hit an indirect access timeout.
		 * Both are idempotent table writes, but never re-run them over
		 * an installed data path, which uses the PUC VoQ admission.
		 */
		if ((!cg->pdc_ready || !cg->puc_ready) && !cg->data_installed) {
			dev_warn(cg->dev,
				 "O5 supervisor: re-running %s%sinit\n",
				 cg->pdc_ready ? "" : "PDC ",
				 cg->puc_ready ? "" : "PUC ");
			if (!cg->pdc_ready)
				cg_pdc_init(cg);
			if (!cg->puc_ready)
				cg_puc_init(cg);
		}
		schedule_work(&cg->isr_work);
		schedule_delayed_work(&cg->coldstart_work,
				      CG_O5_SUPERVISOR_SECS * HZ);
		return;
	}
	if (!ds_locked) {
		/*
		 * No downstream lock: the RX is still settling or the fiber is
		 * dark. A retry cannot help and would pulse the laser into a
		 * dark fiber, so just wait.
		 */
		cg->coldstart_lof = 0;
		cg->coldstart_sync = 0;
		schedule_delayed_work(&cg->coldstart_work, 3 * HZ);
		return;
	}
	if (cg_mac_rd(cg, CG_REG_ALARM)) {
		cg->coldstart_sync = 0;
		if (++cg->coldstart_lof < CG_COLD_LOF_CHECKS) {
			schedule_delayed_work(&cg->coldstart_work, CG_COLD_CHECK);
			return;
		}
	} else {
		cg->coldstart_lof = 0;
		if (!cg->coldstart_sync)
			cg->coldstart_sync = jiffies;
		if (time_before(jiffies, cg->coldstart_sync + CG_COLD_PLOAM_WAIT)) {
			schedule_delayed_work(&cg->coldstart_work, CG_COLD_CHECK);
			return;
		}
	}
	cg->coldstart_lof = 0;
	cg->coldstart_sync = 0;
	/* O1 with downstream locked: the stuck signature */
	cg->coldstart_tries++;
	cg->coldstart_rolls++;
	if (cg->coldstart_tries == CG_COLD_FAST_TRIES)
		dev_warn(cg->dev,
			 "cold-start recovery: %d fast retries, still O1 - retrying every 60s\n",
			 cg->coldstart_tries);
	dev_dbg(cg->dev,
		"cold-start stuck O1, DS locked but no PLOAM (onu=0x%08x rgb8=0x%08x us=0x%08x t3=0x%08x) - SerDes restart #%u\n",
		onu, rgb8, cg_mac_rd(cg, CG_REG_US), cg_mac_rd(cg, CG_REG_T3_PREAMBLE),
		cg->coldstart_rolls);

	/*
	 * Re-run the whole bring-up. Each attempt is internally bounded (SerDes
	 * lock <= 1 s, downstream wait <= 8 s).
	 */
	cg_reinit_link(cg);
	schedule_delayed_work(&cg->coldstart_work,
			      cg->coldstart_tries >= CG_COLD_FAST_TRIES ?
			      60 * HZ : CG_COLD_CHECK);
}

/*
 * Do not read the TX PLOAM MIB (indirect ACCESS/DATA pair at +0x184/+0x188)
 * from the driver: driving that engine during activation wedges the PON PLOAM
 * engine and keeps the FSM at O1.
 */

/*
 * Post-O5 servicing: interrupt handler, FSM tracker and OMCC channel bind.
 *
 * The MAC ranges O1 -> O5 by itself and acknowledges all mandatory PLOAMs;
 * the outcomes software must act on arrive as INTERRUPT sources:
 *   ONU_ST_CHG(31) FSM moved     -> track state, link up/down, datapath reset
 *   ONU_ID(30)     ONU-ID given  -> bind OMCC T-CONT[0] to alloc = ONU-ID
 *   PORTID(17)     OMCI port set -> bind US GEMs 0..7 to omci_port.id
 *   KSW(19)/DACT(8)              -> logged only
 */

static const char *const cg_state_name[8] = {
	"O1-Initial", "O2-Standby", "O3-SerialNumber", "O4-Ranging",
	"O5-Operation", "O6-POPUP", "O7-EmergencyStop", "unknown",
};

static void cg_led_apply(struct led_classdev *l, unsigned long on,
			 unsigned long off)
{
	/* a blink setting also ends a running blink, a brightness does not */
	if (on)
		led_blink_set(l, &on, &off);
	else
		led_set_brightness(l, LED_OFF);
}

static int cg_led_activate(struct led_classdev *l)
{
	struct cg_led *led = container_of(l->trigger, struct cg_led, trig);

	cg_led_apply(l, led->on, led->off);
	return 0;
}

static void cg_led_set(struct cg_led *led, unsigned long on, unsigned long off)
{
	struct led_classdev *l;

	led->on = on;
	led->off = off;
	rcu_read_lock();
	list_for_each_entry_rcu(l, &led->trig.led_cdevs, trig_list)
		cg_led_apply(l, on, off);
	rcu_read_unlock();
}

static void cg_led_unregister(void *data)
{
	struct cortina_gpon *cg = data;

	cancel_delayed_work_sync(&cg->los_work);
	led_trigger_unregister(&cg->led_link.trig);
	led_trigger_unregister(&cg->led_los.trig);
}

/*
 * The LOS LED blinks like a router's status LED from probe until the
 * laser is about to start, then shows only a loss of signal that lasts:
 * not the brief alarm before the downstream locks.
 */
#define CG_BOOT_LED_MS		200
#define CG_LOS_LED_MS		500
#define CG_LOS_LED_DELAY	HZ

static int cg_led_register(struct cortina_gpon *cg)
{
	int ret;

	cg->led_link = (struct cg_led) {
		.trig = { .name = "gpon-link", .activate = cg_led_activate },
	};
	cg->led_los = (struct cg_led) {
		.trig = { .name = "gpon-los", .activate = cg_led_activate },
		.on = CG_BOOT_LED_MS, .off = CG_BOOT_LED_MS,
	};
	ret = led_trigger_register(&cg->led_link.trig);
	if (ret)
		return ret;
	ret = led_trigger_register(&cg->led_los.trig);
	if (ret) {
		led_trigger_unregister(&cg->led_link.trig);
		return ret;
	}
	return devm_add_action_or_reset(cg->dev, cg_led_unregister, cg);
}

static bool cg_los(struct cortina_gpon *cg)
{
	return CG_ONU_STATE(readl(cg->mac + CG_REG_GPON_ONU)) == CG_STATE_INITIAL &&
	       readl(cg->mac + CG_REG_ALARM);
}

static void cg_los_work(struct work_struct *work)
{
	struct cortina_gpon *cg = container_of(to_delayed_work(work),
					       struct cortina_gpon, los_work);

	if (cg_los(cg))
		cg_led_set(&cg->led_los, CG_LOS_LED_MS, CG_LOS_LED_MS);
}

/*
 * The PON and LOS LEDs as ONTs show them: PON on once registered (O5),
 * blinking while the OLT ranges the ONU (O2-O4); LOS blinking without light.
 */
static void cg_led_update(struct cortina_gpon *cg, u8 state)
{
	if (state == CG_STATE_OPERATION || state == CG_STATE_POPUP)
		cg_led_set(&cg->led_link, 1, 0);
	else if (state > CG_STATE_INITIAL && state < CG_STATE_OPERATION)
		cg_led_set(&cg->led_link, CG_LOS_LED_MS, CG_LOS_LED_MS);
	else
		cg_led_set(&cg->led_link, 0, 0);

	if (state == CG_STATE_INITIAL && readl(cg->mac + CG_REG_ALARM)) {
		schedule_delayed_work(&cg->los_work, CG_LOS_LED_DELAY);
	} else {
		cancel_delayed_work(&cg->los_work);
		cg_led_set(&cg->led_los, 0, 0);
	}
}

/* The activation milestones the operator needs, as the vendor firmware logs them. */
static void cg_log_state(struct cortina_gpon *cg, u8 last, u8 state)
{
	cg_led_update(cg, state);

	if (state == CG_STATE_OPERATION && last != CG_STATE_POPUP) {
		dev_info(cg->dev, "registered with OLT (O5, onu-id %u), %u ms\n",
			 cg->omcc_alloc,
			 jiffies_to_msecs(jiffies - cg->activate_jiffies));
		/* the laser has settled by then */
		schedule_delayed_work(&cg->optic_log_work, 10 * HZ);
	} else if (state == CG_STATE_INITIAL) {
		if (!cg->los_jiffies && cg_mac_rd(cg, CG_REG_ALARM)) {
			cg->los_jiffies = jiffies ?: 1;
			dev_warn(cg->dev, "downstream optical signal lost\n");
		}
	} else if (last == CG_STATE_INITIAL) {
		if (cg->los_jiffies)
			dev_info(cg->dev, "downstream optical signal restored (%u ms)\n",
				 jiffies_to_msecs(jiffies - cg->los_jiffies));
		else
			dev_dbg(cg->dev, "downstream optical signal detected\n");
		cg->los_jiffies = 0;
		cg->activate_jiffies = jiffies;
	}
}

/*
 * One indirect table transaction: write ACCESS with go (bit31) plus the
 * command, then poll for go to clear (bounded at 10000 reads). Only runs in the
 * single-threaded work context.
 */
static int cg_tbl_op(struct cortina_gpon *cg, u32 access_reg, u32 cmd)
{
	int i;

	writel(CG_TBL_GO | cmd, cg->mac + access_reg);
	for (i = 0; i < 10000; i++) {
		if (!(readl(cg->mac + access_reg) & CG_TBL_GO))
			return 0;
	}
	dev_warn(cg->dev, "indirect access +0x%03x cmd 0x%08x timed out\n",
		 access_reg, cmd);
	return -ETIMEDOUT;
}

static int cg_us_mib_read(struct cortina_gpon *cg, u32 idx, u32 op,
			  u32 *pcnt, u32 *fcnt, u64 *bcnt)
{
	u32 d3, d2, d1, d0;
	int ret;

	if (idx > CG_US_PORT_IDX_MAX)
		return -EINVAL;
	/*
	 * Single access write go | op_code[29:28] | sel[7:0], poll go, then
	 * read DATA3..DATA0 in descending address order as the vendor does
	 * (reads in this register family can have side effects). op 2 is
	 * read-clear. Only op 0 and op 2 are used by the vendor firmware.
	 */
	/*
	 * With rbw (BIT(30)) set the index selects a GEM; without it every
	 * index reads the same counter. The upstream OMCI counts appear at the
	 * index in US_OMCI_HP_HDR_A (7 by default).
	 */
	ret = cg_tbl_op(cg, CG_REG_US_MIB_ACCESS,
			CG_TBL_WR |
			FIELD_PREP(CG_US_MIB_OP_CODE, op & 3) | (idx & 0xff));
	if (ret)
		return ret;
	d3 = readl(cg->mac + CG_REG_US_MIB_DATA3);
	d2 = readl(cg->mac + CG_REG_US_MIB_DATA2);
	d1 = readl(cg->mac + CG_REG_US_MIB_DATA1);
	d0 = readl(cg->mac + CG_REG_US_MIB_DATA0);
	*pcnt = d0;
	*fcnt = d1;
	*bcnt = ((u64)d3 << 32) | d2;
	return 0;
}

/*
 * Print the non-zero upstream MIB counters (op=0) for active VoQs/GEMs.
 */
static void cg_us_mib_show(struct cortina_gpon *cg, struct seq_file *m)
{
	unsigned int k;

	if (!m)
		return;

	for (k = 0; k < 16; k++) {
		u32 p = 0, f = 0;
		u64 b = 0;

		if (cg_us_mib_read(cg, k, 0, &p, &f, &b))
			continue;
		if (p || f || b)
			seq_printf(m, "us_mib[%u]      = pkt=%u frm=%u byte=%llu\n",
				   k, p, f, b);
	}
}

/*
 * Invalidate a stale T-CONT CAM entry: read-modify-write clear omci_en,
 * ploam_en and the hardware T-CONT index for @alloc. Used when a different
 * alloc replaces a live binding, so grants to a reassigned alloc no longer make
 * this ONU burst.
 */
static void cg_tcont_unbind(struct cortina_gpon *cg, u32 alloc)
{
	u32 d;

	alloc &= 0xfff;
	if (cg_tbl_op(cg, CG_REG_TCONT_ACCESS, alloc))
		return;
	d = readl(cg->mac + CG_REG_TCONT_DATA);
	d &= ~(CG_TCONT_OMCI_EN | CG_TCONT_PLOAM_EN | CG_TCONT_INDEX_MASK);
	writel(d, cg->mac + CG_REG_TCONT_DATA);
	if (cg_tbl_op(cg, CG_REG_TCONT_ACCESS, CG_TBL_WR | alloc))
		return;
	dev_dbg(cg->dev, "T-CONT CAM[alloc %u] invalidated (stale)\n", alloc);
}

/*
 * Bind the OMCC in the T-CONT table: entry[alloc-id = ONU-ID] -> hardware
 * T-CONT 0 with omci_en and ploam_en (G.984.3: the default alloc-id equals the
 * ONU-ID). Read-modify-write of the indirect entry.
 *
 * If a different ONU-ID replaces the binding, invalidate the old entry first so
 * it can never burst into a reassigned grant. This runs during ranging, so the
 * CAM is correct before O5. A re-range with the same ID rewrites the same
 * value.
 */
static void cg_omcc_tcont_bind(struct cortina_gpon *cg, u32 alloc_id)
{
	u16 old = cg->omcc_alloc;
	u32 d;

	alloc_id &= 0xfff;
	/* ONU-ID 0 is legal, hence the explicit validity flag */
	if (cg->omcc_alloc_valid && old != alloc_id)
		cg_tcont_unbind(cg, old);	/* invalidate the stale OMCC entry */

	if (cg_tbl_op(cg, CG_REG_TCONT_ACCESS, alloc_id))
		return;
	d = readl(cg->mac + CG_REG_TCONT_DATA);
	d &= ~CG_TCONT_INDEX_MASK;			/* index = 0 (OMCC T-CONT) */
	d |= CG_TCONT_OMCI_EN | CG_TCONT_PLOAM_EN;
	writel(d, cg->mac + CG_REG_TCONT_DATA);
	if (cg_tbl_op(cg, CG_REG_TCONT_ACCESS, CG_TBL_WR | alloc_id))
		return;

	cg->omcc_alloc = alloc_id;
	/*
	 * Only set once both table ops succeeded; after a timeout the post-O5
	 * supervisor retries the bind.
	 */
	cg->omcc_alloc_valid = true;
	dev_dbg(cg->dev, "OMCC: T-CONT[0] bound to alloc-id %u\n", cg->omcc_alloc);
}

/*
 * Bind the OMCC upstream GEM: point the OMCC upstream GEM slots 0..7 at the
 * OLT-assigned omci_port.id.
 */
static void cg_omcc_gem_bind(struct cortina_gpon *cg, u32 gem_id)
{
	u32 idx, d, n;

	for (n = 0; n < cg_us_omcc_slots.count; n++) {
		idx = gpon_gem_us_index(&cg_us_omcc_slots, n);
		if (cg_tbl_op(cg, CG_REG_US_PORT_ACCESS, idx))
			return;
		d = readl(cg->mac + CG_REG_US_PORT_DATA);
		d = (d & ~0xfffu) | gpon_gem_us_port_id(gem_id);
		writel(d, cg->mac + CG_REG_US_PORT_DATA);
		if (cg_tbl_op(cg, CG_REG_US_PORT_ACCESS, CG_TBL_WR | idx))
			return;
	}

	cg->omcc_gem = gpon_gem_us_port_id(gem_id);
	dev_dbg(cg->dev, "OMCC: us-gem 0..%d bound to GEM port-id %u\n",
		CG_OMCC_US_GEM_IDX_NUM - 1, cg->omcc_gem);
}

/*
 * DS GEM CAM: entry[GEM Port-ID] -> {internal GEM index, vld}. Read-modify-
 * write, as the vendor does: the MAC sets the aes bit itself when the OLT
 * sends Encrypted_Port-ID (it also runs Request_Key and Key_Switching_Time),
 * so an encrypted GEM must keep it across a rebind.
 */
static int cg_ds_gem_bind(struct cortina_gpon *cg, u32 gem_id, u32 idx)
{
	u32 d;

	if (cg_tbl_op(cg, CG_REG_DS_GEM_ACCESS, gem_id & 0xfff))
		return -ETIMEDOUT;
	d = readl(cg->mac + CG_REG_DS_GEM_DATA);
	d &= ~(CG_DS_GEM_VLD | CG_DS_GEM_INDEX(0xff));
	d |= CG_DS_GEM_VLD | CG_DS_GEM_INDEX(idx);
	writel(d, cg->mac + CG_REG_DS_GEM_DATA);
	return cg_tbl_op(cg, CG_REG_DS_GEM_ACCESS, CG_TBL_WR | (gem_id & 0xfff));
}

/*
 * Invalidate a DS GEM CAM entry (vld = 0, index = 0) so a reassigned downstream
 * GEM no longer reaches this ONU.
 */
static int cg_ds_gem_unbind(struct cortina_gpon *cg, u32 gem_id)
{
	if (cg_tbl_op(cg, CG_REG_DS_GEM_ACCESS, gem_id & 0xfff))
		return -ETIMEDOUT;
	writel(0, cg->mac + CG_REG_DS_GEM_DATA);	/* vld=0, index=0 */
	return cg_tbl_op(cg, CG_REG_DS_GEM_ACCESS, CG_TBL_WR | (gem_id & 0xfff));
}

/*
 * Downstream classifier route for one internal GEM index: to CPU port 0, VoQ
 * CA_NI_RX_WAN_VOQ (its own NAPI), with
 * forwarding-engine bypass, lspid = PON (the NI CPU-RX WAN delivery key). With
 * the flow engine active a unicast data GEM instead goes to LDPID L3_WAN (0x18)
 * without bypass, into the L3FE WAN ingress; a hash miss is punted to CPU_0,
 * so unmatched traffic still reaches Linux.
 */
static int cg_ds_route(struct cortina_gpon *cg, u32 idx, bool unicast)
{
	u32 d0 = CG_PDC_D0_COS(CA_NI_RX_WAN_VOQ) |
		 CG_PDC_D0_LDPID(CG_LPORT_CPU_0) |
		 CG_PDC_D0_LSPID(CG_LPORT_PON) | CG_PDC_D0_FE_BYPASS |
		 CG_PDC_D0_NO_DROP;

	if (unicast && cortina_ppe_active())
		d0 = CG_PDC_D0_LDPID(CG_LPORT_L3_WAN) |
		     CG_PDC_D0_LSPID(CG_LPORT_PON);
	return cg_pdc_map_write(cg, idx, d0, CG_PDC_D1_POL_ID(idx));
}

static int cg_us_port_write(struct cortina_gpon *cg, u32 idx, u32 port)
{
	u32 d;

	if (cg_tbl_op(cg, CG_REG_US_PORT_ACCESS, idx))
		return -ETIMEDOUT;
	d = readl(cg->mac + CG_REG_US_PORT_DATA);
	d = (d & ~0xfffu) | gpon_gem_us_port_id(port);
	writel(d, cg->mac + CG_REG_US_PORT_DATA);
	return cg_tbl_op(cg, CG_REG_US_PORT_ACCESS, CG_TBL_WR | idx);
}

static bool cg_gem_dir_us(u8 dir)
{
	return dir == CG_GEM_DIR_US || dir == CG_GEM_DIR_BIDIR;
}

static bool cg_gem_dir_ds(u8 dir)
{
	return dir == CG_GEM_DIR_DS || dir == CG_GEM_DIR_BIDIR;
}

/*
 * Bind the alloc-id of data T-CONT @t (hardware T-CONT t + 1) and enable its
 * eight VoQs the way the vendor queue_add does:
 *   (1) TE_CB threshold profile 3 = admission (cortina_ni_qm_voq_admit())
 *   (2) PUC CIR/PIR buckets, then the pvtbl entry and the valid-VoQ bits
 *   (3) L3QM ES enable = not implemented: its register address is not known
 * qm_voq_idx = 128 + puc_voq_idx. Without (2) the OLT grants the T-CONT no
 * upstream bandwidth.
 *
 * An alloc-id equal to the OMCC's is never rebound: that would move the OMCC
 * off hardware T-CONT 0 and the ONU would silently stop being heard.
 */
static int cg_tcont_arm(struct cortina_gpon *cg, unsigned int t, u16 alloc)
{
	u32 hw = t + 1, d, q;

	if (gpon_gem_us_tcont_decide(alloc, cg->omcc_alloc, false) ==
	    GPON_GEM_US_BIND_IS_OMCC) {
		dev_warn(cg->dev,
			 "data alloc %u == OMCC alloc: NOT rebinding CAM (single-alloc OLT?)\n",
			 alloc);
	} else {
		if (cg_tbl_op(cg, CG_REG_TCONT_ACCESS, alloc))
			return -ETIMEDOUT;
		d = readl(cg->mac + CG_REG_TCONT_DATA);
		d &= ~CG_TCONT_INDEX_MASK;
		d |= CG_TCONT_INDEX(hw) | CG_TCONT_OMCI_EN | CG_TCONT_PLOAM_EN;
		writel(d, cg->mac + CG_REG_TCONT_DATA);
		if (cg_tbl_op(cg, CG_REG_TCONT_ACCESS, CG_TBL_WR | alloc))
			return -ETIMEDOUT;
	}

	for (q = 0; q < CG_PUC_QUEUE_PER_TCONT; q++) {
		cortina_ni_qm_voq_admit(128 + hw * CG_PUC_QUEUE_PER_TCONT + q);
		cg_puc_voq_tbc_init(cg, hw * CG_PUC_QUEUE_PER_TCONT + q);
	}
	if (cg_puc_pvtbl_program(cg, hw, true, NULL))
		return -ETIMEDOUT;
	cg_puc_voq_flush(cg, hw);

	cg->hw_tc[t].alloc = alloc;
	dev_dbg(cg->dev, "data T-CONT %u: alloc %u, VoQ %u..%u\n", hw, alloc,
		hw * CG_PUC_QUEUE_PER_TCONT, hw * CG_PUC_QUEUE_PER_TCONT + 7);
	return 0;
}

/*
 * Drain then clear: disable and flush the VoQs first, so the scheduler stops
 * before the CAM changes underneath it, then invalidate the CAM entry so a
 * reassigned alloc can never make this ONU burst. The OMCC's alloc is kept.
 */
static void cg_tcont_disarm(struct cortina_gpon *cg, unsigned int t)
{
	u32 hw = t + 1;
	u16 alloc = cg->hw_tc[t].alloc;

	cg_puc_pvtbl_program(cg, hw, false, NULL);
	cg_puc_voq_flush(cg, hw);
	if (alloc != cg->omcc_alloc)
		cg_tcont_unbind(cg, alloc);
	cg->hw_tc[t].alloc = 0;
	dev_dbg(cg->dev, "data T-CONT %u: alloc %u released\n", hw, alloc);
}

/*
 * Install GEM table entry @n. An upstream GEM gets the first free queue q of
 * its T-CONT: VoQ, upstream port-map slot and HEADER_A pol_id are all
 * hw * 8 + q, and a bidirectional GEM's downstream index is the same number.
 * A downstream-only GEM gets CG_DS_ONLY_IDX(n).
 */
static int cg_gem_install(struct cortina_gpon *cg, unsigned int n,
			  const struct cg_gem *g, int t)
{
	struct cg_hw_gem *h = &cg->hw_gm[n];
	u32 idx, hw, q;

	if (cg_gem_dir_us(g->dir)) {
		if (t < 0)
			return -EAGAIN;		/* its T-CONT has no alloc yet */
		hw = t + 1;
		q = ffz(cg->hw_tc[t].queues);
		if (q >= CG_PUC_QUEUE_PER_TCONT)
			return -ENOSPC;
		idx = hw * CG_PUC_QUEUE_PER_TCONT + q;
		if (cg_us_port_write(cg, idx, g->port))
			return -ETIMEDOUT;
		cg->hw_tc[t].queues |= BIT(q);
	} else {
		idx = CG_DS_ONLY_IDX(n);
	}

	if (cg_gem_dir_ds(g->dir) &&
	    (cg_ds_gem_bind(cg, g->port, idx) ||
	     cg_ds_route(cg, idx, g->dir == CG_GEM_DIR_BIDIR)))
		dev_warn(cg->dev, "GEM %u: downstream bind timed out\n", g->port);

	h->port = g->port;
	h->dir = g->dir;
	h->tcont = cg_gem_dir_us(g->dir) ? t : -1;
	h->idx = idx;
	dev_dbg(cg->dev, "GEM %u (dir %u): index %u%s\n", g->port, g->dir, idx,
		h->tcont >= 0 ? "" : " (downstream only)");
	return 0;
}

static void cg_gem_remove(struct cortina_gpon *cg, unsigned int n)
{
	struct cg_hw_gem *h = &cg->hw_gm[n];

	if (h->tcont >= 0) {
		cg_us_port_write(cg, h->idx, GPON_GEM_US_PORT_NONE);
		cg->hw_tc[h->tcont].queues &= ~BIT(h->idx % CG_PUC_QUEUE_PER_TCONT);
	}
	if (cg_gem_dir_ds(h->dir))
		cg_ds_gem_unbind(cg, h->port);
	dev_dbg(cg->dev, "GEM %u: index %u released\n", h->port, h->idx);
	memset(h, 0, sizeof(*h));
}

/*
 * The WRR weight of each VoQ of data T-CONT @t (ME 262 instance @inst): the
 * agent's weight for the priority of the services on the VoQ's GEM, at least
 * 1 since a 0 would make the VoQ strict.  False for strict priority.
 */
static bool cg_tcont_weights(const struct cortina_gpon *cg,
			     const struct cg_tcont_qos *tq,
			     const struct cg_gem_prio *gp, u16 inst,
			     unsigned int t, u8 *weight)
{
	const struct cg_tcont_qos *qos = NULL;
	unsigned int i, n, q;

	for (i = 0; i < CG_DATA_TCONT_MAX; i++)
		if (tq[i].used && tq[i].inst == inst)
			qos = &tq[i];
	if (!qos || !qos->wrr)
		return false;

	memset(weight, 1, CG_PUC_QUEUE_PER_TCONT);
	for (n = 0; n < CG_DATA_GEM_MAX; n++) {
		const struct cg_hw_gem *h = &cg->hw_gm[n];

		if (!h->idx || h->tcont != t)
			continue;
		q = h->idx % CG_PUC_QUEUE_PER_TCONT;
		for (i = 0; i < CG_DATA_GEM_MAX; i++)
			if (gp[i].used && gp[i].port == h->port)
				weight[q] = max_t(u8, qos->weight[gp[i].prio], 1);
	}
	return true;
}

/*
 * Bring the hardware in line with the OLT's provisioning: up to
 * CG_DATA_TCONT_MAX data T-CONTs (hardware T-CONT 1..) and CG_DATA_GEM_MAX
 * GEM ports, each GEM on the T-CONT its ME 268 points at, plus the downstream
 * broadcast GEM 4095 while any T-CONT is up. Idempotent and runs in isr_work,
 * so the indirect table ops never race the OMCC binds; a converged path writes
 * nothing. Removals run before installs, so a reassigned alloc or GEM never
 * overlaps its old binding.
 *
 * Upstream frames leave on the default GEM: the first upstream GEM of the
 * lowest T-CONT, preferring a bidirectional one. The WAN carrier follows it.
 */
static void cg_data_sync(struct cortina_gpon *cg)
{
	struct cg_tcont tc[CG_DATA_TCONT_MAX];
	struct cg_gem gm[CG_DATA_GEM_MAX];
	struct cg_tcont_qos tq[CG_DATA_TCONT_MAX];
	struct cg_gem_prio gp[CG_DATA_GEM_MAX];
	int tidx[CG_DATA_GEM_MAX];
	u16 want[CG_DATA_TCONT_MAX];
	u8 weight[CG_PUC_QUEUE_PER_TCONT];
	unsigned int n, t, def = 0;
	bool any = false, resched;

	if (!cg->omcc_up)
		return;

	spin_lock_bh(&cg->data_lock);
	memcpy(tc, cg->tc, sizeof(tc));
	memcpy(gm, cg->gm, sizeof(gm));
	memcpy(tq, cg->tq, sizeof(tq));
	memcpy(gp, cg->gp, sizeof(gp));
	resched = cg->qos_dirty;
	cg->qos_dirty = false;
	spin_unlock_bh(&cg->data_lock);

	for (t = 0; t < CG_DATA_TCONT_MAX; t++)
		want[t] = tc[t].used ? tc[t].alloc & 0xfff : 0;

	/* the hardware T-CONT each GEM's T-CONT pointer names, -1 if none */
	for (n = 0; n < CG_DATA_GEM_MAX; n++) {
		tidx[n] = -1;
		for (t = 0; gm[n].used && t < CG_DATA_TCONT_MAX; t++)
			if (want[t] && tc[t].inst == gm[n].tcont_inst)
				tidx[n] = t;
	}

	/* 1. GEMs that changed, or whose T-CONT is about to change */
	for (n = 0; n < CG_DATA_GEM_MAX; n++) {
		struct cg_hw_gem *h = &cg->hw_gm[n];

		if (!h->idx)
			continue;
		if (!gm[n].used || h->port != gm[n].port || h->dir != gm[n].dir ||
		    (h->tcont >= 0 && (h->tcont != tidx[n] ||
				       cg->hw_tc[h->tcont].alloc != want[h->tcont]))) {
			cg_gem_remove(cg, n);
			resched = true;
		}
	}

	/* 2. T-CONTs whose alloc-id changed or went away */
	for (t = 0; t < CG_DATA_TCONT_MAX; t++)
		if (cg->hw_tc[t].alloc && cg->hw_tc[t].alloc != want[t])
			cg_tcont_disarm(cg, t);

	/* the frames queued before a re-range must not burst after it */
	if (cg->data_flush) {
		for (t = 0; t < CG_DATA_TCONT_MAX; t++)
			if (cg->hw_tc[t].alloc)
				cg_puc_voq_flush(cg, t + 1);
		cg->data_flush = false;
	}

	/* 3. new T-CONTs */
	for (t = 0; t < CG_DATA_TCONT_MAX; t++) {
		if (want[t] && !cg->hw_tc[t].alloc && cg_tcont_arm(cg, t, want[t]))
			return;		/* the O5 supervisor retries */
		any |= !!cg->hw_tc[t].alloc;
	}

	/* 4. new GEMs */
	for (n = 0; n < CG_DATA_GEM_MAX; n++)
		if (gm[n].used && !cg->hw_gm[n].idx &&
		    !cg_gem_install(cg, n, &gm[n], tidx[n]))
			resched = true;

	/* 5. T-CONT scheduling, once the VoQs it weighs are known */
	for (t = 0; resched && t < CG_DATA_TCONT_MAX; t++)
		if (cg->hw_tc[t].alloc)
			cg_puc_pvtbl_program(cg, t + 1, true,
					     cg_tcont_weights(cg, tq, gp, tc[t].inst,
							      t, weight) ?
					     weight : NULL);

	if (any != cg->bcast_bound) {
		if (any)
			any = !cg_ds_gem_bind(cg, CG_MCAST_GEM_ID, CG_MCAST_GEM_IDX) &&
			      !cg_ds_route(cg, CG_MCAST_GEM_IDX, false);
		else
			cg_ds_gem_unbind(cg, CG_MCAST_GEM_ID);
		cg->bcast_bound = any;
	}

	/* 5. the default upstream GEM */
	for (n = 0; n < CG_DATA_GEM_MAX; n++) {
		const struct cg_hw_gem *h = &cg->hw_gm[n];

		if (!h->idx || h->tcont < 0)
			continue;
		if (!def || (h->dir == CG_GEM_DIR_BIDIR) >
			    (cg->hw_gm[def - 1].dir == CG_GEM_DIR_BIDIR) ||
		    ((h->dir == CG_GEM_DIR_BIDIR) ==
		     (cg->hw_gm[def - 1].dir == CG_GEM_DIR_BIDIR) &&
		     h->idx < cg->hw_gm[def - 1].idx))
			def = n + 1;
	}

	if (def) {
		const struct cg_hw_gem *h = &cg->hw_gm[def - 1];

		if (READ_ONCE(cg->us_idx) != h->idx) {
			WRITE_ONCE(cg->us_idx, h->idx);
			cortina_ppe_wan_path_set(h->port, h->tcont + 1);
			dev_dbg(cg->dev, "upstream default: GEM %u, VoQ %u\n",
				h->port, h->idx);
		}
	} else if (READ_ONCE(cg->us_idx)) {
		WRITE_ONCE(cg->us_idx, 0);
		cortina_ppe_wan_path_set(0, 0);
	}

	cg->data_installed = def;
	if (cg->wan_ndev) {
		if (def) {
			cancel_delayed_work(&cg->carrier_work);
			netif_carrier_on(cg->wan_ndev);
		} else if (!delayed_work_pending(&cg->carrier_work)) {
			/* an O5 exit holds the carrier (CG_CARRIER_HOLD) */
			netif_carrier_off(cg->wan_ndev);
		}
	}
}

/*
 * How long the WAN keeps its carrier after leaving O5. A PPPoE server keeps
 * the session of a vanished client until its own keepalive gives up (75-114 s
 * seen on the Airtel BNG) and ignores new discovery from the same MAC until
 * then. Dropping the carrier at once makes netifd kill pppd, so every outage
 * cost that wait. Holding it longer than the server's timeout lets pppd's
 * keepalive decide, as behind a stand-alone ONT: a short outage keeps the
 * session, and after a long one the server has already dropped it. Frames
 * sent meanwhile are discarded (no upstream VoQ is bound).
 */
#define CG_CARRIER_HOLD		(120 * HZ)

static void cg_carrier_work(struct work_struct *work)
{
	struct cortina_gpon *cg = container_of(to_delayed_work(work),
					       struct cortina_gpon, carrier_work);

	if (cg->wan_ndev && !READ_ONCE(cg->data_installed))
		netif_carrier_off(cg->wan_ndev);
}

/*
 * Datapath reset on O5 exit. Drops the soft link state and retires hardware
 * data bindings so the next O5 re-arms everything from the preserved OLT
 * provisioning (tc[] / gm[]): leaving O5 into O1 clears the GPON MAC's T-CONT
 * and DS GEM CAMs in hardware, and on a LOS re-range without an OMCI MIB reset
 * the OLT does not resend ME 262 / ME 268. Keeping tc[] and gm[] across the
 * outage lets cg_data_sync() reprogram the hardware CAMs and PUC on O5 return.
 */
static void cg_datapath_reset(struct cortina_gpon *cg)
{
	unsigned int n, t;

	cg->omcc_up = false;
	cx_notify(CX_EV_CHANNEL);
	/*
	 * disarm the responder; the next O5 re-initialises it with a fresh MIB
	 */
	spin_lock_bh(&cg->omci_lock);
	cg->omci_active = false;
	spin_unlock_bh(&cg->omci_lock);
	cancel_delayed_work(&cg->veip_avc_work);
	cancel_delayed_work(&cg->optic_log_work);
	/*
	 * data link down. Stop WAN TX first, then retire the hardware bindings
	 * and flush the VoQs while keeping tc[] / gm[] OLT provisioning intact.
	 */
	cg->data_installed = false;
	cg->data_flush = true;
	mod_delayed_work(system_wq, &cg->carrier_work, CG_CARRIER_HOLD);
	if (READ_ONCE(cg->us_idx)) {
		WRITE_ONCE(cg->us_idx, 0);
		cortina_ppe_wan_path_set(0, 0);
	}
	for (n = 0; n < CG_DATA_GEM_MAX; n++)
		if (cg->hw_gm[n].idx)
			cg_gem_remove(cg, n);
	if (cg->bcast_bound) {
		cg_ds_gem_unbind(cg, CG_MCAST_GEM_ID);
		cg->bcast_bound = false;
	}
	for (t = 0; t < CG_DATA_TCONT_MAX; t++)
		if (cg->hw_tc[t].alloc)
			cg_tcont_disarm(cg, t);
	cg->omcc_alloc_valid = false;
	cg->omcc_gem = 0;
	/*
	 * Re-arm the stuck-O1 watchdog: the analog lock can also be lost
	 * mid-uptime and the re-range can then wedge at O1 the same way. Fresh
	 * episode, fresh fast-retry budget; a synced downstream gets
	 * CG_COLD_PLOAM_WAIT, which leaves a healthy re-range alone.
	 */
	cg->coldstart_tries = 0;
	cg->coldstart_lof = 0;
	cg->coldstart_sync = 0;
	/*
	 * mod_delayed_work(): the post-O5 supervisor keeps this work pending,
	 * and schedule_delayed_work() would keep its old, possibly nearly
	 * expired, deadline.
	 */
	mod_delayed_work(system_wq, &cg->coldstart_work, CG_COLD_CHECK);
	dev_dbg(cg->dev, "O5 exit: datapath reset (OMCC + data down, provisioning kept)\n");
}

/* Try to bring the OMCC link up: needs O5 + HW-filled omci_port.en. */
static void cg_omcc_try_up(struct cortina_gpon *cg, u8 state)
{
	u32 omci_port;

	if (cg->omcc_up || state != CG_STATE_OPERATION)
		return;
	omci_port = cg_mac_rd(cg, CG_REG_OMCI_PORT);
	if (!(omci_port & CG_OMCI_PORT_EN)) {
		dev_dbg(cg->dev, "O5 but omci_port not enabled yet (0x%08x)\n",
			omci_port);
		return;
	}
	cg_omcc_gem_bind(cg, CG_OMCI_PORT_ID(omci_port));
	cg->omcc_up = true;
	dev_info(cg->dev, "OMCC link up (alloc %u, gem %u)\n",
		 cg->omcc_alloc, cg->omcc_gem);
	cx_notify(CX_EV_ONU_ID | CX_EV_CHANNEL);

	/*
	 * Arm the G.988 responder on a fresh MIB, with cg->sn as the ME 256
	 * serial number. The MIB-Data-Sync seed 200 must not match the OLT's
	 * stored copy, so its audit fails and it re-provisions from MIB-Reset.
	 * Also start the ~31 s VEIP oper-up AVC timer. Not used while the xPON
	 * framework owns the OMCC.
	 */
	if (cg->omci && !cx_active()) {
		char sn_str[13];

		spin_lock_bh(&cg->omci_lock);
		omci_onu_init(cg->omci, cg->sn, 200);
		cg->omci_active = true;
		spin_unlock_bh(&cg->omci_lock);
		cg->veip_avc_retry_ms = 0;
		/* cg_veip_avc_work() defers while omcid owns the OMCC. */
		schedule_delayed_work(&cg->veip_avc_work, 31 * HZ);
		cg_sn_format(cg->sn, sn_str);
		dev_dbg(cg->dev, "OMCI responder armed (%u MIB rows, mds seed 200, sn %s)\n",
			cg->omci->nrows, sn_str);
	}
}


/*
 * Periodic OMCI counter dump, enabled with cortina_gpon.stats_s=<seconds>.
 * ds_omci_rx counts downstream OMCI received in software; the MAC hw_gem/hw_pkt
 * values do not track downstream OMCI and are not meaningful.
 */
/*
 * PON window register dump (cortina_gpon.dump_pon=1, once, late in bring-up).
 * Lists only offsets this driver reads on the live path: a write does not prove
 * an address decodes (writes are posted), and reading an undecoded address
 * aborts. Anything added must be known to be readable.
 */
static const u16 cg_pon_dump_offs[] = {
	0x0074, 0x007c, 0x0094, 0x0130, 0x0134, 0x013c, 0x0140, 0x0194, 0x0198, 0x01b0, 0x01b4, 0x022c,
	0x042c, 0x1000, 0x6000, 0x600c, 0x6010, 0x6014, 0x6018, 0x601c, 0x6020, 0x6024, 0x6078, 0x607c,
	0x6080, 0x6084, 0x6088, 0x608c, 0x6090, 0x6094, 0x6098, 0x609c, 0x60a0, 0x60a4, 0x60a8, 0x60bc,
	0x60c0, 0x60c8, 0x60d8, 0x60fc, 0x6114, 0x6118, 0x612c, 0x6130, 0x6134, 0x6138, 0x6170, 0x6174,
	0x61a8, 0x61ac, 0x61b0, 0x61b4, 0x61b8, 0x61bc, 0x61c0, 0x61c4, 0x61c8, 0x61cc, 0x61e0, 0x61f8,
	0x61fc, 0x6800, 0x6804, 0x6808, 0x680c, 0x6810, 0x6814, 0x6818, 0x6e14, 0x8000, 0x8004, 0x8008,
	0x800c, 0x8010, 0x8014, 0x804c, 0x8050, 0x808c, 0x8090, 0x80d0, 0x80d4, 0x80d8, 0x80dc, 0x80e4,
	0x80e8, 0x80ec, 0x80f4, 0x813c, 0x8140, 0x8144, 0x814c, 0x8150, 0x8154, 0x8158, 0x815c, 0x8160,
	0x8164, 0x8168, 0x8174, 0x8178, 0x817c, 0x8180, 0x8184, 0x8188, 0x8194, 0x8198, 0x81bc, 0x8230,
	0x8234, 0x9014, 0x9020, 0x9024, 0x9028,

	/*
	 * the remaining named GPON MAC registers, including the upstream side
	 * (BWmap control/data, MIB, debug status)
	 */
	0x6004, 0x6008, 0x6028, 0x602c, 0x6030, 0x6034, 0x6038, 0x603c, 0x6040, 0x6044, 0x6048, 0x604c,
	0x6050, 0x6054, 0x6058, 0x605c, 0x6060, 0x6064, 0x6068, 0x606c, 0x6070, 0x6074, 0x60ac, 0x60b0,
	0x60b4, 0x60b8, 0x60c4, 0x60cc, 0x60d0, 0x60d4, 0x60dc, 0x60e0, 0x60e4, 0x60e8, 0x60ec, 0x60f0,
	0x60f4, 0x60f8, 0x6100, 0x6104, 0x6108, 0x610c, 0x6110, 0x611c, 0x6120, 0x6124, 0x6128, 0x613c,
	0x6140, 0x6144, 0x6148, 0x614c, 0x6150, 0x6154, 0x6158, 0x615c, 0x6160, 0x6164, 0x6168, 0x616c,
	0x6178, 0x617c, 0x6180, 0x6184, 0x6188, 0x618c, 0x6190, 0x6194, 0x6198, 0x619c, 0x61a0, 0x61a4,
	0x61d0, 0x61d4, 0x61d8, 0x61dc, 0x61e4, 0x61e8, 0x61ec, 0x61f0, 0x61f4,
};
static void cg_stats_work(struct work_struct *work)
{
	struct cortina_gpon *cg = container_of(to_delayed_work(work),
					       struct cortina_gpon, stats_work);

	cg_puc_ctrl_sample(cg);		/* refresh the PUC accumulators first */
	dev_dbg(cg->dev,
		"OMCI-STATS: ds_omci_rx=%u short=%u hw_gem=%u hw_pkt=%u | omci_tx=%u fail=%u ds_crc ok=%u bad=%u | us_omcc(0x1e0)=%u | PUC omci_us=%u len_err=%u rx=%u enq=%u drop=%u hdr0=%08x%s hdr1=%08x | omcc=%s alloc=%u gem=%u onu=0x%08x\n",
		cg->omci_rx, cg->omci_rx_short,
		cg_mac_rd(cg, CG_REG_DS_OMCI_GEM), cg_mac_rd(cg, CG_REG_DS_OMCI_PKT),
		cg->omci_tx, cg->omci_tx_fail,
		cg->omci_ds_crc_ok, cg->omci_ds_crc_bad,
		/* GPON MAC upstream OMCC frame counter (not validated) */
		cg_mac_rd(cg, CG_REG_US_OMCC_CNT),
		/* PUC counters, sampled just above */
		cg->puc_omci_us, cg->puc_len_err,
		readl(cg->pon + CG_PUC_BMC_RX_PKT) & CG_PUC_BMC_CNTR_MASK,
		readl(cg->pon + CG_PUC_BMC_RX_PKT_ENQ) & CG_PUC_BMC_CNTR_MASK,
		readl(cg->pon + CG_PUC_BMC_FORCE_DROP) & CG_PUC_BMC_CNTR_MASK,
		readl(cg->pon + CG_PUC_BMC_PKT_HDR0),
		readl(cg->pon + CG_PUC_BMC_PKT_HDR0) == CG_PUC_HDR0_GOLDEN ? "(expected)" : "",
		readl(cg->pon + CG_PUC_BMC_PKT_HDR1),
		cg->omcc_up ? "UP" : "down", cg->omcc_alloc, cg->omcc_gem,
		cg_mac_rd(cg, CG_REG_GPON_ONU));
	/*
	 * with the NI receive delivery counters, to tell "the OLT is not
	 * sending" from a loss between MAC and CPU
	 */
	cortina_ni_rx_delivery_dump();
	/* and the upstream MIB, which counts what was actually transmitted */
	cg_us_mib_show(cg, NULL);
}

static int cg_omci_tx(struct cortina_gpon *cg, const u8 *pdu48)
{
	int ret = -ENODEV;

	if (IS_REACHABLE(CONFIG_CORTINA_NI))
		ret = cortina_ni_pon_tx(pdu48, OMCI_LEN);
	if (ret) {
		cg->omci_tx_fail++;
		dev_warn_ratelimited(cg->dev, "US OMCI TX failed (%d)\n", ret);
	} else {
		cg->omci_tx++;
		/*
		 * Sample the OMCI control packet counter once the frame has
		 * reached the PUC, within its clear-on-read window
		 * (cg_puc_ctrl_sample()).
		 */
		schedule_delayed_work(&cg->puc_cnt_work,
				      msecs_to_jiffies(CG_PUC_CNT_TX_DELAY_MS));
	}
	/*
	 * Return the error: an unsolicited AVC has no OLT retry behind it, see
	 * cg_veip_avc_work().
	 */
	return ret;
}

/*
 * Dying gasp, as the vendor firmware does it: the SoC's power-fail detector
 * interrupts through the peripheral interrupt controller (line 24), and the
 * ONU sends an ONU-G alarm notification with alarm 7 (dying gasp, G.988
 * 9.1.1) while the supply capacitors last. The PDU and its MIC are built
 * at probe so the interrupt only has to transmit it.
 */
#define CG_DG_CFG		0x00	/* PER_DYING_GASP */
#define CG_DG_IE		0x04	/* PER_DG_IE_0 */
#define CG_DG_INT		0x08	/* PER_DG_INT_0, write 1 to clear */
#define CG_DG_CFG_VAL		0x105	/* the vendor firmware's detector setting */
#define CG_PER_INT		0x00	/* PER_PERIPHERAL_INTERRUPT_0 */
#define CG_PER_INTEN		0x04	/* PER_PERIPHERAL_INTENABLE_0 */
#define CG_PER_INT_DG		BIT(24)
#define CG_OMCI_MT_ALARM	0x10
#define CG_OMCI_DEV_BASELINE	0x0a
#define CG_ONUG_CLASS		256
#define CG_ONUG_ALARM_DG	7

static void cg_dg_build(struct cortina_gpon *cg)
{
	u8 *p = cg->dg_pdu;

	memset(p, 0, OMCI_LEN);
	p[2] = CG_OMCI_MT_ALARM;
	p[3] = CG_OMCI_DEV_BASELINE;
	put_unaligned_be16(CG_ONUG_CLASS, p + 4);	/* ONU-G, instance 0 */
	p[8 + CG_ONUG_ALARM_DG / 8] = 0x80 >> (CG_ONUG_ALARM_DG % 8);
	p[8 + 31] = 1;					/* alarm sequence number */
	put_unaligned_be16(40, p + 42);			/* CPCS-SDU length */
	put_unaligned_be32(~crc32_be(~0U, p, 44), p + 44);
}

static irqreturn_t cg_dg_isr(int irq, void *data)
{
	struct cortina_gpon *cg = data;
	u32 v = readl(cg->per_ictl + CG_PER_INTEN);

	if (!(readl(cg->per_ictl + CG_PER_INT) & v & CG_PER_INT_DG))
		return IRQ_NONE;

	/* once: the power is going */
	writel(v & ~CG_PER_INT_DG, cg->per_ictl + CG_PER_INTEN);
	writel(0, cg->dg + CG_DG_IE);
	writel(1, cg->dg + CG_DG_INT);
	return IRQ_WAKE_THREAD;
}

static irqreturn_t cg_dg_thread(int irq, void *data)
{
	struct cortina_gpon *cg = data;

	if (cortina_epon_active(cg->epon)) {
		cortina_epon_dying_gasp(cg->epon);
		dev_emerg(cg->dev, "dying gasp OAM sent to the OLT\n");
		return IRQ_HANDLED;
	}

	if (cg->last_state == CG_STATE_OPERATION && cg->omcc_up &&
	    !cg_omci_tx(cg, cg->dg_pdu))
		dev_emerg(cg->dev, "dying gasp sent to the OLT\n");
	else
		dev_emerg(cg->dev, "dying gasp: not registered with the OLT, nothing sent\n");
	return IRQ_HANDLED;
}

static void cg_dg_setup(struct cortina_gpon *cg, struct platform_device *pdev)
{
	int irq, ret;

	cg->dg = devm_platform_ioremap_resource_byname(pdev, "dying-gasp");
	cg->per_ictl = devm_platform_ioremap_resource_byname(pdev, "per-ictl");
	irq = platform_get_irq_byname_optional(pdev, "dying-gasp");
	if (IS_ERR(cg->dg) || IS_ERR(cg->per_ictl) || irq < 0) {
		dev_dbg(cg->dev, "no dying gasp in device tree\n");
		return;
	}

	cg_dg_build(cg);
	ret = devm_request_threaded_irq(cg->dev, irq, cg_dg_isr, cg_dg_thread,
					IRQF_ONESHOT, "dying-gasp", cg);
	if (ret) {
		dev_warn(cg->dev, "dying gasp IRQ %d: %d\n", irq, ret);
		return;
	}
	writel(CG_DG_CFG_VAL, cg->dg + CG_DG_CFG);
	writel(1, cg->dg + CG_DG_INT);
	writel(1, cg->dg + CG_DG_IE);
	writel(readl(cg->per_ictl + CG_PER_INTEN) | CG_PER_INT_DG,
	       cg->per_ictl + CG_PER_INTEN);
	dev_dbg(cg->dev, "dying gasp armed\n");
}

/*
 * Print an optical power in centi-dBm, or "-inf" when the optic reports no
 * light; a 0 would read as a healthy +0 dBm.
 */
static void cg_seq_cdbm(struct seq_file *m, s32 cdbm)
{
	if (cdbm == CG_DDM_CDBM_NONE)
		seq_puts(m, "-inf");
	else
		seq_printf(m, "%d", cdbm);
}

/* centi-dBm as "-18.52" */
static const char *cg_fmt_cdbm(char *buf, size_t len, s32 cdbm)
{
	if (cdbm == CG_DDM_CDBM_NONE)
		return "-inf";
	snprintf(buf, len, "%s%d.%02d", cdbm < 0 ? "-" : "",
		 abs(cdbm) / 100, abs(cdbm) % 100);
	return buf;
}

static void cg_optic_log(struct cortina_gpon *cg, const struct cg_bosa_ddm *d,
			 s32 rx_cdbm, s32 tx_cdbm)
{
	s32 temp = cg_ddm_temp_dc(d->temp);
	u32 bias = cg_ddm_bias_ua(d->bias), vcc = cg_ddm_vcc_mv(d->vcc);
	char rx[16], tx[16];

	dev_info(cg->dev, "optical: rx %s dBm, tx %s dBm, bias %u.%03u mA, temp %s%d.%d C, vcc %u.%03u V\n",
		 cg_fmt_cdbm(rx, sizeof(rx), rx_cdbm),
		 cg_fmt_cdbm(tx, sizeof(tx), tx_cdbm),
		 bias / 1000, bias % 1000, temp < 0 ? "-" : "",
		 abs(temp) / 10, abs(temp) % 10, vcc / 1000, vcc % 1000);
}

static void cg_optic_anig_show(struct cortina_gpon *cg, struct seq_file *m)
{
	if (!cg->omci) {
		seq_puts(m, "optic_anig     = (responder not allocated)\n");
		return;
	}
	seq_printf(m, "optic_anig     = %s  me263 #10 rx=0x%04x #14 tx=0x%04x  (G.988 0.002 dB units)\n",
		   cg->omci->anig_live ? "live" : "FALLBACK (static, no DDM sample yet)",
		   cg->omci->anig_rx_level, cg->omci->anig_tx_level);
}

/*
 * Sample the optic's SFF-8472 A2h diagnostics, print them to @m when non-NULL,
 * and publish the two optical levels into ME 263 ANI-G #10/#14.
 *
 * Process context only: cg_bosa_ddm_read() sleeps, so the read happens outside
 * omci_lock. Sampled on demand (here and ~31 s after O5, when OLTs audit
 * ANI-G); ten reads take ~1 ms. A failed read keeps the responder's fallback
 * values and prints "unavailable".
 */
static void cg_optic_sample(struct cortina_gpon *cg, struct seq_file *m)
{
	struct cg_bosa_ddm d;
	s32 rx_cdbm, tx_cdbm;

	if (cg_bosa_ddm_read(cg->dev, &d) != CG_DDM_OK) {
		if (m) {
			seq_printf(m, "optic_ddm      = %s\n",
				   cg_ddm_status_str(d.status));
			cg_optic_anig_show(cg, m);
		} else {
			dev_info(cg->dev, "optical sample unavailable: %s\n",
				 cg_ddm_status_str(d.status));
		}
		return;
	}

	rx_cdbm = cg_ddm_uw10_to_cdbm(d.rx_pwr);
	tx_cdbm = cg_ddm_uw10_to_cdbm(d.tx_pwr);

	if (cg->omci) {
		u16 rx = cg_ddm_cdbm_to_omci(rx_cdbm);
		u16 tx = cg_ddm_cdbm_to_omci(tx_cdbm);

		spin_lock_bh(&cg->omci_lock);
		omci_onu_set_optical(cg->omci, rx, tx);
		spin_unlock_bh(&cg->omci_lock);
	}
	if (!m)
		cg_optic_log(cg, &d, rx_cdbm, tx_cdbm);

	if (m) {
		unsigned int i;

		seq_printf(m, "optic_ddm      = live (SFF-8472 A2h 0x%02x-0x%02x)\n",
			   CG_DDM_BASE, CG_DDM_BASE + CG_DDM_LEN - 1);
		/* raw words are printed too, so levels can be re-derived */
		seq_printf(m, "optic_rx_raw: 0x%04x optic_rx_cdbm: ", d.rx_pwr);
		cg_seq_cdbm(m, rx_cdbm);
		seq_printf(m, " optic_tx_raw: 0x%04x\n", d.tx_pwr);
		seq_printf(m, "optic_env:   temp_dc=%d bias_ua=%u tx_cdbm=",
			   cg_ddm_temp_dc(d.temp), cg_ddm_bias_ua(d.bias));
		cg_seq_cdbm(m, tx_cdbm);
		seq_printf(m, " vcc_mv=%u\n", cg_ddm_vcc_mv(d.vcc));
		seq_printf(m, "optic_ddm_raw: %02x..%02x =",
			   CG_DDM_BASE, CG_DDM_BASE + CG_DDM_LEN - 1);
		for (i = 0; i < CG_DDM_LEN; i++)
			seq_printf(m, " %02x", d.raw[i]);
		seq_putc(m, '\n');
		cg_optic_anig_show(cg, m);
	}
}

/*
 * VEIP (ME 329) operational-up AVC ~31 s after O5. Some OLTs keep the service
 * unmatched until the ONU reports the WAN port up.
 */
static void cg_optic_log_work(struct work_struct *work)
{
	cg_optic_sample(container_of(to_delayed_work(work), struct cortina_gpon,
				     optic_log_work), NULL);
}

static void cg_veip_avc_work(struct work_struct *work)
{
	struct cortina_gpon *cg = container_of(to_delayed_work(work),
					       struct cortina_gpon,
					       veip_avc_work);
	u8 frame[OMCI_LEN];
	bool emit = false;

	/*
	 * While omcid (xPON framework) owns the OMCC it sends its own VEIP AVC.
	 * Check again later so this fallback still runs if omcid goes away.
	 */
	if (cx_active()) {
		schedule_delayed_work(&cg->veip_avc_work, 31 * HZ);
		return;
	}

	/*
	 * Publish a fresh optical reading first: the OLT starts auditing ANI-G
	 * about now.
	 */
	cg_optic_sample(cg, NULL);

	spin_lock_bh(&cg->omci_lock);
	if (cg->omci_active && !cg->omci->avc_veip_up_sent) {
		omci_onu_emit_veip_up_avc(cg->omci, frame);
		emit = true;
	}
	spin_unlock_bh(&cg->omci_lock);
	if (!emit)
		return;

	if (!cg_omci_tx(cg, frame)) {
		cg->veip_avc_retry_ms = 0;
		dev_dbg(cg->dev, "VEIP oper-up AVC emitted (~31s post-O5)\n");
		return;
	}

	/*
	 * The responder sets avc_veip_up_sent when it builds the frame, so
	 * clear it on a failed transmit and retry; the OLT never asks for this
	 * AVC again. Retries are rate limited (doubling backoff up to a
	 * ceiling) but never stop.
	 */
	spin_lock_bh(&cg->omci_lock);
	if (cg->omci_active)
		cg->omci->avc_veip_up_sent = false;
	spin_unlock_bh(&cg->omci_lock);

	cg->veip_avc_retry_ms = cg->veip_avc_retry_ms
		? min(cg->veip_avc_retry_ms * 2u,
		      (unsigned int)CG_VEIP_AVC_RETRY_MAX_MS)
		: CG_VEIP_AVC_RETRY_MIN_MS;
	dev_warn(cg->dev, "VEIP oper-up AVC TX failed; retrying in %u ms\n",
		 cg->veip_avc_retry_ms);
	schedule_delayed_work(&cg->veip_avc_work,
			      msecs_to_jiffies(cg->veip_avc_retry_ms));
}

#define CG_OMCI_LOG_N	24

static struct cg_tcont *cg_tcont_slot(struct cortina_gpon *cg, u16 inst)
{
	struct cg_tcont *free = NULL;
	unsigned int i;

	for (i = 0; i < CG_DATA_TCONT_MAX; i++) {
		if (cg->tc[i].used && cg->tc[i].inst == inst)
			return &cg->tc[i];
		if (!cg->tc[i].used && !free)
			free = &cg->tc[i];
	}
	return free;
}

static struct cg_gem *cg_gem_slot(struct cortina_gpon *cg, u16 inst, bool create)
{
	struct cg_gem *free = NULL;
	unsigned int i;

	for (i = 0; i < CG_DATA_GEM_MAX; i++) {
		if (cg->gm[i].used && cg->gm[i].inst == inst)
			return &cg->gm[i];
		if (!cg->gm[i].used && !free)
			free = &cg->gm[i];
	}
	return create ? free : NULL;
}

/*
 * Follow the OLT's data path provisioning in the OMCI it sends:
 *  - ME 262 T-CONT Set (attribute 1) or Create: the alloc-id. 0x00ff and
 *    0xffff are the G.984/G.987 "unassigned" values and release it.
 *  - ME 268 GEM port network CTP Create: port-id, T-CONT pointer, direction;
 *    a Set of attributes 1 to 3 changes them, a Delete removes the port.
 *    The OMCC and the broadcast GEM (installed on its own) are skipped.
 *  - MIB-Reset voids all of it.
 * Runs in the RX softirq; cg_data_sync() applies the result in isr_work.
 */
static void cg_snoop_provisioning(struct cortina_gpon *cg, u8 m, u16 class_id,
				  u16 inst, const u8 *pdu, unsigned int len)
{
	u16 mask = get_unaligned_be16(pdu + 8);
	bool changed = false;

	spin_lock(&cg->data_lock);
	if (m == 15) {
		memset(cg->tc, 0, sizeof(cg->tc));
		memset(cg->gm, 0, sizeof(cg->gm));
		memset(cg->tq, 0, sizeof(cg->tq));
		memset(cg->gp, 0, sizeof(cg->gp));
		changed = true;
	} else if (class_id == 262 && len >= 12 && (m == 4 || (m == 8 && (mask & 0x8000)))) {
		u16 alloc = get_unaligned_be16(pdu + (m == 4 ? 8 : 10));
		struct cg_tcont *t = cg_tcont_slot(cg, inst);

		if (alloc == 0xff || alloc == 0xffff)
			alloc = 0;
		if (t && (!t->used || t->alloc != alloc)) {
			*t = (struct cg_tcont){ .inst = inst, .alloc = alloc, .used = true };
			changed = true;
			dev_dbg(cg->dev, "OMCI: T-CONT 0x%04x alloc-id %u\n", inst, alloc);
		}
	} else if (class_id == 268 && m == 6) {
		struct cg_gem *g = cg_gem_slot(cg, inst, false);

		if (g) {
			g->used = false;
			changed = true;
			dev_dbg(cg->dev, "OMCI: GEM CTP 0x%04x deleted\n", inst);
		}
	} else if (class_id == 268 && len >= 13 && (m == 4 || m == 8)) {
		struct cg_gem *g = cg_gem_slot(cg, inst, m == 4);
		struct cg_gem v = { .inst = inst, .used = true };
		const u8 *p = pdu + 10;

		if (g && m == 8 && g->used)
			v = *g;
		if (m == 4) {
			v.port = get_unaligned_be16(pdu + 8);
			v.tcont_inst = get_unaligned_be16(pdu + 10);
			v.dir = pdu[12];
		} else {
			if (mask & 0x8000) {
				v.port = get_unaligned_be16(p);
				p += 2;
			}
			if (mask & 0x4000) {
				v.tcont_inst = get_unaligned_be16(p);
				p += 2;
			}
			if (mask & 0x2000)
				v.dir = *p;
		}
		v.port &= 0xfff;
		if (v.port == cg->omcc_gem || v.port == CG_MCAST_GEM_ID)
			g = NULL;
		if (g && (!g->used || g->port != v.port ||
			  g->tcont_inst != v.tcont_inst || g->dir != v.dir)) {
			*g = v;
			changed = true;
			dev_dbg(cg->dev, "OMCI: GEM %u (CTP 0x%04x, T-CONT 0x%04x, dir %u)\n",
				v.port, inst, v.tcont_inst, v.dir);
		}
	}
	spin_unlock(&cg->data_lock);

	if (changed) {
		if (m == 15) {
			cg->data_installed = false;
			if (cg->wan_ndev)
				netif_carrier_off(cg->wan_ndev);
		}
		schedule_work(&cg->isr_work);
	}
}

/*
 * Downstream OMCI receive: the NI CPU-RX hook passes each OMCI PDU (PON control
 * header stripped). Log it, answer with the G.988 responder and send the reply
 * upstream. Runs in NAPI softirq context; the responder context is protected by
 * omci_lock.
 *
 * Baseline OMCI PDU layout (G.988, big-endian):
 *   [0:1] TCI    [2] msg-type {AR=bit6, AK=bit5, MT=bits4:0}
 *   [3]   device-id (0x0a = baseline)
 *   [4:5] ME class    [6:7] ME instance
 *   [8:39] contents   [40:47] trailer (incl. the 4-byte MIC)
 */
static void cg_rx_omci(const u8 *pdu, unsigned int len)
{
	struct cortina_gpon *cg = READ_ONCE(cg_singleton);
	static const char *const mt_name[32] = {
		[4] = "Create", [5] = "Delete", [8] = "Set", [9] = "Get",
		[11] = "Get-all-alarms", [12] = "Get-all-alarms-next",
		[13] = "MIB-upload", [14] = "MIB-upload-next",
		[15] = "MIB-reset", [16] = "Alarm", [17] = "AVC", [18] = "Test",
		[19] = "Start-SW-dl", [20] = "DL-section", [21] = "End-SW-dl",
		[22] = "Activate-SW", [23] = "Commit-SW", [24] = "Sync-time",
		[25] = "Reboot", [26] = "Get-next", [27] = "Test-result",
		[28] = "Get-current-data", [29] = "Set-table",
	};
	const char *name;
	u8 mt;

	if (!cg)
		return;
	if (len < 8) {
		cg->omci_rx_short++;
		return;
	}
	cg->omci_rx++;

	mt = pdu[2];
	name = mt_name[mt & 0x1f] ? mt_name[mt & 0x1f] : "?";
	/* log the first omci_log_n PDUs, then 1 in 64 */
	if (cg->omci_rx <= CG_OMCI_LOG_N || !(cg->omci_rx & 63))
		dev_dbg(cg->dev,
			"DS OMCI #%u: len=%u tci=0x%02x%02x mt=%u(%s)%s%s dev=0x%02x me=%u/%u\n",
			cg->omci_rx, len, pdu[0], pdu[1], mt & 0x1f, name,
			(mt & BIT(6)) ? " AR" : "", (mt & BIT(5)) ? " AK" : "",
			pdu[3], (pdu[4] << 8) | pdu[5], (pdu[6] << 8) | pdu[7]);

	/*
	 * Also log the attribute payload (pdu[8..39]) of Create and Set, e.g.
	 * to see the VLAN tagging filter (ME 84) the OLT configures.
	 */
	if (cg->omci_rx <= CG_OMCI_LOG_N && len >= 40 &&
	    ((mt & 0x1f) == 4 || (mt & 0x1f) == 8))
		dev_dbg(cg->dev, "DS OMCI #%u: me=%u/%u %s payload %*ph\n",
			cg->omci_rx, (pdu[4] << 8) | pdu[5],
			(pdu[6] << 8) | pdu[7],
			(mt & 0x1f) == 4 ? "Create" : "Set", 32, pdu + 8);

	/*
	 * Downstream MIC check on the first PDUs (diagnostic): be =
	 * I.363.5/AAL5 (~crc32_be, G.984.4, what the responder emits), le =
	 * reflected zlib CRC.
	 */
	if (len >= OMCI_LEN && cg->omci_ds_crc_ok + cg->omci_ds_crc_bad < 16) {
		u32 want = ((u32)pdu[44] << 24) | ((u32)pdu[45] << 16) |
			   ((u32)pdu[46] << 8) | pdu[47];
		u32 be = ~crc32_be(~0u, pdu, 44);
		u32 le = crc32_le(~0u, pdu, 44) ^ ~0u;

		if (be == want)
			cg->omci_ds_crc_ok++;
		else
			cg->omci_ds_crc_bad++;
		if (cg->omci_rx <= 4)
			dev_dbg(cg->dev, "DS OMCI MIC self-check: %s (want %08x be %08x le %08x)\n",
				be == want ? "AAL5-BE" :
				(le == want ? "ZLIB-LE" : "NEITHER"),
				want, be, le);
	}

	/*
	 * Snoop the MEs that define the data path; the responder still answers
	 * them, the driver additionally installs the hardware tables.
	 */
	{
		u16 class_id = ((u16)pdu[4] << 8) | pdu[5];
		u16 inst = ((u16)pdu[6] << 8) | pdu[7];
		u8 m = mt & 0x1f;
		bool cfg = (m == 4 || m == 8 || m == 6);	/* Create/Set/Delete */

		/* log the bodies of data path / bridging MEs (bounded) */
		if (cfg && cg->omci_cfg_log < 48 && len >= 24) {
			switch (class_id) {
			case 45: case 47: case 84: case 130: case 171:
			case 262: case 266: case 268: case 277: case 280:
			case 281: case 309: case 329:
				cg->omci_cfg_log++;
				dev_dbg(cg->dev,
					"OMCI cfg mt=%u me=%u/0x%04x body=%*phN\n",
					m, class_id, inst, 16, pdu + 8);
				break;
			}
		}

		if (class_id == 262 || class_id == 268 || m == 15)
			cg_snoop_provisioning(cg, m, class_id, inst, pdu, len);
	}

	/* omcid, when attached, answers the OLT instead of the in-kernel responder */
	if (cx_omci_rx(pdu, len))
		return;

	/*
	 * Answer with the responder and send the reply upstream. Clamp a padded
	 * frame to 48 bytes so a Create body never includes pad bytes.
	 */
	if (len > OMCI_LEN)
		len = OMCI_LEN;
	{
		u8 resp[OMCI_LEN];
		int n = 0;

		spin_lock(&cg->omci_lock);
		if (cg->omci_active)
			n = omci_onu_input(cg->omci, pdu, len, resp);
		spin_unlock(&cg->omci_lock);
		if (n == OMCI_LEN)
			cg_omci_tx(cg, resp);
	}
}

/* Bottom half: drain the event ring and run the FSM tracker + OMCC binds. */
static void cg_isr_work(struct work_struct *work)
{
	struct cortina_gpon *cg = container_of(work, struct cortina_gpon, isr_work);
	struct cg_evt ev;
	unsigned long flags;

	for (;;) {
		spin_lock_irqsave(&cg->evt_lock, flags);
		if (cg->evt_tail == cg->evt_head) {
			spin_unlock_irqrestore(&cg->evt_lock, flags);
			break;
		}
		ev = cg->evt[cg->evt_tail % CG_EVT_RING_SZ];
		cg->evt_tail++;
		spin_unlock_irqrestore(&cg->evt_lock, flags);


		if (ev.intr & CG_INT_ONU_ST_CHG) {
			u8 last = cg->last_state;

			if (last != ev.state)
				dev_dbg(cg->dev, "FSM %s -> %s (onu-id %u)\n",
					cg_state_name[last & 7],
					cg_state_name[ev.state & 7], ev.id);
			/*
			 * O5 exit = link down (vendor condition): leaving
			 * Operation for anything but POPUP, leaving POPUP for
			 * anything but Operation/Ranging (POPUP->Ranging is the
			 * Type-B popup, kept alive), or entering EmergencyStop.
			 */
			if ((last == CG_STATE_OPERATION && ev.state != CG_STATE_OPERATION &&
			     ev.state != CG_STATE_POPUP) ||
			    (last == CG_STATE_POPUP && ev.state != CG_STATE_OPERATION &&
			     ev.state != CG_STATE_RANGING) ||
			    ev.state == CG_STATE_ESTOP)
				cg_datapath_reset(cg);
			if (last != ev.state)
				cg_log_state(cg, last, ev.state);
			cg->last_state = ev.state;
			cx_notify(CX_EV_STATE | CX_EV_LOS);
		}

		if ((ev.intr & CG_INT_ONU_ID) && ev.id != CG_ONU_ID_NONE) {
			cg_omcc_tcont_bind(cg, ev.id);
			cx_notify(CX_EV_ONU_ID);
		}

		if (ev.intr & CG_INT_DACT)
			dev_info(cg->dev, "Deactivate_ONU-ID received\n");

		cg_ds_fec_event(cg, ev.intr);

		if (ev.intr & CG_INT_KSW)
			dev_dbg(cg->dev, "Key_Switching_Time\n");

		/*
		 * recompute frame_var on every downstream PLOAM;
		 * Extended_Burst_Length may change any time from O2
		 */
		if (ev.intr & (CG_INT_PLOAMD | CG_INT_ONU_ST_CHG))
			cg_frame_var_update(cg);

		/*
		 * OMCC bring-up on PORTID in O5, and also on entering O5, which
		 * covers a PORTID interrupt that fired before the FSM reached
		 * Operation.
		 */
		if (ev.intr & (CG_INT_PORTID | CG_INT_ONU_ST_CHG))
			cg_omcc_try_up(cg, ev.state);
	}

	/*
	 * Reconcile soft state against the live FSM register. The event ring
	 * drops events on overflow, and a settled O5 generates no further
	 * interrupts, so a lost O5 entry, Assign_ONU-ID or Configure_Port-ID
	 * would leave the OMCC down on healthy hardware until the OLT
	 * deactivates the ONU. CG_REG_GPON_ONU is not lossy, so re-derive from
	 * it.
	 *
	 * Bring-up only: this can add a bind the hardware says should exist,
	 * but never infers an O5 exit from a poll; the event path owns exits.
	 * The FSM tracker is moved forward to Operation together with the
	 * replay, so a later O5 exit is still detected.
	 *
	 * On a converged link this is a few status register reads and no
	 * writes; no indirect ACCESS/DATA engine is touched.
	 */
	{
		u32 onu = cg_mac_rd(cg, CG_REG_GPON_ONU);
		u8 live = CG_ONU_STATE(onu);
		u8 id = CG_ONU_ID(onu);

		if (live == CG_STATE_OPERATION) {
			if (cg->last_state != CG_STATE_OPERATION) {
				dev_dbg(cg->dev,
					"reconcile: live FSM is %s while the tracker says %s (%u events dropped) - replaying from the register\n",
					cg_state_name[CG_STATE_OPERATION],
					cg_state_name[cg->last_state & 7],
					cg->evt_drop);
				cg_log_state(cg, cg->last_state, CG_STATE_OPERATION);
				cg->last_state = CG_STATE_OPERATION;
			}
			/*
			 * A lost Assign_ONU-ID leaves the OMCC T-CONT unbound
			 * and the ONU without grants. omcc_up does not depend
			 * on this bind, so do not gate on it. Only binds when
			 * the shadow disagrees.
			 */
			if (id != CG_ONU_ID_NONE &&
			    (!cg->omcc_alloc_valid || cg->omcc_alloc != id))
				cg_omcc_tcont_bind(cg, id);
			/*
			 * a lost downstream PLOAM event can leave frame_var
			 * stale; only writes on change
			 */
			cg_frame_var_update(cg);
			if (!cg->omcc_up)
				cg_omcc_try_up(cg, live);
		}
	}

	/*
	 * (Re-)install the data path once the OMCC is up and both provisioning
	 * halves are known; also kicked by cg_rx_omci() when ME 262/268 arrive.
	 */
	cg_data_sync(cg);
}

/*
 * Service one interrupt group: read enable and status, zero the enable, W1C the
 * enabled sources, return status & enable, re-arm the enable. The zero/re-arm
 * bracket is required; without it no further interrupts arrive.
 */
static u32 cg_intr_group_service(struct cortina_gpon *cg, u32 sts_off, u32 en_off)
{
	u32 intre, intrs;

	intre = readl(cg->mac + en_off);
	intrs = readl(cg->mac + sts_off);
	writel(0, cg->mac + en_off);
	writel(intrs & intre, cg->mac + sts_off);	/* W1C */
	writel(intre, cg->mac + en_off);
	return intrs & intre;
}

/*
 * Top-level ISR on the shared NE global line (GIC SPI 1): mask the PON
 * aggregate, loop { save and zero int_top_en, read int_top (read-clears),
 * service the pending groups, restore int_top_en } bounded at 32 passes, then
 * ack, unmask and kick the bottom half.
 */
static irqreturn_t cg_isr(int irq, void *data)
{
	struct cortina_gpon *cg = data;
	bool pending = false, queued = false;
	u32 glb_ie, ie, top, src;
	int pass;

	/* mask the PON aggregate at the GLB level */
	glb_ie = readl(cg->glb + CG_GLB_PON_INTEN0);
	writel(glb_ie & ~CG_PON_INT0_PON_MAC, cg->glb + CG_GLB_PON_INTEN0);

	for (pass = 0; pass < 32; pass++) {
		ie = readl(cg->mac + CG_REG_INT_TOP_EN);
		writel(0, cg->mac + CG_REG_INT_TOP_EN);
		top = readl(cg->mac + CG_REG_INT_TOP) & ie;	/* read-clears */

		if (top & BIT(0)) {
			src = cg_intr_group_service(cg, CG_REG_INT, CG_REG_INT_EN);
			if (src) {
				u32 onu = cg_mac_rd(cg, CG_REG_GPON_ONU);

				spin_lock(&cg->evt_lock);
				if (cg->evt_head - cg->evt_tail < CG_EVT_RING_SZ) {
					struct cg_evt *ev =
						&cg->evt[cg->evt_head % CG_EVT_RING_SZ];

					ev->intr = src;
					ev->state = CG_ONU_STATE(onu);
					ev->id = CG_ONU_ID(onu);
					cg->evt_head++;
					queued = true;
				} else {
					cg->evt_drop++;
				}
				spin_unlock(&cg->evt_lock);
			}
		}
		/*
		 * groups 2/3/4 have their enables at 0; still run the
		 * W1C/re-arm bracket
		 */
		if (top & BIT(1))
			cg_intr_group_service(cg, CG_REG_INT2, CG_REG_INT2_EN);
		if (top & BIT(2))
			cg_intr_group_service(cg, CG_REG_INT3, CG_REG_INT3_EN);
		if (top & BIT(3))
			cg_intr_group_service(cg, CG_REG_INT4, CG_REG_INT4_EN);

		writel(ie, cg->mac + CG_REG_INT_TOP_EN);
		if (!top)
			break;
		pending = true;
	}

	/* ack the ne_ictl line (needed if its status latches) */
	writel(CG_NE_ICTL_PON_LINE, cg->glb + CG_GLB_NE_ICTL_STS);
	/* unmask the PON aggregate */
	writel(glb_ie | CG_PON_INT0_PON_MAC, cg->glb + CG_GLB_PON_INTEN0);

	if (queued)
		schedule_work(&cg->isr_work);
	if (!pending)
		return IRQ_NONE;	/* shared line, not ours */
	cg->irq_count++;
	return IRQ_HANDLED;
}

/*
 * Arm the interrupt path: silence the top, read-clear stale int_top, per group
 * {disable, W1C the default mask, enable}, then open int_top_en and the two GLB
 * aggregation gates.
 */
static int cg_intr_setup(struct cortina_gpon *cg, struct platform_device *pdev)
{
	u32 v;
	int ret;

	cg->irq = platform_get_irq(pdev, 0);
	if (cg->irq < 0) {
		dev_warn(cg->dev, "no interrupt in DT (%d) - post-O5 servicing OFF\n",
			 cg->irq);
		return cg->irq;
	}
	/* request before unmasking the hardware gates so no edge is lost */
	ret = devm_request_irq(cg->dev, cg->irq, cg_isr, IRQF_SHARED,
			       DRV_NAME, cg);
	if (ret) {
		dev_warn(cg->dev, "request_irq(%d) failed: %d\n", cg->irq, ret);
		return ret;
	}

	cg_mac_intr_arm(cg);	/* the four MAC int groups + int_top */

	/*
	 * GLB aggregation: PON_MACe (level 1) and ne_ictl line 5 (level 2).
	 * Only set our bits: the other ne_ictl lines belong to the NI.
	 */
	v = readl(cg->glb + CG_GLB_PON_INTEN0);
	writel(v | CG_PON_INT0_PON_MAC, cg->glb + CG_GLB_PON_INTEN0);
	v = readl(cg->glb + CG_GLB_NE_ICTL_EN);
	writel(v | CG_NE_ICTL_PON_LINE, cg->glb + CG_GLB_NE_ICTL_EN);

	dev_dbg(cg->dev, "interrupts armed: irq %d, int_en=0x%08x int_top_en=0x%x pon_inten0=0x%08x ne_ictl_en=0x%08x\n",
		cg->irq, readl(cg->mac + CG_REG_INT_EN),
		readl(cg->mac + CG_REG_INT_TOP_EN),
		readl(cg->glb + CG_GLB_PON_INTEN0),
		readl(cg->glb + CG_GLB_NE_ICTL_EN));
	return 0;
}

static void cg_intr_teardown(struct cortina_gpon *cg)
{
	u32 v;

	if (cg->irq >= 0) {
		/*
		 * close the gates first, so nothing can queue more work behind
		 * the flush below
		 */
		writel(0, cg->mac + CG_REG_INT_TOP_EN);
		v = readl(cg->glb + CG_GLB_NE_ICTL_EN);
		writel(v & ~CG_NE_ICTL_PON_LINE, cg->glb + CG_GLB_NE_ICTL_EN);
	}
	/*
	 * Always flush the bottom half: the OMCI RX hook and the post-O5
	 * supervisor queue isr_work even if the interrupt path was never armed.
	 * cortina_gpon_remove() has already cancelled coldstart_work, so
	 * nothing re-queues after this.
	 */
	cancel_work_sync(&cg->isr_work);
}

/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */

static int cg_wan_open(struct net_device *ndev)
{
	struct cortina_gpon *cg = cg_singleton;

	if (cg && cg->data_installed)
		netif_carrier_on(ndev);
	else
		netif_carrier_off(ndev);
	netif_start_queue(ndev);
	return 0;
}

static int cg_wan_stop(struct net_device *ndev)
{
	netif_stop_queue(ndev);
	return 0;
}

/*
 * The VoQ of an installed upstream GEM, 0 if there is none. Read locklessly
 * from the transmit path; isr_work updates the table.
 */
static u16 cg_gem_voq(struct cortina_gpon *cg, u16 port)
{
	unsigned int n;

	for (n = 0; n < CG_DATA_GEM_MAX; n++) {
		const struct cg_hw_gem *h = &cg->hw_gm[n];

		if (READ_ONCE(h->port) == port && READ_ONCE(h->tcont) >= 0)
			return READ_ONCE(h->idx);
	}
	return 0;
}

static netdev_tx_t cg_wan_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct cortina_gpon *cg = cg_singleton;

	u16 idx = cg ? READ_ONCE(cg->us_idx) : 0;
	int port;

	if (cg && cortina_epon_active(cg->epon))
		return cortina_epon_wan_xmit(cg->epon, skb, ndev);

	if (!idx || !IS_REACHABLE(CONFIG_CORTINA_NI))
		goto drop;
	port = cx_us_service(skb);
	if (port < 0)
		goto drop;
	if (port)
		idx = cg_gem_voq(cg, port) ?: idx;
	return cortina_ni_pon_data_tx(skb, ndev, idx);

drop:
	ndev->stats.tx_dropped++;
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

static int cg_wan_get_module_info(struct net_device *ndev,
				  struct ethtool_modinfo *modinfo)
{
	modinfo->type = ETH_MODULE_SFF_8472;
	modinfo->eeprom_len = ETH_MODULE_SFF_8472_LEN;
	return 0;
}

static int cg_wan_get_module_eeprom(struct net_device *ndev,
				    struct ethtool_eeprom *ee, u8 *data)
{
	struct cortina_gpon *cg = cg_singleton;
	u16 off = ee->offset, len = ee->len;
	int ret;

	if (!cg || !len)
		return -EINVAL;

	if (off < 256) {
		u16 n = min_t(u16, len, 256 - off);

		ret = cg_bosa_eeprom_read(cg->dev, 0x50, 0, off, n, data);
		if (ret < 0)
			return ret;
		data += n;
		off += n;
		len -= n;
	}
	if (len > 0 && off >= 256 && off < 512) {
		u16 n = min_t(u16, len, 512 - off);

		ret = cg_bosa_eeprom_read(cg->dev, 0x51, 0, off - 256, n, data);
		if (ret < 0)
			return ret;
	}
	return 0;
}

static int cg_wan_get_module_eeprom_by_page(struct net_device *ndev,
					    const struct ethtool_module_eeprom *page,
					    struct netlink_ext_ack *extack)
{
	struct cortina_gpon *cg = cg_singleton;

	if (!cg)
		return -ENODEV;
	return cg_bosa_eeprom_read(cg->dev, page->i2c_address, page->page,
				   page->offset, page->length, page->data);
}

static int cg_wan_get_link_ksettings(struct net_device *ndev,
				     struct ethtool_link_ksettings *cmd)
{
	struct cortina_gpon *cg = cg_singleton;
	bool epon = cg && cortina_epon_active(cg->epon);

	ethtool_link_ksettings_zero_link_mode(cmd, supported);
	ethtool_link_ksettings_zero_link_mode(cmd, advertising);
	ethtool_link_ksettings_add_link_mode(cmd, supported, FIBRE);
	ethtool_link_ksettings_add_link_mode(cmd, advertising, FIBRE);
	if (epon) {
		ethtool_link_ksettings_add_link_mode(cmd, supported, 1000baseX_Full);
		ethtool_link_ksettings_add_link_mode(cmd, advertising, 1000baseX_Full);
	} else {
		ethtool_link_ksettings_add_link_mode(cmd, supported, 2500baseX_Full);
		ethtool_link_ksettings_add_link_mode(cmd, advertising, 2500baseX_Full);
	}

	cmd->base.port = PORT_FIBRE;
	cmd->base.autoneg = AUTONEG_DISABLE;

	if (netif_carrier_ok(ndev)) {
		cmd->base.speed = epon ? SPEED_1000 : SPEED_2500;
		cmd->base.duplex = DUPLEX_FULL;
	} else {
		cmd->base.speed = SPEED_UNKNOWN;
		cmd->base.duplex = DUPLEX_UNKNOWN;
	}
	return 0;
}

static const struct ethtool_ops cg_wan_ethtool_ops = {
	.get_link			= ethtool_op_get_link,
	.get_link_ksettings		= cg_wan_get_link_ksettings,
	.get_module_info		= cg_wan_get_module_info,
	.get_module_eeprom		= cg_wan_get_module_eeprom,
	.get_module_eeprom_by_page	= cg_wan_get_module_eeprom_by_page,
};

static const struct net_device_ops cg_wan_ops = {
	.ndo_open		= cg_wan_open,
	.ndo_stop		= cg_wan_stop,
	.ndo_start_xmit		= cg_wan_xmit,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_set_mac_address	= eth_mac_addr,
	/*
	 * nf_flow_table HW offload binds a flow block on every hooked device,
	 * so the WAN netdev needs the same setup_tc entry as eth0 (cortina-ni);
	 * without it the flowtable offload setup fails with -EOPNOTSUPP.
	 */
#if IS_REACHABLE(CONFIG_CORTINA_NI)
	.ndo_setup_tc		= cortina_ppe_setup_tc,
#endif
};

/*
 * Register the WAN netdev pon0 with the MAC from the DT, or a random one.
 * Carrier follows the data path install.
 */
/* netifd reports it as the device type, so LuCI can draw a PON adapter */
static const struct device_type cg_pon_type = {
	.name = "pon",
};

static void cg_wan_create(struct cortina_gpon *cg)
{
	struct net_device *ndev;

	ndev = alloc_etherdev(0);
	if (!ndev)
		return;
	strscpy(ndev->name, "pon0", sizeof(ndev->name));
	ndev->netdev_ops = &cg_wan_ops;
	ndev->ethtool_ops = &cg_wan_ethtool_ops;
	if (is_valid_ether_addr(cg->wan_addr))
		eth_hw_addr_set(ndev, cg->wan_addr);
	else
		eth_hw_addr_random(ndev);
	SET_NETDEV_DEV(ndev, cg->dev);
	SET_NETDEV_DEVTYPE(ndev, &cg_pon_type);
	netif_carrier_off(ndev);
	if (register_netdev(ndev)) {
		dev_warn(cg->dev, "pon0 register failed - no WAN netdev\n");
		free_netdev(ndev);
		return;
	}
	cg->wan_ndev = ndev;
	if (IS_REACHABLE(CONFIG_CORTINA_NI))
		cortina_ni_pon_wan_ndev_set(ndev, cx_ds_service);
	dev_dbg(cg->dev, "WAN netdev pon0 registered (%pM)\n", ndev->dev_addr);
}

/* Read the 4 ASCII bytes of the vendor-id register in wire order. */
static void cg_read_vendor(struct cortina_gpon *cg, char out[5])
{
	u32 v = cg_mac_rd(cg, CG_REG_VENDOR);

	out[0] = (v >> 24) & 0xff;
	out[1] = (v >> 16) & 0xff;
	out[2] = (v >> 8) & 0xff;
	out[3] = v & 0xff;
	out[4] = '\0';
}

/*
 * GPON MAC hardware error and statistics counters. BIP-8 and FEC show the
 * downstream link quality; the GEM/PLend/BWmap counters show frames mangled in
 * the GTC layer; ds_asmbl_drop and gem_frag_drop show frames dropped inside the
 * GEM stage. Read-only and idempotent. A block that reads all-ones is undecoded
 * or powered down and must be treated as unavailable, not as zero.
 */
static void cg_show_gpon_mib(struct seq_file *m, struct cortina_gpon *cg)
{
	u32 bip = cg_mac_rd(cg, CG_REG_BIP_ERR);
	u32 accum = cg_mac_rd(cg, CG_REG_BIP_ERR_ACCUM);
	u32 frames = cg_mac_rd(cg, CG_REG_BIP_ERR_FRAMES);
	u32 fec_total = cg_mac_rd(cg, CG_REG_FEC_BLK_TOTAL);
	u32 v;
	bool live = !(bip == U32_MAX && accum == U32_MAX &&
		      frames == U32_MAX && fec_total == U32_MAX);

	seq_printf(m,
		   "gpon_ds_err    = %s bip=%u bip_accum=%u bip_frames=%u gem_frag_drop=%u gem_1bit=%u gem_2bit=%u gem_uncorr=%u omci_crc=%u ds_asmbl_drop=%u (accumulating, sw-cleared)\n",
		   live ? "live" : "UNAVAILABLE (block reads all-ones)",
		   bip, accum, frames,
		   cg_mac_rd(cg, CG_REG_GEM_FRAG_DROP),
		   cg_mac_rd(cg, CG_REG_GEM_1BITERR),
		   cg_mac_rd(cg, CG_REG_GEM_2BITERR),
		   cg_mac_rd(cg, CG_REG_GEM_UNCORR),
		   cg_mac_rd(cg, CG_REG_OMCI_CRC),
		   cg_mac_rd(cg, CG_REG_DS_ASMBL_DROP));
	seq_printf(m,
		   "gpon_ds_mib    = omci_gem=%u omci_pkt=%u ds_crc=%u undersize=%u oversize=%u superframe=%u (hardware DS counts)\n",
		   cg_mac_rd(cg, CG_REG_DS_OMCI_GEM),
		   cg_mac_rd(cg, CG_REG_DS_OMCI_PKT),
		   cg_mac_rd(cg, CG_REG_DS_PKT_CRC),
		   cg_mac_rd(cg, CG_REG_DS_UNDERSIZE),
		   cg_mac_rd(cg, CG_REG_DS_OVERSIZE),
		   cg_mac_rd(cg, CG_REG_SUPERFRAME));
	seq_printf(m,
		   "gpon_us_grant  = bwmap_drop=%u bwmap_corr=%u bwmap_uncorr=%u plend_err=%u plend_biterr=%u o5=%u us_omcc=%u\n",
		   cg_mac_rd(cg, CG_REG_BWMAP_DROP),
		   cg_mac_rd(cg, CG_REG_BWMAP_CORR),
		   cg_mac_rd(cg, CG_REG_BWMAP_UNCORR),
		   cg_mac_rd(cg, CG_REG_PLEND_ERR),
		   cg_mac_rd(cg, CG_REG_PLEND_BITERR),
		   cg_mac_rd(cg, CG_REG_O5),
		   cg_mac_rd(cg, CG_REG_US_OMCC_CNT));
	/*
	 * Latch reporting a hang of the GPON MAC to PUC interface, with the
	 * offending T-CONT.
	 */
	v = cg_mac_rd(cg, CG_REG_PUCIF_PROTECT);
	seq_printf(m,
		   "gpon_pucif_hang= %s (raw=0x%08x, tcont=%u) (latched, not cleared by this read)\n",
		   (v & BIT(0)) ? "HUNG" : "no", v, (v >> 1) & 0x1f);
	seq_printf(m,
		   "gpon_fec       = downstream %s, upstream %s\n"
		   "gpon_fec_regs  = ctrl=0x%08x status=0x%08x total=%u clean=%u corr=%u uncorr=%u corr_bytes=%u\n",
		   cg->ds_fec < 0 ? "not signalled yet" : cg->ds_fec ? "on" : "off",
		   cg->us_fec < 0 ? "not sampled yet" : cg->us_fec ? "on" : "off",
		   cg_mac_rd(cg, CG_REG_FEC_CTRL),
		   cg_mac_rd(cg, CG_REG_FEC_MISC_STATUS),
		   fec_total,
		   cg_mac_rd(cg, CG_REG_FEC_CLEAN_BLK),
		   cg_mac_rd(cg, CG_REG_FEC_CORR_BLK),
		   cg_mac_rd(cg, CG_REG_FEC_UNCORR_BLK),
		   cg_mac_rd(cg, CG_REG_FEC_CORR_BYTES));
}

static int cg_status_show(struct seq_file *m, void *v)
{
	struct cortina_gpon *cg = m->private;
	char vendor[5], sn_str[13];
	u32 onu, alarm;

	cg_read_vendor(cg, vendor);
	onu = cg_mac_rd(cg, CG_REG_GPON_ONU);
	alarm = cg_mac_rd(cg, CG_REG_ALARM);

	seq_printf(m, "gpon-mac @ phys 0x%llx + 0x%x\n",
		   (unsigned long long)CG_PON_WINDOW_PHYS, CG_GPON_MAC_OFF);
	seq_printf(m, "vendor-id      = 0x%08x (\"%s\")\n",
		   cg_mac_rd(cg, CG_REG_VENDOR), vendor);
	seq_printf(m, "vendor-spec    = 0x%08x\n", cg_mac_rd(cg, CG_REG_VENDOR_SPEC));
	/*
	 * The identity and its source: "board" means read from this unit; NONE
	 * = ranging held off waiting for it; FALLBACK = placeholder.
	 */
	cg_sn_format(cg->sn, sn_str);
	seq_printf(m, "serial-number  = %s\n",
		   cg->sn_src == CG_SN_NONE ? "(not provisioned)" : sn_str);
	seq_printf(m, "sn-source      = %s%s\n", cg_sn_src_name[cg->sn_src],
		   cg->activated ? "" : " (ranging not started)");
	seq_printf(m, "gpon_ds        = 0x%08x\n", cg_mac_rd(cg, CG_REG_GPON_DS));
	seq_printf(m, "onu(state+id)  = 0x%08x\n", onu);
	seq_printf(m, "main(eqd)      = 0x%08x\n", cg_mac_rd(cg, CG_REG_GPON_MAIN));
	seq_printf(m, "alarm          = 0x%08x%s\n", alarm,
		   alarm ? " (LOS/LOF!)" : " (no alarm, DS locked)");
	seq_printf(m, "onu_cfg        = 0x%08x\n", cg_mac_rd(cg, CG_REG_ONU_CFG_REAL));
	seq_printf(m, "us(frame_var)  = 0x%08x  t3_preamble = 0x%08x  gpon_ctrl = 0x%08x\n",
		   cg_mac_rd(cg, CG_REG_US), cg_mac_rd(cg, CG_REG_T3_PREAMBLE),
		   cg_mac_rd(cg, CG_REG_GPON_MAC_CTRL));
	/* post-O5 servicing: interrupts, FSM tracker, OMCC bind */
	seq_puts(m, "-- post-O5 servicing --\n");
	seq_printf(m, "irq            = %d (count=%u, evt_drop=%u)\n",
		   cg->irq, cg->irq_count, cg->evt_drop);
	seq_printf(m, "fsm            = %s (live id 0x%02x), tracked %s\n",
		   cg_state_name[CG_ONU_STATE(onu)], CG_ONU_ID(onu),
		   cg_state_name[cg->last_state & 7]);
	seq_printf(m, "omcc           = %s (alloc=%u gem=%u)\n",
		   cg->omcc_up ? "UP" : "down", cg->omcc_alloc, cg->omcc_gem);
	seq_printf(m, "ds_omci_rx     = %u (short=%u)  pdc_ctrl = 0x%08x (%s, expect 0x02870002)\n",
		   cg->omci_rx, cg->omci_rx_short, readl(cg->pon + CG_PDC_CTRL),
		   cg->pdc_ready ? "programmed" : "NOT programmed");
	/*
	 * us_rx/enq/drop are short-window upstream admission counters, read
	 * raw; they only mean something as a delta across traffic the reader
	 * generates. FORCE_DROP is 16 bits wide.
	 */
	seq_printf(m, "puc            = %s  us_rx=%u enq=%u drop=%u  pucif=0x%08x\n",
		   cg->puc_ready ? "programmed" : "NOT programmed",
		   readl(cg->pon + CG_PUC_BMC_RX_PKT),
		   readl(cg->pon + CG_PUC_BMC_RX_PKT_ENQ),
		   readl(cg->pon + CG_PUC_BMC_FORCE_DROP) & CG_PUC_BMC_CNTR_MASK,
		   readl(cg->pon + CG_GPON_MAC_PUCIF_CTRL));
	/*
	 * Cumulative count of upstream frames matched against the OMCI link
	 * type, summed from the clear-on-read deltas (see
	 * cg_puc_ctrl_sample()). Reading it takes a sample itself. Upstream
	 * user data cannot inflate it; compare it with omci_resp tx= to tell
	 * "no reply built" from "never left the CPU".
	 */
	cg_puc_ctrl_sample(cg);
	spin_lock(&cg->puc_cnt_lock);
	seq_printf(m,
		   "puc_ctrl       = us_omci=%u ctrl_mac=%u len_err=%u samples=%u lnk_type=0x%04x (cumulative)\n",
		   cg->puc_omci_us, cg->puc_ctrl_mac, cg->puc_len_err,
		   cg->puc_cnt_samples,
		   readl(cg->pon + CG_PUC_GLOBAL_LNK_TYPE) >> 16);
	spin_unlock(&cg->puc_cnt_lock);
	cg_show_gpon_mib(m, cg);
	seq_printf(m, "omci_resp      = %s tx=%u fail=%u ds_crc ok=%u bad=%u",
		   cg->omci_active ? "armed" : "off",
		   cg->omci_tx, cg->omci_tx_fail,
		   cg->omci_ds_crc_ok, cg->omci_ds_crc_bad);
	if (cg->omci)
		seq_printf(m, "  mds=%u store=%u avc=%u unhandled=%u dup_replay=%u ext=%u no_ack=%u",
			   cg->omci->mds, cg->omci->store_n,
			   cg->omci->avc_count, cg->omci->unhandled,
			   cg->omci->dup_replay, cg->omci->rx_extended,
			   cg->omci->no_ack);
	seq_putc(m, '\n');
	seq_printf(m, "data           = %s default VoQ %u bcast=%s carrier=%d\n",
		   cg->data_installed ? "INSTALLED" : "down", cg->us_idx,
		   cg->bcast_bound ? "bound" : "off",
		   cg->wan_ndev ? netif_carrier_ok(cg->wan_ndev) : -1);
	{
		unsigned int i;

		for (i = 0; i < CG_DATA_TCONT_MAX; i++)
			if (cg->tc[i].used || cg->hw_tc[i].alloc)
				seq_printf(m, "  tcont %u      = me 0x%04x alloc %u, bound %u, queues 0x%02x\n",
					   i + 1, cg->tc[i].inst, cg->tc[i].alloc,
					   cg->hw_tc[i].alloc, cg->hw_tc[i].queues);
		for (i = 0; i < CG_DATA_GEM_MAX; i++)
			if (cg->gm[i].used || cg->hw_gm[i].idx)
				seq_printf(m, "  gem %-4u     = ctp 0x%04x tcont-ptr 0x%04x dir %u, index %u\n",
					   cg->gm[i].port, cg->gm[i].inst,
					   cg->gm[i].tcont_inst, cg->gm[i].dir,
					   cg->hw_gm[i].idx);
	}
	seq_printf(m, "omci_port      = 0x%08x (en=%d id=%u)\n",
		   cg_mac_rd(cg, CG_REG_OMCI_PORT),
		   !!(cg_mac_rd(cg, CG_REG_OMCI_PORT) & CG_OMCI_PORT_EN),
		   CG_OMCI_PORT_ID(cg_mac_rd(cg, CG_REG_OMCI_PORT)));
	seq_printf(m, "int_en/top_en  = 0x%08x / 0x%x  (int2/3/4_en = 0x%x/0x%x/0x%x)\n",
		   cg_mac_rd(cg, CG_REG_INT_EN), cg_mac_rd(cg, CG_REG_INT_TOP_EN),
		   cg_mac_rd(cg, CG_REG_INT2_EN), cg_mac_rd(cg, CG_REG_INT3_EN),
		   cg_mac_rd(cg, CG_REG_INT4_EN));
	if (cg->glb)
		seq_printf(m, "glb pon_int0   = 0x%08x en=0x%08x  ne_ictl sts=0x%08x en=0x%08x\n",
			   readl(cg->glb + CG_GLB_PON_INT0),
			   readl(cg->glb + CG_GLB_PON_INTEN0),
			   readl(cg->glb + CG_GLB_NE_ICTL_STS),
			   readl(cg->glb + CG_GLB_NE_ICTL_EN));

	/* serdes/gearbox/laser (PON window raw offsets) */
	seq_puts(m, "-- serdes/gbox/laser --\n");
	/*
	 * Read back the two tables the upstream data path depends on: every VoQ
	 * of the data T-CONT (slots 8..15) must stamp the data GEM port-id, or
	 * the OLT cannot match upstream frames; and the T-CONT CAM entry the
	 * OLT's grants address must carry the data hardware T-CONT index.
	 */
	{
		unsigned int i;

		seq_puts(m, "us_port_tbl   : ");
		for (i = 0; i < (CG_DATA_TCONT_MAX + 1) * CG_PUC_QUEUE_PER_TCONT; i++) {
			u32 v;

			if (cg_tbl_op(cg, CG_REG_US_PORT_ACCESS, i))
				break;
			v = readl(cg->mac + CG_REG_US_PORT_DATA) & 0xfff;
			if (v || i < CG_OMCC_US_GEM_IDX_NUM)
				seq_printf(m, "%u:%u ", i, v);
		}
		seq_putc(m, '\n');
	}
	cg_us_mib_show(cg, m);
	{
		unsigned int t;

		seq_puts(m, "bufocc        : ");
		for (t = 0; t < 4; t++) {
			u32 v = 0;

			if (cg_tbl_op(cg, CG_REG_DBRU_BUFOCC_ACCESS, t & 0x1f))
				break;
			v = readl(cg->mac + CG_REG_DBRU_BUFOCC_DATA) & 0xffff;
			seq_printf(m, "tcont%u=%u ", t, v);
		}
		seq_puts(m, " (LEVEL not total - sample DURING a dial.  >0 = frames reached the PUC and await a GRANT; ==0 = they never got there)\n");
	}
	seq_printf(m, "rgb8(a05c)     = 0x%08x  (DS-lock: (v&0x9c01)==0x9c00)\n", readl(cg->pon + 0xa05c));
	/*
	 * PSDS internal CMU register 0x400 (the relock strobe target), read
	 * through the indirect interface
	 */
	writel(CG_PSDS_IND_READ | CG_PSDS_CMU_IDX, cg->pon + CG_PSDS_IND_CMD);
	udelay(10);
	seq_printf(m, "cmu[0x400]     = a090=0x%08x a08c=0x%08x  (re-lock strobes [7:4]; coldstart re-rolls=%u episode=%d)\n",
		   readl(cg->pon + CG_PSDS_IND_RDATA), readl(cg->pon + CG_PSDS_IND_WDATA),
		   cg->coldstart_rolls, cg->coldstart_tries);
	seq_printf(m, "gbox(a060)     = 0x%08x  (stock 0x454 rx/tx bit-order)\n", readl(cg->pon + 0xa060));
	seq_printf(m, "reg(a064)      = 0x%08x  (stock 0)\n", readl(cg->pon + 0xa064));
	seq_printf(m, "reg(a068)      = 0x%08x  (stock 1)\n", readl(cg->pon + 0xa068));
	seq_printf(m, "reg(a070)      = 0x%08x  (stock 1)\n", readl(cg->pon + 0xa070));
	seq_printf(m, "psds_init(glb) = 0x%08x  (ben_oen bit4, pow_pcix bit5)\n", readl(cg->glb + CG_GLB_PSDS_INIT));
	/*
	 * The laser route and GPIO registers must not be read here:
	 * CG_GLB_PINROUTE, the GPIO mux and CG_PERGPIO_* are not accessed
	 * anywhere else, and reading them from /proc caused a synchronous
	 * external abort. Only dump registers the driver already uses on a
	 * working path.
	 */
	cg_bosa_proc_show(cg->dev, m);
	/*
	 * live optical diagnostics; also refreshes the ANI-G levels the OLT
	 * reads
	 */
	cg_optic_sample(cg, m);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(cg_status);

/*
 * omcid's controls, at /sys/class/net/pon0/device/. allow_tx programs the laser
 * driver; until it is set nothing can burst.
 */
static ssize_t allow_tx_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct cortina_gpon *cg = dev_get_drvdata(dev);

	if (!cg)
		return -ENODEV;
	return sysfs_emit(buf, "%d\n", cg->tx_allowed);
}

static ssize_t allow_tx_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct cortina_gpon *cg = dev_get_drvdata(dev);
	bool on;
	int ret;

	if (!cg)
		return -ENODEV;
	ret = kstrtobool(buf, &on);
	if (ret)
		return ret;
	if (on == cg->tx_allowed)
		return count;
	if (!on)
		return -EOPNOTSUPP;
	cg_tx_pins(cg, true);
	mdelay(10);
	ret = cg_bosa_init(dev, cg->bosa_cal);
	if (ret) {
		cg_tx_pins(cg, false);
		return ret;
	}
	cg->tx_allowed = true;
	return count;
}
static DEVICE_ATTR_RW(allow_tx);

/* omci-agent's release after applying the identity; ranging already waits for it. */
static ssize_t gpon_identity_wait_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	return count;
}
static DEVICE_ATTR_WO(gpon_identity_wait);

/*
 * The GN25L95 keeps chip registers where SFF-8472 has its levels (A2h
 * 0x18-0x27), so the BOSA's own levels are given here, in nW, as on the
 * V2802RH with the same BOSA: a 2.5G PIN receiver holds about -30 dBm and
 * overloads near -7 dBm; the laser is set for the class B+ 0.5 to 5 dBm.
 */
#define CG_BOSA_RX_ALARM_LOW	1000		/* -30 dBm */
#define CG_BOSA_RX_WARN_LOW	1585		/* -28 dBm */
#define CG_BOSA_RX_WARN_HIGH	158489		/* -8 dBm */
#define CG_BOSA_RX_ALARM_HIGH	199526		/* -7 dBm */
#define CG_BOSA_TX_ALARM_LOW	1000000		/* 0 dBm */
#define CG_BOSA_TX_WARN_LOW	1122018		/* 0.5 dBm */
#define CG_BOSA_TX_WARN_HIGH	3162278		/* 5 dBm */
#define CG_BOSA_TX_ALARM_HIGH	3981072		/* 6 dBm */

static ssize_t bosa_thresholds_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf,
			  "tx_alarm_high_nw=%u tx_alarm_low_nw=%u tx_warn_high_nw=%u tx_warn_low_nw=%u "
			  "rx_alarm_high_nw=%u rx_alarm_low_nw=%u rx_warn_high_nw=%u rx_warn_low_nw=%u\n",
			  CG_BOSA_TX_ALARM_HIGH, CG_BOSA_TX_ALARM_LOW,
			  CG_BOSA_TX_WARN_HIGH, CG_BOSA_TX_WARN_LOW,
			  CG_BOSA_RX_ALARM_HIGH, CG_BOSA_RX_ALARM_LOW,
			  CG_BOSA_RX_WARN_HIGH, CG_BOSA_RX_WARN_LOW);
}
static DEVICE_ATTR_RO(bosa_thresholds);

static ssize_t bosa_ddm_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct cg_bosa_ddm d;

	if (cg_bosa_ddm_read(dev, &d) != CG_DDM_OK)
		return -EIO;

	return sysfs_emit(buf,
			  "temp_mc=%ld vcc_uv=%ld bias_ua=%ld tx_nw=%ld rx_nw=%ld raw=%04x%04x%04x%04x%04x\n",
			  (long)cg_ddm_temp_mdegc(d.temp),
			  (long)cg_ddm_vcc_mv(d.vcc) * 1000L,
			  (long)cg_ddm_bias_ua(d.bias),
			  (long)d.tx_pwr * 100L,
			  (long)d.rx_pwr * 100L,
			  (u16)d.temp, d.vcc, d.bias, d.tx_pwr, d.rx_pwr);
}
static DEVICE_ATTR_RO(bosa_ddm);

static ssize_t epon_state_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct cortina_gpon *cg = dev_get_drvdata(dev);

	return cg ? cortina_epon_state_show(cg->epon, buf) : -ENODEV;
}
static DEVICE_ATTR_RO(epon_state);

static ssize_t epon_mac_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct cortina_gpon *cg = dev_get_drvdata(dev);

	return cg ? cortina_epon_mac_show(cg->epon, buf) : -ENODEV;
}

static ssize_t epon_mac_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct cortina_gpon *cg = dev_get_drvdata(dev);

	return cg ? cortina_epon_mac_store(cg->epon, buf, count) : -ENODEV;
}
static DEVICE_ATTR_RW(epon_mac);

static ssize_t epon_oam_link_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct cortina_gpon *cg = dev_get_drvdata(dev);

	return cg ? cortina_epon_oam_link_store(cg->epon, buf, count) : -ENODEV;
}
static DEVICE_ATTR_WO(epon_oam_link);

static ssize_t epon_register_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct cortina_gpon *cg = dev_get_drvdata(dev);

	return cg ? cortina_epon_register_store(cg->epon, buf, count) : -ENODEV;
}
static DEVICE_ATTR_WO(epon_register);

static ssize_t epon_key_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct cortina_gpon *cg = dev_get_drvdata(dev);

	return cg ? cortina_epon_key_store(cg->epon, buf, count) : -ENODEV;
}
static DEVICE_ATTR_WO(epon_key);

static ssize_t epon_holdover_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct cortina_gpon *cg = dev_get_drvdata(dev);

	return cg ? cortina_epon_holdover_show(cg->epon, buf) : -ENODEV;
}

static ssize_t epon_holdover_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct cortina_gpon *cg = dev_get_drvdata(dev);

	return cg ? cortina_epon_holdover_store(cg->epon, buf, count) : -ENODEV;
}
static DEVICE_ATTR_RW(epon_holdover);

static ssize_t tx_hold_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct cortina_gpon *cg = dev_get_drvdata(dev);

	return cg ? cortina_epon_tx_hold_show(cg->epon, buf) : -ENODEV;
}

static ssize_t tx_hold_store(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct cortina_gpon *cg = dev_get_drvdata(dev);

	return cg ? cortina_epon_tx_hold_store(cg->epon, buf, count) : -ENODEV;
}
static DEVICE_ATTR_RW(tx_hold);

static ssize_t tx_hold_seconds_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct cortina_gpon *cg = dev_get_drvdata(dev);

	return cg ? cortina_epon_tx_hold_seconds_show(cg->epon, buf) : -ENODEV;
}

static ssize_t tx_hold_seconds_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct cortina_gpon *cg = dev_get_drvdata(dev);

	return cg ? cortina_epon_tx_hold_seconds_store(cg->epon, buf, count) :
		    -ENODEV;
}
static DEVICE_ATTR_RW(tx_hold_seconds);

static struct attribute *cg_attrs[] = {
	&dev_attr_allow_tx.attr,
	&dev_attr_gpon_identity_wait.attr,
	&dev_attr_bosa_thresholds.attr,
	&dev_attr_bosa_ddm.attr,
	&dev_attr_epon_state.attr,
	&dev_attr_epon_mac.attr,
	&dev_attr_epon_oam_link.attr,
	&dev_attr_epon_register.attr,
	&dev_attr_epon_key.attr,
	&dev_attr_epon_holdover.attr,
	&dev_attr_tx_hold.attr,
	&dev_attr_tx_hold_seconds.attr,
	NULL
};
ATTRIBUTE_GROUPS(cg);

/* ---- Standard Linux hwmon interface for SFF-8472 BOSA DDM ---- */

static umode_t cg_bosa_hwmon_is_visible(const void *data,
					enum hwmon_sensor_types type,
					u32 attr, int channel)
{
	return 0444;
}

static int cg_bosa_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			      u32 attr, int channel, long *val)
{
	struct cg_bosa_ddm d;

	if (type == hwmon_temp && attr == hwmon_temp_max) {
		*val = 85000;
		return 0;
	}
	if (type == hwmon_temp && attr == hwmon_temp_crit) {
		*val = 95000;
		return 0;
	}

	if (cg_bosa_ddm_read(dev, &d) != CG_DDM_OK)
		return -EIO;

	switch (type) {
	case hwmon_temp:
		*val = cg_ddm_temp_mdegc(d.temp);
		return 0;
	case hwmon_in:
		*val = cg_ddm_vcc_mv(d.vcc);
		return 0;
	case hwmon_curr:
		*val = DIV_ROUND_CLOSEST(cg_ddm_bias_ua(d.bias), 1000);
		return 0;
	case hwmon_power:
		/* power1 = TX (uW), power2 = RX (uW); SFF-8472 word is 0.1 uW */
		*val = (long)(channel == 0 ? d.tx_pwr : d.rx_pwr) / 10;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int cg_bosa_hwmon_read_string(struct device *dev,
				     enum hwmon_sensor_types type,
				     u32 attr, int channel, const char **str)
{
	switch (type) {
	case hwmon_temp:
		*str = "bosa_temp";
		return 0;
	case hwmon_in:
		*str = "bosa_vcc";
		return 0;
	case hwmon_curr:
		*str = "bosa_tx_bias";
		return 0;
	case hwmon_power:
		*str = channel == 0 ? "bosa_tx_power" : "bosa_rx_power";
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static const struct hwmon_channel_info * const cg_bosa_hwmon_info[] = {
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT | HWMON_T_MAX | HWMON_T_CRIT | HWMON_T_LABEL),
	HWMON_CHANNEL_INFO(in, HWMON_I_INPUT | HWMON_I_LABEL),
	HWMON_CHANNEL_INFO(curr, HWMON_C_INPUT | HWMON_C_LABEL),
	HWMON_CHANNEL_INFO(power,
			   HWMON_P_INPUT | HWMON_P_LABEL,
			   HWMON_P_INPUT | HWMON_P_LABEL),
	NULL
};

static const struct hwmon_ops cg_bosa_hwmon_ops = {
	.is_visible	= cg_bosa_hwmon_is_visible,
	.read		= cg_bosa_hwmon_read,
	.read_string	= cg_bosa_hwmon_read_string,
};

static const struct hwmon_chip_info cg_bosa_hwmon_chip_info = {
	.ops	= &cg_bosa_hwmon_ops,
	.info	= cg_bosa_hwmon_info,
};

/* ---- xPON/OMCI framework glue: what cortina-xpon.c needs from this MAC ---- */

static int cg_cx_omci_tx(void *cookie, const u8 *pdu)
{
	return cg_omci_tx(cookie, pdu);
}

static int cg_cx_set_serial(void *cookie, const u8 serial[8])
{
	char str[13];

	cg_sn_format(serial, str);
	return cg_sn_set(cookie, str, CG_SN_BOARD);
}

/* Takes effect at the next activation, like a changed serial number. */
static int cg_cx_set_password(void *cookie, const u8 password[10])
{
	struct cortina_gpon *cg = cookie;

	mutex_lock(&cg->sn_lock);
	memcpy(cg->password, password, sizeof(cg->password));
	mutex_unlock(&cg->sn_lock);
	return 0;
}

static int cg_cx_get_serial(void *cookie, u8 serial[8])
{
	struct cortina_gpon *cg = cookie;

	if (cg->sn_src == CG_SN_NONE)
		return -ENODATA;
	memcpy(serial, cg->sn, 8);
	return 0;
}

static int cg_cx_state(void *cookie)
{
	return CG_ONU_STATE(cg_mac_rd(cookie, CG_REG_GPON_ONU)) + 1;
}

static int cg_cx_onu_id(void *cookie)
{
	u8 id = CG_ONU_ID(cg_mac_rd(cookie, CG_REG_GPON_ONU));

	return id == CG_ONU_ID_NONE ? -1 : id;
}

static int cg_cx_omcc_gem(void *cookie)
{
	struct cortina_gpon *cg = cookie;

	return cg->omcc_up ? cg->omcc_gem : -1;
}

static int cg_cx_los(void *cookie)
{
	return !!cg_mac_rd(cookie, CG_REG_ALARM);
}

static bool cg_alloc_ours(struct cortina_gpon *cg, u16 alloc)
{
	unsigned int t;

	if (cg->omcc_alloc_valid && alloc == cg->omcc_alloc)
		return true;
	for (t = 0; t < CG_DATA_TCONT_MAX; t++)
		if (cg->hw_tc[t].alloc && cg->hw_tc[t].alloc == alloc)
			return true;
	return false;
}

/* 1 or 0 from our allocations in captured BWmaps, -1 if none was seen */
static int cg_us_fec_sample(struct cortina_gpon *cg)
{
	int fec = -1, try;

	for (try = 0; try < CG_BWMAP_TRIES && fec < 0; try++) {
		u32 ctl = cg_mac_rd(cg, CG_REG_BWMAP_CTL);
		unsigned int n, i;

		if (!(ctl & CG_BWMAP_DONE))
			break;
		writel((ctl & ~CG_BWMAP_DONE) | CG_BWMAP_CAPTURE,
		       cg->mac + CG_REG_BWMAP_CTL);
		msleep(30);
		n = FIELD_GET(CG_BWMAP_COUNT, cg_mac_rd(cg, CG_REG_BWMAP_CTL));
		for (i = 0; i < n; i++) {
			u32 v = cg_mac_rd(cg, CG_REG_BWMAP_DATA1);

			if (cg_alloc_ours(cg, FIELD_GET(CG_BWMAP_ALLOC, v))) {
				fec = !!(v & CG_BWMAP_FEC);
				if (fec)
					break;
			}
		}
	}
	writel(cg_mac_rd(cg, CG_REG_BWMAP_CTL) & ~CG_BWMAP_CAPTURE,
	       cg->mac + CG_REG_BWMAP_CTL);
	return fec;
}

static void cg_qos_changed(struct cortina_gpon *cg)
	__releases(&cg->data_lock)
{
	cg->qos_dirty = true;
	spin_unlock_bh(&cg->data_lock);
	schedule_work(&cg->isr_work);
}

/* T-CONT @inst: WRR with one weight per priority, or strict if @weight is NULL */
static int cg_cx_tcont_qos(void *cookie, u16 inst, const u8 *weight)
{
	struct cortina_gpon *cg = cookie;
	struct cg_tcont_qos *qos = NULL, *free = NULL;
	unsigned int i;

	spin_lock_bh(&cg->data_lock);
	for (i = 0; i < CG_DATA_TCONT_MAX; i++) {
		if (cg->tq[i].used && cg->tq[i].inst == inst)
			qos = &cg->tq[i];
		else if (!cg->tq[i].used && !free)
			free = &cg->tq[i];
	}
	if (!qos && !weight) {
		spin_unlock_bh(&cg->data_lock);
		return 0;	/* strict is the default */
	}
	if (!qos)
		qos = free;
	if (!qos) {
		spin_unlock_bh(&cg->data_lock);
		return -ENOSPC;
	}
	*qos = (struct cg_tcont_qos){ .inst = inst, .used = true, .wrr = !!weight };
	if (weight)
		memcpy(qos->weight, weight, sizeof(qos->weight));
	cg_qos_changed(cg);
	return 0;
}

/* the priority (0 highest) of the services the agent put on GEM @port */
static void cg_cx_gem_prio(void *cookie, u16 port, u8 prio)
{
	struct cortina_gpon *cg = cookie;
	struct cg_gem_prio *p = NULL, *free = NULL;
	unsigned int i;

	spin_lock_bh(&cg->data_lock);
	for (i = 0; i < CG_DATA_GEM_MAX; i++) {
		if (cg->gp[i].used && cg->gp[i].port == port)
			p = &cg->gp[i];
		else if (!cg->gp[i].used && !free)
			free = &cg->gp[i];
	}
	if (!p)
		p = free;
	if (!p || (p->used && p->prio == (prio & 7))) {
		spin_unlock_bh(&cg->data_lock);
		return;
	}
	*p = (struct cg_gem_prio){ .port = port, .prio = prio & 7, .used = true };
	cg_qos_changed(cg);
}

/*
 * The FEC block counters run freely (they are not cleared by reading), 32
 * bits wide: add each read's difference to 64-bit totals since boot.
 */
static void cg_cx_fec(void *cookie, struct cx_fec *f)
{
	static const u32 regs[4] = {
		CG_REG_FEC_CORR_BYTES, CG_REG_FEC_CORR_BLK,
		CG_REG_FEC_UNCORR_BLK, CG_REG_FEC_BLK_TOTAL,
	};
	struct cortina_gpon *cg = cookie;
	unsigned int i;

	mutex_lock(&cg->fec_lock);
	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		u32 v = cg_mac_rd(cg, regs[i]);

		cg->fec_sum[i] += v - cg->fec_last[i];
		cg->fec_last[i] = v;
	}
	if (cg->last_state == CG_STATE_OPERATION &&
	    time_after(jiffies, cg->us_fec_jiffies + CG_US_FEC_CACHE)) {
		int fec = cg_us_fec_sample(cg);

		if (fec >= 0 && fec != cg->us_fec)
			dev_dbg(cg->dev, "upstream FEC %s by OLT\n",
				 fec ? "enabled" : "disabled");
		if (fec >= 0)
			cg->us_fec = fec;
		cg->us_fec_jiffies = jiffies;
	}
	*f = (struct cx_fec) {
		.downstream = cg->ds_fec,
		.upstream = cg->us_fec,
		.corrected_bytes = cg->fec_sum[0],
		.corrected_codewords = cg->fec_sum[1],
		.uncorrectable_codewords = cg->fec_sum[2],
		.total_codewords = cg->fec_sum[3],
	};
	mutex_unlock(&cg->fec_lock);
}

static void cg_epon_tx_pins(void *cookie, bool on)
{
	cg_tx_pins(cookie, on);
}

static void cg_epon_led_update(void *cookie, bool registered, bool los)
{
	struct cortina_gpon *cg = cookie;

	cg_led_update(cg, registered ? CG_STATE_OPERATION :
		      los ? CG_STATE_INITIAL : CG_STATE_RANGING);
}

static int cg_cx_set_mode(void *cookie, struct xpon_device *xpon,
			  enum xpon_mode mode)
{
	struct cortina_gpon *cg = cookie;

	if (mode == XPON_MODE_EPON) {
		if (!cg->epon)
			return -EOPNOTSUPP;
		if (cortina_epon_active(cg->epon))
			return 0;

		cancel_delayed_work_sync(&cg->sn_wait_work);
		cancel_delayed_work_sync(&cg->coldstart_work);
		cancel_delayed_work_sync(&cg->carrier_work);
		cancel_delayed_work_sync(&cg->puc_cnt_work);
		cancel_work_sync(&cg->activate_work);
		if (IS_REACHABLE(CONFIG_CORTINA_NI))
			cortina_ni_pon_rx_hook_set(NULL);

		return cortina_epon_start(cg->epon, xpon);
	}

	if (mode == XPON_MODE_GPON) {
		if (!cortina_epon_active(cg->epon))
			return 0;

		cortina_epon_stop(cg->epon);
		if (cg->glb) {
			cg_glb_reset(cg);
			cg_psds_init(cg);
		}
		if (IS_REACHABLE(CONFIG_CORTINA_NI)) {
			cortina_ni_pon_rx_hook_set(cg_rx_omci);
			if (cg->wan_ndev)
				cortina_ni_pon_wan_ndev_set(cg->wan_ndev,
							    cx_ds_service);
		}
		schedule_work(&cg->activate_work);
		return 0;
	}

	return -EOPNOTSUPP;
}

static const struct cx_hw_ops cg_cx_hw_ops = {
	.omci_tx	= cg_cx_omci_tx,
	.set_serial	= cg_cx_set_serial,
	.set_password	= cg_cx_set_password,
	.get_serial	= cg_cx_get_serial,
	.state		= cg_cx_state,
	.onu_id		= cg_cx_onu_id,
	.omcc_gem	= cg_cx_omcc_gem,
	.los		= cg_cx_los,
	.fec		= cg_cx_fec,
	.tcont_qos	= cg_cx_tcont_qos,
	.gem_prio	= cg_cx_gem_prio,
	.set_mode	= cg_cx_set_mode,
};

/* Read an optional nvmem cell; returns NULL if the DT does not name it. */
static void *cg_read_cell(struct device *dev, const char *name, size_t *len)
{
	struct nvmem_cell *cell;
	void *buf;

	cell = devm_nvmem_cell_get(dev, name);
	if (IS_ERR(cell))
		return PTR_ERR(cell) == -ENOENT ? NULL : cell;
	buf = nvmem_cell_read(cell, len);
	devm_nvmem_cell_put(dev, cell);
	return buf;
}

static int cg_read_factory_data(struct cortina_gpon *cg)
{
	size_t len;
	u8 *buf;

	buf = cg_read_cell(cg->dev, "calibration", &len);
	if (IS_ERR(buf))
		return dev_err_probe(cg->dev, PTR_ERR(buf), "calibration cell\n");
	if (buf) {
		cg->bosa_cal = devm_kmemdup(cg->dev, buf, len, GFP_KERNEL);
		kfree(buf);
		if (!cg->bosa_cal)
			return -ENOMEM;
		if (len != CG_BOSA_CAL_SIZE) {
			dev_err(cg->dev, "calibration cell is %zu bytes, want %d\n",
				len, CG_BOSA_CAL_SIZE);
			cg->bosa_cal = NULL;
		}
	}
	return 0;
}

static int cortina_gpon_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct cortina_gpon *cg;
	char vendor[5];
	u32 onu;
	int ret;

	cg = devm_kzalloc(dev, sizeof(*cg), GFP_KERNEL);
	if (!cg)
		return -ENOMEM;
	cg->dev = dev;
	cg->irq = -1;		/* until cg_intr_setup succeeds */

	/* The factory data may live in flash that is not registered yet. */
	if (of_get_mac_address(dev->of_node, cg->wan_addr) == -EPROBE_DEFER)
		return -EPROBE_DEFER;
	ret = cg_read_factory_data(cg);
	if (ret)
		return ret;

	/*
	 * The G.988 responder context is allocated up front so the OMCC-up path
	 * only initialises it.
	 */
	spin_lock_init(&cg->omci_lock);
	spin_lock_init(&cg->data_lock);
	mutex_init(&cg->sn_lock);
	spin_lock_init(&cg->puc_cnt_lock);
	/*
	 * Initialise the event ring and bottom half here rather than in
	 * cg_intr_setup(): isr_work is also queued from the OMCI RX hook and
	 * the post-O5 supervisor, which run even with intr=0 or reset=0, and
	 * ranging is autonomous in hardware.
	 */
	spin_lock_init(&cg->evt_lock);
	cg->ds_fec = -1;
	INIT_DELAYED_WORK(&cg->los_work, cg_los_work);
	ret = cg_led_register(cg);
	if (ret)
		return ret;
	cg->us_fec = -1;
	cg->us_fec_jiffies = jiffies - CG_US_FEC_CACHE - 1;
	mutex_init(&cg->fec_lock);
	INIT_WORK(&cg->isr_work, cg_isr_work);
	INIT_DELAYED_WORK(&cg->veip_avc_work, cg_veip_avc_work);
	INIT_DELAYED_WORK(&cg->optic_log_work, cg_optic_log_work);
	INIT_DELAYED_WORK(&cg->stats_work, cg_stats_work);
	/*
	 * Start the counter dump at probe rather than when the OMCI responder
	 * arms, so the first sample precedes the OLT's downstream OMCI burst at
	 * O5 entry.
	 */
	INIT_DELAYED_WORK(&cg->coldstart_work, cg_coldstart_work);
	INIT_DELAYED_WORK(&cg->carrier_work, cg_carrier_work);
	INIT_DELAYED_WORK(&cg->sn_wait_work, cg_sn_wait_work);
	INIT_WORK(&cg->activate_work, cg_activate_work);
	INIT_DELAYED_WORK(&cg->puc_cnt_work, cg_puc_cnt_work);
	cg->omci = devm_kzalloc(dev, sizeof(*cg->omci), GFP_KERNEL);
	if (!cg->omci)
		dev_warn(dev, "no OMCI responder ctx - DS OMCI will not be answered\n");

	/*
	 * Map the whole PON window. It is a 40-bit AXI address; the fixed
	 * physical base is mapped directly rather than as a DT resource,
	 * because the sibling cortina-ni node lists the same window and a
	 * non-exclusive map avoids a request_mem_region conflict.
	 */
	cg->pon = devm_ioremap(dev, CG_PON_WINDOW_PHYS, CG_PON_WINDOW_SIZE);
	if (!cg->pon) {
		dev_err(dev, "failed to map PON window 0x%llx\n",
			(unsigned long long)CG_PON_WINDOW_PHYS);
		return -ENOMEM;
	}
	cg->mac = cg->pon + CG_GPON_MAC_OFF;

	/*
	 * Map the GLB reset/clock window and print the reset control registers.
	 */
	cg->glb = devm_ioremap(dev, CG_GLB_WINDOW_PHYS, CG_GLB_WINDOW_SIZE);
	cg->gpio = devm_ioremap(dev, CG_PERGPIO_PHYS, CG_PERGPIO_SIZE);

	cg->tx_disable = devm_gpiod_get_optional(dev, "tx-disable", GPIOD_OUT_HIGH);
	if (IS_ERR(cg->tx_disable))
		return dev_err_probe(dev, PTR_ERR(cg->tx_disable),
				     "cannot get tx-disable GPIO\n");
	cg->tx_power = devm_gpiod_get_optional(dev, "tx-power", GPIOD_OUT_HIGH);
	if (IS_ERR(cg->tx_power))
		return dev_err_probe(dev, PTR_ERR(cg->tx_power),
				     "cannot get tx-power GPIO\n");
	if (cg->glb) {
		dev_dbg(dev, "GLB reset regs: EPON_CNTL=0x%08x GPON_CNTL=0x%08x PON_CNTL=0x%08x PSDS_INIT=0x%08x\n",
			readl(cg->glb + CG_GLB_EPON_CNTL),
			readl(cg->glb + CG_GLB_GPON_CNTL),
			readl(cg->glb + CG_GLB_PON_CNTL),
			readl(cg->glb + CG_GLB_PSDS_INIT));
		dev_dbg(dev, "GLB reset regs (released): EPON_CNTL=0x00030000 GPON_CNTL=0x00000003 PON_CNTL=0x0000030e\n");

		cg_glb_reset(cg);
		cg_psds_init(cg);
		dev_dbg(dev, "GLB after: EPON=0x%08x GPON=0x%08x PON=0x%08x PSDS_INIT=0x%08x\n",
			readl(cg->glb + CG_GLB_EPON_CNTL),
			readl(cg->glb + CG_GLB_GPON_CNTL),
			readl(cg->glb + CG_GLB_PON_CNTL),
			readl(cg->glb + CG_GLB_PSDS_INIT));
		dev_dbg(dev, "PSDS after: MODE=0x%08x RGB8=0x%08x (bit11 CKRDY_TX=%d)\n",
			readl(cg->pon + CG_PSDS_MODE),
			readl(cg->pon + CG_PSDS_RGB8),
			!!(readl(cg->pon + CG_PSDS_RGB8) & BIT(11)));

		/* dump the whole GLB reset/clock window */

		/*
		 * Experiment: the PUC QM sub-block (0x8238+)
		 * read-aborts, i.e. its clock/reset domain is dead. Set
		 * the GLB clock/reset region to the vendor firmware
		 * values, then read PLEN_MEM_CTL (0x8274) as the check;
		 * it aborts if the domain is still dead.
		 */

		/*
		 * arm the post-O5 servicing before ranging starts, so
		 * the events of the first O1 -> O5 pass are serviced
		 */
		cg_intr_setup(cg, pdev);
		cg_dg_setup(cg, pdev);

		/*
		 * Identity gate: ranging announces the serial
		 * number, so it only starts once this board's
		 * is known; it is pushed in from userspace (see
		 * the cg_sn_* code), bounded by cg_sn_wait_work.
		 */
		schedule_delayed_work(&cg->sn_wait_work,
				      CG_SN_WAIT_SECS * HZ);
	
		if (cg->activated) {
			int i;

			/*
			 * poll the hardware ranging FSM: onu.state,
			 * RGB8 (bit15 BER_NOTIFY = downstream frame
			 * sync) and the superframe counter. Re-check
			 * frame_var each pass, for intr=0 and for a
			 * PLOAM event missed while the IRQ path was
			 * arming.
			 */
			for (i = 0; i < 30; i++) {
				cg_frame_var_update(cg);
				dev_dbg(dev, "range t=%ds: onu=0x%08x rgb8=0x%08x superframe=0x%08x alarm=0x%08x us=0x%08x psds_init=0x%08x\n",
					i, cg_mac_rd(cg, CG_REG_GPON_ONU),
					readl(cg->pon + CG_PSDS_RGB8),
					cg_mac_rd(cg, 0xfc),
					cg_mac_rd(cg, CG_REG_ALARM),
					cg_mac_rd(cg, CG_REG_US),
					readl(cg->glb + CG_GLB_PSDS_INIT));
				msleep(200);
			}
		}
	
	} else {
		dev_warn(dev, "failed to map GLB window 0x%llx\n",
			 (unsigned long long)CG_GLB_WINDOW_PHYS);
	}

	cg_read_vendor(cg, vendor);
	onu = cg_mac_rd(cg, CG_REG_GPON_ONU);
	/*
	 * Not a correctness check: before the identity is provisioned this
	 * reads the reset value. cg_activate_start() checks the readback
	 * against what it programmed.
	 */
	dev_dbg(dev, "GPON MAC vendor-id \"%s\" onu=0x%08x alarm=0x%08x\n",
		 vendor, onu, cg_mac_rd(cg, CG_REG_ALARM));

	cg_singleton = cg;
	/*
	 * Receive the downstream OMCI PDUs the NI CPU-RX path classifies out
	 * (ethertype 0xfff1). Registered after cg_singleton so the handler
	 * never sees a half-initialised context.
	 */
	if (IS_REACHABLE(CONFIG_CORTINA_NI))
		cortina_ni_pon_rx_hook_set(cg_rx_omci);
	cg_wan_create(cg);
	devm_hwmon_device_register_with_info(dev, "pon0_bosa", cg,
					     &cg_bosa_hwmon_chip_info, NULL);
	cg->debugfs = debugfs_create_dir(DRV_NAME, NULL);
	if (!IS_ERR_OR_NULL(cg->debugfs))
		debugfs_create_file("status", 0400, cg->debugfs, cg,
				    &cg_status_fops);
	if (cg->glb) {
		struct cortina_epon_plat epon_plat = {
			.dev		= dev,
			.pon		= cg->pon,
			.glb		= cg->glb,
			.wan_ndev	= cg->wan_ndev,
			.tx_disable	= cg->tx_disable,
			.tx_power	= cg->tx_power,
			.bosa_cal	= cg->bosa_cal,
			.mac_addr	= cg->wan_addr,
			.tx_pins	= cg_epon_tx_pins,
			.led_update	= cg_epon_led_update,
			.cookie		= cg,
		};
		struct cortina_epon *epon = cortina_epon_create(&epon_plat);

		if (IS_ERR(epon)) {
			dev_warn(dev, "epon: init failed: %ld\n", PTR_ERR(epon));
		} else {
			cg->epon = epon;
			cortina_epon_debugfs_init(cg->epon, cg->debugfs);
		}
	}
	if (cx_attach(dev, cg->wan_ndev, cg, &cg_cx_hw_ops))
		dev_dbg(dev, "xPON/OMCI framework not attached; in-kernel OMCI responder in use\n");
	platform_set_drvdata(pdev, cg);
	dev_dbg(dev, "cortina-gpon probe complete\n");
	return 0;
}

static void cortina_gpon_remove(struct platform_device *pdev)
{
	struct cortina_gpon *cg = platform_get_drvdata(pdev);

	/* unhook the downstream OMCI RX path before detaching from the xPON core */
	if (IS_REACHABLE(CONFIG_CORTINA_NI)) {
		cortina_ni_pon_wan_ndev_set(NULL, NULL);
		cortina_ni_pon_rx_hook_set(NULL);
	}
	cx_detach(cg);
	cortina_epon_destroy(cg->epon);
	cg->epon = NULL;
	cancel_delayed_work_sync(&cg->veip_avc_work);
	cancel_delayed_work_sync(&cg->optic_log_work);
	cancel_delayed_work_sync(&cg->coldstart_work);
	cancel_delayed_work_sync(&cg->carrier_work);
	cancel_delayed_work_sync(&cg->sn_wait_work);
	cancel_work_sync(&cg->activate_work);
	cancel_delayed_work_sync(&cg->puc_cnt_work);
	cg_intr_teardown(cg);
	if (cg->wan_ndev) {
		unregister_netdev(cg->wan_ndev);
		free_netdev(cg->wan_ndev);
	}
	debugfs_remove_recursive(cg->debugfs);
	if (cg_singleton == cg)
		cg_singleton = NULL;
}

static const struct of_device_id cortina_gpon_of_match[] = {
	{ .compatible = "realtek,rtl9607dq-pon" },
	{ }
};
MODULE_DEVICE_TABLE(of, cortina_gpon_of_match);

static struct platform_driver cortina_gpon_driver = {
	.probe	= cortina_gpon_probe,
	.remove	= cortina_gpon_remove,
	.driver	= {
		.name		= DRV_NAME,
		.dev_groups	= cg_groups,
		.of_match_table	= cortina_gpon_of_match,
	},
};
module_platform_driver(cortina_gpon_driver);

MODULE_DESCRIPTION("Cortina-Access GPON MAC driver for Realtek RTL9607DQ");
MODULE_LICENSE("GPL");
