/* SPDX-License-Identifier: GPL-2.0 */
/*
 * On-panel boot progress indicator - port of the 4.14 v5 bar plus a
 * status grid, for the 7.x bring-up on the Moto E32.
 *
 * The bootloader leaves the panel scanning out the logo buffer at
 * 0x9e000000 (portrait 720x1600, stride 2160 = 720*3 RGB888, shown
 * 1:1) and passes console=null with no usable UART, so this is the
 * only boot visibility the device has.
 *
 * Bar (4.14 v5 geometry): rows 24..39, columns 72..648, 12 segments of
 * 48 px, mirrored at rows 1560..1575.  head.S paints the track and
 * segments 0..2 pre-MMU; setup.c repaints track + segments 0..7 after
 * paging_init(); init/main.c fills 8..11 after do_basic_setup().
 *
 * Status grid (7.x bring-up): two rows of 8 slots below the bar.
 * A slot painted on the TOP row means "step ok", on the BOTTOM row
 * "step failed"; unpainted = not reached.  Slot map:
 *   0 = init about to exec, 1 = hsphy probed, 2 = hsphy powered on,
 *   3 = musb glue probed,  4 = UDC registered, 5 = gadget bound,
 *   6 = gadget connected,  7 = heartbeat (blinks 1 Hz while the
 * scheduler lives).  DIAG_BANNER_INIT paints the dim grid; the hooks
 * light the squares as boot progresses.
 *
 * The logobuffer region is NOT marked no-map (and 7.x additionally
 * memblock_reserves 4 MiB in setup.c) so phys_to_virt() works here and
 * the page allocator cannot scribble over the panel.
 */
#ifndef _LINUX_DIAG_STAGE_H
#define _LINUX_DIAG_STAGE_H

#define DIAG_BANNER 1

#if DIAG_BANNER
#include <asm/memory.h>
#include <asm/cacheflush.h>

#define DIAG_FB_PHYS	0x9e000000UL
#define DIAG_FB_STRIDE	2880UL	/* 720 * 4 bytes, a8r8g8b8, portrait */
#define DIAG_FB_COLS	720U
#define DIAG_FB_ROWS	1600U

#define DIAG_BAR_X0	72U		/* first pixel column */
#define DIAG_BAR_SEG	48U		/* pixels per segment  */
#define DIAG_BAR_SEGS	12U
#define DIAG_BAR_ROWS	16U		/* bar thickness        */
#define DIAG_BAR_TOP	24U		/* first bar row        */
#define DIAG_BAR_BOT	(DIAG_FB_ROWS - DIAG_BAR_TOP - DIAG_BAR_ROWS)
#define DIAG_BAR_TRACK	0x28U
#define DIAG_BAR_SHADE	0xb8U
#define DIAG_BAR_SHADE_FULL 0xe8U

#define DIAG_ST_X0		72U
#define DIAG_ST_PITCH		72U
#define DIAG_ST_W		48U
#define DIAG_ST_H		16U
#define DIAG_ST_ROW_OK		60U
#define DIAG_ST_ROW_FAIL	84U
#define DIAG_ST_SHADE_DIM	0x28U
#define DIAG_ST_SHADE_FULL	0xb8U
#define DIAG_ST_SLOTS		8U
#define DIAG_ST_HB		7U	/* heartbeat slot */


/* one 8-byte-store run of npix pixels on row y starting at column x.
 * a8r8g8b8: bytes [B, G, R, A] - grayscale shade with alpha 0xff so the
 * DPU does not blend the pixel away. */
static inline void diag_bar_run(void *base, unsigned int y, unsigned int x,
				unsigned int npix, u8 shade)
{
	u32 px = 0xff000000u | (0x010101u * shade);
	u64 pat = (u64)px | ((u64)px << 32);
	u8 *p = (u8 *)base + (unsigned long)y * DIAG_FB_STRIDE +
		(unsigned long)x * 4UL;
	unsigned int i;

	for (i = 0; i < npix * 4U; i += 8)
		*(u64 *)(p + i) = pat;
}

