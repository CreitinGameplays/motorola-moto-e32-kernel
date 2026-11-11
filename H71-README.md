# Linux 7.1-rc1 for the Motorola Moto E32 (XT2227-1, "hawaii")

This branch is the **mainline 7.1-rc1 bring-up tree** for the Moto E32
(XT2227-1, Unisoc **UMS9230** / T606, Android 11, vendor kernel 4.14).
It is based on the UMS9230 mainline snapshot in
<https://codeberg.org/ums9230-mainline/linux> (branch `ums9230`) and
carries the hardware work needed to boot the device on 7.1.

The companion **4.14 vendor tree** (the daily driver) lives on the
`4.14` branch of the same repository.

## Build

```sh
make O=$HOME/l71out ARCH=arm64 LLVM=1 -j$(nproc) Image
```

* Toolchain: Debian clang/LLSD 19.1.7 (`LLVM=1`, cross target aarch64).
* Config: `arch/arm64/configs/hawaii71_defconfig` (canonical
  `savedefconfig` output of the working configuration).
  `arch/arm64/configs/hawaii71.config` is the full `.config` of the
  tested build, for byte-exact reproduction:

  ```sh
  cp arch/arm64/configs/hawaii71.config $HOME/l71out/.config
  make O=$HOME/l71out ARCH=arm64 LLVM=1 olddefconfig
  make O=$HOME/l71out ARCH=arm64 LLVM=1 -j$(nproc) Image
  ```

## What this branch adds on top of the base snapshot

| Area | What |
| --- | --- |
| Display | `sprd` DRM port: DPU r5p0 (panel size, DPI timing, clock parent/rate), DSI host (vendor r1p0 IP, DCS init, brightness), generic MIPI panel driver, DPHY PLL, display-graph phandle fixups for the LK dtbo merge |
| Display bug fixes | DSI lane isolation must stay **off** (`0x64560034` bit0), DPI mux 6 = `dphy-333m3` for `sprd,dpi-clk-div = 6` panels, and a probe-time **DPU hard reset** (the boot-time stop handshake could wedge the shadow register bank, which is what kept the panel dark) |
| USB | musb-sprd: DT shim for the broken vendor phy binding, UTMI phy power in gadget-only mode, OTG reference clock, vddusb33 PMIC power-up over raw ADI (configfs CDC-ACM gadget enumerates, live serial shell) |
| Storage | `ufs-sprd` host port (4 KiB-sector safe), clocks/reset/syscon fixups |
| Boot policy | `HAWAII-BOOTWDT` self-reset deadline (disabled by default), raw ADI -> SC2730 PMIC reset path, panic delay, early FIQ-wdt state print, `hawaii.selfreset=` / `hawaii.panicdelay=` cmdline knobs |
| Panel log | `diagfb` early console: progress bar + status grid painted from `head.S` pre-MMU, runtime-switchable (`/proc/diagfb_log`), auto-quiet when userspace starts |
| Device mapper | `dm-default-key` port from android-4.14 to the 7.1 blk-crypto API (see the known gaps - the 7.1 fallback architecture differs) |

## Status (2026-11)

Working on 7.1:

* **Display end to end**: DPU scanout -> DSI video -> panel, 60 Hz vsync,
  clean dmesg, `/dev/fb0` (720x1600 `sprddrmfb`) painting real content.
* **USB gadget**: configfs CDC-ACM enumerates on the host and gives a
  busybox shell over the serial line (the debug channel of the whole
  bring-up).
* **UFS**: sda/sdb/sdc enumerate, 133 MB/s reads, writable.

Not working yet:

* **Touch**: Novatek controller on SPI (`compatible = "novatek,NVT-ts-spi"`,
  vendor "adaptive-touchscreen" driver, PMIC EIC irq/reset gpios).  The
  mainline `novatek-nvt-ts` driver is i2c-only - a vendor port is needed.
* **Power key / extcon**: `gpio-keys` and `extcon-gpio` defer on the PMIC
  EIC phandle (needs a PMIC regmap over ADI).
* **/data**: userdata is **metadata-encrypted** with a *keymaster-wrapped*
  key (`/metadata/vold/metadata_encryption/key/{encrypted_key,keymaster_key_blob}`),
  so without a Trusty port the key cannot be unwrapped and the partition
  cannot be mounted on 7.1.  The ported `dm-default-key` target constructs
  and maps, but 7.1 moved the blk-crypto software fallback to the
  filesystem submit path, so a dm target that attaches crypto contexts
  gets I/O errors - a 7.1-correct version needs in-target skcipher work
  (dm-crypt style).
* **Android userspace**: the stock recovery-in-boot ramdisk boots the
  first-stage init on 7.1 (ext4 dynamic partitions over dm-linear, AVB,
  APEX activation); the run then continues into the real Android init.
  The sprd vendor HALs (gralloc/HWC/camera/modem) are 4.14-era blobs and
  are not expected to work, so `adbd` is the milestone signal, not a
  working launcher.
* **Parked-boot hang**: a parked 7.1 userspace can freeze silently after
  10-30 min (kernel timers alive, no panic in the ring) - open.

## Related work (other repository)

The flashing/debugging tooling and the full bring-up log live in the
`moto-root-test` repository:

* `docs/23-7.1-flash-bringup.md` - the complete 7.1 bring-up journal
  (every test, every register-level finding).
* `scripts/host/pack_and_sign_boot.py` - packs kernel + DTB + initramfs
  into a signed Unisoc boot image (`--text-offset 0x80000`).
* `scripts/host/run_boot_b_test.ps1` - the `boot_b` test loop with
  one-attempt slot arming (the `boot_a` rescue stays intact).
* `scripts/host/logsave.c` - the parking initramfs (USB serial shell,
  ring-buffer saver into `sysdumpdb`, watchdog petter).

## Boot image contents used by the test loop

* kernel: this tree (`Image`)
* DTB: the vendor DTB with the UFS `reset-cells` fix (`dtb_surgery.py`
  in `moto-root-test`), merged with the vendor dtbo by the bootloader
* ramdisk: the parking initramfs (debug) or the stock `boot_b` ramdisk
  (Android userspace attempt)

The vendor DTB is used as-is on purpose: the kernel side resolves the
display graph and phandles by path/compatible instead of doing DTB
surgery.
