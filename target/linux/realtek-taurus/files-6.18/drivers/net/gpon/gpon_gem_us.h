/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) BRULE Herman <alpha_one_x86@first-world.info> (github.com/alphaonex86)
 *
 * Taken from https://github.com/alphaonex86/openwrt.
 *
 * GPON upstream mapping helpers: whether an OLT-assigned Alloc-ID may be
 * bound to the data T-CONT, and which upstream port-map slots stamp a GEM
 * Port-ID. Pure functions of their arguments; GEM framing itself is done by
 * the MAC hardware.
 */
#ifndef GPON_GEM_US_H
#define GPON_GEM_US_H

#include <linux/types.h>

/* G.984.3 field widths: Alloc-ID and GEM Port-ID are both 12 bits. */
#define GPON_GEM_US_PORT_MASK	0x0fffu
#define GPON_GEM_US_ALLOC_MASK	0x0fffu

/* Value written to an upstream port-map slot that must stamp nothing. */
#define GPON_GEM_US_PORT_NONE	0u

/*
 * The upstream port-map slots that carry one GEM Port-ID, as declared by
 * the caller. Slot numbering is chip specific (on Cortina the slot is the
 * VoQ, tcont * 8 + queue), so it is never derived here.
 *
 * @index_max is the last slot the chip's port-map array has; writing past it
 * would land in an unrelated register.
 */
struct gpon_gem_us_range {
	u16	base;		/* first upstream slot that stamps the Port-ID */
	u16	count;		/* how many consecutive slots (>= 1) */
	u16	index_max;	/* last slot the chip's port-map array has */
};

/* Range check usable in static_assert() with constant arguments. */
#define GPON_GEM_US_RANGE_OK(base, count, index_max)			\
	((count) >= 1u && (u32)(base) + (u32)(count) - 1u <= (u32)(index_max))

/* Runtime form of GPON_GEM_US_RANGE_OK(). */
bool gpon_gem_us_range_ok(const struct gpon_gem_us_range *r);

/*
 * The n-th upstream slot of @r, for n = 0 .. r->count - 1. Only meaningful
 * when gpon_gem_us_range_ok(r).
 */
static inline u16 gpon_gem_us_index(const struct gpon_gem_us_range *r,
				    unsigned int n)
{
	return (u16)(r->base + n);
}

/* Mask a GEM Port-ID to its 12 on-wire bits. */
static inline u16 gpon_gem_us_port_id(u32 port_id)
{
	return (u16)(port_id & GPON_GEM_US_PORT_MASK);
}

/* Mask an Alloc-ID to its 12 on-wire bits. */
static inline u16 gpon_gem_us_alloc_id(u32 alloc)
{
	return (u16)(alloc & GPON_GEM_US_ALLOC_MASK);
}

/* Result of gpon_gem_us_tcont_decide(). */
enum gpon_gem_us_bind {
	/* Bind this Alloc-ID to the data T-CONT. */
	GPON_GEM_US_BIND_TCONT = 0,
	/* Already bound, nothing to do. */
	GPON_GEM_US_BIND_DONE,
	/* This is the OMCC's Alloc-ID; binding it would move the OMCC. */
	GPON_GEM_US_BIND_IS_OMCC,
};

/*
 * Decide whether @alloc may be bound to the data T-CONT.
 *
 * @alloc:         the Alloc-ID the OLT provisioned for user data
 * @omcc_alloc:    the Alloc-ID the OMCC's T-CONT is bound to
 * @already_bound: the caller's "data T-CONT already bound" flag
 *
 * No state and no side effects; safe from any context.
 */
enum gpon_gem_us_bind gpon_gem_us_tcont_decide(u16 alloc, u16 omcc_alloc,
					       bool already_bound);

/* Short name of a verdict, for logs. */
const char *gpon_gem_us_bind_name(enum gpon_gem_us_bind v);

#endif /* GPON_GEM_US_H */