/* arbitrary rectangle at rows [y, y+h), columns [x, x+w) */
static inline void diag_rect(unsigned int x, unsigned int y,
			     unsigned int w, unsigned int h, u8 shade)
{
	void *base = phys_to_virt(DIAG_FB_PHYS);
	unsigned int row;

	if (x >= DIAG_FB_COLS || y + h > DIAG_FB_ROWS)
		return;
	for (row = 0; row < h; row++)
		diag_bar_run(base, y + row, x, w, shade);
	dcache_clean_inval_poc((unsigned long)base + (unsigned long)y * DIAG_FB_STRIDE,
			       (unsigned long)base +
			       (unsigned long)(y + h) * DIAG_FB_STRIDE);
}

/* a rectangle at rows [DIAG_BAR_TOP, DIAG_BAR_TOP + DIAG_BAR_ROWS) plus
 * its mirror near the bottom edge */
static inline void diag_bar_rect(unsigned int x, unsigned int npix, u8 shade)
{
	unsigned int y;

	if (x >= DIAG_FB_COLS || npix > DIAG_FB_COLS - x)
		return;

	for (y = 0; y < DIAG_BAR_ROWS; y++) {
		diag_rect(x, DIAG_BAR_TOP + y, npix, 1, shade);
		diag_rect(x, DIAG_BAR_BOT + y, npix, 1, shade);
	}
}

/* the dim track the segments are drawn on */
static inline void diag_bootbar_track(void)
{
	diag_bar_rect(DIAG_BAR_X0, DIAG_BAR_SEGS * DIAG_BAR_SEG, DIAG_BAR_TRACK);
}

/* fill segments [seg0, seg1) of the boot progress bar */
static inline void diag_bootbar(unsigned int seg0, unsigned int seg1, u8 shade)
{
	unsigned int s;

	if (seg0 >= DIAG_BAR_SEGS || seg1 > DIAG_BAR_SEGS || seg0 >= seg1)
		return;
	for (s = seg0; s < seg1; s++)
		diag_bar_rect(DIAG_BAR_X0 + s * DIAG_BAR_SEG, DIAG_BAR_SEG, shade);
}


/* milestone dashes: a row of small squares painted as the boot passes
 * each early milestone (row 110, one 12x10 dash every 16 px, up to 40).
 * Count the dashes to see how far boot got.
 */
#define DIAG_MILE_ROW	110U
#define DIAG_MILE_X0	24U
#define DIAG_MILE_PITCH	16U
#define DIAG_MILE_W	12U
#define DIAG_MILE_H	10U
#define DIAG_MILE_MAX	40U

static inline void diag_number(unsigned int n);

/* 5x7 digit font, MSB = leftmost column */
static const u8 diag_digits[10][7] = {
	{0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e},	/* 0 */
	{0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e},	/* 1 */
	{0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f},	/* 2 */
	{0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e},	/* 3 */
	{0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02},	/* 4 */
	{0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e},	/* 5 */
	{0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e},	/* 6 */
	{0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},	/* 7 */
	{0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e},	/* 8 */
	{0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c},	/* 9 */
};

/* one font cell scaled to cell x cell panel pixels */
static inline void diag_bigcell(unsigned int digit, unsigned int col,
				unsigned int row, unsigned int x0,
				unsigned int y0, unsigned int cell)
{
	unsigned int r, c, x, y;

	if (digit > 9)
		return;
	for (r = 0; r < 7; r++)
		for (c = 0; c < 5; c++)
			if (diag_digits[digit][r] & (0x10 >> c))
				for (y = 0; y < cell; y++)
					diag_rect(x0 + col * (5 * cell + cell) + c * cell,
						  y0 + row * (7 * cell + cell / 2) + y * cell,
						  cell, cell, 0xff);
}

/* paint n (0..99) huge at three vertical positions: even when the
 * scanout shears or ghosts one copy, another stays readable */
