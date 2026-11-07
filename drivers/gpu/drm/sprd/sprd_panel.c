// SPDX-License-Identifier: GPL-2.0
/*
 * Unisoc generic DSI panel driver
 *
 * 7.1 port of the vendor 4.14 drivers/gpu/drm/sprd/sprd_panel.c,
 * trimmed to what the Moto E32 (UMS9230 "hawaii") needs: the panel
 * parameters live in a /lcds/lcd_* DT node selected by the lcd_name=
 * cmdline argument the bootloader passes, the power/reset gpios hang
 * off the panel node itself and the DCS init/sleep command lists come
 * from the LCD node properties (docs/evidence/
 * 2026-11-06-7.1-merged-live.dtb has this device's own node,
 * /lcds/lcd_nt36525c_boe_mipi_fhd).  Dropped from the vendor driver:
 * the sysfs display class device, the ESD work (this panel ships
 * esd-check-enable = 0), the OLED backlight parser (no backlight
 * phandle on this panel node) and the per-board touchpanel hooks.
 *
 * Copyright (C) 2018 Spreadtrum Communications Inc.
 */

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <video/mipi_display.h>

#define SPRD_MIPI_DSI_FMT_DSC	0xff

/*
 * "sprd,dpi-clk-div" from the selected LCD node: 6 means the vendor
 * div6 scheme (DPI mux 6 = dphy-333m3, divider bypassed), 8 means
 * div8, 0 means the DPI clock is set to the pixel clock directly.
 * sprd_dpu.c reads this when it programs the DPI clock.
 */
unsigned int sprd_panel_dpi_clk_div;
EXPORT_SYMBOL_GPL(sprd_panel_dpi_clk_div);
#define SPRD_GPIO_SEQ_MAX	16

enum {
	CMD_CODE_INIT = 0,
	CMD_CODE_SLEEP_IN,
	CMD_CODE_SLEEP_OUT,
	CMD_CODE_DOZE_IN,
	CMD_CODE_DOZE_OUT,
	CMD_CODE_MAX,
};

enum {
	SPRD_DSI_MODE_CMD = 0,
	SPRD_DSI_MODE_VIDEO_BURST,
	SPRD_DSI_MODE_VIDEO_SYNC_PULSE,
	SPRD_DSI_MODE_VIDEO_SYNC_EVENT,
};

struct dsi_cmd_desc {
	u8 data_type;
	u8 wait;
	u8 wc_h;
	u8 wc_l;
	u8 payload[];
};

struct gpio_timing {
	u32 level;
	u32 delay;
};

struct reset_sequence {
	u32 items;
	struct gpio_timing timing[SPRD_GPIO_SEQ_MAX];
};

struct panel_info {
	struct device_node *of_node;
	struct drm_display_mode mode;
	struct gpio_desc *avdd_gpio;
	struct gpio_desc *avee_gpio;
	struct gpio_desc *reset_gpio;
	struct reset_sequence rst_on_seq;
	struct reset_sequence rst_off_seq;
	const void *cmds[CMD_CODE_MAX];
	int cmds_len[CMD_CODE_MAX];
	u32 format;
	u32 lanes;
	u32 mode_flags;
	bool use_dcs;
	u32 power_gpio_delay;
	u32 bit_clk_khz;	/* sprd,phy-bit-clock (kHz, vendor units) */
	u32 esc_clk_khz;	/* sprd,phy-escape-clock (kHz, capped 20 MHz) */
	/* oled-backlight child of the LCD node: a DCS brightness command
	 * (this panel: 23 00 00 02 51 ff = SET_DISPLAY_BRIGHTNESS) */
	void *bl_cmd;		/* writable copy (payload[1] = level) */
	int bl_cmd_len;
	u32 bl_max_level;
};

struct sprd_panel {
	struct drm_panel base;
	struct mipi_dsi_device *slave;
	struct panel_info info;
	struct regulator *supply;
};

static DEFINE_MUTEX(panel_lock);

/*
 * The bootloader passes the panel name on the cmdline
 * (lcd_name=lcd_nt36525c_boe_mipi_fhd); the matching /lcds/<name> node
 * holds this device's panel parameters.
 */
