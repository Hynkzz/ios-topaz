# ios-ginkgo

**iOS 16 on the Redmi Note 8 (ginkgo).**

This branch ports [MakrSas/ios-topaz](https://github.com/MakrSas/ios-topaz) (Redmi Note 12 4G), itself based on [dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet) (Redmi Note 10 Pro), to the **Xiaomi Redmi Note 8** (`ginkgo`, Qualcomm **SM6125 "trinket"** / Snapdragon 665, 4 GB model). Many drivers follow [Floppikrob/willow-hackios](https://github.com/Floppikrob/willow-hackios) (Redmi Note 8T, same SoC).

On top of Apple's open-source XNU kernel (xnu-8796) runs the **unmodified iPhone X system, iOS 16.7.16 (20H392)**.

> Experimental. Not a daily-driver system. Installing uses the `userdata` partition and **erases Android's data**. No boot partition is written: the kernel starts from RAM with `fastboot boot`.

Detailed status, per-patch notes and the roadmap: [GINKGO.md](GINKGO.md).

## What works

| Area | State |
|---|---|
| Boot | Xiaomi ABL (unlocked) → `fastboot boot` of the shim (boot image **v2**) → XNU |
| CPU | Boot core switched from Silver to Gold `0x100` (the Silver cores have no 16K pages). `cpus=4` (SMP over PSCI); Gold at 2016 MHz |
| Memory | 3 GB at `0x80000000` (the one large free RAM block on the 4 GB model) |
| Storage | **New eMMC driver** (SDHCI 3.0, `sdhci@4744000`): card re-init, 8-bit high speed, **ADMA2** reads and writes. The iOS root is an HFS+ volume on `userdata` |
| Display | ABL framebuffer at `0x5c000000` (1080×2340), software rendering, presented to iOS as the iPhone X screen |
| UI | Boot logo, lock screen, home screen, Siri, Spotlight, keyboard |
| Buttons | Power (hold for Siri), Volume + / − |
| Touch | **Both panels.** FT8719 (Huaxing) and NT36672A (Shenchao/Tianma) over SPI (QUP0 SE2); the kernel downloads the touch firmware into the chip's RAM every boot. Needs the firmware image in the payload (see *Touch firmware*) |
| Brightness | Boot-arg `sweetbl_level=N` (the iOS slider does not work yet) |
| Clock | PMIC RTC (read only) plus a per-phone offset, boot-arg `sweetrtc_off` |
| Battery | Percentage from the PMI632 fuel gauge (voltage based); charging does not run under iOS yet |
| USB networking + SSH | CDC-ECM gadget (`sweetusb=2 sweethw=1`): phone `192.168.7.2`, live kernel log on UDP 5140, Dropbear SSH (key only) |
| `DYLD_*` / inserted libraries | The AMFI stand-in answers dyld's policy call |
| GPU (Adreno 610) | Powered, zap shader through TrustZone, own SMMU page tables, the `SweetGPU` service (msm interface), up to 745 MHz. **Mesa freedreno draws** (`gl_triangle`, OpenGL ES 3.2). The UI still renders in software |

## What does not work

- **Wi-Fi, Bluetooth, cellular, audio, camera, sensors**; GPU-accelerated UI (Mesa) is the next step.
- **Scanout flip.** MDP scanout switching has no effect on ginkgo, so `sweetfb_noflip=1` is required; console and UI share one buffer.
- **Charging under iOS** (the charger stays paused) and the iOS brightness slider.
- **microSD root** (the card stops after ACMD41/CMD2).

## Differences from topaz

| Item | topaz (Note 12 4G) | ginkgo (Note 8) |
|---|---|---|
| SoC | SM6225 "khaje" | SM6125 "trinket" |
| Gold cores | `0x100`–`0x103` | `0x100`–`0x103` (same) |
| GIC | `0xf200000` | `0xf200000` (same) |
| Framebuffer | `0x5c000000`, 1080×2400 | `0x5c000000`, **1080×2340** |
| Boot image format | v4 | **v2** |
| Kernel RAM | `0x1_40000000` + 4 GB | **`0x80000000` + 3 GB** |
| Storage | UFS (`0x4804000`) | **eMMC (`0x4744000`)** |
| Touch | FT5452, I²C on QUP0 SE2 | **FT8719 or NT36672A, SPI** on QUP0 SE2, IRQ GPIO88, reset GPIO87 |
| Buttons | PM6125 PON + GPIO5 | same |
| Display | 1080×2400 DSC | 1080×2340, video mode |

## Files of this branch

| Path | What it is |
|---|---|
| `shim/build-ginkgo.sh` | Builds `shim-ginkgo.img` (boot image v2 with the original DTB). Without an argument only the shim test; with `payload.bin` it starts XNU |
| `shim/sdhci.c` | eMMC test in the shim (PIO and ADMA2), used during the port |
| `shim/console.h`, `shim/shim.c` | `-DGINKGO`: 2340-line screen, none of topaz's UFS/IMEM/PS_HOLD writes |
| `tools/mkadt.py --ginkgo` | Apple device tree for the Note 8 (RAM, display, `sweet-sdhci` node) |
| `tools/mkpayload.py` | Kernel + ADT payload; `--touchfw` and `--gpufw` add firmware images |
| `xnu/patches/0200`–`0227` | ginkgo patches (eMMC, display, touch, USB, RTC, battery, AMFI, GPU); one line each in [GINKGO.md](GINKGO.md) |
| `.github/workflows/xnu.yml`, `dropbear.yml` | Kernel build (macOS runner) and the SSH server |

## Building

The general steps (build the kernel on GitHub Actions, build the iOS image, flash it to `userdata`) are those of the [ios-topaz README](https://github.com/MakrSas/ios-topaz/blob/topaz/README.md). What differs on ginkgo:

**Back up first.** Keep `persist`, `modemst1`, `modemst2`, `fsg`, `fsc`, `boot`, `dtbo` and `vbmeta`. The shim uses the DTB of your original `boot.img`:
```
python3 tools/unpack_bootv2.py backup/boot.img dtb/boot_unpacked
```

**Kernel:** run the `xnu.yml` workflow on the `ginkgo-sd` branch.

**Boot image:**
```
python3 tools/mkadt.py shim/out/ginkgo.adt --ginkgo
python3 tools/mkpayload.py xnu/out/kernel.development.sweet shim/out/ginkgo.adt shim/out/payload.bin \
  --touchfw ~/touch-fw/<your panel's firmware>.bin \
  --cmdline 'debug=0xc serial=3 cpus=4 keepsyms=1 -noprogress rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin ufsrw=1 rootrw=1 launchd_unsecure_cache=1 wdt=-1 sweetfb_hz=60 sweetfb_noflip=1 sweetfb_noconsole=1 sweetfb_stayon=1 sweetusb=2 sweethw=1'
cd shim && ./build-ginkgo.sh ../shim/out/payload.bin
```
`ufsrw=1 rootrw=1` are required: without them the root mounts read-only and `launchd` panics (`mount_hfs: Read-only file system`).

**Flash and boot:**
```
fastboot -S 512M flash userdata ios16-root.simg
fastboot boot shim/out/shim-ginkgo.img
```
Always flash the whole `.simg`; never patch `userdata` in place. A normal reboot goes to the Android `boot` partition, so return to iOS through fastboot.

### ginkgo boot-args

| Boot-arg | Effect |
|---|---|
| `sweetfb_noflip=1` | Required: never flip the scanout (flipping does not work on ginkgo) |
| `sweetfb_stayon=1` | Ignore the auto-lock screen blank |
| `sweetbl_level=N` | Panel brightness (DCS 0x51) |
| `sweetrtc_off=N` | Seconds added to the PMIC RTC; set the time once over ssh, the log prints the value |
| `sweetusb=2 sweethw=1` | USB networking (CDC-ECM) and the live kernel log |
| `sweetbattery=0` | Static battery instead of the PMI632 gauge |
| `sweetgpu=1` | Allow the Adreno 610 bring-up steps (`sysctl kern.sweetgpu=N`, N = 1–14) |
| `sweetgpu_smmu=1` | GPU through our own SMMU page tables instead of the untranslated context |
| `emmcdiv=N` | eMMC clock divider (default 4, at most ~50 MHz) |
| `sweettouch=0` | Touch driver off |
| `sweettouchdelay=N` | Wait N seconds before starting touch |
| `sweettouchdiag=1` | Freeze the screen with a summary if the touch driver stops |
| `sweettouchfw=0` | Do not download touch firmware even if the payload has it |

### Touch firmware

Both touch controllers sit in their boot ROM until the host downloads firmware into their RAM; on Android the Xiaomi driver does that every boot, and the kernel here does the same. Nothing is written to the chip's flash. The kernel picks the driver from the image's header.

- **FT8719 (Huaxing):** `fw_huaxing_v09` from LineageOS's ginkgo kernel (commit `8aaa002b`), converted from `.i` to binary:
  ```
  curl -LO https://raw.githubusercontent.com/LineageOS/android_kernel_xiaomi_sm6125/8aaa002bfb7ba1cac2c02dbb42ae0259ef2e47ba/drivers/input/touchscreen/ft8719_spi_c3j/include/firmware/fw_huaxing_v09.i
  python3 -c "import re; t=open('fw_huaxing_v09.i').read(); open('fw_huaxing_v09.bin','wb').write(bytes(int(x,16) for x in re.findall(r'0x([0-9a-fA-F]{1,2})',t)))"
  sha256sum fw_huaxing_v09.bin   # 7406fbcd395a3ef8dcab098377938bc3ad83096db1979798d49902926b995dfc, 131072 bytes
  ```
- **NT36672A (Shenchao/Tianma):** the phone's own `novatek_ts_*_fw.bin` from `/vendor/firmware`.

`mkpayload.py --touchfw` puts the image into the payload and the kernel finds it through the ADT (`/chosen/memory-map` `TouchFirmware`). Without it touch does not work.

### GPU firmware

The Adreno 610 bring-up needs the phone's `/vendor/firmware` files `a610_zap.mdt`, `a610_zap.b00`–`b02` and `a630_sqe.fw`. Put them in one directory and pass it with `mkpayload.py --gpufw DIR` (ADT `GPUFirmware`).

**Never commit or publish firmware images** (touch or GPU) or put them in CI artifacts: they carry no explicit license. The same goes for `*.img`, `*.simg`, `research/` and backups: large files with device or Apple data.

### Notes

- **WSL:** the stock WSL2 kernel has no HFS+. To mount the image, build the WSL kernel (`linux-msft-wsl-*`) with `CONFIG_HFSPLUS_FS=y` and point `.wslconfig` at it.

## Back to Android

TWRP or OrangeFox → *Wipe* → *Format Data*. The `persist` and `modemst*` backups can be restored with `fastboot flash`.

## Credits

Based on [ios-topaz](https://github.com/MakrSas/ios-topaz) and [ios-sweet](https://github.com/dipa-studio/ios-sweet), which did most of the work, with drivers following [willow-hackios](https://github.com/Floppikrob/willow-hackios). Uses Apple's open sources (xnu, IOHIDFamily, libpthread, hfs), [BearSSL](https://bearssl.org), [Dropbear](https://matt.ucc.asn.au/dropbear/dropbear.html), [blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build), [ipsw](https://github.com/blacktop/ipsw) and the kernel Xiaomi published for ginkgo (hardware reference). iOS belongs to Apple and is not distributed here.
