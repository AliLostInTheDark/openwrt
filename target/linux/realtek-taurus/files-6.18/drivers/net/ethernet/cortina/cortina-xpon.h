/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 *
 * Glue between the Cortina GPON MAC driver and the xPON/OMCI framework
 * (net/xpon).  See cortina-xpon.c.
 */
#ifndef CORTINA_XPON_H
#define CORTINA_XPON_H

#include <linux/types.h>

struct sk_buff;

struct device;
struct net_device;
struct xpon_device;
enum xpon_mode;

/* What the MAC driver tells the glue to re-read and report. */
#define CX_EV_STATE	BIT(0)
#define CX_EV_ONU_ID	BIT(1)
#define CX_EV_CHANNEL	BIT(2)
#define CX_EV_IDENTITY	BIT(3)
#define CX_EV_LOS	BIT(4)

/**
 * struct cx_hw_ops - what the glue needs from the MAC driver
 * @omci_tx: send one complete 48-byte baseline OMCI PDU on the OMCC
 * @set_serial: range with this eight-byte serial number
 * @set_password: the ten-byte registration password for the next activation
 * @get_serial: the serial number in use, -ENODATA if none yet
 * @state: activation state as the O-number, 1 (O1) to 7 (O7)
 * @onu_id: assigned ONU-ID, -1 if none
 * @omcc_gem: OMCC GEM port-id, -1 while the OMCC is down
 * @los: 1 for loss of signal or frame, 0 if not, <0 if unknown
 * @fec: FEC state and downstream totals
 * @tcont_qos: T-CONT scheduling: WRR weights by priority, NULL for strict
 * @gem_prio: the priority (0 highest) of the services on an upstream GEM
 */
/* data path capacity reported to the OLT and kept by the MAC driver */
#define CX_DATA_TCONT_MAX	8
#define CX_DATA_GEM_MAX		32
/*
 * The OMCI agent installs a new service set before it removes the old one,
 * so the table holds two sets of one service per data GEM.
 */
#define CX_SERVICES		(2 * CX_DATA_GEM_MAX)

/* FEC as the OLT set it (1 on, 0 off, -1 unknown) and the downstream totals */
struct cx_fec {
	int downstream;
	int upstream;
	u64 corrected_bytes;
	u64 corrected_codewords;
	u64 uncorrectable_codewords;
	u64 total_codewords;
};

struct cx_hw_ops {
	int (*omci_tx)(void *cookie, const u8 *pdu);
	int (*set_serial)(void *cookie, const u8 serial[8]);
	int (*set_password)(void *cookie, const u8 password[10]);
	int (*get_serial)(void *cookie, u8 serial[8]);
	int (*state)(void *cookie);
	int (*onu_id)(void *cookie);
	int (*omcc_gem)(void *cookie);
	int (*los)(void *cookie);
	void (*fec)(void *cookie, struct cx_fec *fec);
	int (*tcont_qos)(void *cookie, u16 tcont, const u8 *weight);
	void (*gem_prio)(void *cookie, u16 gem, u8 prio);
	int (*set_mode)(void *cookie, struct xpon_device *xpon,
			enum xpon_mode mode);
};

#if IS_ENABLED(CONFIG_XPON_OMCI)
int cx_attach(struct device *dev, struct net_device *wan, void *cookie,
	      const struct cx_hw_ops *hw);
void cx_detach(void *cookie);
void cx_notify(unsigned long events);
bool cx_omci_rx(const u8 *msg, unsigned int len);
bool cx_active(void);
int cx_us_service(struct sk_buff *skb);
bool cx_ds_service(struct sk_buff *skb);
#else
static inline int cx_attach(struct device *dev, struct net_device *wan,
			    void *cookie, const struct cx_hw_ops *hw)
{
	return -EOPNOTSUPP;
}

static inline void cx_detach(void *cookie) { }
static inline void cx_notify(unsigned long events) { }
static inline bool cx_omci_rx(const u8 *msg, unsigned int len) { return false; }
static inline bool cx_active(void) { return false; }
static inline int cx_us_service(struct sk_buff *skb) { return 0; }
static inline bool cx_ds_service(struct sk_buff *skb) { return false; }
#endif

#endif /* CORTINA_XPON_H */