static const char *lcd_name;
static int __init lcd_name_get(char *str)
{
	if (str)
		lcd_name = str;
	return 0;
}
__setup("lcd_name=", lcd_name_get);

static inline struct sprd_panel *to_sprd_panel(struct drm_panel *panel)
{
	return container_of(panel, struct sprd_panel, base);
}

static int sprd_panel_send_cmds(struct mipi_dsi_device *dsi,
				const void *data, int size)
{
	struct sprd_panel *panel = mipi_dsi_get_drvdata(dsi);
	const struct dsi_cmd_desc *cmds = data;
	u16 len;

	if (!cmds || !dsi)
		return -EINVAL;

	while (size > 0) {
		len = (cmds->wc_h << 8) | cmds->wc_l;

		if (panel && panel->info.use_dcs)
			mipi_dsi_dcs_write_buffer(dsi, cmds->payload, len);
		else
			mipi_dsi_generic_write(dsi, cmds->payload, len);

		if (cmds->wait)
			msleep(cmds->wait);
		cmds = (const struct dsi_cmd_desc *)(cmds->payload + len);
		size -= (len + 4);
	}

	return 0;
}

static int sprd_panel_parse_reset_seq(struct device_node *lcd,
				      struct reset_sequence *seq,
				      const char *propname)
{
	u32 tmp[SPRD_GPIO_SEQ_MAX * 2];
	int i, count;

	memset(tmp, 0, sizeof(tmp));
	count = of_property_read_variable_u32_array(lcd, propname, tmp,
						    0, ARRAY_SIZE(tmp));
	if (count <= 0)
		return 0;

	/* each item is a (level, delay_ms) pair */
	seq->items = 0;
	for (i = 0; i + 1 < count && seq->items < SPRD_GPIO_SEQ_MAX;
	     i += 2) {
		seq->timing[seq->items].level = tmp[i];
		seq->timing[seq->items].delay = tmp[i + 1];
		seq->items++;
	}
	return 0;
}

static void sprd_panel_power_off(struct sprd_panel *panel)
{
	struct gpio_timing *timing;
	int i;

	if (panel->info.reset_gpio && panel->info.rst_off_seq.items) {
		timing = panel->info.rst_off_seq.timing;
		for (i = 0; i < panel->info.rst_off_seq.items; i++) {
			gpiod_direction_output(panel->info.reset_gpio,
					       timing[i].level);
			mdelay(timing[i].delay);
		}
	}
	if (panel->info.avee_gpio) {
		gpiod_direction_output(panel->info.avee_gpio, 0);
		mdelay(panel->info.power_gpio_delay);
	}
	if (panel->info.avdd_gpio) {
		gpiod_direction_output(panel->info.avdd_gpio, 0);
		mdelay(5);
	}
}

static int sprd_panel_power_on(struct sprd_panel *panel)
{
	struct gpio_timing *timing;
	int i;

	if (panel->info.avdd_gpio) {
		gpiod_direction_output(panel->info.avdd_gpio, 1);
		mdelay(panel->info.power_gpio_delay);
	}
	if (panel->info.avee_gpio) {
		gpiod_direction_output(panel->info.avee_gpio, 1);
		mdelay(5);
	}
	if (panel->info.reset_gpio && panel->info.rst_on_seq.items) {
		timing = panel->info.rst_on_seq.timing;
		for (i = 0; i < panel->info.rst_on_seq.items; i++) {
			gpiod_direction_output(panel->info.reset_gpio,
					       timing[i].level);
			mdelay(timing[i].delay);
		}
	}

	return 0;
}

static int sprd_panel_prepare(struct drm_panel *p)
{
	struct sprd_panel *panel = to_sprd_panel(p);

	return sprd_panel_power_on(panel);
}

static int sprd_panel_unprepare(struct drm_panel *p)
{
	struct sprd_panel *panel = to_sprd_panel(p);

	sprd_panel_power_off(panel);
	regulator_disable(panel->supply);

	return 0;
}

