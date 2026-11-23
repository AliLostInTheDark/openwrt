// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) BRULE Herman <alpha_one_x86@first-world.info> (github.com/alphaonex86)
 *
 * Taken from https://github.com/alphaonex86/openwrt.
 *
 * GPON upstream mapping helpers: Alloc-ID to T-CONT binding decision and
 * upstream port-map ranges. Protocol logic only, no hardware access; builds
 * for big- and little-endian targets.
 */

#include <linux/types.h>

#include "gpon_gem_us.h"

bool gpon_gem_us_range_ok(const struct gpon_gem_us_range *r)
{
	if (!r)
		return false;
	return GPON_GEM_US_RANGE_OK(r->base, r->count, r->index_max);
}

/*
 * Alloc-ID -> T-CONT.
 *
 * Never rebind the OMCC's own Alloc-ID to the data T-CONT: the OMCC would
 * still look configured, but the OLT's grants for it would no longer reach
 * any T-CONT and the ONU would silently stop being heard.
 *
 * @omcc_alloc and @already_bound come from the caller: what "OMCC Alloc-ID"
 * and "already bound" mean is target specific. Whether an Alloc-ID has been
 * provisioned at all is checked by the caller before getting here.
 */
enum gpon_gem_us_bind gpon_gem_us_tcont_decide(u16 alloc, u16 omcc_alloc,
					       bool already_bound)
{
	if (already_bound)
		return GPON_GEM_US_BIND_DONE;
	if (alloc == omcc_alloc)
		return GPON_GEM_US_BIND_IS_OMCC;
	return GPON_GEM_US_BIND_TCONT;
}

const char *gpon_gem_us_bind_name(enum gpon_gem_us_bind v)
{
	switch (v) {
	case GPON_GEM_US_BIND_TCONT:
		return "bind-data-tcont";
	case GPON_GEM_US_BIND_DONE:
		return "already-bound";
	case GPON_GEM_US_BIND_IS_OMCC:
		return "is-omcc-alloc";
	}
	return "unknown";
}
