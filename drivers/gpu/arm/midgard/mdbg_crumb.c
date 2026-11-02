// mdbg_crumb.c - flash-persistent breadcrumb recorder for the Mali probe.
//
// The module probe dies with the CPU: no console tail, no pstore, page
// cache gone on the cold reset. This driver-side writer puts each
// breadcrumb DIRECTLY on flash at miscdata+0xA0000 (a verified zero
// scratch area of the production-data partition, see
// scripts/device/write_crumb.sh), so the trail survives any reset.
//
// Layout: "CRUMB01" then one byte per step in order:
//   0x00 magic[7]  ("CRUMB01")   written at registration
//   0x07..        step bytes, one per mdbg_crumb(n) call, in call order
//
// It uses kernel_write() on the block device - the same path adb's dd
// uses - so writes hit flash even when everything else is dead, and it
// fsyncs the file each time. UFS: zeroing later erases.
//
// Why a separate module and not inside mali_kbase: the breadcrumbs must
// exist from the very first instruction of mali's module init; a tiny
// module with zero dependencies loads first, and mali_kbase calls into it
// via mdbg_crumb() exported below. If THIS module fails to load, the
// flash trail says so (magic missing entirely).

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#define CRUMB_MAGIC "CRUMB01"
#define CRUMB_MAGIC_LEN 7
#define CRUMB_MAX 64

static struct file *crumb_filp;
static u8 crumb_buf[CRUMB_MAGIC_LEN + CRUMB_MAX];
static int crumb_idx;

static long crumb_off = 0xA0000;
module_param(crumb_off, long, 0444);
MODULE_PARM_DESC(crumb_off, "miscdata byte offset for the crumb area");

static int __init mdbg_crumb_init(void)
{
	ssize_t n;
	loff_t pos = crumb_off;

	crumb_filp = filp_open("/dev/block/by-name/miscdata",
			      O_RDWR | O_LARGEFILE, 0);
	if (IS_ERR(crumb_filp)) {
		pr_err("MDBG crumb: cannot open miscdata: %ld\n",
		       PTR_ERR(crumb_filp));
		crumb_filp = NULL;
		return 0; /* not fatal for the experiment */
	}

	memset(crumb_buf, 0, sizeof(crumb_buf));
	memcpy(crumb_buf, CRUMB_MAGIC, CRUMB_MAGIC_LEN);
	crumb_idx = 0;

	n = kernel_write(crumb_filp, crumb_buf, sizeof(crumb_buf), &pos);
	vfs_fsync(crumb_filp, 0);
	pr_info("MDBG crumb: armed at miscdata+%#llx (%zd bytes)\n",
		(unsigned long long)crumb_off, n);
	return 0;
}

static void __exit mdbg_crumb_exit(void)
{
	if (crumb_filp)
		filp_close(crumb_filp, NULL);
}

/**
 * mdbg_crumb - record breadcrumb step @step on flash.
 * Called by the instrumented mali_kbase at every glue/DDK boundary.
 * Safe against everything except a CPU stall *during* the write itself;
 * each call writes just one byte, so the window is tiny.
 */
void mdbg_crumb(int step)
{
	ssize_t n;
	loff_t off;
	if (!crumb_filp || crumb_idx >= CRUMB_MAX)
		return;

	crumb_buf[CRUMB_MAGIC_LEN + crumb_idx] = (u8)step;
	crumb_idx++;
	off = crumb_off + CRUMB_MAGIC_LEN + crumb_idx - 1;
	n = kernel_write(crumb_filp,
			 crumb_buf + CRUMB_MAGIC_LEN + crumb_idx - 1, 1, &off);
	vfs_fsync(crumb_filp, 0);
	pr_info("MDBG crumb: step %d written (%zd)\n", step, n);
}
EXPORT_SYMBOL_GPL(mdbg_crumb);

module_init(mdbg_crumb_init);
module_exit(mdbg_crumb_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("flash-persistent breadcrumb recorder for hang probes");
