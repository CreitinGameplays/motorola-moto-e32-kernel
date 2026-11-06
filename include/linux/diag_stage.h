#ifndef _LINUX_DIAG_STAGE_H
#define _LINUX_DIAG_STAGE_H
/*
 * DIAG instrumentation.  DIAG_SCREEN / DIAG_PAINT and the checkpoint hooks
 * below are diag-build only; DIAG_BANNER (the on-panel boot message drawn
 * once after paging_init()) is part of every production build.
 *
 *   1  first statement of start_kernel()   (proves the Image is executing)
 *   2  after setup_arch()                  (DTB, memory map, paging, PSCI fw)
 *   3  after build_all_zonelists/page_alloc
 *   4  after check_bugs()
 *   5  just before rest_init()
 *   6  first statement of kernel_init()
 *   7  after do_pre_smp_initcalls()
 *   8  after do_basic_setup()              (every initcall, all drivers)
 */

#include <linux/io.h>
#include <linux/bitops.h>
#include <linux/arm-smccc.h>
#include <linux/kallsyms.h>
#include <linux/mm.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <asm/cacheflush.h>
#include <asm/barrier.h>
#include <asm/cputype.h>
#include <asm/early_ioremap.h>
#include <asm/fixmap.h>
#include <asm/virt.h>
#define DIAG_SCREEN 0
#define DIAG_BANNER 1	/* production: on-panel boot progress bar */

/*
 * Framebuffer text console.
 *
 * The bootloader leaves the panel scanning out lcd_base=0x9e000000, and that
 * picture survives while a half-booted kernel hangs.  Geometry is measured,
 * not assumed: the command line says lcd_size=1600x720 and logo_bpix=24, and
 * /proc/device-tree/reserved-memory/logobuffer@9e000000 is 11.2 MB, so the
 * buffer is packed RGB888 (24 bpp, logo_bpix=24).  Writing 32-bit pixels
 * there shears every glyph row (the "straight white lines" artefact), so
 * all painting is byte-wise, three bytes per pixel.
 *
 * GEOMETRY (settled by experiment, 2026-11-04): the logo partition
 * (sda12) holds gzip-compressed BMPs that are 720x1600 24-bpp with the
 * picture UPRIGHT in the canvas (content bounding boxes are taller than
 * wide), and the panel's DRM mode is 720x1600.  The bootloader therefore
 * copies the BMP straight into lcd_base: the framebuffer is PORTRAIT,
 * 720 columns x 1600 rows, stride 2160 = 720 * 3, displayed 1:1.
 *
 *   buffer element  = panel element   (row = Y down, column = X right)
 *
 * History of the earlier wrong guesses, kept because they explain the
 * artefacts: the cmdline's lcd_size=1600x720 describes the panel in its
 * native landscape orientation, not the buffer, and two of the charging
 * BMPs (720x800) do hold sideways content.  Painting with the 4800-byte
 * landscape stride into the real 2160-byte buffer shears every row onto
 * a lattice of columns about 80 px apart (the "several vertical dotted
 * bars" of v4), and painting "a bar along rows 0..1" was invisible.
 * RGB888 packed; per-channel byte order unconfirmed - keep indicator
 * colors byte-symmetric (gray/white/black).
 * Row 0 is a permanent milestone line; rows 1..3 hold EL/KVM, CPU ID, and
 * the single current setup/initcall status.  printk mirroring is deliberately
 * disabled: vprintk_emit() may be entered concurrently by several CPUs, and
 * drawing there races the shared cursor and overwrites text.
 */
#define DIAG_FB_PHYS	0x9e000000UL
#define DIAG_FB_STRIDE	2880UL	/* 720 * 4 bytes a8r8g8b8: PORTRAIT 720x1600 (measured 2026-11-05 on the 7.1 boot) */
#define DIAG_FB_ROWS	1600U

#if DIAG_SCREEN
#include <linux/diag_font.h>
#define DIAG_GLYPH_W	8U
#define DIAG_GLYPH_H	16U
#define DIAG_SX		2U	/* 16 px wide glyphs */
#define DIAG_SY		2U	/* 32 px tall glyphs */
#define DIAG_CW		(DIAG_GLYPH_W * DIAG_SX + 2U)	/* 2 px column gap */
#define DIAG_CH		(DIAG_GLYPH_H * DIAG_SY + 4U)	/* 4 px line gap */
#define DIAG_TOP	48U
#define DIAG_COLS	(DIAG_FB_STRIDE / 4UL / DIAG_CW)
#define DIAG_ROWS	((DIAG_FB_ROWS - DIAG_TOP) / DIAG_CH)
#define DIAG_BG	0x000000U

/*
 * The console state is ONE object with external linkage, not a set of
 * file-scope statics.  This header is included by several translation units
 * (setup.c, printk.c, init/main.c), and with statics each of them had its own
 * diag_scr_ready - permanently false everywhere except setup.c.  That is why
 * the stage tags compiled into init/main.c never appeared on the milestone
 * line, and why the printk mirror in printk.c drew nothing.  setup.c defines
 * the single instance.
 */
#define DIAG_HEAD	4U	/* row 0: milestones, rows 1..3: permanent info */

struct diag_console {
	bool ready;
	unsigned int row;
	unsigned int trace_len;
	char trace[DIAG_COLS + 1];
	unsigned int head_lines;
	char head[DIAG_HEAD - 1][DIAG_COLS + 1];
};

