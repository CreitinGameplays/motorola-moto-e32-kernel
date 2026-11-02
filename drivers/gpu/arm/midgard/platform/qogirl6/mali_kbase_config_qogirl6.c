/*
 *
 * (C) COPYRIGHT ARM Limited. All rights reserved.
 *
 * This program is free software and is provided to you under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation, and any use by you of this program is subject to the terms
 * of such GNU licence.
 *
 * A copy of the licence is included with the program, and can also be obtained
 * from Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 *
 */

/*
 * Unisoc qogirl6 (UMS9230 / T606 family) platform integration.
 *
 * Adapted for the Arm Bifrost r28p0-01eac0 DDK from the Unisoc vendor fork
 * (dikeckaan/mu300-linux upstream/modules/mali/platform/qogirl6, r40p0) and
 * to the device tree of this board (hawaii, Moto E32):
 *
 *  - the board describes its syscons with generic "syscons"/"syscon-names"
 *    properties, so the top-force-shutdown register is looked up by name
 *    (syscon_regmap_lookup_by_name / syscon_get_args_by_name, both exported
 *    by this kernel) instead of vendor-specific phandle properties
 *  - gpu_top_state and the power-domain state syscons of the vendor trees
 *    are not present here; the corresponding health waits degrade to a
 *    short settle delay
 *  - devfreq statistics hooks of the newer vendor DDK are not part of the
 *    r28p0 core and were removed
 */

#include <linux/ioport.h>
#include <linux/version.h>
#include <linux/delay.h>
#include <linux/clk.h>
#include <linux/hwspinlock.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>
#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/regulator/consumer.h>
#include <linux/nvmem-consumer.h>
#include <mali_kbase.h>
#include <mali_kbase_defs.h>
#include <mali_kbase_config.h>
#include <mali_kbase_debug.h>
#include <mali_kbase_ioctl.h>
#ifdef KBASE_PM_RUNTIME
#include <linux/pm_runtime.h>
#endif
#include <gpu,qogirl6-regs.h>
#include <gpu,qogirl6-mask.h>

#define DTS_CLK_OFFSET      4
#define PM_RUNTIME_DELAY_MS 50
#define FREQ_KHZ            1000

#define T606_GPLL_FREQ      650000000
#define T616_GPLL_FREQ      750000000

#define GPU_768M_FREQ       768000000
#define GPU_850M_FREQ       850000000

struct gpu_qos_config {
	u8 arqos;
	u8 awqos;
	u8 arqos_threshold;
	u8 awqos_threshold;
};

struct gpu_freq_info {
	struct clk *clk_src;
	int freq;	/* kHz */
	int volt;	/* uV */
	int div;
	int dvfs_index;
};

struct gpu_reg_info {
	struct regmap *regmap_ptr;
	uint32_t args[2];
};

struct gpu_dvfs_context {
	int gpu_clock_on;
	int gpu_power_on;
	int cur_voltage;

	struct clk *clk_gpu_i;
	struct clk *clk_gpu_core_eb;
	struct clk *clk_gpu_mem_eb;
	struct clk *clk_gpu_sys_eb;
	struct clk **gpu_clk_src;
	int gpu_clk_num;

	struct gpu_freq_info *freq_list;
	int freq_list_len;

	int cur_index;
	const struct gpu_freq_info *freq_cur;
	const struct gpu_freq_info *freq_default;

	struct semaphore *sem;

	struct gpu_reg_info top_force_reg;
	struct gpu_reg_info gpu_top_state_reg;
	struct gpu_reg_info gpu_qos_sel;
	struct gpu_reg_info gpu_qos;
	struct gpu_reg_info dvfs_index_cfg;
	struct gpu_reg_info sw_dvfs_ctrl;
	struct gpu_reg_info freq_upd_cfg;
	struct gpu_reg_info core_index0_map;
	struct gpu_reg_info core_index1_map;
	struct gpu_reg_info core_index2_map;
	struct gpu_reg_info core_index3_map;
	struct gpu_reg_info core_index4_map;
	struct gpu_reg_info core_index5_map;
	struct gpu_reg_info core_index6_map;
	struct gpu_reg_info core_index7_map;
	struct gpu_reg_info dvfs_voltage_value1;

	struct regmap *gpu_apb_base_ptr;
	struct regmap *gpu_dvfs_apb_base_ptr;

	u32 gpu_binning;
};

DEFINE_SEMAPHORE(gpu_dfs_sem);
static struct gpu_dvfs_context gpu_dvfs_ctx = {
	.gpu_clock_on = 0,
	.gpu_power_on = 0,

	.sem = &gpu_dfs_sem,
};

static struct gpu_qos_config gpu_qos_cfg;

int gpu_boost_level;

/*
 * The board uses the generic syscon binding: a list of phandles with their
 * arguments in "syscons" plus symbolic names in "syscon-names". The vendor
 * tree used dedicated phandle-with-args properties instead. Look the named
 * syscon up and recover its register offset/mask arguments.
 */
