// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) AK Sharma <maskachoska@yahoo.com> (github.com/AKoo7)
 *
 * Taken from https://github.com/AKoo7/openwrt/tree/aot5221zy.
 *
 * DSA tagger for the Cortina NI on the Realtek RTL9607DQ.
 *
 * The NI carries the port selector out of band: on RX in the DMA header,
 * on TX in the DMA descriptor. The conduit driver turns it into a 2-byte
 * software tag, MAGIC | port: on RX appended after the frame, on TX pushed in
 * front of the DMAC and pulled off before DMA. Neither reaches the wire.
 *
 * The switch never forwards a frame it hands to the CPU (lookup misses and
 * frames to our own MAC are trapped, not flooded), so received frames carry
 * no offload_fwd_mark and the bridge floods them in software.
 *
 * CORTINA_TAG_* must match CA_NI_DSA_TAG_* in
 * drivers/net/ethernet/cortina/cortina-ni-regs.h, which cannot be included
 * from net/dsa/.
 */

#include "tag.h"

#define CORTINA_NAME		"cortina"

#define CORTINA_TAG_LEN		2
#define CORTINA_TAG_PORT_OFF	0	/* byte[0] = MAGIC | dp->index */
#define CORTINA_TAG_PORT_MASK	0x0f
#define CORTINA_TAG_MAGIC	0xd0	/* high nibble marks a tagged frame */

static struct sk_buff *cortina_tag_xmit(struct sk_buff *skb,
					struct net_device *dev)
{
	struct dsa_port *dp = dsa_user_to_port(dev);
	u8 *tag;

	/*
	 * Not an EtherType header: the conduit strips it in software before
	 * DMA. The magic nibble tells it apart from an untagged frame sent by
	 * the conduit itself.
	 */
	skb_push(skb, CORTINA_TAG_LEN);
	tag = skb->data;
	tag[CORTINA_TAG_PORT_OFF] = CORTINA_TAG_MAGIC | (dp->index & CORTINA_TAG_PORT_MASK);
	tag[1] = 0;

	return skb;
}

static struct sk_buff *cortina_tag_rcv(struct sk_buff *skb,
				       struct net_device *dev)
{
	u8 *tag;

	if (skb_linearize(skb) || skb->len < CORTINA_TAG_LEN)
		return NULL;

	tag = skb_tail_pointer(skb) - CORTINA_TAG_LEN;
	if ((tag[CORTINA_TAG_PORT_OFF] & ~CORTINA_TAG_PORT_MASK) !=
	    CORTINA_TAG_MAGIC)
		return NULL;

	skb->dev = dsa_conduit_find_user(dev, 0,
					 tag[CORTINA_TAG_PORT_OFF] &
					 CORTINA_TAG_PORT_MASK);
	if (!skb->dev)
		return NULL;

	if (pskb_trim_rcsum(skb, skb->len - CORTINA_TAG_LEN))
		return NULL;

	return skb;
}

static const struct dsa_device_ops cortina_netdev_ops = {
	.name			= CORTINA_NAME,
	.proto			= DSA_TAG_PROTO_CORTINA,
	.xmit			= cortina_tag_xmit,
	.rcv			= cortina_tag_rcv,
	.needed_headroom	= CORTINA_TAG_LEN,
};

module_dsa_tag_driver(cortina_netdev_ops);
MODULE_ALIAS_DSA_TAG_DRIVER(DSA_TAG_PROTO_CORTINA, CORTINA_NAME);
MODULE_DESCRIPTION("DSA tag driver for the Cortina NI on the Realtek RTL9607DQ");
MODULE_LICENSE("GPL");
