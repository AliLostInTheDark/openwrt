/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * RTL9607DQ L3FE flow engine (PPE): state shared by cortina-ppe.c and
 * cortina-ppe-debugfs.c.
 */

#ifndef _CORTINA_PPE_H
#define _CORTINA_PPE_H

#include <linux/atomic.h>
#include <linux/etherdevice.h>
#include <linux/in6.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/rhashtable.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/workqueue.h>
#include <net/flow_offload.h>

#include "cortina-ppe-regs.h"

#define PPE_ENTRIES			65536
#define PPE_HASH_WAYS			8
#define PPE_AGE_SLOTS			32
#define PPE_AGE_ROWS			(PPE_ENTRIES / PPE_AGE_SLOTS)
#define PPE_ACT_BYTES			(PPE_ACT_BITS / 8)
#define PPE_KEY_TBL_BYTES		(PPE_ENTRIES * sizeof(u32))
#define PPE_ACT_TBL_BYTES		(PPE_ENTRIES * PPE_ACT_BYTES)

struct cortina_ppe_tuple {
	union {
		u32		sip;
		struct in6_addr	sip6;
	};
	union {
		u32		dip;
		struct in6_addr	dip6;
	};
	u16	sport;
	u16	dport;
	u8	proto;
	bool	ipv6;
};

/* one offloaded direction of a flowtable flow: one hash entry */
struct cortina_ppe_flow {
	struct rhash_head	node;
	unsigned long		cookie;
	struct cortina_ppe_tuple tuple;
	u32			hash_idx;
	u16			crc16;
	u16			pppoe_sid;	/* pushed upstream, 0 = none */
	unsigned long		last_hit;
	bool			ds;
	struct flow_block	*block;		/* compared only */
};

struct cortina_ppe {
	struct device		*dev;
	void __iomem		*base;
	spinlock_t		reg_lock;	/* indirect register access */
	struct mutex		flow_lock;	/* flows, sessions, the sweep */

	/* DDR tables the engine reads; the NE fabric is not coherent */
	void			*carve;
	dma_addr_t		carve_pa;
	u32			*key_tbl;	/* CRC32 per entry */
	void			*act_tbl;

	u32			*shadow_crc32;	/* per entry, 0 = free */
	u16			*shadow_crc16;	/* for the cache invalidate */
	struct cortina_ppe_flow	**flow_by_idx;
	u8			*row_occ;	/* entries per age row */

	struct rhashtable	flow_table;
	struct list_head	block_cb_list;
	struct delayed_work	sweep_work;
	struct notifier_block	ppp_nb;

	u8			router_mac[ETH_ALEN];
	bool			router_mac_valid;
	u8			wan_mac[ETH_ALEN];	/* from the last US flow */
	bool			install_ok;
	bool			ds_armed;

	/* WAN data path from the GPON driver; gem 0 = not armed */
	u16			data_gem;
	u8			data_tcont;
	u16			pppoe_session;	/* 0 = IPoE */

	struct {
		atomic_t installed;
		atomic_t ds_installed;
		atomic_t us_hits;
		atomic_t ds_hits;
		atomic_t refused;
		atomic_t refused_unsupp;
		atomic_t refused_full;
		atomic_t refused_dup;
		atomic_t refused_err;
	} stats;
};

#endif /* _CORTINA_PPE_H */