static struct regmap *qogirl6_syscon_by_name(struct device_node *np,
					     const char *name, u32 *args)
{
	struct regmap *regmap;
	int ret;

	regmap = syscon_regmap_lookup_by_name(np, name);
	if (IS_ERR(regmap)) {
		pr_info("mali: syscon '%s' unavailable (%ld)\n", name,
			PTR_ERR(regmap));
		return NULL;
	}

	if (args) {
		ret = syscon_get_args_by_name(np, name, 2, args);
		if (ret < 0)
			pr_warn("mali: syscon '%s' has no argument cells (%d)\n",
				name, ret);
	}

	return regmap;
}

static int sprd_gpu_cal_read(struct device_node *np, const char *cell_id,
			     u32 *val)
{
	struct nvmem_cell *cell;
	void *buf;
	size_t len;

	cell = of_nvmem_cell_get(np, cell_id);
	if (IS_ERR(cell))
		return PTR_ERR(cell);

	buf = nvmem_cell_read(cell, &len);
	if (IS_ERR(buf)) {
		nvmem_cell_put(cell);
		return PTR_ERR(buf);
	}

	memcpy(val, buf, min(len, sizeof(u32)));

	kfree(buf);
	nvmem_cell_put(cell);

	return 0;
}

static inline void mali_freq_init(struct device *dev)
{
	int i, clk_cnt, ret;
	struct device_node *qos_node;
	struct device_node *hwf;
	const char *auto_efuse = NULL;
	struct regmap *topdvfs_controller_base_ptr;

	gpu_dvfs_ctx.top_force_reg.regmap_ptr =
		qogirl6_syscon_by_name(dev->of_node, "top-force-shutdown",
				       gpu_dvfs_ctx.top_force_reg.args);
	if (!gpu_dvfs_ctx.top_force_reg.regmap_ptr)
		dev_err(dev, "mali: top-force-shutdown syscon is required\n");

	/* Optional: GPU top power state monitor (absent on this board) */
	gpu_dvfs_ctx.gpu_top_state_reg.regmap_ptr =
		qogirl6_syscon_by_name(dev->of_node, "gpu_top_state",
				       gpu_dvfs_ctx.gpu_top_state_reg.args);

	topdvfs_controller_base_ptr =
		syscon_regmap_lookup_by_phandle(dev->of_node,
						"topdvfs-controller");
	if (IS_ERR(topdvfs_controller_base_ptr)) {
		dev_err(dev, "mali: topdvfs-controller missing\n");
		topdvfs_controller_base_ptr = NULL;
	}

	gpu_dvfs_ctx.gpu_apb_base_ptr =
		syscon_regmap_lookup_by_phandle(dev->of_node,
						"sprd,gpu-apb-syscon");
	if (IS_ERR(gpu_dvfs_ctx.gpu_apb_base_ptr)) {
		dev_err(dev, "mali: sprd,gpu-apb-syscon missing\n");
		gpu_dvfs_ctx.gpu_apb_base_ptr = NULL;
	}

	gpu_dvfs_ctx.gpu_dvfs_apb_base_ptr =
		syscon_regmap_lookup_by_phandle(dev->of_node,
						"sprd,gpu-dvfs-apb-syscon");
	if (IS_ERR(gpu_dvfs_ctx.gpu_dvfs_apb_base_ptr)) {
		dev_err(dev, "mali: sprd,gpu-dvfs-apb-syscon missing\n");
		gpu_dvfs_ctx.gpu_dvfs_apb_base_ptr = NULL;
	}

	/* DCDC_MM_DVFS_VOLTAGE_VALUE1 */
	gpu_dvfs_ctx.dvfs_voltage_value1.regmap_ptr = topdvfs_controller_base_ptr;
	gpu_dvfs_ctx.dvfs_voltage_value1.args[0] = 0x00E8;
	gpu_dvfs_ctx.dvfs_voltage_value1.args[1] = 0x1FF;

	/* QoS */
	gpu_dvfs_ctx.gpu_qos_sel.regmap_ptr = gpu_dvfs_ctx.gpu_apb_base_ptr;
	gpu_dvfs_ctx.gpu_qos_sel.args[0] = REG_GPU_APB_RF_GPU_NIC400_QOS;
	gpu_dvfs_ctx.gpu_qos_sel.args[1] = MASK_GPU_APB_RF_GPU_QOS_SEL;

	gpu_dvfs_ctx.gpu_qos.regmap_ptr = gpu_dvfs_ctx.gpu_apb_base_ptr;
	gpu_dvfs_ctx.gpu_qos.args[0] = REG_GPU_APB_RF_GPU_NIC400_QOS;
	gpu_dvfs_ctx.gpu_qos.args[1] = MASK_GPU_APB_RF_AWQOS_THRESHOLD_GPU |
				      MASK_GPU_APB_RF_ARQOS_THRESHOLD_GPU |
				      MASK_GPU_APB_RF_AWQOS_GPU |
				      MASK_GPU_APB_RF_ARQOS_GPU;

	/* QoS configuration node (optional) */
	qos_node = of_parse_phandle(dev->of_node, "sprd,qos", 0);
	if (qos_node) {
		of_property_read_u8(qos_node, "arqos", &gpu_qos_cfg.arqos);
		of_property_read_u8(qos_node, "awqos", &gpu_qos_cfg.awqos);
		of_property_read_u8(qos_node, "arqos-threshold",
				    &gpu_qos_cfg.arqos_threshold);
		of_property_read_u8(qos_node, "awqos-threshold",
				    &gpu_qos_cfg.awqos_threshold);
		of_node_put(qos_node);
	}

	/* DVFS index configuration */
	gpu_dvfs_ctx.dvfs_index_cfg.regmap_ptr =
		gpu_dvfs_ctx.gpu_dvfs_apb_base_ptr;
	gpu_dvfs_ctx.dvfs_index_cfg.args[0] =
		REG_GPU_DVFS_APB_RF_GPU_DVFS_INDEX_CFG;
	gpu_dvfs_ctx.dvfs_index_cfg.args[1] = MASK_GPU_DVFS_APB_RF_GPU_DVFS_INDEX;

	gpu_dvfs_ctx.sw_dvfs_ctrl.regmap_ptr =
		gpu_dvfs_ctx.gpu_dvfs_apb_base_ptr;
	gpu_dvfs_ctx.sw_dvfs_ctrl.args[0] =
		REG_GPU_DVFS_APB_RF_GPU_SW_DVFS_CTRL;
	gpu_dvfs_ctx.sw_dvfs_ctrl.args[1] = MASK_GPU_DVFS_APB_RF_GPU_DVFS_ACK |
					   MASK_GPU_DVFS_APB_RF_GPU_DVFS_VOLTAGE_SW |
					   MASK_GPU_DVFS_APB_RF_GPU_DVFS_REQ_SW;

	gpu_dvfs_ctx.freq_upd_cfg.regmap_ptr =
		gpu_dvfs_ctx.gpu_dvfs_apb_base_ptr;
	gpu_dvfs_ctx.freq_upd_cfg.args[0] =
		REG_GPU_DVFS_APB_RF_GPU_FREQ_UPD_TYPE_CFG;
	gpu_dvfs_ctx.freq_upd_cfg.args[1] =
		MASK_GPU_DVFS_APB_RF_GPU_CORE_FREQ_UPD_HDSK_EN |
		MASK_GPU_DVFS_APB_RF_GPU_CORE_FREQ_UPD_DELAY_EN;

	gpu_dvfs_ctx.core_index0_map.regmap_ptr =
		gpu_dvfs_ctx.gpu_dvfs_apb_base_ptr;
	gpu_dvfs_ctx.core_index0_map.args[0] =
		REG_GPU_DVFS_APB_RF_GPU_CORE_INDEX0_MAP;
	gpu_dvfs_ctx.core_index0_map.args[1] =
		MASK_GPU_DVFS_APB_RF_GPU_CORE_VOL_INDEX0;

	gpu_dvfs_ctx.core_index1_map.regmap_ptr =
		gpu_dvfs_ctx.gpu_dvfs_apb_base_ptr;
	gpu_dvfs_ctx.core_index1_map.args[0] =
		REG_GPU_DVFS_APB_RF_GPU_CORE_INDEX1_MAP;
	gpu_dvfs_ctx.core_index1_map.args[1] =
		MASK_GPU_DVFS_APB_RF_GPU_CORE_VOL_INDEX1;

	gpu_dvfs_ctx.core_index2_map.regmap_ptr =
		gpu_dvfs_ctx.gpu_dvfs_apb_base_ptr;
	gpu_dvfs_ctx.core_index2_map.args[0] =
		REG_GPU_DVFS_APB_RF_GPU_CORE_INDEX2_MAP;
	gpu_dvfs_ctx.core_index2_map.args[1] =
		MASK_GPU_DVFS_APB_RF_GPU_CORE_VOL_INDEX2;

	gpu_dvfs_ctx.core_index3_map.regmap_ptr =
		gpu_dvfs_ctx.gpu_dvfs_apb_base_ptr;
	gpu_dvfs_ctx.core_index3_map.args[0] =
		REG_GPU_DVFS_APB_RF_GPU_CORE_INDEX3_MAP;
	gpu_dvfs_ctx.core_index3_map.args[1] =
		MASK_GPU_DVFS_APB_RF_GPU_CORE_VOL_INDEX3;

	gpu_dvfs_ctx.core_index4_map.regmap_ptr =
		gpu_dvfs_ctx.gpu_dvfs_apb_base_ptr;
	gpu_dvfs_ctx.core_index4_map.args[0] =
		REG_GPU_DVFS_APB_RF_GPU_CORE_INDEX4_MAP;
	gpu_dvfs_ctx.core_index4_map.args[1] =
		MASK_GPU_DVFS_APB_RF_GPU_CORE_VOL_INDEX4;

	gpu_dvfs_ctx.core_index5_map.regmap_ptr =
		gpu_dvfs_ctx.gpu_dvfs_apb_base_ptr;
	gpu_dvfs_ctx.core_index5_map.args[0] =
		REG_GPU_DVFS_APB_RF_GPU_CORE_INDEX5_MAP;
	gpu_dvfs_ctx.core_index5_map.args[1] =
		MASK_GPU_DVFS_APB_RF_GPU_CORE_VOL_INDEX5;

	gpu_dvfs_ctx.core_index6_map.regmap_ptr =
		gpu_dvfs_ctx.gpu_dvfs_apb_base_ptr;
	gpu_dvfs_ctx.core_index6_map.args[0] =
		REG_GPU_DVFS_APB_RF_GPU_CORE_INDEX6_MAP;
	gpu_dvfs_ctx.core_index6_map.args[1] =
		MASK_GPU_DVFS_APB_RF_GPU_CORE_VOL_INDEX6;

	gpu_dvfs_ctx.core_index7_map.regmap_ptr =
		gpu_dvfs_ctx.gpu_dvfs_apb_base_ptr;
	gpu_dvfs_ctx.core_index7_map.args[0] =
		REG_GPU_DVFS_APB_RF_GPU_CORE_INDEX7_MAP;
	gpu_dvfs_ctx.core_index7_map.args[1] =
		MASK_GPU_DVFS_APB_RF_GPU_CORE_VOL_INDEX7;

	/* Speed bin (optional) */
	ret = sprd_gpu_cal_read(dev->of_node, "gpu_bin",
				&gpu_dvfs_ctx.gpu_binning);
	if (ret)
		dev_warn(dev, "mali: gpu_bin nvmem cell read failed: %d\n", ret);

	gpu_dvfs_ctx.clk_gpu_i = of_clk_get(dev->of_node, 0);
	gpu_dvfs_ctx.clk_gpu_core_eb = of_clk_get(dev->of_node, 1);
	gpu_dvfs_ctx.clk_gpu_mem_eb = of_clk_get(dev->of_node, 2);
	gpu_dvfs_ctx.clk_gpu_sys_eb = of_clk_get(dev->of_node, 3);

	clk_cnt = of_clk_get_parent_count(dev->of_node);
	gpu_dvfs_ctx.gpu_clk_num = clk_cnt - DTS_CLK_OFFSET;
	if (gpu_dvfs_ctx.gpu_clk_num <= 0) {
		dev_err(dev, "mali: malformed clocks property (%d)\n", clk_cnt);
		gpu_dvfs_ctx.gpu_clk_num = 0;
		return;
	}

	gpu_dvfs_ctx.gpu_clk_src =
		vmalloc(sizeof(struct clk *) * gpu_dvfs_ctx.gpu_clk_num);
	if (!gpu_dvfs_ctx.gpu_clk_src) {
		gpu_dvfs_ctx.gpu_clk_num = 0;
		return;
	}

	for (i = 0; i < gpu_dvfs_ctx.gpu_clk_num; i++)
		gpu_dvfs_ctx.gpu_clk_src[i] =
			of_clk_get(dev->of_node, i + DTS_CLK_OFFSET);

	gpu_dvfs_ctx.freq_list_len = of_property_count_elems_of_size(
		dev->of_node, "sprd,dvfs-lists", 4 * sizeof(u32));
	if (gpu_dvfs_ctx.freq_list_len <= 0) {
		dev_err(dev, "mali: no sprd,dvfs-lists\n");
		gpu_dvfs_ctx.freq_list_len = 0;
		return;
	}

	gpu_dvfs_ctx.freq_list =
		vmalloc(sizeof(struct gpu_freq_info) * gpu_dvfs_ctx.freq_list_len);
	if (!gpu_dvfs_ctx.freq_list) {
		gpu_dvfs_ctx.freq_list_len = 0;
		return;
	}

	for (i = 0; i < gpu_dvfs_ctx.freq_list_len; i++) {
		int clk = 0;

		of_property_read_u32_index(dev->of_node, "sprd,dvfs-lists",
					   4 * i + 2, &clk);
		if (clk < DTS_CLK_OFFSET ||
		    (clk - DTS_CLK_OFFSET) >= gpu_dvfs_ctx.gpu_clk_num) {
			dev_warn(dev, "mali: bad clock index %d in dvfs list\n",
				 clk);
			clk = DTS_CLK_OFFSET;
		}
		gpu_dvfs_ctx.freq_list[i].clk_src =
			gpu_dvfs_ctx.gpu_clk_src[clk - DTS_CLK_OFFSET];
		of_property_read_u32_index(dev->of_node, "sprd,dvfs-lists",
					   4 * i,
					   &gpu_dvfs_ctx.freq_list[i].freq);
		of_property_read_u32_index(dev->of_node, "sprd,dvfs-lists",
					   4 * i + 1,
					   &gpu_dvfs_ctx.freq_list[i].volt);
		of_property_read_u32_index(dev->of_node, "sprd,dvfs-lists",
					   4 * i + 3,
					   &gpu_dvfs_ctx.freq_list[i].div);
		gpu_dvfs_ctx.freq_list[i].dvfs_index = i;
	}

	hwf = of_find_node_by_path("/hwfeature/auto");
	if (hwf)
		auto_efuse = (char *)of_get_property(hwf, "efuse", NULL);

	if (auto_efuse && (!strcmp(auto_efuse, "T606") ||
			   !strcmp(auto_efuse, "T612"))) {
		/* GPLL max frequency for T606/T612 is 650 MHz */
		gpu_dvfs_ctx.freq_list[gpu_dvfs_ctx.freq_list_len - 1].freq =
			T606_GPLL_FREQ / FREQ_KHZ;

		/* drop the 768 MHz entry, keep 650 MHz in its place */
		memcpy(&gpu_dvfs_ctx.freq_list[gpu_dvfs_ctx.freq_list_len - 2],
		       &gpu_dvfs_ctx.freq_list[gpu_dvfs_ctx.freq_list_len - 1],
		       sizeof(struct gpu_freq_info));
		memset(&gpu_dvfs_ctx.freq_list[gpu_dvfs_ctx.freq_list_len - 1],
		       0, sizeof(struct gpu_freq_info));
		gpu_dvfs_ctx.freq_list_len--;
	} else if (auto_efuse && !strcmp(auto_efuse, "T616")) {
		/* GPLL max frequency for T616 is 750 MHz */
		gpu_dvfs_ctx.freq_list[gpu_dvfs_ctx.freq_list_len - 1].freq =
			T616_GPLL_FREQ / FREQ_KHZ;

		memcpy(&gpu_dvfs_ctx.freq_list[gpu_dvfs_ctx.freq_list_len - 2],
		       &gpu_dvfs_ctx.freq_list[gpu_dvfs_ctx.freq_list_len - 1],
		       sizeof(struct gpu_freq_info));
		memset(&gpu_dvfs_ctx.freq_list[gpu_dvfs_ctx.freq_list_len - 1],
		       0, sizeof(struct gpu_freq_info));
		gpu_dvfs_ctx.freq_list_len--;
	}

	/* fixed board default, the r28p0 core has no DVFS table hook */
	i = 0;
	of_property_read_u32(dev->of_node, "sprd,dvfs-default", &i);
	if (i < 0 || i >= gpu_dvfs_ctx.freq_list_len)
		i = 0;

	gpu_dvfs_ctx.freq_default = &gpu_dvfs_ctx.freq_list[i];
	gpu_dvfs_ctx.cur_index = i;
	gpu_dvfs_ctx.freq_cur = gpu_dvfs_ctx.freq_default;
	gpu_dvfs_ctx.cur_voltage = gpu_dvfs_ctx.freq_cur->volt;
}

