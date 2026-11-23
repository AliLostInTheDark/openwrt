/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 Ifran Ali <khanu.i.i.k.2000@gmail.com>
 *
 * Realtek RTL9607DQ (Cortina Taurus) IEEE 802.3ah 1G EPON MAC, MPCP,
 * FEC, churning crypto, Dying Gasp, and OAM support.
 */
#ifndef _CORTINA_EPON_H_
#define _CORTINA_EPON_H_

#include <linux/netdevice.h>
#include <linux/types.h>

struct dentry;
struct device;
struct gpio_desc;
struct xpon_device;
struct cortina_epon;

#define CORTINA_EPON_MAX_LLID		8

enum cortina_epon_laser_mode {
	CORTINA_EPON_LASER_BURST = 0,	/* MPCP-controlled normal burst */
	CORTINA_EPON_LASER_ON,		/* Continuous TX (optical test) */
	CORTINA_EPON_LASER_OFF,		/* Forced laser off */
};

struct cortina_epon_plat {
	struct device *dev;
	void __iomem *pon;
	void __iomem *glb;
	struct net_device *wan_ndev;
	struct gpio_desc *tx_disable;
	struct gpio_desc *tx_power;
	const u8 *bosa_cal;
	const u8 *mac_addr;
	void (*tx_pins)(void *cookie, bool on);
	void (*led_update)(void *cookie, bool registered, bool los);
	void *cookie;
};

#if IS_ENABLED(CONFIG_CORTINA_EPON)
struct cortina_epon *cortina_epon_create(const struct cortina_epon_plat *plat);
void cortina_epon_destroy(struct cortina_epon *ce);
int cortina_epon_start(struct cortina_epon *ce, struct xpon_device *xpon);
void cortina_epon_stop(struct cortina_epon *ce);
bool cortina_epon_active(const struct cortina_epon *ce);
netdev_tx_t cortina_epon_wan_xmit(struct cortina_epon *ce,
				  struct sk_buff *skb,
				  struct net_device *ndev);
void cortina_epon_dying_gasp(struct cortina_epon *ce);
void cortina_epon_debugfs_init(struct cortina_epon *ce, struct dentry *parent);
ssize_t cortina_epon_state_show(struct cortina_epon *ce, char *buf);
ssize_t cortina_epon_mac_show(struct cortina_epon *ce, char *buf);
ssize_t cortina_epon_mac_store(struct cortina_epon *ce, const char *buf,
			       size_t count);
ssize_t cortina_epon_oam_link_store(struct cortina_epon *ce, const char *buf,
				    size_t count);
ssize_t cortina_epon_register_store(struct cortina_epon *ce, const char *buf,
				    size_t count);
ssize_t cortina_epon_key_store(struct cortina_epon *ce, const char *buf,
			       size_t count);
ssize_t cortina_epon_holdover_show(struct cortina_epon *ce, char *buf);
ssize_t cortina_epon_holdover_store(struct cortina_epon *ce, const char *buf,
				    size_t count);
ssize_t cortina_epon_tx_hold_show(struct cortina_epon *ce, char *buf);
ssize_t cortina_epon_tx_hold_store(struct cortina_epon *ce, const char *buf,
				   size_t count);
ssize_t cortina_epon_tx_hold_seconds_show(struct cortina_epon *ce, char *buf);
ssize_t cortina_epon_tx_hold_seconds_store(struct cortina_epon *ce,
					   const char *buf, size_t count);
#else
static inline struct cortina_epon *
cortina_epon_create(const struct cortina_epon_plat *plat)
{
	return NULL;
}

static inline void cortina_epon_destroy(struct cortina_epon *ce) { }

static inline int cortina_epon_start(struct cortina_epon *ce,
				     struct xpon_device *xpon)
{
	return -EOPNOTSUPP;
}

static inline void cortina_epon_stop(struct cortina_epon *ce) { }

static inline bool cortina_epon_active(const struct cortina_epon *ce)
{
	return false;
}

static inline netdev_tx_t cortina_epon_wan_xmit(struct cortina_epon *ce,
						struct sk_buff *skb,
						struct net_device *ndev)
{
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

static inline void cortina_epon_dying_gasp(struct cortina_epon *ce) { }

static inline void cortina_epon_debugfs_init(struct cortina_epon *ce,
					     struct dentry *parent) { }

static inline ssize_t cortina_epon_state_show(struct cortina_epon *ce,
					      char *buf)
{
	return -EOPNOTSUPP;
}

static inline ssize_t cortina_epon_mac_show(struct cortina_epon *ce, char *buf)
{
	return -EOPNOTSUPP;
}

static inline ssize_t cortina_epon_mac_store(struct cortina_epon *ce,
					     const char *buf, size_t count)
{
	return -EOPNOTSUPP;
}

static inline ssize_t cortina_epon_oam_link_store(struct cortina_epon *ce,
						  const char *buf, size_t count)
{
	return -EOPNOTSUPP;
}

static inline ssize_t cortina_epon_register_store(struct cortina_epon *ce,
						  const char *buf, size_t count)
{
	return -EOPNOTSUPP;
}

static inline ssize_t cortina_epon_key_store(struct cortina_epon *ce,
					     const char *buf, size_t count)
{
	return -EOPNOTSUPP;
}

static inline ssize_t cortina_epon_holdover_show(struct cortina_epon *ce,
						 char *buf)
{
	return -EOPNOTSUPP;
}

static inline ssize_t cortina_epon_holdover_store(struct cortina_epon *ce,
						  const char *buf, size_t count)
{
	return -EOPNOTSUPP;
}

static inline ssize_t cortina_epon_tx_hold_show(struct cortina_epon *ce,
						char *buf)
{
	return -EOPNOTSUPP;
}

static inline ssize_t cortina_epon_tx_hold_store(struct cortina_epon *ce,
						 const char *buf, size_t count)
{
	return -EOPNOTSUPP;
}

static inline ssize_t cortina_epon_tx_hold_seconds_show(struct cortina_epon *ce,
							char *buf)
{
	return -EOPNOTSUPP;
}

static inline ssize_t
cortina_epon_tx_hold_seconds_store(struct cortina_epon *ce, const char *buf,
				   size_t count)
{
	return -EOPNOTSUPP;
}
#endif

#endif /* _CORTINA_EPON_H_ */
