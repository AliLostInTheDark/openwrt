/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * udsp SLIC plug-in for the MaxLinear DUSLIC-XS (PEF3200x)
 *
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 */
#ifndef DXS_H
#define DXS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <udsp/event.h>
#include <udsp/slic.h>

/* host register interface, 16-bit registers at even offsets */
#define DXS_HOST_CFG		0x06
#define  DXS_CFG_INT_POS	(1 << 0)
#define  DXS_CFG_SCON		(1 << 1)	/* self-clearing interrupt bits */
#define  DXS_CFG_8BIT		(1 << 2)
#define  DXS_CFG_RSTCORE	(1 << 7)
#define DXS_HOST_IEN1		0x08
#define DXS_HOST_IEN2		0x0a
#define DXS_HOST_STAT1		0x0c
#define DXS_HOST_INT1		0x10
#define  DXS_INT1_OBX_RDY	(1 << 11)
#define  DXS_INT1_ERR		(1 << 0)
#define DXS_HOST_INT2		0x12
#define DXS_HOST_DATA		0x14	/* 32-bit mailbox window */
#define DXS_HOST_LEN		0x18	/* in-box space, out-box fill, in 16-bit words */
#define DXS_HOST_BCFG		0x20
#define  DXS_BCFG_ROM		0x0
#define  DXS_BCFG_SPI		0x3
#define DXS_HOST_BINF		0x24
#define  DXS_BINF_STATE		0x00ff
#define  DXS_BINF_RUNNING	0x10
#define DXS_HOST_TEST		0x48	/* scratch register, values 0..31 */

/* command and event words */
#define DXS_HDR_RW		(1u << 31)
#define DXS_HDR_CMD(x)		((uint32_t)(x) << 24)
#define DXS_HDR_CHAN(x)		((uint32_t)(x) << 16)
#define DXS_HDR_MOD(x)		((uint32_t)(x) << 13)
#define DXS_HDR_ECMD(x)		((uint32_t)(x) << 8)
#define DXS_HDR_LEN(w)		((w) & 0xff)
#define DXS_HDR_CMD_GET(w)	(((w) >> 24) & 0x1f)
#define DXS_HDR_MOD_GET(w)	(((w) >> 13) & 0x7)
#define DXS_HDR_ECMD_GET(w)	(((w) >> 8) & 0x1f)

#define DXS_HDR_CHAN_GET(w)	(((w) >> 16) & 0xf)

#define DXS_CMD_SDD		1
#define DXS_CMD_EOP		6
#define DXS_CMD_EVT		9
#define DXS_MOD_SDD		0
#define DXS_MOD_ALI		1	/* events */
#define DXS_MOD_SYS		7
#define DXS_ECMD_SYS_VERS	6
#define DXS_ECMD_SYS_INTERR	11	/* internal error event */
#define  DXS_INTERR_CODE(w)	((w) & 0x1f)
#define  DXS_INTERR_CLK_FAIL	2
#define  DXS_INTERR_SYNC_FAIL	3
#define DXS_ECMD_SDD_CONTMEAS	2	/* continuous line measurement */
#define DXS_ECMD_SDD_BASIC	4
#define DXS_ECMD_SDD_RING	5
#define DXS_ECMD_SDD_MWL	6
#define DXS_ECMD_SDD_OPMODE	7
#define DXS_ECMD_SDD_LT_CFG	8	/* line test limits */
#define DXS_ECMD_SDD_DCDC	12
#define DXS_ECMD_SDD_COEFF	13
#define DXS_ECMD_SDD_CAPMEAS	16	/* capacitance result */
#define DXS_ECMD_SDD_GAIN	14
#define DXS_ECMD_EVT_SDD	4

/* SDD event payload: time stamp, operating mode, event code */
#define DXS_EVT_SDD_OPMODE(w)	(((w) >> 8) & 0xff)
#define DXS_EVT_SDD_CODE(w)	((w) & 0x1f)
#define DXS_EVT_OTEMP		0	/* line disabled by overtemperature */
#define DXS_EVT_LT_FIN		1	/* line test finished */
#define DXS_EVT_GF		3	/* ground fault */
#define DXS_EVT_OPC		5	/* operating mode changed */
#define DXS_EVT_ONH		9
#define DXS_EVT_OFFH		10
#define DXS_EVT_GF_FIN		12
#define DXS_EVT_OTEMP_FIN	14
#define DXS_EVT_LT_ABORT	19

#define DXS_OPMODE_DISABLED	0
#define DXS_OPMODE_RING		1
#define DXS_OPMODE_STANDBY	2
#define DXS_OPMODE_ACTIVE	3
#define DXS_OPMODE_CALIBRATE	6
#define DXS_OPMODE_RING_REV	17
#define DXS_OPMODE_ACTIVE_REV	19
#define DXS_OPMODE_CAPMEAS	23

#define DXS_MBX_WORDS		8	/* 32 bytes each way */
#define DXS_CHANNELS_MAX	2

struct dxs_line {
	uint8_t opmode;		/* as last reported by the firmware */
	bool offhook;
	bool bbd_loaded;
	uint32_t faults;	/* enum udsp_fault_bit, read to clear */
	int8_t lt_done;		/* 1 finished, -1 aborted, 0 running */
};

struct dxs_version {
	uint8_t dev;		/* PEB number; silicon revision in the low 5 bits */
	uint8_t channels;
	uint8_t major;		/* bit 7 marks a test firmware */
	uint8_t minor;
	uint8_t hotfix;
	uint8_t patch;
};

struct dxs {
	const struct udsp_slic_host *host;
	struct dxs_version ver;
	unsigned int channels;
	bool fw_loaded;
	bool pcm_on;
	struct dxs_line line[DXS_CHANNELS_MAX];
	char name[16];
};

void dxs_log(struct dxs *d, enum udsp_slic_log_level level, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

int dxs_reg_read(struct dxs *d, uint8_t reg, uint16_t *val);
int dxs_reg_write(struct dxs *d, uint8_t reg, uint16_t val);
int dxs_link_check(struct dxs *d);

size_t dxs_frame_words(uint8_t *buf, const uint32_t *words, size_t n);
int dxs_cmd_write(struct dxs *d, const uint32_t *cmd);
int dxs_cmd_read(struct dxs *d, uint32_t hdr, uint32_t *out, size_t words);
int dxs_events_poll(struct dxs *d);
uint32_t dxs_faults_take(struct dxs *d, unsigned int chan);
int dxs_measure(struct dxs *d, unsigned int chan,
		struct udsp_slic_monitor *out);
int dxs_capacitance(struct dxs *d, unsigned int chan, uint32_t nf[3]);
uint32_t dxs_sdd_hdr(unsigned int chan, unsigned int ecmd, unsigned int len);
uint32_t dxs_opmode_cmd(unsigned int chan, uint8_t opmode, uint32_t *payload);

int dxs_version_read(struct dxs *d);
int dxs_boot_rom(struct dxs *d);
int dxs_fw_load(struct dxs *d, const char *path);

int dxs_bbd_load(struct dxs *d, unsigned int chan, const char *path);

int dxs_pcm_start(struct dxs *d, unsigned int chan);
int dxs_pcm_ch_set(struct dxs *d, unsigned int chan, bool on);

void dxs_event(struct dxs *d, const uint32_t *msg);
int dxs_opmode_set(struct dxs *d, unsigned int chan, uint8_t opmode);
int dxs_calibrate(struct dxs *d, unsigned int chan);
int dxs_level_set(struct dxs *d, unsigned int chan, int tx_half_db,
		  int rx_half_db);

#endif
