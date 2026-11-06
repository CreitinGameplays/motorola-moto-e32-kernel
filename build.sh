#!/bin/bash
# Build the vendor-module-compatible kernel for the Moto E32 (hawaii):
#   base config  = the exact .config recovered from the shipping kernel
#                  (stock_kernel_config.txt in the repo root)
#   + the Mali / WLAN / BT / KVM options of the custom kernel
#   - LTO + CFI (they hang this board at boot; struct module layout is
#     restored by the vendor-module-compat patch that is already in the
#     tree)
#
# Works from any fresh clone: run it from the kernel tree root on any
# x86_64 Linux host with clang+lld 11.0.1 and the usual kernel build deps.
# Environment overrides (all optional):
#   TOOLCHAIN  dir containing clang/lld binaries (default:
#              ~/toolchains/clang+llvm-11.0.1-x86_64-linux-gnu-ubuntu-20.10;
#              if it does not exist, whatever clang/lld is on PATH is used)
#   CONFIG     the .config to start from (default: stock_kernel_config.txt
#              in the repo root)
#   OUT        build directory (default: /tmp/kbuild; note that /tmp is
#              RAM-backed on many hosts and wiped on reboot)
#   STABLE     output directory that survives reboots (default: ~/kernel-out)
#   CLEAN      1 = wipe OUT first and build from scratch (default: 0,
#              incremental: OUT is reused, the .config is regenerated
#              from $CONFIG + the tweaks and installed only when it
#              differs, so config drift cannot leak into a build)
#
# The finished build copies the boot-critical set - Image, mali_kbase.ko,
# sprdwl_ng.ko, sprdbt_tty.ko, Module.symvers, kernel.config and a
# SHA256SUMS manifest - into $STABLE.  Image and modules MUST come from
# the same build (vermagic + module_layout CRC), so always flash and
# stage from one $STABLE run.
set -e
cd "$(cd -- "$(dirname -- "$0")" && pwd)"

TOOLCHAIN="${TOOLCHAIN:-$HOME/toolchains/clang+llvm-11.0.1-x86_64-linux-gnu-ubuntu-20.10}"
if [ -x "$TOOLCHAIN/bin/clang" ]; then
    export PATH="$TOOLCHAIN/bin:$PATH"
fi

CONFIG="${CONFIG:-stock_kernel_config.txt}"
if [ ! -f "$CONFIG" ]; then
    echo "FATAL: config '$CONFIG' not found (set CONFIG=<path to a .config>)" >&2
    exit 1
fi

OUT="${OUT:-/tmp/kbuild}"
STABLE="${STABLE:-$HOME/kernel-out}"

# Incremental by default: $OUT is REUSED when it exists, so a code-only
# change relinks instead of rebuilding the world (~7 min -> ~1 min).
# The .config is always regenerated from $CONFIG + the tweaks below in
# a scratch tree and installed into $OUT only when it differs - a
# config change then rebuilds exactly the affected objects (kbuild
# handles that itself), and a stale $OUT/.config (hand-edited, or a
# from-scratch olddefconfig run against the wrong base) can never leak
# into a build.  Set CLEAN=1 to wipe $OUT first and build from scratch.
CLEAN="${CLEAN:-0}"
if [ "$CLEAN" = "1" ]; then
    echo "--- CLEAN=1: wiping $OUT (from-scratch build)"
    rm -rf "$OUT"
fi
mkdir -p "$OUT"

TMPCFG="$(mktemp -d)"
trap 'rm -rf "$TMPCFG"' EXIT
cp "$CONFIG" "$TMPCFG/.config"

S="scripts/config --file $TMPCFG/.config"
# bootability: no LTO, no CFI
$S --disable LTO --disable LTO_CLANG --enable LTO_NONE \
   --disable CFI --disable CFI_CLANG --disable CFI_CLANG_SHADOW
# in-tree Mali r28p0 DDK (module, qogirl6 glue)
$S --module MALI_MIDGARD --set-str MALI_PLATFORM_NAME qogirl6 \
   --enable MALI_REAL_HW --enable MALI_DMA_FENCE --enable MALI_GATOR_SUPPORT
# in-tree SC2355 WLAN + BT tty-sdio ports
$S --module SPRD_WLAN_COMBO --module SPRDBT_TTY
# KVM (verified feature of the custom kernel)
$S --enable VIRTUALIZATION --enable KVM --enable KVM_ARM_HOST \
   --enable KVM_ARM_PMU --enable KVM_VFIO --enable VHOST \
   --enable VHOST_NET --enable VHOST_CROSS_ENDIAN_LEGACY \
   --enable VSOCKETS --enable MMU_NOTIFIER --enable PREEMPT_NOTIFIERS

# kexec: boot another Image (+DTB) from userspace without touching flash.
# Added for the modern-kernel bring-up (docs/21): a kexec'd test kernel
# that hangs costs only a power cycle (the boot slot is never touched, no
# BROM recovery), and the next boot can read the test kernel's pstore log.
$S --enable KEXEC

M="make O=$OUT ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- CC=clang \
   LD=ld.lld LLVM_AR=llvm-ar LLVM_NM=llvm-nm OBJDUMP=llvm-objdump"

# resolve the desired config in the scratch tree, then swap it in only
# when it differs from what $OUT was built with
make O=$TMPCFG ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- CC=clang \
     LD=ld.lld LLVM_AR=llvm-ar LLVM_NM=llvm-nm OBJDUMP=llvm-objdump \
     olddefconfig
if [ ! -f "$OUT/.config" ] || ! cmp -s "$TMPCFG/.config" "$OUT/.config"; then
    echo "--- config changed (or first build): installing the new .config"
    cp "$TMPCFG/.config" "$OUT/.config"
else
    echo "--- config unchanged: incremental build in $OUT"
fi

echo "--- kernelrelease with LOCALVERSION:"
$M -s kernelrelease LOCALVERSION="+-ab612c2"
echo "--- building"
$M -j"$(nproc)" Image modules LOCALVERSION="+-ab612c2"
ls -la "$OUT/arch/arm64/boot/Image" "$OUT/Module.symvers"

# --- stable output copy (survives reboots; OUT may be on a tmpfs) ---
# The Image alone cannot boot the phone to the UI: SurfaceFlinger needs the
# mali_kbase module and WiFi needs sprdwl_ng, staged on the device as Magisk
# modules (see the analysis repo docs).  Image and .ko MUST come from the
# same build (same vermagic + module_layout CRC), so they are copied
# together, with a manifest that binds them.
rm -rf "$STABLE"
mkdir -p "$STABLE"
cp "$OUT/arch/arm64/boot/Image" "$STABLE/Image"
cp "$OUT/drivers/gpu/arm/midgard/mali_kbase.ko" "$STABLE/mali_kbase.ko"
cp "$OUT/drivers/net/wireless/sprd_wlan/sprdwl_ng.ko" "$STABLE/sprdwl_ng.ko"
cp "$OUT/drivers/misc/sprdbt_tty/sprdbt_tty.ko" "$STABLE/sprdbt_tty.ko" || true
cp "$OUT/Module.symvers" "$STABLE/Module.symvers"
cp "$OUT/.config" "$STABLE/kernel.config"
( cd "$STABLE" && sha256sum ./* > SHA256SUMS )
cat "$STABLE/SHA256SUMS"
echo "--- stable output in $STABLE (Image + modules, one build)"
echo DONE
