#!/bin/bash
# Build the vendor-module-compatible kernel:
#   base config  = the exact .config recovered from the shipping kernel
#                  (artifacts/kernel-build/stock_kernel_config.txt)
#   + the Mali / WLAN / BT / KVM options of the custom kernel
#   - LTO + CFI (they hang this board at boot; struct module layout is
#     restored by the apply_vendor_module_compat.py patch instead)
# Output goes to /tmp/kbuild.  Run inside the kernel source tree.
set -e
cd ~/kernel-sprd
export PATH=~/toolchains/clang+llvm-11.0.1-x86_64-linux-gnu-ubuntu-20.10/bin:$PATH
OUT=/tmp/kbuild
rm -rf "$OUT"
mkdir -p "$OUT"
cp ~/stock_kernel_config.txt "$OUT/.config"
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
M="make O=$OUT ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- CC=clang \
   LD=ld.lld LLVM_AR=llvm-ar LLVM_NM=llvm-nm OBJDUMP=llvm-objdump"
$M olddefconfig
echo "--- kernelrelease with LOCALVERSION:"
$M -s kernelrelease LOCALVERSION="+-ab612c2"
echo "--- building"
$M -j2 Image modules LOCALVERSION="+-ab612c2"
ls -la "$OUT/arch/arm64/boot/Image" "$OUT/Module.symvers"


# --- stable output copy (survives reboots; /tmp is RAM-backed) ---
# The Image alone cannot boot the phone to the UI: SurfaceFlinger needs the
# mali_kbase module and WiFi needs sprdwl_ng, staged on the device as Magisk
# modules (docs/18 sec. 6).  Image and .ko MUST come from the same build
# (same vermagic + module_layout CRC), so they are copied together, with a
# manifest that binds them.
STABLE=~/kernel-out
rm -rf "$STABLE"
mkdir -p "$STABLE"
cp "$OUT/arch/arm64/boot/Image" "$STABLE/Image"
cp "$OUT/drivers/gpu/arm/midgard/mali_kbase.ko" "$STABLE/mali_kbase.ko"
cp "$OUT/drivers/net/wireless/sprd_wlan/sprdwl_ng.ko" "$STABLE/sprdwl_ng.ko"
cp "$OUT/drivers/misc/sprdbt_tty/sprdbt_tty.ko" "$STABLE/sprdbt_tty.ko" || true
cp "$OUT/Module.symvers" "$STABLE/Module.symvers"
cp "$OUT/.config" "$STABLE/kernel.config"
sha256sum "$STABLE"/* | sed "s|$STABLE/||" > "$STABLE/SHA256SUMS"
cat "$STABLE/SHA256SUMS"
echo "--- stable output in $STABLE (Image + modules, one build)"
echo DONE