static bool mali_top_pwr_state_check(void)
{
	u32 top_state;
	const u32 top_mask = 0x1f << 16;
	const u32 top_pwr_on = 0;
	int cnt_1 = 0, cnt_2 = 0;

	/* This board does not describe the GPU top power state syscon. */
	if (!gpu_dvfs_ctx.gpu_top_state_reg.regmap_ptr) {
		udelay(200);
		return true;
	}

	for (;;) {
		regmap_read(gpu_dvfs_ctx.gpu_top_state_reg.regmap_ptr,
			    gpu_dvfs_ctx.gpu_top_state_reg.args[0], &top_state);
		top_state &= top_mask;
		if (top_state == top_pwr_on) {
			if (++cnt_1 >= 5)
				return true;
		} else {
			cnt_1 = 0;
			udelay(50);
			if (++cnt_2 >= 200) {
				pr_err("mali: gpu top power on is timeout!\n");
				WARN_ON(1);
				return false;
			}
		}
	}

	return true;
}

static inline void mali_power_on(void)
{
	if (!gpu_dvfs_ctx.top_force_reg.regmap_ptr)
		return;

	regmap_update_bits(gpu_dvfs_ctx.top_force_reg.regmap_ptr,
			   gpu_dvfs_ctx.top_force_reg.args[0],
			   gpu_dvfs_ctx.top_force_reg.args[1],
			   ~gpu_dvfs_ctx.top_force_reg.args[1]);

	udelay(100);

	if (!mali_top_pwr_state_check()) {
		pr_err("mali: gpu top not power on\n");
		WARN_ON(1);
	}

	gpu_dvfs_ctx.gpu_power_on = 1;
}