extern struct diag_console diag_state;

static inline u8 *diag_screen_row(unsigned int row)
{
	return (u8 *)phys_to_virt(DIAG_FB_PHYS +
			(unsigned long)(DIAG_TOP + row * DIAG_CH) *
			DIAG_FB_STRIDE);
}

static inline void diag_scr_pixel(u8 *p, u32 rgb)
{
	/* a8r8g8b8: bytes [B, G, R, A]; alpha 0xff so the DPU does not
	 * blend the pixel away */
	p[0] = rgb & 0xff;
	p[1] = (rgb >> 8) & 0xff;
	p[2] = (rgb >> 16) & 0xff;
	p[3] = 0xff;
}

static inline void diag_scr_glyph(u8 *base, unsigned char ch)
{
	const unsigned char *g = &diag_font_data[(unsigned int)ch * DIAG_GLYPH_H];
	unsigned int y, x, i, j;

	for (y = 0; y < DIAG_GLYPH_H; y++) {
		u8 *line = base + (unsigned long)y * DIAG_SY * DIAG_FB_STRIDE;

		for (x = 0; x < DIAG_GLYPH_W; x++) {
			u32 rgb = (g[y] & (0x80U >> x)) ? 0xffffffU : DIAG_BG;

			for (i = 0; i < DIAG_SY; i++)
				for (j = 0; j < DIAG_SX; j++)
					diag_scr_pixel(line +
						i * DIAG_FB_STRIDE +
						(x * DIAG_SX + j) * 4U, rgb);
		}
	}
}

/* Draw one text row; the cursor does not move. */
static inline void diag_screen_draw(unsigned int row, const char *text,
				    unsigned int len)
{
	u8 *r = diag_screen_row(row);
	unsigned int i;

	for (i = 0; i < DIAG_COLS; i++) {
		char c = (i < len && text[i] >= 0x20 && text[i] <= 0x7e) ?
			 text[i] : ' ';

		diag_scr_glyph(r + (unsigned long)i * DIAG_CW * 4U, c);
	}
	__flush_dcache_area(r, DIAG_CH * DIAG_FB_STRIDE);
}

static inline void diag_screen_clear(void)
{
	u8 *fb = diag_screen_row(0);
	unsigned long y;
	unsigned int i;

	for (y = 0; y < (unsigned long)(DIAG_FB_ROWS - DIAG_TOP); y++)
		memset(fb + y * DIAG_FB_STRIDE, 0, DIAG_FB_STRIDE);
	__flush_dcache_area(fb, (unsigned long)(DIAG_FB_ROWS - DIAG_TOP) *
			    DIAG_FB_STRIDE);
	diag_state.row = DIAG_HEAD;
	if (diag_state.trace_len)
		diag_screen_draw(0, diag_state.trace, diag_state.trace_len);
	for (i = 0; i < diag_state.head_lines; i++)
		diag_screen_draw(i + 1, diag_state.head[i],
				 strlen(diag_state.head[i]));
}

/*
 * Permanent information rows (1..3).  They survive both scrolling and the
 * clear() that scrolling performs, so a fact like "which EL did the kernel
 * boot at" is still readable when the log is long.
 */
static inline void diag_screen_head(unsigned int n, const char *text,
				    unsigned int len)
{
	unsigned int i;

	if (n >= DIAG_HEAD - 1)
		return;
	if (len > DIAG_COLS)
		len = DIAG_COLS;
	for (i = 0; i < len; i++)
		diag_state.head[n][i] =
			(text[i] >= 0x20 && text[i] <= 0x7e) ? text[i] : ' ';
	diag_state.head[n][len] = '\0';
	if (n + 1 > diag_state.head_lines)
		diag_state.head_lines = n + 1;
	diag_screen_draw(n + 1, diag_state.head[n], len);
}

static inline void diag_screen_puts(const char *text, size_t len)
{
	size_t i = 0;

	if (!diag_state.ready || !text)
		return;
	while (i < len) {
		unsigned int n = len - i;

		if (n > DIAG_COLS)
			n = DIAG_COLS;
		if (diag_state.row >= DIAG_ROWS)
			diag_screen_clear();
		diag_screen_draw(diag_state.row, text + i, n);
		diag_state.row++;
		i += n;
	}
}

/* Append a band number to the permanent milestone line. */
static inline void diag_screen_tag(unsigned int n)
{
	if (!diag_state.ready)
		return;
	if (diag_state.trace_len >= 3 &&
	    diag_state.trace[diag_state.trace_len - 3] == '0' + (n / 10) % 10 &&
	    diag_state.trace[diag_state.trace_len - 2] == '0' + n % 10)
		return;	/* no consecutive duplicates */
	if (diag_state.trace_len + 4 > DIAG_COLS)
		return;
	diag_state.trace[diag_state.trace_len++] = '0' + (n / 10) % 10;
	diag_state.trace[diag_state.trace_len++] = '0' + n % 10;
	diag_state.trace[diag_state.trace_len++] = ' ';
	diag_state.trace[diag_state.trace_len] = '\0';
	diag_screen_draw(0, diag_state.trace, diag_state.trace_len);
}

