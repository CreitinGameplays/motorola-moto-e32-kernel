// SPDX-License-Identifier: GPL-2.0
/*
 * Generic framebuffer early console - hawaii port.
 *
 * Adapted from drivers/firmware/efi/earlycon.c: the geometry comes from
 * the kernel command line instead of the EFI screen_info, because this
 * bootloader scanout is a plain system-RAM framebuffer:
 *
 *     earlycon=simplefb,<addr>,<width>x<height>x<bpp>
 *
 * The Unisoc DPU fetches directly from DRAM and is not cache-coherent
 * with the CPU on this SoC, so every mapping is write-combine.
 */

#include <linux/console.h>
#include <linux/font.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/serial_core.h>
#include <linux/string.h>

#include <asm/early_ioremap.h>

static const struct console *earlycon_console __initdata;
static const struct font_desc *font;
static u16 cur_line_y, max_line_y;
static u32 efi_x_array[1024];
static u32 efi_x, efi_y;
static u64 fb_base;
static void *fb_virt;
static u32 fb_width, fb_height, fb_stride;

/*
 * early_memremap() is not usable for 'earlycon=simplefb keep_bootcon',
 * memremap() should be used instead.  memremap() will be available after
 * paging_init() which is earlier than initcall callbacks.  Thus adding
 * this early initcall function to map the whole framebuffer.
 */
static int __init fb_earlycon_remap_fb(void)
{
	/* bail if there is no bootconsole or it was unregistered already */
	if (!earlycon_console || !console_is_registered(earlycon_console))
		return 0;

	fb_virt = memremap(fb_base, fb_stride * fb_height, MEMREMAP_WC);

	return fb_virt ? 0 : -ENOMEM;
}
early_initcall(fb_earlycon_remap_fb);

static int __init fb_earlycon_unmap_fb(void)
{
	/* unmap the bootconsole fb unless keep_bootcon left it registered */
	if (fb_virt && !console_is_registered(earlycon_console))
		memunmap(fb_virt);
	return 0;
}
late_initcall(fb_earlycon_unmap_fb);

static __ref void *fb_earlycon_map(unsigned long start, unsigned long len)
{
	pgprot_t fb_prot;

	if (fb_virt)
		return fb_virt + start;

	/* write-combine: the DPU reads DRAM directly and is not
	 * cache-coherent with the CPU on this SoC */
	fb_prot = pgprot_writecombine(PAGE_KERNEL);
	return early_memremap_prot(fb_base + start, len, pgprot_val(fb_prot));
}

static __ref void fb_earlycon_unmap(void *addr, unsigned long len)
{
	if (fb_virt)
		return;

	early_memunmap(addr, len);
}

static void fb_earlycon_clear_scanline(unsigned int y)
{
	unsigned long *dst;
	u32 len;

	len = fb_stride;
	dst = fb_earlycon_map(y * len, len);
	if (!dst)
		return;

	memset(dst, 0, len);
	fb_earlycon_unmap(dst, len);
}

static void fb_earlycon_scroll_up(void)
{
	unsigned long *dst, *src;
	u32 maxlen = 0;
	u32 len;
	u32 i, height;

	/* Find the cached maximum x coordinate */
	for (i = 0; i < max_line_y; i++) {
		if (efi_x_array[i] > maxlen)
			maxlen = efi_x_array[i];
	}
	maxlen *= 4;

	len = fb_stride;
	height = fb_height;

	for (i = 0; i < height - font->height; i++) {
		dst = fb_earlycon_map(i * len, len);
		if (!dst)
			return;

		src = fb_earlycon_map((i + font->height) * len, len);
		if (!src) {
			fb_earlycon_unmap(dst, len);
			return;
		}

		memmove(dst, src, maxlen);

		fb_earlycon_unmap(src, len);
		fb_earlycon_unmap(dst, len);
	}
}

static void fb_earlycon_write_char(u32 *dst, unsigned char c, unsigned int h)
{
	/* a8r8g8b8: the high byte is ALPHA - the DPU blends the layer, so
	 * both colors need alpha 0xff or the text is fully transparent
	 * (measured: 0x00-alpha text is invisible over the logo) */
	const u32 color_black = 0xff000000;
	const u32 color_white = 0xffffffff;
	const u8 *src;
	int m, n, bytes;
	u8 x;

	bytes = BITS_TO_BYTES(font->width);
	src = font->data + c * font->height * bytes + h * bytes;

	for (m = 0; m < font->width; m++) {
		n = m % 8;
		x = *(src + m / 8);
		if ((x >> (7 - n)) & 1)
			*dst = color_white;
		else
			*dst = color_black;
		dst++;
	}
}

static void
fb_earlycon_write(struct console *con, const char *str, unsigned int num)
{
	u32 cur_fb_x = efi_x;
	unsigned int len;
	const char *s;
	void *dst;

	len = fb_stride;

	while (num) {
		unsigned int linemax = (fb_width - efi_x) / font->width;
		unsigned int h, count;

		count = strnchrnul(str, num, '\n') - str;
		if (count > linemax)
			count = linemax;

		for (h = 0; h < font->height; h++) {
			unsigned int n, x;

			dst = fb_earlycon_map((efi_y + h) * len, len);
			if (!dst)
				return;

			s = str;
			n = count;
			x = efi_x;

			while (n-- > 0) {
				fb_earlycon_write_char(dst + x * 4, *s, h);
				x += font->width;
				s++;
			}

			fb_earlycon_unmap(dst, len);
		}

		num -= count;
		efi_x += count * font->width;
		str += count;

		if (num > 0 && *s == '\n') {
			cur_fb_x = efi_x;
			efi_x = 0;
			efi_y += font->height;
			str++;
			num--;
		}

		if (efi_x + font->width > fb_width) {
			cur_fb_x = efi_x;
			efi_x = 0;
			efi_y += font->height;
		}

		if (efi_y + font->height > fb_height) {
			u32 i;

			efi_x_array[cur_line_y] = cur_fb_x;
			cur_line_y = (cur_line_y + 1) % max_line_y;

			efi_y -= font->height;
			fb_earlycon_scroll_up();

			for (i = 0; i < font->height; i++)
				fb_earlycon_clear_scanline(efi_y + i);
		}
	}
}

static int __init fb_earlycon_setup(struct earlycon_device *device,
				    const char *opt)
{
	u32 xres, yres, depth;
	u32 i;
	int ret;

	if (!device->port.mapbase)
		return -ENODEV;

	ret = sscanf(device->options, "%ux%ux%u", &xres, &yres, &depth);
	if (ret != 3)
		return -ENODEV;

	/*
	 * fb_earlycon_write_char() implicitly assumes a framebuffer with
	 * 32 bits per pixel.
	 */
	if (depth != 32)
		return -ENODEV;

	fb_base = device->port.mapbase;
	fb_width = xres;
	fb_height = yres;
	fb_stride = xres * 4;

	font = get_default_font(xres, yres, NULL, NULL);
	if (!font)
		return -ENODEV;

	/* Fill the cache with maximum possible value of x coordinate */
	memset32(efi_x_array, rounddown(xres, font->width), ARRAY_SIZE(efi_x_array));
	efi_y = rounddown(yres, font->height);

	/* Make sure we have cache for the x coordinate for the full screen */
	max_line_y = efi_y / font->height + 1;
	cur_line_y = 0;

	efi_y -= font->height;
	for (i = 0; i < (yres - efi_y) / font->height; i++)
		fb_earlycon_scroll_up();

	device->con->write = fb_earlycon_write;
	earlycon_console = device->con;
	return 0;
}
EARLYCON_DECLARE(simplefb, fb_earlycon_setup);