static inline void mali_power_off(void)
{
	gpu_dvfs_ctx.gpu_power_on = 0;

	if (!gpu_dvfs_ctx.top_force_reg.regmap_ptr)
		return;

	regmap_update_bits(gpu_dvfs_ctx.top_force_reg.regmap_ptr,
			   gpu_dvfs_ctx.top_force_reg.args[0],
			   gpu_dvfs_ctx.top_force_reg.args[1],
			   gpu_dvfs_ctx.top_force_reg.args[1]);
}

static void qogirl6_clk_enable(struct clk *clk)
{
	if (!IS_ERR_OR_NULL(clk))
		clk_prepare_enable(clk);
}

static void qogirl6_clk_disable(struct clk *clk)
{
	if (!IS_ERR_OR_NULL(clk))
		clk_disable_unprepare(clk);
}

static void mali_qos_config(void)
{
	if (!gpu_dvfs_ctx.gpu_qos_sel.regmap_ptr || !gpu_dvfs_ctx.gpu_qos.regmap_ptr)
		return;

	regmap_update_bits(gpu_dvfs_ctx.gpu_qos_sel.regmap_ptr,
			   gpu_dvfs_ctx.gpu_qos_sel.args[0],
			   gpu_dvfs_ctx.gpu_qos_sel.args[1],
			   gpu_dvfs_ctx.gpu_qos_sel.args[1]);
	regmap_update_bits(gpu_dvfs_ctx.gpu_qos.regmap_ptr,
			   gpu_dvfs_ctx.gpu_qos.args[0],
			   gpu_dvfs_ctx.gpu_qos.args[1],
			   ((gpu_qos_cfg.awqos_threshold << 12) |
			    (gpu_qos_cfg.arqos_threshold << 8) |
			    (gpu_qos_cfg.awqos << 4) | gpu_qos_cfg.arqos));
}

