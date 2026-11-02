/*
 * MDBG bring-up instrumentation, compile-time gated.
 *
 * With CONFIG_MALI_MDBG_CRUMB=m/n the mali_kbase.ko carries breadcrumbs
 * (flash crumb recorder + dev_info at every glue/DDK boundary) used to
 * localize GPU bring-up hangs that kill the CPU before any console output
 * survives. For production builds (CONFIG_MALI_MDBG_CRUMB not set) every
 * MDBG_CRUMB() and MDBG_INFO() compiles to nothing, so mali_kbase.ko has
 * no dependency on the crumb recorder and no per-transition log spam.
 *
 * SPDX-License-Identifier: GPL-2.0
 */
#ifndef _KBASE_MDBG_H_
#define _KBASE_MDBG_H_

#include <linux/device.h>
#include <linux/printk.h>

#if IS_ENABLED(CONFIG_MALI_MDBG_CRUMB)
extern void mdbg_crumb(int step);
#define MDBG_CRUMB(step) mdbg_crumb(step)
#define MDBG_INFO(dev, fmt, ...) dev_info(dev, fmt, ##__VA_ARGS__)
#else
#define MDBG_CRUMB(step) do { } while (0)
#define MDBG_INFO(dev, fmt, ...) no_printk(fmt, ##__VA_ARGS__)
#endif

#endif /* _KBASE_MDBG_H_ */
