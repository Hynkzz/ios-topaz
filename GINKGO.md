# ginkgo / willow (Redmi Note 8 / 8T, SM6125) — status and roadmap

Branch `ginkgo-sd` of this fork. Same SoC family as the other trinket/bengal
phones: most of this (SPI touch, SDHCI, RPM votes, DSI) carries over.

## Boot

`fastboot boot shim/out/shim-ginkgo-*.img` (RAM only; nothing is flashed but
the root image). Root: iOS 16.7.16 (20H392, iPhone X userland) as HFS+ in
`userdata` (`fastboot -S 512M flash userdata ios16-root.simg`; wipes Android
data). Kernel from CI (`gh workflow run xnu.yml -f runner=macos-15-intel`).

Payload: `tools/mkpayload.py KERNEL shim/out/ginkgo-emmc.adt OUT --touchfw FW
--cmdline '... sweetfb_noflip=1 sweetfb_noconsole=1 sweetfb_stayon=1'`.
`FW` is the phone's own touch firmware (FT8719: `fw_huaxing_*.bin`; NT36672A:
`novatek_ts_{ebbg,tianma}_fw.bin` from `/vendor/firmware`). Vendor files:
never in git or CI artifacts.

## Status (2026-11-06)

| Part | State | Notes |
|---|---|---|
| Boot to SpringBoard | ✅ | lock screen, home screen, Spotlight, keyboard |
| Touch FT8719 (Huaxing) | ✅ | 0202–0206 |
| Touch NT36672A (Shenchao/Tianma) | ✅ | 0209 firmware download, 0210 edge stretch |
| Buttons | ✅ | |
| eMMC root | ✅ | |
| microSD root | ❌ | 0208: card stops after ACMD41/CMD2; log int status before line reset |
| Brightness (boot-arg) | ✅ | `sweetbl_level=N` (DCS 0x51, two bytes MSB first); iOS slider ❌ |
| RTC / date | ✅ | 0215: PM6125 RTC (read only) + boot-arg `sweetrtc_off` (per phone: set the time once over ssh, the log prints the value); also ends IOKit's 30 s IORTC wait |
| Battery % | ⚠️ | 0216: PMI632 QG voltage/current + charger state (read only), SOC estimated from voltage; the charger stays paused under iOS (no charging) |
| GPU power-up (steps 1-6) | ✅ | 0218: boot-arg `sweetgpu=1`, then `sysctl kern.sweetgpu=6`: CX and GX GDSCs on, 3D core at 19.2 MHz, RBBM_STATUS answers (0x1) |
| GPU SMMU read, TrustZone, zap shader (steps 7-9) | ✅ | 0219/0220: `sysctl kern.sweetgpu=9`; firmware from the phone's vendor image via `mkpayload.py --gpufw` (not in git); TrustZone accepts the signature and starts the zap shader in gpu_region 0x57515000 |
| GPU command processor (steps 10-12) | ✅ | 0221: `sysctl kern.sweetgpu=12`: SQE microcode, ring, GPU stream to SMMU context 4 untranslated, CP_ME_INIT, CP_SET_SECURE_MODE 0 (TRUST_CNTL 1 -> 0), CP_MEM_WRITE lands in our memory |
| GPU 2D engine to the panel (steps 13-14) | ✅ | 0222: solid fill verified in memory, then three bands drawn into the boot framebuffer (0x5c000000) |
| GPU SMMU page tables | ✅ | 0223: boot-arg `sweetgpu_smmu=1`: context 0 with our tables (4K, 39 bits); steps 12-14 pass translated, no faults |
| GPU kept on, submissions | ✅ | 0224: `sysctl kern.sweetgpu=15`: GPU stays on; command buffer in scattered pages submitted, fence waited (6 ms); `sweet_a610_*` interface for a service |
| GPU service SweetGPU | ✅ | 0225: IOKit service with the msm DRM requests as selectors; `sweetgpu=2` keeps the GPU on at boot (20 s after the service starts) |
| DYLD_* / inserted libraries | ✅ | 0217: the AMFI stand-in answers dyld's policy call |
| USB networking | ✅ | 0214, boot-args `sweetusb=2 sweethw=1`; phone 192.168.7.2, live log udp/5140 |
| SSH | ✅ | dropbear from `dropbear.yml` with `tools/dropbear/sweet_pw.c`: libinfo user lookups fail on the port, so OpenSSH rejects every user |
| CPU cores | 4 | `cpus=4`: the 16K-page kernel runs on the A73 cores only (A53 has no 16K granule — verify) |
| GPU for the UI (Mesa) | ❌ | software rendering; next: willow's RedwoodGPU user client + SMMU page tables, then Mesa freedreno |
| Wi-Fi, cellular, BT, audio, camera, sensors | ❌ | |

## Roadmap (in order)

1. **Brightness**: DSI0 command-DMA short write `15 01 .. 51 XX` (video mode,
   during blanking); boot-arg for a fixed level first, then the iOS slider
   (which IOKit property backboardd drives — find before writing).
2. **USB networking** (CDC-ECM from sweet, ginkgo DWC3 addresses) → ssh/logs.
3. RTC (PMIC RTC over SPMI, read-only), battery % (QG).
4. More cores (page-size question), cpufreq.
5. GPU (willow-hackios reference; check its licence before reusing code).
6. Wi-Fi / modem data (willow: MPSS boots, `wlan_pd` asserts).
7. microSD root again.