static inline void mali_clock_on(void)
{
	struct device_node *hwf;
	const char *auto_efuse = NULL;
	int i;

	hwf = of_find_node_by_path("/hwfeature/auto");
	if (hwf)
		auto_efuse = (char *)of_get_property(hwf, "efuse", NULL);

	for (i = 0; i < gpu_dvfs_ctx.gpu_clk_num; i++)
		qogirl6_clk_enable(gpu_dvfs_ctx.gpu_clk_src[i]);

	qogirl6_clk_enable(gpu_dvfs_ctx.clk_gpu_i);
	qogirl6_clk_enable(gpu_dvfs_ctx.clk_gpu_core_eb);
	qogirl6_clk_enable(gpu_dvfs_ctx.clk_gpu_mem_eb);
	qogirl6_clk_enable(gpu_dvfs_ctx.clk_gpu_sys_eb);
	udelay(200);

	mali_top_pwr_state_check();

	/*
	 * The max GPLL source needs to be trimmed on T606/T612 (650 MHz) and
	 * T616 (750 MHz); the lower sources are fixed-rate clock entries.
	 */
	if (auto_efuse && gpu_dvfs_ctx.freq_list_len) {
		int last = gpu_dvfs_ctx.freq_list_len - 1;

		if (!strcmp(auto_efuse, "T606") ||
		    !strcmp(auto_efuse, "T612") ||
		    !strcmp(auto_efuse, "T616"))
			clk_set_rate(gpu_dvfs_ctx.freq_list[last].clk_src,
				     (unsigned long)gpu_dvfs_ctx.freq_list[last].freq *
					     FREQ_KHZ);
	}

	/* Program the core voltage index map for the detected speed bin. */
	if (auto_efuse && (!strcmp(auto_efuse, "T606") ||
			   !strcmp(auto_efuse, "T612"))) {
		if (1 == gpu_dvfs_ctx.gpu_binning) {
			regmap_update_bits(gpu_dvfs_ctx.core_index1_map.regmap_ptr,
					   gpu_dvfs_ctx.core_index1_map.args[0],
					   gpu_dvfs_ctx.core_index1_map.args[1],
					   0 << 14);
			regmap_update_bits(gpu_dvfs_ctx.core_index3_map.regmap_ptr,
					   gpu_dvfs_ctx.core_index3_map.args[0],
					   gpu_dvfs_ctx.core_index3_map.args[1],
					   1 << 14);
			regmap_update_bits(gpu_dvfs_ctx.core_index4_map.regmap_ptr,
					   gpu_dvfs_ctx.core_index4_map.args[0],
					   gpu_dvfs_ctx.core_index4_map.args[1],
					   1 << 14);
			regmap_update_bits(gpu_dvfs_ctx.core_index5_map.regmap_ptr,
					   gpu_dvfs_ctx.core_index5_map.args[0],
					   gpu_dvfs_ctx.core_index5_map.args[1],
					   1 << 14);
			regmap_update_bits(gpu_dvfs_ctx.core_index6_map.regmap_ptr,
					   gpu_dvfs_ctx.core_index6_map.args[0],
					   gpu_dvfs_ctx.core_index6_map.args[1],
					   1 << 14);
			regmap_update_bits(gpu_dvfs_ctx.core_index7_map.regmap_ptr,
					   gpu_dvfs_ctx.core_index7_map.args[0],
					   gpu_dvfs_ctx.core_index7_map.args[1],
					   1 << 14);
		}
	} else if (2 == gpu_dvfs_ctx.gpu_binning) {
		regmap_update_bits(gpu_dvfs_ctx.core_index3_map.regmap_ptr,
				   gpu_dvfs_ctx.core_index3_map.args[0],
				   gpu_dvfs_ctx.core_index3_map.args[1], 3 << 14);
		regmap_update_bits(gpu_dvfs_ctx.core_index4_map.regmap_ptr,
				   gpu_dvfs_ctx.core_index4_map.args[0],
				   gpu_dvfs_ctx.core_index4_map.args[1], 3 << 14);
		regmap_update_bits(gpu_dvfs_ctx.core_index5_map.regmap_ptr,
				   gpu_dvfs_ctx.core_index5_map.args[0],
				   gpu_dvfs_ctx.core_index5_map.args[1], 3 << 14);
		regmap_update_bits(gpu_dvfs_ctx.core_index6_map.regmap_ptr,
				   gpu_dvfs_ctx.core_index6_map.args[0],
				   gpu_dvfs_ctx.core_index6_map.args[1], 3 << 14);
		regmap_update_bits(gpu_dvfs_ctx.core_index7_map.regmap_ptr,
				   gpu_dvfs_ctx.core_index7_map.args[0],
				   gpu_dvfs_ctx.core_index7_map.args[1], 3 << 14);
	}

	/* program the DVFS update path and select the boot frequency */
	if (gpu_dvfs_ctx.freq_upd_cfg.regmap_ptr)
		regmap_update_bits(gpu_dvfs_ctx.freq_upd_cfg.regmap_ptr,
				   gpu_dvfs_ctx.freq_upd_cfg.args[0],
				   gpu_dvfs_ctx.freq_upd_cfg.args[1], 1);

	if (gpu_dvfs_ctx.dvfs_index_cfg.regmap_ptr && gpu_dvfs_ctx.freq_list_len)
		regmap_update_bits(gpu_dvfs_ctx.dvfs_index_cfg.regmap_ptr,
				   gpu_dvfs_ctx.dvfs_index_cfg.args[0],
				   gpu_dvfs_ctx.dvfs_index_cfg.args[1],
				   gpu_dvfs_ctx.freq_list[gpu_dvfs_ctx.cur_index].dvfs_index);

	mali_qos_config();

	gpu_dvfs_ctx.gpu_clock_on = 1;
}