static int sprd_panel_enable(struct drm_panel *p)
{
	struct sprd_panel *panel = to_sprd_panel(p);

	mutex_lock(&panel_lock);
	sprd_panel_send_cmds(panel->slave,
			     panel->info.cmds[CMD_CODE_INIT],
			     panel->info.cmds_len[CMD_CODE_INIT]);
	/* the DCS brightness command (patched to the DT default level at
	 * parse time) - without it this LCD stays invisible */
	if (panel->info.bl_cmd)
		sprd_panel_send_cmds(panel->slave,
				     panel->info.bl_cmd,
				     panel->info.bl_cmd_len);
	mutex_unlock(&panel_lock);

	return 0;
}

static int sprd_panel_disable(struct drm_panel *p)
{
	struct sprd_panel *panel = to_sprd_panel(p);

	mutex_lock(&panel_lock);
	sprd_panel_send_cmds(panel->slave,
			     panel->info.cmds[CMD_CODE_SLEEP_IN],
			     panel->info.cmds_len[CMD_CODE_SLEEP_IN]);
	mutex_unlock(&panel_lock);

	return 0;
}

static int sprd_panel_get_modes(struct drm_panel *p,
				struct drm_connector *connector)
{
	struct sprd_panel *panel = to_sprd_panel(p);
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &panel->info.mode);
	if (!mode)
		return 0;

	mode->type |= DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	return 1;
}

static const struct drm_panel_funcs sprd_panel_funcs = {
	.prepare = sprd_panel_prepare,
	.unprepare = sprd_panel_unprepare,
	.enable = sprd_panel_enable,
	.disable = sprd_panel_disable,
	.get_modes = sprd_panel_get_modes,
};

static int sprd_panel_parse_lcd(struct device_node *lcd_node,
				struct sprd_panel *panel)
{
	struct panel_info *info = &panel->info;
	u32 bus_flags = 0, val;
	const char *str;
	const void *p;
	int bytes, ret;

	info->of_node = lcd_node;

	val = SPRD_DSI_MODE_VIDEO_SYNC_EVENT;
	of_property_read_u32(lcd_node, "sprd,dsi-work-mode", &val);
	switch (val) {
	case SPRD_DSI_MODE_CMD:
		info->mode_flags = 0;
		break;
	case SPRD_DSI_MODE_VIDEO_BURST:
		info->mode_flags = MIPI_DSI_MODE_VIDEO |
			MIPI_DSI_MODE_VIDEO_BURST;
		break;
	case SPRD_DSI_MODE_VIDEO_SYNC_PULSE:
		info->mode_flags = MIPI_DSI_MODE_VIDEO |
			MIPI_DSI_MODE_VIDEO_SYNC_PULSE;
		break;
	default:
		info->mode_flags = MIPI_DSI_MODE_VIDEO;
		break;
	}
	if (of_property_read_bool(lcd_node, "sprd,dsi-non-continuous-clock"))
		info->mode_flags |= MIPI_DSI_CLOCK_NON_CONTINUOUS;

	val = 4;
	of_property_read_u32(lcd_node, "sprd,dsi-lane-number", &val);
	info->lanes = val;

	if (!of_property_read_string(lcd_node, "sprd,dsi-color-format",
				     &str)) {
		if (!strcmp(str, "rgb888"))
			info->format = MIPI_DSI_FMT_RGB888;
		else if (!strcmp(str, "rgb666"))
			info->format = MIPI_DSI_FMT_RGB666;
		else if (!strcmp(str, "rgb666_packed"))
			info->format = MIPI_DSI_FMT_RGB666_PACKED;
		else if (!strcmp(str, "rgb565"))
			info->format = MIPI_DSI_FMT_RGB565;
		else if (!strcmp(str, "dsc"))
			info->format = SPRD_MIPI_DSI_FMT_DSC;
	} else {
		info->format = MIPI_DSI_FMT_RGB888;
	}

	info->mode.width_mm = 68;
	of_property_read_u32(lcd_node, "sprd,width-mm", &val);
	info->mode.width_mm = val;
	info->mode.height_mm = 121;
	of_property_read_u32(lcd_node, "sprd,height-mm", &val);
	info->mode.height_mm = val;

	info->power_gpio_delay = 5;
	of_property_read_u32(lcd_node, "sprd,power-gpio-delay",
			     &info->power_gpio_delay);