static inline void diag_number(unsigned int n)
{
	unsigned int y;

	if (n > 99)
		n = 99;
	for (y = 0; y < 3; y++) {
		unsigned int y0 = 200U + y * 440U;
		unsigned int x0 = 64U;

		if (n >= 10) {
			diag_bigcell(n / 10, 0, 0, x0, y0, 32);
			diag_bigcell(n % 10, 1, 0, x0, y0, 32);
		} else {
			diag_bigcell(n, 0, 0, x0 + 5 * 32, y0, 32);
		}
	}
}

static inline void diag_paint_newlog(void);

static inline void diag_milestone(unsigned int n)
{
	if (n >= DIAG_MILE_MAX)
		return;
	diag_paint_newlog();
	diag_rect(DIAG_MILE_X0 + n * DIAG_MILE_PITCH, DIAG_MILE_ROW,
		  DIAG_MILE_W, DIAG_MILE_H, DIAG_BAR_SHADE_FULL);
}
/* status grid: slot n bright on the OK row (ok) or FAIL row (!ok) */
static inline void diag_status(unsigned int n, int ok)
{
	if (n >= DIAG_ST_SLOTS)
		return;
	diag_rect(DIAG_ST_X0 + n * DIAG_ST_PITCH,
		  ok ? DIAG_ST_ROW_OK : DIAG_ST_ROW_FAIL,
		  DIAG_ST_W, DIAG_ST_H, DIAG_ST_SHADE_FULL);
}

/* dim outline of all slots so "not reached" is distinguishable */
static inline void diag_status_grid(void)
{
	unsigned int n;

	for (n = 0; n < DIAG_ST_SLOTS; n++) {
		diag_rect(DIAG_ST_X0 + n * DIAG_ST_PITCH, DIAG_ST_ROW_OK,
			  DIAG_ST_W, DIAG_ST_H, DIAG_ST_SHADE_DIM);
		diag_rect(DIAG_ST_X0 + n * DIAG_ST_PITCH, DIAG_ST_ROW_FAIL,
			  DIAG_ST_W, DIAG_ST_H, DIAG_ST_SHADE_DIM);
	}
}

/* heartbeat: blinks the last slot while the scheduler lives */
static inline void diag_heartbeat(int on)
{
	diag_rect(DIAG_ST_X0 + DIAG_ST_HB * DIAG_ST_PITCH, DIAG_ST_ROW_OK,
		  DIAG_ST_W, DIAG_ST_H, on ? DIAG_ST_SHADE_FULL : DIAG_ST_SHADE_DIM);
}

#define DIAG_BANNER_INIT \
	(diag_bootbar_track(), diag_bootbar(0, 8, DIAG_BAR_SHADE))
#define DIAG_BOOTBAR_FULL diag_bootbar(8, 12, DIAG_BAR_SHADE_FULL)
#else
#define DIAG_BANNER_INIT do { } while (0)
#define DIAG_BOOTBAR_FULL do { } while (0)
#endif


/* ------------------------------------------------------------------ */
/* Panel log console (7.1 bring-up): the kernel log tail painted on the
 * framebuffer by this same drawer.  No earlycon core, no DT node, no
 * cmdline - the LK strips cmdline tokens and rewrites the DTB bootargs,
 * so a registered printk console is the only channel it cannot take
 * away.  The console registers at the end of setup_arch() with
 * CON_PRINTBUFFER, which first replays the whole log buffer: messages
 * printed before registration appear too.  Geometry: the 4.14's proven
 * 8x16 VGA font at 2x (16x32 px glyphs), text rows below the milestone
 * dashes, stopping above the mirrored bottom bar.  .write is
 * serialized by the printk core, so the shared cursor is safe (this is
 * why the 4.14's vprintk_emit mirror was racy and this is not).
 */
#include <linux/diag_font.h>