static inline void mali_clock_off(void)
{
	int i;

	gpu_dvfs_ctx.gpu_clock_on = 0;

	qogirl6_clk_disable(gpu_dvfs_ctx.clk_gpu_core_eb);
	qogirl6_clk_disable(gpu_dvfs_ctx.clk_gpu_mem_eb);
	qogirl6_clk_disable(gpu_dvfs_ctx.clk_gpu_sys_eb);
	qogirl6_clk_disable(gpu_dvfs_ctx.clk_gpu_i);

	for (i = 0; i < gpu_dvfs_ctx.gpu_clk_num; i++)
		qogirl6_clk_disable(gpu_dvfs_ctx.gpu_clk_src[i]);
}

static int mali_platform_init(struct kbase_device *kbdev)
{
	mali_freq_init(kbdev->dev);

	mali_power_on();
	mali_clock_on();

	return 0;
}

static void mali_platform_term(struct kbase_device *kbdev)
{
	down(gpu_dvfs_ctx.sem);

	mali_clock_off();
	mali_power_off();

	vfree(gpu_dvfs_ctx.freq_list);
	vfree(gpu_dvfs_ctx.gpu_clk_src);

	up(gpu_dvfs_ctx.sem);
}

struct kbase_platform_funcs_conf platform_qogirl6_funcs = {
	.platform_init_func = mali_platform_init,
	.platform_term_func = mali_platform_term,
};