/* Hex without printf: this runs long before any console driver exists. */
static inline char *diag_hex(char *p, unsigned long long v, unsigned int digits)
{
	while (digits--) {
		unsigned int d = (unsigned int)(v >> (digits * 4)) & 0xf;

		*p++ = d < 10 ? '0' + d : 'a' + d - 10;
	}
	return p;
}

/*
 * Which exception level did the bootloader enter the kernel at?
 *
 * It cannot be asked afterwards: el2_setup() drops to EL1 before any driver
 * runs, and the "CPU: All CPU(s) started at ELx" line of a normal boot has
 * long scrolled out of dmesg (pstore only keeps post-console messages).  KVM
 * on arm64 needs the kernel to have been entered at EL2 (or to run VHE), so
 * put the raw __boot_cpu_mode[] pair, the is_hyp_mode_available() verdict and
 * ID_AA64PFR0_EL1 on the permanent rows while the console still works.
 */
static inline void diag_el_report(void)
{
	char line[DIAG_COLS + 1];
	char *p;
	u64 pfr0;

	asm volatile("mrs %0, id_aa64pfr0_el1" : "=r" (pfr0));

	memcpy(line, "EL bcm=", 7);
	p = diag_hex(line + 7, __boot_cpu_mode[0], 8);
	*p++ = '/';
	p = diag_hex(p, __boot_cpu_mode[1], 8);
	memcpy(p, " hyp=", 5);
	p += 5;
	*p++ = is_hyp_mode_available() ? '1' : '0';
	memcpy(p, " KVM:", 5);
	p += 5;
	if (is_hyp_mode_available()) {
		memcpy(p, "possible", 8);
		p += 8;
	} else {
		memcpy(p, "impossible", 10);
		p += 10;
	}
	*p = '\0';
	diag_screen_head(0, line, p - line);

	memcpy(line, "PFR0=", 5);
	p = diag_hex(line + 5, pfr0, 16);
	memcpy(p, " MIDR=", 6);
	p = diag_hex(p + 6, read_cpuid_id(), 8);
	*p = '\0';
	diag_screen_head(1, line, p - line);

	diag_state.row = DIAG_HEAD;
}

/*
 * The initcall that is about to run.  do_one_initcall() calls this before
 * every initcall; if one of them never returns, its name is still on the
 * permanent row and the photo names the culprit.  %pF resolves through
 * kallsyms, which is a static table and is usable long before do_initcalls().
 */
static inline void diag_show_initcall(void *fn)
{
	char line[DIAG_COLS + 1];
	int n;

 n = scnprintf(line, sizeof(line), "INIT: %pF", fn);
	if (n > 0)
		diag_screen_head(2, line, (unsigned int)n);
}

#define DIAG_TAG(n) diag_screen_tag(n)

/*
 * Post-setup_arch stages report on the milestone line only: the paint
 * bands stop at 19, and a tag cannot collide with a band number.
 */
#define DIAG_STAGE_TAG(n) \
	do { if (DIAG_PAINT) DIAG_TAG(n); } while (0)

static inline void diag_screen_init(void)
{
	diag_state.ready = true;
	diag_screen_clear();
	diag_screen_tag(15);	/* paging_init() just returned */
}
#define DIAG_EL_REPORT diag_el_report()
#define DIAG_INITCALL_NAME(fn) \
	do { if (DIAG_SCREEN) diag_show_initcall((void *)(fn)); } while (0)
#define DIAG_SCR_READY diag_state.ready
#define DIAG_SETUP_STATUS(name) \
	do { diag_screen_head(2, "SETUP: " name, sizeof("SETUP: " name) - 1); } while (0)
#else
static inline void diag_screen_init(void) { }
#define DIAG_TAG(n) do { } while (0)
#define DIAG_EL_REPORT do { } while (0)
#define DIAG_INITCALL_NAME(fn) do { } while (0)
#define DIAG_SCR_READY 0
#define DIAG_STAGE_TAG(n) do { } while (0)
#define DIAG_SETUP_STATUS(name) do { } while (0)
#endif

#define DIAG_SCREEN_INIT diag_screen_init()

/*
 * Production boot indicator (DIAG_BANNER): a 16-px-tall progress bar of
 * solid rectangles at panel rows 24..39 (X = 72..648, 12 segments of
 * 48 px), mirrored at rows 1560..1575 so a vertically flipped scanout
 * reads the same.  Segments fill left to right:
 *
 *   head.S  dim track + segments 0,1,2 (0x28 then 0x60/0x90/0xb8)
 *   setup.c segments 0..7  (0xb8)   right after paging_init()
 *   main.c  segments 8..11 (0xe8)   after do_basic_setup()
 *
 * Rectangles only: no glyphs, no fonts, nothing that can shear.  A
 * hang names its last completed step by the filled fraction.  Colors
 * are byte-symmetric gray (per-channel byte order unconfirmed).
 */
#if DIAG_BANNER
#define DIAG_BAR_X0	72U		/* first pixel column  */
#define DIAG_BAR_SEG	48U		/* pixels per segment  */
#define DIAG_BAR_SEGS	12U
#define DIAG_BAR_ROWS	16U		/* bar thickness, rows */
#define DIAG_BAR_TOP	24U		/* first bar row       */
#define DIAG_BAR_BOT	(DIAG_FB_ROWS - DIAG_BAR_TOP - DIAG_BAR_ROWS)
#define DIAG_BAR_TRACK	0x28U
#define DIAG_BAR_SHADE	0xb8U
#define DIAG_BAR_SHADE_FULL 0xe8U