#define DIAG_TXT_TOP	128U	/* below the milestone dashes (row 110) */
#define DIAG_TXT_BOTTOM	1548U	/* above the mirrored bar (row 1560)  */
#define DIAG_TXT_GW	8U	/* font cell 8x16                     */
#define DIAG_TXT_GH	16U
#define DIAG_TXT_SX	2U	/* 16x32 px glyphs, like the 4.14     */
#define DIAG_TXT_SY	2U
#define DIAG_TXT_CW	(DIAG_TXT_GW * DIAG_TXT_SX + 2U)	/* 18 */
#define DIAG_TXT_CH	(DIAG_TXT_GH * DIAG_TXT_SY + 4U)	/* 36 */
#define DIAG_TXT_COLS	(DIAG_FB_COLS / DIAG_TXT_CW)		/* 40 */
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
	unsigned long a = (unsigned long)base;

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
	dcache_clean_inval_poc(a, a + (unsigned long)DIAG_TXT_CH * DIAG_FB_STRIDE);
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
	dcache_clean_inval_poc((unsigned long)base + top,
			       (unsigned long)base + top + area);
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
	unsigned long bot = (unsigned long)DIAG_TXT_BOTTOM * DIAG_FB_STRIDE;
	u64 pat = 0xff000000ff000000ULL;	/* opaque black */
	unsigned long y;
	unsigned int i;

	for (y = top; y < bot; y += DIAG_FB_STRIDE) {
		u8 *r = base + y;

		for (i = 0; i < DIAG_FB_COLS * 4U; i += 8)
			*(u64 *)(r + i) = pat;
	}
	dcache_clean_inval_poc((unsigned long)base + top, (unsigned long)base + bot);
	diag_txt_state.ready = true;
	diag_txt_state.row = 0;
	diag_txt_state.col = 0;
}


/* heartbeat overlay: the kernel-liveness clock, painted straight over
 * text row 0 with diag_txt_glyph - no printk involved, so it is
 * visible even when the console output path is dead (tests 28/29
 * printed nothing despite hundreds of printks). */
static inline void diag_txt_hb(unsigned int secs)
{
	char buf[14];
	char d[10];
	unsigned int n = 0, m = 0, v = secs;

	if (!diag_txt_state.ready)
		return;
	buf[n++] = 'T';
	buf[n++] = '+';
	if (!v) {
		buf[n++] = '0';
	} else {
		while (v && m < 10) {
			d[m++] = (char)('0' + v % 10);
			v /= 10;
		}
		while (m)
			buf[n++] = d[--m];
	}
	buf[n++] = 's';
	for (m = 0; m < n; m++)
		diag_txt_glyph(0, m, buf[m]);
}


/* ------------------------------------------------------------------ */
/* printk-independent log painter: kmsg_dump_get_line() reads records
 * straight from the printk ring with a private iterator - no console
 * list, no flush-type logic, no printer kthreads.  Called at every
 * diag_milestone() and heartbeat tick; state lives in .bss, defined
 * once in arch/arm64/kernel/setup.c next to the console. */
#include <linux/kmsg_dump.h>

#define DIAG_KLOG_LINE	2048

struct diag_klog {
	bool ready;
	struct kmsg_dump_iter iter;
	char line[DIAG_KLOG_LINE];
};

extern struct diag_klog diag_klog;

/* paint every record the iterator has not painted yet */
static inline void diag_paint_newlog(void)
{
	size_t len;

	if (!diag_klog.ready)
		return;
	while (kmsg_dump_get_line(&diag_klog.iter, false,
				  diag_klog.line,
				  sizeof(diag_klog.line) - 1, &len))
		diag_txt_puts(diag_klog.line, len);
}

/* fixed overlay line at text row 0/1 - no cursor impact, no printk */
static inline void diag_txt_overlay(unsigned int row, const char *s,
				    unsigned int len)
{
	unsigned int i;

	if (!diag_txt_state.ready || row > 1)
		return;
	for (i = 0; i < len && i < DIAG_TXT_COLS; i++)
		diag_txt_glyph(row, i, s[i]);
}

#endif /* _LINUX_DIAG_STAGE_H */