	info->use_dcs = of_property_read_bool(lcd_node, "sprd,use-dcs-write");

	/* DPHY clocks: the vendor driver feeds the raw DT value (kHz) to
	 * its own phy; the 7.1 stack reads hs_rate/lp_rate from the DSI
	 * device in Hz, so convert (megacores_pll.c: freq/1e6 -> MHz vs
	 * the ums9230 fvco bands 1250..2500, so 844000 kHz -> 844 MHz
	 * -> x2 = 1688 MHz fvco) */
	info->bit_clk_khz = 500000;
	of_property_read_u32(lcd_node, "sprd,phy-bit-clock",
			     &info->bit_clk_khz);
	sprd_panel_dpi_clk_div = 0;
	of_property_read_u32(lcd_node, "sprd,dpi-clk-div",
			     &sprd_panel_dpi_clk_div);

	info->esc_clk_khz = 20000;
	of_property_read_u32(lcd_node, "sprd,phy-escape-clock",
			     &info->esc_clk_khz);
	if (info->esc_clk_khz > 20000)
		info->esc_clk_khz = 20000;

	sprd_panel_parse_reset_seq(lcd_node, &info->rst_on_seq,
				   "sprd,reset-on-sequence");
	sprd_panel_parse_reset_seq(lcd_node, &info->rst_off_seq,
				   "sprd,reset-off-sequence");

	p = of_get_property(lcd_node, "sprd,initial-command", &bytes);
	if (p) {
		info->cmds[CMD_CODE_INIT] = p;
		info->cmds_len[CMD_CODE_INIT] = bytes;
	} else {
		pr_err("panel: can't find sprd,initial-command\n");
		return -ENODEV;
	}

	p = of_get_property(lcd_node, "sprd,sleep-in-command", &bytes);
	if (p) {
		info->cmds[CMD_CODE_SLEEP_IN] = p;
		info->cmds_len[CMD_CODE_SLEEP_IN] = bytes;
	}

	p = of_get_property(lcd_node, "sprd,sleep-out-command", &bytes);
	if (p) {
		info->cmds[CMD_CODE_SLEEP_OUT] = p;
		info->cmds_len[CMD_CODE_SLEEP_OUT] = bytes;
	}

	p = of_get_property(lcd_node, "sprd,doze-in-command", &bytes);
	if (p) {
		info->cmds[CMD_CODE_DOZE_IN] = p;
		info->cmds_len[CMD_CODE_DOZE_IN] = bytes;
	}

	p = of_get_property(lcd_node, "sprd,doze-out-command", &bytes);
	if (p) {
		info->cmds[CMD_CODE_DOZE_OUT] = p;
		info->cmds_len[CMD_CODE_DOZE_OUT] = bytes;
	}

	/* the LCD node has a display-timings child with timing0/1; use
	 * timing0 as the (preferred) native mode, like the vendor driver */
	ret = of_get_drm_display_mode(lcd_node, &info->mode, &bus_flags, 0);
	if (ret)
		return ret;

	/* the oled-backlight child carries the DCS brightness command
	 * (vendor of_parse_oled_cmds): one dsi_cmd_desc whose payload[1]
	 * is the brightness level - without it the panel stays dark */
	{
		struct device_node *bl = of_get_child_by_name(lcd_node,
				"oled-backlight");
		const void *p;
		int bytes;

		if (bl) {
			info->bl_max_level = 255;
			of_property_read_u32(bl, "sprd,max-level",
					     &info->bl_max_level);
			p = of_get_property(bl, "brightness-levels", &bytes);
			if (p && bytes > 4) {
				info->bl_cmd = kmemdup(p, bytes, GFP_KERNEL);
				info->bl_cmd_len = bytes;
				/* level = default * max / 255 (vendor math) */
				of_property_read_u32(bl,
					"default-brightness-level", &val);
				{
					u32 level = val * info->bl_max_level / 255;
					struct dsi_cmd_desc *c = info->bl_cmd;
					u16 len = (c->wc_h << 8) | c->wc_l;

					if (len >= 2)
						c->payload[1] = level;
				}
			}
			of_node_put(bl);
		}
	}

	return 0;
}

