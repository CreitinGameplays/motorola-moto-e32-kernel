/* SPDX-License-Identifier: GPL-2.0 */
/*
 * 4.14 compat for the SC2355 combo WLAN driver (written for 5.4+, with
 * KERNEL_VERSION gates down to 4.14). Only the few things 4.14 lacks.
 * The Android tree already carries the WAPI nl80211 extension
 * (NL80211_WAPI_VERSION_1 in include/uapi/linux/nl80211.h), so no WAPI
 * shim is needed here.
 */
#ifndef _SPRD_WLAN_4_14_COMPAT_H
#define _SPRD_WLAN_4_14_COMPAT_H

#include <linux/version.h>

/* timer_shutdown_sync() is 6.2+; 4.14 uses del_timer_sync() */
#define timer_shutdown_sync(t) del_timer_sync(t)

#endif /* _SPRD_WLAN_4_14_COMPAT_H */
