#!/bin/bash
# Build one bisection step. Usage: stage_build.sh [panic] [arm] [smc]
#   panic  0..8  panic() there (only resets from stage 8 onwards: PSCI's
#                restart handler is an arch_initcall)
#   arm    0..8, or 9 = every stage >= 2  arm the AON watchdog there and keep
#                booting.  Needs the AON clock, so it only works once the
#                clock framework is up; arm at stage 2 is therefore not a
#                reliable "did setup_arch finish" probe.
#   smc    0..8  reset through PSCI SYSTEM_RESET (SMCCC 0x84000009) at that
#                stage.  No MMIO, no clock, works at the first statement of
#                start_kernel(); the call returns if PSCI does not implement
#                it, so a normal boot after this build means "SMC unsupported".
#   paint  0|1   1 = cumulative visual trace: at every stage from 2 to 8 fill
#                another white band at the bootloader's framebuffer
#                (phys 0x9e000000, lcd_base).  The panel keeps scanning that
#                buffer while a half-booted kernel hangs, so the number of
#                white bands on screen is the last stage reached - no reset,
#                no ramoops, no console.  Combine with --entry-paint in
#                pack_and_sign_boot.py, which paints band 0 from an entry
#                stub that runs with the MMU off.
# Reuses the object tree (same KCFLAGS as the v4 build), so only init/main.c
# recompiles: seconds, not a full build.
set -eo pipefail
panic_stage="${1:-0}"
arm_stage="${2:-0}"
smc_stage="${3:-0}"
paint="${4:-0}"
screen="${5:-0}"
tag="p${panic_stage}a${arm_stage}s${smc_stage}q${paint}c${screen}"
cd "$HOME/kernel-sprd"
export PATH="$HOME/toolchains/clang+llvm-11.0.1-x86_64-linux-gnu-ubuntu-20.10/bin:$PATH"

python3 "$HOME/prepare_stage_panics.py" "$panic_stage" "$arm_stage" "$smc_stage" "$paint" "$screen"
grep -n -E '^#define DIAG_(PANIC_STAGE|ARM_STAGE|ARM_TIMEOUT|SMC_STAGE|PAINT)' include/linux/diag_stage.h
echo "arm hooks: $(grep -c 'DIAG_ARM_HOOK' init/main.c)  smc hooks: $(grep -c 'DIAG_SMC_HOOK' init/main.c)  paint hooks: $(grep -c 'DIAG_PAINT_HOOK' init/main.c)"
grep -n 'DIAG_PAINT_HOOK' init/main.c | head -9

cp .config.diagwdt .config
make -j2 ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
    CC="clang --target=aarch64-linux-gnu-" LD=ld.lld LLVM_AR=llvm-ar LLVM_NM=llvm-nm \
    KCFLAGS="-DDIAG_WDT_ARM -DDIAG_PANIC_AFTER_INIT -DDIAG_KVM_LATE_INIT -DDIAG_EARLY_PAINT" \
    Image 2>&1 | tail -12

cp arch/arm64/boot/Image "Image.$tag"
echo "--- Image.$tag ---"
ls -l "Image.$tag"
sha256sum "Image.$tag"