static int sprd_panel_probe(struct mipi_dsi_device *slave)
{
	struct sprd_panel *panel;
	struct device_node *lcd_node;
	char lcd_path[60];
	int ret;

	panel = devm_kzalloc(&slave->dev, sizeof(*panel), GFP_KERNEL);
	if (!panel)
		return -ENOMEM;

	mipi_dsi_set_drvdata(slave, panel);
	panel->slave = slave;

	panel->supply = devm_regulator_get(&slave->dev, "power");
	if (IS_ERR(panel->supply))
		return PTR_ERR(panel->supply);

	if (!lcd_name) {
		dev_err(&slave->dev, "no lcd_name= on the cmdline\n");
		return -ENODEV;
	}
	snprintf(lcd_path, sizeof(lcd_path), "/lcds/%s", lcd_name);
	lcd_node = of_find_node_by_path(lcd_path);
	if (!lcd_node) {
		dev_err(&slave->dev, "could not find %s\n", lcd_path);
		return -ENODEV;
	}
	ret = sprd_panel_parse_lcd(lcd_node, panel);
	of_node_put(lcd_node);
	if (ret) {
		dev_err(&slave->dev, "parse panel info failed\n");
		return ret;
	}

	panel->info.avdd_gpio = devm_gpiod_get_optional(&slave->dev,
							"avdd", GPIOD_OUT_LOW);
	if (IS_ERR(panel->info.avdd_gpio))
		return PTR_ERR(panel->info.avdd_gpio);
	panel->info.avee_gpio = devm_gpiod_get_optional(&slave->dev,
							"avee", GPIOD_OUT_LOW);
	if (IS_ERR(panel->info.avee_gpio))
		return PTR_ERR(panel->info.avee_gpio);
	panel->info.reset_gpio = devm_gpiod_get_optional(&slave->dev,
							"reset", GPIOD_OUT_LOW);
	if (IS_ERR(panel->info.reset_gpio))
		return PTR_ERR(panel->info.reset_gpio);

	drm_panel_init(&panel->base, &slave->dev, &sprd_panel_funcs,
		       DRM_MODE_CONNECTOR_DSI);

	/* drm_panel_add() returns void since 6.x */
	drm_panel_add(&panel->base);

	/* the panel node itself carries no DSI parameters - they live in
	 * the /lcds/<name> node, so fill in the device before attaching */
	slave->lanes = panel->info.lanes;
	slave->format = panel->info.format;
	slave->mode_flags = panel->info.mode_flags;
	slave->hs_rate = (unsigned long)panel->info.bit_clk_khz * 1000;
	slave->lp_rate = (unsigned long)panel->info.esc_clk_khz * 1000;

	ret = mipi_dsi_attach(slave);
	if (ret) {
		dev_err(&slave->dev, "failed to attach to DSI host: %d\n", ret);
		drm_panel_remove(&panel->base);
		return ret;
	}

	dev_info(&slave->dev, "panel %s: %ux%u, %u lanes, format %u\n",
		 lcd_name, panel->info.mode.hdisplay,
		 panel->info.mode.vdisplay, panel->info.lanes,
		 panel->info.format);

	return 0;
}

static void sprd_panel_remove(struct mipi_dsi_device *slave)
{
	struct sprd_panel *panel = mipi_dsi_get_drvdata(slave);

	mipi_dsi_detach(slave);
	drm_panel_remove(&panel->base);
}

static const struct of_device_id panel_of_match[] = {
	{ .compatible = "sprd,generic-mipi-panel", },
	{ }
};

static struct mipi_dsi_driver sprd_panel_driver = {
	.driver = {
		.name = "sprd-generic-panel",
		.of_match_table = panel_of_match,
	},
	.probe = sprd_panel_probe,
	.remove = sprd_panel_remove,
};
module_mipi_dsi_driver(sprd_panel_driver);

MODULE_AUTHOR("Flat Cat <flatchapapp@gmail.com>");
MODULE_AUTHOR("Porter Xu <porter.xu@unisoc.com>");
MODULE_DESCRIPTION("Unisoc Generic MIPI DSI Panel Driver");
MODULE_LICENSE("GPL v2");