static void mali_power_mode_change(struct kbase_device *kbdev, int power_mode)
{
	down(gpu_dvfs_ctx.sem);

	switch (power_mode) {
	case 0: /* power on */
		if (!gpu_dvfs_ctx.gpu_power_on) {
			mali_power_on();
			mali_clock_on();
		}

		if (!gpu_dvfs_ctx.gpu_clock_on)
			mali_clock_on();
		break;

	case 1: /* light sleep */
	case 2: /* deep sleep */
		if (gpu_dvfs_ctx.gpu_clock_on)
			mali_clock_off();

		if (gpu_dvfs_ctx.gpu_power_on)
			mali_power_off();
		break;

	default:
		break;
	}

	up(gpu_dvfs_ctx.sem);
}

static void pm_callback_power_off(struct kbase_device *kbdev)
{
#ifdef KBASE_PM_RUNTIME
	int res;

	res = pm_runtime_put_sync(kbdev->dev);
	if (res < 0)
		dev_err(kbdev->dev, "mali: pm_runtime_put_sync failed (%d)\n",
			res);
#endif

	mali_power_mode_change(kbdev, 1);
}

static int pm_callback_power_on(struct kbase_device *kbdev)
{
	mali_power_mode_change(kbdev, 0);

#ifdef KBASE_PM_RUNTIME
	{
		int res;

		res = pm_runtime_get_sync(kbdev->dev);
		if (res < 0)
			dev_err(kbdev->dev,
				"mali: pm_runtime_get_sync failed (%d)\n", res);
	}
#endif

	return 1;
}

