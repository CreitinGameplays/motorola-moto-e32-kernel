// SPDX-License-Identifier: GPL-2.0
/*
 * Boot-time status print for the Unisoc FIQ watchdog
 * ("sprd,wdt-r2p0-fiq", watchdog@644e0000 on the vendor DTB).
 *
 * The 4.14 vendor kernel arms this watchdog itself from the hang_debug
 * driver (drivers/soc/sprd/debug/hang_debug/sprd_wdf.c: get_dev ->
 * set_timeout/set_pretimeout/start, then per-CPU feed threads), so on
 * the daily driver the counter runs and is constantly fed.  This 7.1
 * port has neither hang_debug nor the sprd FIQ watchdog driver, so
 * nothing here arms or feeds it - but the flash tests (docs/
 * 23-7.1-flash-bringup.md, tests 44/45) showed the board dying ~145 s
 * into the boot with no panic, which is exactly what an un-fed
 * bootloader-armed WDT would look like.  The first question is whether
 * the bootloader leaves this WDT armed at kernel entry.
 *
 * This prints the counter state early (the H71- prefix shows it in
 * every panel log mode) and deliberately does NOT disarm it: if it is
 * armed and the 7.1 hangs before /init, the bite is the only thing that
 * recovers the phone without a power hold.  logsave (scripts/host/
 * logsave.c) pets the counter from userspace every few seconds while
 * the boot is alive.
 *
 * Register map: 4.14 drivers/watchdog/sprd_wdt_fiq.c - 32768 counter
 * steps per second.
 */
#include <linux/init.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/printk.h>

#define WDT_LOAD_LOW	0x0
#define WDT_LOAD_HIGH	0x4
#define WDT_CTRL	0x8
#define WDT_INT_RAW	0x10
#define WDT_CNT_LOW	0x18
#define WDT_CNT_HIGH	0x1c

static int __init h71_wdt_report(void)
{
	struct device_node *np;
	void __iomem *base;
	u32 ctrl, load, raw, cnt;

	np = of_find_compatible_node(NULL, NULL, "sprd,wdt-r2p0-fiq");
	if (!np) {
		pr_info("H71-WDT: no sprd,wdt-r2p0-fiq node in the DTB\n");
		return 0;
	}
	base = of_iomap(np, 0);
	of_node_put(np);
	if (!base) {
		pr_info("H71-WDT: watchdog iomap failed\n");
		return 0;
	}

	ctrl = readl_relaxed(base + WDT_CTRL);
	load = readl_relaxed(base + WDT_LOAD_LOW) |
	       readl_relaxed(base + WDT_LOAD_HIGH) << 16;
	raw = readl_relaxed(base + WDT_INT_RAW);
	cnt = readl_relaxed(base + WDT_CNT_LOW) |
	      readl_relaxed(base + WDT_CNT_HIGH) << 16;
	iounmap(base);

	pr_info("H71-WDT: ctrl=%#x int_en=%d cnt_en=%d new_ver=%d rst_en=%d load=%u (%u ms) cnt=%u raw=%#x\n",
		ctrl, !!(ctrl & BIT(0)), !!(ctrl & BIT(1)),
		!!(ctrl & BIT(2)), !!(ctrl & BIT(3)),
		load, load / 33u, cnt, raw);
	return 0;
}
early_initcall(h71_wdt_report);