/* one 8-byte-per-store run of `npix` pixels on row y, starting at column x */
static inline void diag_bar_run(u8 *base, unsigned int y, unsigned int x,
				unsigned int npix, u8 shade)
{
	u32 px = 0xff000000u | (0x010101u * shade);
	u64 pat = (u64)px | ((u64)px << 32);
	u8 *p = base + (unsigned long)y * DIAG_FB_STRIDE + (unsigned long)x * 4UL;
	unsigned int i;

	for (i = 0; i < npix * 4U; i += 8)
		*(u64 *)(p + i) = pat;
}

/* a rectangle at rows [DIAG_BAR_TOP, DIAG_BAR_TOP + DIAG_BAR_ROWS) plus
 * its mirror near the bottom edge */
static inline void diag_bar_rect(unsigned int x, unsigned int npix, u8 shade)
{
	u8 *base = (u8 *)phys_to_virt(DIAG_FB_PHYS);
	unsigned int y;

	if (x >= DIAG_FB_STRIDE / 4UL || npix > DIAG_FB_STRIDE / 4UL - x)
		return;

	for (y = 0; y < DIAG_BAR_ROWS; y++) {
		diag_bar_run(base, DIAG_BAR_TOP + y, x, npix, shade);
		diag_bar_run(base, DIAG_BAR_BOT + y, x, npix, shade);
	}
	__flush_dcache_area(base + (unsigned long)DIAG_BAR_TOP * DIAG_FB_STRIDE,
			    (unsigned long)DIAG_BAR_ROWS * DIAG_FB_STRIDE);
	__flush_dcache_area(base + (unsigned long)DIAG_BAR_BOT * DIAG_FB_STRIDE,
			    (unsigned long)DIAG_BAR_ROWS * DIAG_FB_STRIDE);
}

/* the dim track the segments are drawn on */
static inline void diag_bootbar_track(void)
{
	diag_bar_rect(DIAG_BAR_X0, DIAG_BAR_SEGS * DIAG_BAR_SEG, DIAG_BAR_TRACK);
}

/* fill segments [seg0, seg1) of the boot progress bar */
static inline void diag_bootbar(unsigned int seg0, unsigned int seg1,
				u8 shade)
{
	unsigned int s;

	if (seg0 >= DIAG_BAR_SEGS || seg1 > DIAG_BAR_SEGS || seg0 >= seg1)
		return;
	for (s = seg0; s < seg1; s++)
		diag_bar_rect(DIAG_BAR_X0 + s * DIAG_BAR_SEG, DIAG_BAR_SEG, shade);
}
#define DIAG_BANNER_INIT \
	(diag_bootbar_track(), diag_bootbar(0, 8, DIAG_BAR_SHADE))
#define DIAG_BOOTBAR_FULL diag_bootbar(8, 12, DIAG_BAR_SHADE_FULL)
#else
#define DIAG_BANNER_INIT do { } while (0)
#define DIAG_BOOTBAR_FULL do { } while (0)
#endif

/*
 * printk mirror entry point, defined exactly once, in
 * arch/arm64/kernel/setup.c, next to the console itself.  The state is a
 * single global now, but keeping one entry point means the mirror cannot
 * drift from the console (and is the place a lock would go if printk from
 * several CPUs ever races here).
 */
void diag_stage_mirror(const char *text, size_t len);

/*
 * Production boot-progress line: the ONLY printk text the panel console
 * paints.  The console drops every other record before it reaches the
 * painter, so a boot costs a handful of glyph draws instead of repainting
 * the whole kernel log (which also made every driver message pay a glyph
 * paint plus cache maintenance).  Call sites stay in the boot core:
 * setup_arch, mm_init, the initcall phases and exec of /init.
 *
 * Level 0 (EMERG) on purpose: this bootloader passes loglevel=1, so
 * anything above KERN_EMERG is suppressed before it can reach a console
 * (measured on the quiet daily driver: KERN_INFO progress lines existed
 * in the ring but never painted).
 */