static void pm_callback_power_suspend(struct kbase_device *kbdev)
{
	mali_power_mode_change(kbdev, 2);
}

static void pm_callback_power_resume(struct kbase_device *kbdev)
{
	mali_power_mode_change(kbdev, 0);
}

#ifdef KBASE_PM_RUNTIME
static int pm_callback_power_runtime_init(struct kbase_device *kbdev)
{
	pm_runtime_set_active(kbdev->dev);
	pm_suspend_ignore_children(kbdev->dev, true);
	pm_runtime_set_autosuspend_delay(kbdev->dev, PM_RUNTIME_DELAY_MS);
	pm_runtime_use_autosuspend(kbdev->dev);
	pm_runtime_enable(kbdev->dev);

	return 0;
}

static void pm_callback_power_runtime_term(struct kbase_device *kbdev)
{
	pm_runtime_disable(kbdev->dev);
}
#endif /* KBASE_PM_RUNTIME */

struct kbase_pm_callback_conf pm_qogirl6_callbacks = {
	.power_off_callback = pm_callback_power_off,
	.power_on_callback = pm_callback_power_on,
	.power_suspend_callback = pm_callback_power_suspend,
	.power_resume_callback = pm_callback_power_resume,
#ifdef KBASE_PM_RUNTIME
	.power_runtime_init_callback = pm_callback_power_runtime_init,
	.power_runtime_term_callback = pm_callback_power_runtime_term,
	.power_runtime_off_callback = NULL,
	.power_runtime_on_callback = NULL,
#endif
};

static struct kbase_platform_config qogirl6_platform_config;

struct kbase_platform_config *kbase_get_platform_config(void)
{
	return &qogirl6_platform_config;
}

int kbase_platform_early_init(void)
{
	return 0;
}

#ifdef CONFIG_MALI_DEVFREQ
static int freq_search(struct gpu_freq_info freq_list[], int len, int key)
{
	int low = 0, high = len - 1, mid;

	if (key < 0)
		return -1;

	while (low <= high) {
		mid = (low + high) / 2;
		if (key == freq_list[mid].freq)
			return mid;

		if (key < freq_list[mid].freq)
			high = mid - 1;
		else
			low = mid + 1;
	}
	return -1;
}

int kbase_platform_get_init_freq(void)
{
	return gpu_dvfs_ctx.freq_default->freq * FREQ_KHZ;
}

int kbase_platform_get_min_freq(void)
{
	return gpu_dvfs_ctx.freq_list[0].freq * FREQ_KHZ;
}

int kbase_platform_get_max_freq(void)
{
	return gpu_dvfs_ctx.freq_list[gpu_dvfs_ctx.freq_list_len - 1].freq *
	       FREQ_KHZ;
}

void kbase_platform_limit_max_freq(struct device *dev)
{
	struct device_node *hwf;
	const char *auto_efuse = NULL;

	hwf = of_find_node_by_path("/hwfeature/auto");
	if (hwf)
		auto_efuse = (char *)of_get_property(hwf, "efuse", NULL);

	if (auto_efuse && (!strcmp(auto_efuse, "T606") ||
			   !strcmp(auto_efuse, "T612") ||
			   !strcmp(auto_efuse, "T616"))) {
		dev_pm_opp_disable(dev, GPU_768M_FREQ);
		dev_pm_opp_disable(dev, GPU_850M_FREQ);
		dev_pm_opp_add(dev,
			       gpu_dvfs_ctx.freq_list[gpu_dvfs_ctx.freq_list_len - 1].freq * FREQ_KHZ,
			       gpu_dvfs_ctx.freq_list[gpu_dvfs_ctx.freq_list_len - 1].volt);
	}
}

int kbase_platform_set_freq_volt(int freq, int volt)
{
	int index;

	freq = freq / FREQ_KHZ;
	index = freq_search(gpu_dvfs_ctx.freq_list, gpu_dvfs_ctx.freq_list_len,
			    freq);
	if (index < 0)
		return 0;

	down(gpu_dvfs_ctx.sem);

	if (gpu_dvfs_ctx.gpu_power_on && gpu_dvfs_ctx.gpu_clock_on)
		regmap_update_bits(gpu_dvfs_ctx.dvfs_index_cfg.regmap_ptr,
				   gpu_dvfs_ctx.dvfs_index_cfg.args[0],
				   gpu_dvfs_ctx.dvfs_index_cfg.args[1],
				   gpu_dvfs_ctx.freq_list[index].dvfs_index);

	gpu_dvfs_ctx.cur_index = index;
	gpu_dvfs_ctx.freq_cur = &gpu_dvfs_ctx.freq_list[index];

	up(gpu_dvfs_ctx.sem);

	return 0;
}
#endif /* CONFIG_MALI_DEVFREQ */

int kbase_platform_set_DVFS_table(struct kbase_device *kbdev)
{
	return 0;
}
