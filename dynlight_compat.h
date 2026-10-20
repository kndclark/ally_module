/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Out-of-tree build shims for OGC linux-unstable PR #17 (Dynamic Lighting)
 * on the SteamOS 7.2.x kernel. Force-included from the Makefile so the PR
 * sources stay unmodified. Everything here is a no-op on a kernel that
 * already provides the symbol.
 */
#ifndef _DYNLIGHT_COMPAT_H
#define _DYNLIGHT_COMPAT_H

#include <linux/bits.h>
#include <linux/leds.h>

/* include/linux/leds.h: PR 17 adds this flag after LED_TRIG_HW_CHANGED. */
#ifndef LED_DYNAMIC_LIGHTING
#define LED_DYNAMIC_LIGHTING	BIT(27)
#endif

/* include/linux/minmax.h in 7.3-rc: three-way compare helper. */
#ifndef cmp_int
#define cmp_int(l, r)	(((l) > (r)) - ((l) < (r)))
#endif

#endif /* _DYNLIGHT_COMPAT_H */