#define DIAG_PROGRESS_TAG "diag: "
#define DIAG_PROGRESS(fmt, ...) \
	printk(KERN_EMERG DIAG_PROGRESS_TAG fmt "\n", ##__VA_ARGS__)

/*
 * Panel log policy (runtime-switchable): int diag_panel_log_mode,
 * defined in arch/arm64/kernel/setup.c next to the console:
 *   0 = progress only (default): progress lines + crash signatures
 *   1 = full kernel log until userspace starts, then back to mode 0
 *   2 = full kernel log, no auto-quiet (explicit debug override)
 *
 * diag_panel_boot_done is set when /init is about to exec - the panic
 * guard: a full log painted past the init exec has to scroll ~4 MB of
 * framebuffer per line under userspace log spam, stalling the console
 * lock (and with it every printk) until the system dies.  Runtime
 * switch: /proc/diagfb_log (root, created by a late initcall).
 */
extern int diag_panel_log_mode;
extern bool diag_panel_boot_done;

/* one line-assembly state per painting path (the console .write is
 * serialized by the printk core) */
struct diag_panel_line {
	char buf[512];
	unsigned int len;
	bool skip;
};

/* filter + paint a raw chunk of log text (records may be split across
 * calls); implemented once in setup.c */
void diag_panel_write(struct diag_panel_line *st, const char *s, size_t n);


#define DIAG_PANIC_STAGE 0
#define DIAG_ARM_STAGE 0
#define DIAG_ARM_TIMEOUT 10
#define DIAG_SMC_STAGE 0
#define DIAG_CHECKPOINT_KIND 0
#define DIAG_CHECKPOINT_SETUP_ID 0
#define DIAG_CHECKPOINT_LEVEL ""
#define DIAG_CHECKPOINT_ORDINAL 0
#define DIAG_CHECKPOINT_NAME ""
#define DIAG_CHECKPOINT_LABEL "off"
#define DIAG_PSCI_USE_HVC 0

/*
 * Reset the SoC through the PSCI SYSTEM_RESET function (SMCCC 0x84000009).
 *
 * This reset source is usable at the very first statement of start_kernel():
 * it is a secure-monitor call, so it needs no device mapping, clock or driver.
 * The warm reset preserves DRAM. A checkpoint marker is present in ramoops
 * only if the ramoops console has already initialized; earlier probes rely on
 * the validated reset outcome (or a separately verified serial capture).
 *
 * If the monitor does not implement the function the call simply returns and
 * the boot continues, so "no reset" must be read as "the code did not run"
 * only after a stage that is known to be reached.
 */
static inline long diag_psci_reset(void)
{
	struct arm_smccc_res res;

#if DIAG_PSCI_USE_HVC
	arm_smccc_hvc(0x84000009UL, 0, 0, 0, 0, 0, 0, 0, &res);
#else
	arm_smccc_smc(0x84000009UL, 0, 0, 0, 0, 0, 0, 0, &res);
#endif
	return (long)res.a0;
}

static inline void diag_checkpoint_reset(const char *label)
{
	long status;

	dsb(sy);
	status = diag_psci_reset();
	pr_emerg("DIAG: PSCI_SYSTEM_RESET returned: checkpoint=%s status=%ld\n",
		 label, status);
}

#define DIAG_SMC_HOOK(n) \
	do { if (DIAG_SMC_STAGE == (n)) { \
		char diag_label[16]; \
		scnprintf(diag_label, sizeof(diag_label), "stage:%d", (n)); \
		pr_emerg("DIAG: CHECKPOINT reached: %s\n", diag_label); \
		diag_checkpoint_reset(diag_label); \
	} } while (0)

#define DIAG_SETUP_CHECKPOINT(id, name) \
	do { if (DIAG_CHECKPOINT_KIND == 1 && \
		DIAG_CHECKPOINT_SETUP_ID == (id)) { \
		pr_emerg("DIAG: CHECKPOINT reached: setup:%s\n", name); \
		diag_checkpoint_reset("setup:" name); \
	} } while (0)

static inline int diag_initcall_symbol_name(initcall_t fn, char *symbol)
{
	return lookup_symbol_name((unsigned long)fn, symbol);
}

static inline void diag_log_initcall(initcall_t fn)
{
	char symbol[KSYM_NAME_LEN];

	if (diag_initcall_symbol_name(fn, symbol))
		pr_emerg("DIAG: initcall %#lx\n", (unsigned long)fn);
	else
		pr_emerg("DIAG: initcall %s\n", symbol);
}

struct diag_initcall_symbol_match {
	const char *name;
	unsigned long body;
	unsigned long cfi_jt;
};

static inline int diag_find_initcall_symbol(void *data, const char *name,
					    struct module *mod,
					    unsigned long address)
{
	struct diag_initcall_symbol_match *match = data;
	size_t len = strlen(match->name);

	if (mod || strncmp(name, match->name, len) ||
	    (name[len] && name[len] != '$' && name[len] != '.'))
		return 0;
	if (strstr(name, ".cfi_jt"))
		match->cfi_jt = address;
	else
		match->body = address;
	return 0;
}

static inline int diag_initcall_name_matches(initcall_t fn)
{
	static int resolved;
	static unsigned long target, target_cfi;

	if (!resolved) {
		struct diag_initcall_symbol_match match = {
			.name = DIAG_CHECKPOINT_NAME,
		};

		kallsyms_on_each_symbol(diag_find_initcall_symbol, &match);
		target = match.body;
		target_cfi = match.cfi_jt;
		resolved = 1;
		pr_emerg("DIAG: initcall target %s body=%#lx cfi_jt=%#lx\n",
			 DIAG_CHECKPOINT_NAME, target, target_cfi);
	}
	return (unsigned long)fn == target ||
	       (unsigned long)fn == target_cfi;
}

static inline void diag_initcall_checkpoint(initcall_t fn, const char *level,
					    unsigned int ordinal)
{
	char symbol[KSYM_NAME_LEN];
	int match = 0;

	if (DIAG_CHECKPOINT_KIND == 2)
		match = !strcmp(level, DIAG_CHECKPOINT_LEVEL) &&
			ordinal == DIAG_CHECKPOINT_ORDINAL;
	else if (DIAG_CHECKPOINT_KIND == 3)
		match = (!DIAG_CHECKPOINT_LEVEL[0] ||
			 !strcmp(level, DIAG_CHECKPOINT_LEVEL)) &&
			diag_initcall_name_matches(fn);
	if (match) {
		if (!diag_initcall_symbol_name(fn, symbol))
			pr_emerg("DIAG: CHECKPOINT reached: %s (level=%s ordinal=%u function=%s)\n",
				 DIAG_CHECKPOINT_LABEL, level, ordinal, symbol);
		else
			pr_emerg("DIAG: CHECKPOINT reached: %s (level=%s ordinal=%u function=%#lx)\n",
				 DIAG_CHECKPOINT_LABEL, level, ordinal,
				 (unsigned long)fn);
		dsb(sy);
		diag_checkpoint_reset(DIAG_CHECKPOINT_LABEL);
	}
}

#define DIAG_INITCALL_CHECKPOINT(fn, level, ordinal) \
	do { if (DIAG_CHECKPOINT_KIND == 2 || DIAG_CHECKPOINT_KIND == 3) \
		diag_initcall_checkpoint((fn), (level), (ordinal)); } while (0)

#if DIAG_SCREEN
#define DIAG_INITCALL_LOG(fn) diag_log_initcall(fn)
#else
/* production: one pr_emerg + kallsyms lookup per initcall was pure
 * overhead (measured as the dominant console cost of a normal boot) */
#define DIAG_INITCALL_LOG(fn) do { } while (0)
#endif

#define DIAG_PAINT 0

/*
 * Visual stage trace: paint white bands at the bootloader's framebuffer.
 *
 * The bootloader leaves the panel scanning out of lcd_base=0x9e000000, and
 * that picture stays up while a half-booted kernel hangs (the operator sees
 * the Motorola logo).  Filling horizontal bands there therefore gives a
 * progress bar that needs no reset, no ramoops and no console.
 *
 * Layout: band n occupies bytes [n*DIAG_PAINT_BAND, (n+1)*DIAG_PAINT_BAND) of
 * physical 0x9e000000, so the bands stack downwards and their number is the
 * answer.  10 bands are 1.28 MB, comfortably inside the 1600x720x32 panel
 * buffer and below the next reserved region at 0x9f000000.
 *
 * band 0 is painted by the entry stub (scripts/host/entry_paint.py, MMU off,
 * before head.S).  DIAG_PAINT=1 paints start_kernel()'s stages 2..8 into
 * bands 1..7; DIAG_PAINT=2 paints the setup_arch() milestones below into
 * bands 1..9.  Two paint paths, because the linear mapping is only complete
 * after paging_init():
 *   diag_paint_early()  early_ioremap(), usable from early_ioremap_init()
 *                       until early_ioremap_reset() inside setup_arch();
 *   diag_paint()        phys_to_virt(), valid once paging_init() has mapped
 *                       the reserved-memory node (it has no `no-map`).
 */
#define DIAG_PAINT_BASE 0x9e000000UL
#define DIAG_PAINT_BAND 0x20000UL	/* 131072 bytes = 128 KB */

static inline void diag_paint(unsigned int band)
{
	volatile u64 *p = (volatile u64 *)phys_to_virt(DIAG_PAINT_BASE +
					      band * DIAG_PAINT_BAND);
	unsigned int i;

	/*
	 * Once the text console is up the band would only overwrite the log it
	 * is pointing at, so keep the milestone tag and skip the fill.
	 */
	if (!DIAG_SCR_READY) {
		for (i = 0; i < DIAG_PAINT_BAND / sizeof(u64); i++)
			p[i] = ~0ULL;
		dsb(sy);
	}
	DIAG_TAG(band);
}

static inline void diag_paint_early(unsigned int band)
{
	void __iomem *p = early_ioremap(DIAG_PAINT_BASE + band * DIAG_PAINT_BAND,
					DIAG_PAINT_BAND);

	if (!p)
		return;
	memset_io(p, 0xff, DIAG_PAINT_BAND);
	early_iounmap(p, DIAG_PAINT_BAND);
	dsb(sy);
	DIAG_TAG(band);
}

/* start_kernel() stages (mode 1) */
#define DIAG_PAINT_HOOK(n) \
	do { if (DIAG_PAINT == 1) diag_paint((n) - 1); } while (0)

/* setup_arch() milestones (mode 2); e = 1 for the early_ioremap path */
#define DIAG_TRACE_HOOK(n, e) \
	do { if (DIAG_PAINT == 2) { \
		if (e) { diag_paint_early(n); } else { diag_paint(n); } \
	} } while (0)

/*
 * Pre-arch paint: start_kernel() and the first statements of setup_arch()
 * run before paging_init() has mapped RAM, so diag_paint() cannot be used.
 * The fixmap, however, can map any physical page very early, so we set it up
 * ourselves and paint through early_ioremap().  Both helpers are idempotent
 * and setup_arch() calls them again a few lines later.
 */
static bool diag_prearch_ready;

static inline void diag_paint_prearch(unsigned int band)
{
	if (!diag_prearch_ready) {
		early_fixmap_init();
		early_ioremap_init();
		diag_prearch_ready = true;
	}
	diag_paint_early(band);
}

#define DIAG_EARLY_HOOK(n) \
	do { if (DIAG_PAINT == 2) diag_paint_prearch(n); } while (0)


/*
 * Arm the SoC's AON watchdog directly.  This is deliberately independent of
 * the driver: drivers/watchdog/sprd_wdt_fiq.c registers with
 * module_platform_driver(), i.e. a device_initcall, which is far too late to
 * cover a stall in start_kernel()/setup_arch().  The register sequence is
 * copied from sprd_wdt_fiq_{unlock,load_value,enable,start}(); interrupts are
 * left off (INT_EN clear) so a timeout is a plain SoC reset, no FIQ needed.
 *
 * 0x644e0000 = /soc/aon/watchdog@644e0000 (sprd,wdt-r2p0-fiq).
 */
static inline void diag_arm_aon_wdt(void)
{
	void __iomem *base = ioremap(0x644e0000, 0x1000);
	u32 step = DIAG_ARM_TIMEOUT * 32768U;
	int i;

	if (!base)
		return;

	writel_relaxed(0xe551, base + 0x20);       /* LOCK: unlock key */
	writel_relaxed((step >> 16) & 0xffff, base + 0x04); /* LOAD_HIGH */
	writel_relaxed(step & 0xffff, base + 0x00);         /* LOAD_LOW */
	writel_relaxed(0, base + 0x30);            /* IRQ_LOAD_HIGH */
	writel_relaxed(0, base + 0x2c);            /* IRQ_LOAD_LOW */
	writel_relaxed(0, base + 0x20);            /* LOCK: lock again */

	/* wait for LD_BUSY to clear, as the driver does */
	for (i = 0; i < 1000; i++) {
		if (!(readl_relaxed(base + 0x10) & BIT(4)))
			break;
		cpu_relax();
	}

	/* CTRL: NEW_VER_EN, then CNT_EN | RST_EN (no INT_EN) */
	writel_relaxed(readl_relaxed(base + 0x08) | BIT(2), base + 0x08);
	writel_relaxed(readl_relaxed(base + 0x08) | BIT(1) | BIT(3), base + 0x08);
}

#define DIAG_ARM_HOOK(n) \
	do { if (DIAG_ARM_STAGE == (n) || DIAG_ARM_STAGE == 9) diag_arm_aon_wdt(); } while (0)

#if DIAG_PANIC_STAGE == 1
#define DIAG_STAGE_1 panic("DIAG: stage 1 reached")
#else
#define DIAG_STAGE_1 do { } while (0)
#endif

#if DIAG_PANIC_STAGE == 2
#define DIAG_STAGE_2 panic("DIAG: stage 2 reached")
#else
#define DIAG_STAGE_2 do { } while (0)
#endif

#if DIAG_PANIC_STAGE == 3
#define DIAG_STAGE_3 panic("DIAG: stage 3 reached")
#else
#define DIAG_STAGE_3 do { } while (0)
#endif

#if DIAG_PANIC_STAGE == 4
#define DIAG_STAGE_4 panic("DIAG: stage 4 reached")
#else
#define DIAG_STAGE_4 do { } while (0)
#endif

#if DIAG_PANIC_STAGE == 5
#define DIAG_STAGE_5 panic("DIAG: stage 5 reached")
#else
#define DIAG_STAGE_5 do { } while (0)
#endif

#if DIAG_PANIC_STAGE == 6
#define DIAG_STAGE_6 panic("DIAG: stage 6 reached")
#else
#define DIAG_STAGE_6 do { } while (0)
#endif

#if DIAG_PANIC_STAGE == 7
#define DIAG_STAGE_7 panic("DIAG: stage 7 reached")
#else
#define DIAG_STAGE_7 do { } while (0)
#endif

#if DIAG_PANIC_STAGE == 8
#define DIAG_STAGE_8 panic("DIAG: stage 8 reached")
#else
#define DIAG_STAGE_8 do { } while (0)
#endif


/* ------------------------------------------------------------------ */
/* Panel log console: the kernel log tail painted on the framebuffer by
 * our own drawer.  The older DIAG_SCREEN engine drew whole rows and was
 * fed from vprintk_emit (racy: several CPUs may enter vprintk_emit at
 * once); a console .write callback is serialized by the printk core,
 * which is the race fix.  CON_PRINTBUFFER replays the entire log
 * buffer at registration, so nothing printed before the end of
 * setup_arch() is lost.  Geometry: 8x16 VGA font at 2x, 40 columns x
 * 39 rows, below the banner/grid/milestone instruments, above the
 * mirrored bottom bar.
 */
#include <linux/diag_font.h>

#define DIAG_FB_COLS	720U	/* not defined in this header's banner block */
#define DIAG_TXT_TOP	128U	/* below the milestone dashes (row 110) */
#define DIAG_TXT_BOTTOM	1548U	/* above the mirrored bar (row 1560)  */
#define DIAG_TXT_GW	8U	/* font cell 8x16                     */
#define DIAG_TXT_GH	16U
#define DIAG_TXT_SX	2U	/* 16x32 px glyphs                    */
#define DIAG_TXT_SY	2U
#define DIAG_TXT_CW	(DIAG_TXT_GW * DIAG_TXT_SX + 2U)	/* 18 */
#define DIAG_TXT_CH	(DIAG_TXT_GH * DIAG_TXT_SY + 4U)	/* 36 */
#define DIAG_TXT_COLS	(DIAG_FB_STRIDE / 4UL / DIAG_TXT_CW)	/* 40 */
#define DIAG_TXT_ROWS	((DIAG_TXT_BOTTOM - DIAG_TXT_TOP) / DIAG_TXT_CH)	/* 39 */
#define DIAG_TXT_FG	0xffffffU
#define DIAG_TXT_BG	0x000000U

struct diag_txt_state {
	bool ready;
	unsigned int row;
	unsigned int col;
};

/* defined exactly once, in arch/arm64/kernel/setup.c, next to the console */
extern struct diag_txt_state diag_txt_state;

static inline void diag_txt_pixel(u8 *p, u32 rgb)
{
	/* a8r8g8b8: bytes [B, G, R, A]; alpha 0xff or the DPU blends away */
	p[0] = rgb & 0xff;
	p[1] = (rgb >> 8) & 0xff;
	p[2] = (rgb >> 16) & 0xff;
	p[3] = 0xff;
}

/* one glyph at text position (row, col); the cursor does not move */
static inline void diag_txt_glyph(unsigned int row, unsigned int col,
				  unsigned char ch)
{
	u8 *base = (u8 *)phys_to_virt(DIAG_FB_PHYS) +
		(unsigned long)(DIAG_TXT_TOP + row * DIAG_TXT_CH) * DIAG_FB_STRIDE +
		(unsigned long)col * DIAG_TXT_CW * 4U;
	const unsigned char *g = &diag_font_data[(unsigned int)ch * DIAG_TXT_GH];
	unsigned int x, y, i, j;

	for (y = 0; y < DIAG_TXT_GH; y++) {
		u8 *line = base + (unsigned long)y * DIAG_TXT_SY * DIAG_FB_STRIDE;

		for (x = 0; x < DIAG_TXT_GW; x++) {
			u32 rgb = (g[y] & (0x80U >> x)) ? DIAG_TXT_FG : DIAG_TXT_BG;

			for (i = 0; i < DIAG_TXT_SY; i++)
				for (j = 0; j < DIAG_TXT_SX; j++)
					diag_txt_pixel(line + i * DIAG_FB_STRIDE +
						       (x * DIAG_TXT_SX + j) * 4U,
						       rgb);
		}
	}
	/* write back only this glyph cell's scanlines: flushing the whole
	 * 36-row band (DIAG_TXT_CH * DIAG_FB_STRIDE, ~103 KB) per glyph was
	 * the single largest boot-logging cost in the full-log console */
	for (y = 0; y < DIAG_TXT_GH * DIAG_TXT_SY; y++)
		__flush_dcache_area(base + (unsigned long)y * DIAG_FB_STRIDE,
				    (size_t)DIAG_TXT_GW * DIAG_TXT_SX * 4U);
}

/* shift the text area up one glyph row and blank the last line */
static inline void diag_txt_scroll(void)
{
	u8 *base = (u8 *)phys_to_virt(DIAG_FB_PHYS);
	unsigned long top = (unsigned long)DIAG_TXT_TOP * DIAG_FB_STRIDE;
	unsigned long area = (unsigned long)DIAG_TXT_ROWS *
		(unsigned long)DIAG_TXT_CH * DIAG_FB_STRIDE;
	unsigned long line = (unsigned long)DIAG_TXT_CH * DIAG_FB_STRIDE;
	u8 *last = base + top + (unsigned long)(DIAG_TXT_ROWS - 1) * line;
	u64 pat = 0xff000000ff000000ULL;	/* opaque black */
	unsigned int i;

	memmove(base + top, base + top + line, area - line);
	for (i = 0; i < (unsigned int)line; i += 8)
		*(u64 *)(last + i) = pat;
	__flush_dcache_area(base + top, area);
}

static inline void diag_txt_newline(void)
{
	diag_txt_state.col = 0;
	if (++diag_txt_state.row >= DIAG_TXT_ROWS) {
		diag_txt_scroll();
		diag_txt_state.row = DIAG_TXT_ROWS - 1;
	}
}

/* the console .write callback body: feed formatted printk text */
static inline void diag_txt_puts(const char *s, size_t n)
{
	if (!diag_txt_state.ready)
		return;
	while (n--) {
		char c = *s++;

		if (c == '\n')
			diag_txt_newline();
		else if (c == '\r')
			diag_txt_state.col = 0;
		else {
			if (c == '\t')
				c = ' ';
			if (c < 0x20 || c > 0x7e)
				c = '?';
			diag_txt_glyph(diag_txt_state.row, diag_txt_state.col, c);
			if (++diag_txt_state.col >= DIAG_TXT_COLS)
				diag_txt_newline();
		}
	}
}

/* clear the text area and arm the console; called once from setup_arch() */
static inline void diag_txt_init(void)
{
	u8 *base = (u8 *)phys_to_virt(DIAG_FB_PHYS);
	unsigned long top = (unsigned long)DIAG_TXT_TOP * DIAG_FB_STRIDE;
	unsigned long len = ((unsigned long)DIAG_TXT_BOTTOM -
			     DIAG_TXT_TOP) * DIAG_FB_STRIDE;
	u64 pat = 0xff000000ff000000ULL;	/* opaque black */
	unsigned long y;
	unsigned int i;

	for (y = top; y < top + len; y += DIAG_FB_STRIDE) {
		u8 *r = base + y;

		for (i = 0; i < DIAG_FB_COLS * 4U; i += 8)
			*(u64 *)(r + i) = pat;
	}
	__flush_dcache_area(base + top, len);
	diag_txt_state.ready = true;
	diag_txt_state.row = 0;
	diag_txt_state.col = 0;
}

#endif /* _LINUX_DIAG_STAGE_H */
