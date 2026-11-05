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
rm -rf "$OUT"
mkdir -p "$OUT"
cp "$CONFIG" "$OUT/.config"

S="scripts/config --file $OUT/.config"
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
$M olddefconfig
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
