# Can the R1 run a newer Linux kernel?

Evaluation done 2026-08-15 against the Ingenic SDK bundle in `~/Downloads/ingenic-hiby`
(18 GB, torrent-sourced), the stock `r1.upt`, and the live device.

## Verdict

**Harder than a recompile, much easier than it first looks.**

Ingenic ships an official X1600 SDK for kernel **6.6** (plus 5.10, alongside the 4.4.94
the R1 runs) that supports the exact reference board the R1 identifies as. Of the 28
kernel modules the R1 loads, **19 have source available** — 11 from Ingenic's SDK, 8 as
mainline drivers rebuilt out-of-tree. Only **9 small modules (~112 KB of code total)**
are genuinely missing, and the board configuration for all of them is sitting in the
clear in shell scripts inside the firmware.

The two real constraints are physical: a stock 6.6 kernel is **5.5% too big for the
kernel partition**, and RAM is already at ~1.5 MB free on a device with an OOM history.

## Correction to my first pass

My initial search concluded "essentially none of the drivers are in either SDK." That was
wrong, and wrong in a way worth recording: I searched for **built module names**
(`soc_fb`, `soc_msc`, `soc_aic`) when the SDK carries them under **upstream-style source
names** (`ingenicfb.c`, `ingenic_mmc.c`, `asoc-aic.c`). The Ingenic SoC layer does have
source, in both SDK generations. The genuinely-missing set is roughly a third of what I
first reported.

## The 28 modules, by what they actually drive

Read from each module's ELF `.modinfo` section, extracted from `r1.upt` rather than the
device (`iso_extract.py` in the session scratchpad parses the ISO9660 container and
reassembles the rootfs; md5 verified against the image manifest).

### A. Ingenic SoC drivers — source in the SDK (11)

| Module | Drives | SDK source |
|---|---|---|
| `soc_fb` | LCD controller / framebuffer | `fbdev/ingenic/fb_stage/ingenicfb.c` |
| `soc_aic` | AIC audio controller (I²S to the DAC) | `sound/soc/ingenic/as-v1/asoc-aic.c` |
| `soc_msc` | MMC/SD host — "MMC version 1.2", bo.liu@ingenic.cn | `drivers/mmc/host/ingenic_mmc.c` |
| `soc_i2c` | X1600 I²C controller | `drivers/i2c/` |
| `soc_pwm` | PWM (backlight + LEDs) | `drivers/pwm/` |
| `soc_gpio` | GPIO | `drivers/pinctrl/` |
| `soc_adc` | SoC ADC — feeds keys, headset detect, panel ID | `drivers/iio/` |
| `soc_efuse` | eFuse (chip ID / serial) | `drivers/misc/` |
| `soc_utils`, `utils` | Ingenic's out-of-tree shim layer everything depends on | SDK-wide |
| `rmem_manager` | Reserved-memory manager (framebuffer + DMA pools) | SDK |

### B. Mainline drivers rebuilt as out-of-tree modules (8)

Identifiable because they kept their original upstream authorship in `.modinfo` — these
are stock kernel drivers with an Ingenic `utils` dependency bolted on, not HiBy work.

| Module | Drives | Upstream identity |
|---|---|---|
| `axp2101` | X-Powers AXP2101 PMIC — every rail on the board | "Regulator Driver for AXP20X PMIC", Carlo Caione |
| `cw2015` | CellWise CW2015 battery fuel gauge | "CW2015 FGADC Device Driver V3.0", Chaman Qi |
| `cst8xx_touch` | Hynitron CST8xx touchscreen | "hyn Touchscreen Driver", hyn Driver Team |
| `cywdhd` | Cypress/Broadcom WiFi **and** BT power (1.4 MB, SDIO IDs `02D0:*`) | Cypress DHD |
| `keyboard_gpio_add` | GPIO buttons | "Keyboard driver for GPIOs", Phil Blundell |
| `i2c_gpio_add` | Bit-banged I²C bus | "I2C-Bus bit-banging algorithm", Simon G. Vogl |
| `leds_pwm_add` | PWM LEDs (the lock-indicator LEDs) | "PWM LED driver for PXA", Luotao Fu |
| `pwm_backlight` | Panel backlight | mainline `pwm_bl` |

6.6 has upstream equivalents for all of these, including `hynitron_cstxxx.c` for the
touch controller and `cw2015_battery.c` for the gauge.

### C. Genuinely bespoke — no source anywhere (9, ~112 KB total)

| Module | Size | Drives | Author |
|---|---|---|---|
| `codec_cs43131` | 20.6 KB | **The Cirrus CS43131 DAC** — the whole point of the device | `ringsd` (HiBy) |
| `x1600_hiby_r1_sound_card` | 6.3 KB | ASoC machine driver binding AIC ↔ CS43131 | HiBy |
| `lcd_lg35583` | 17.8 KB | LG35583 panel, SPI-bitbang init | HiBy |
| `keyboard_adc_multifunc` | 20.5 KB | ADC ladder for the side buttons | "x2000 ADC_KEYBOARD driver" (Ingenic-derived) |
| `sa_earpods_adc` | 15.0 KB | Inline-remote decode — "Earpods adc For APPLE" | leeo@**smartaction.com** |
| `sa_sound_switch` | 10.1 KB | Headphone/line-out detect and routing | leeo@smartaction.com |
| `sa_hgl_dma` | 7.8 KB | "Smartaction user dma driver" (userspace DMA for the FB) | SmartAction |
| `sa_config_module` | 7.0 KB | "SmartAction configs driver" | leeo@smartaction.com |
| `tcs1421_add` | 8.2 KB | TCS1421 USB switch | `ringsd@hiby.com` |

**New fact worth noting: "SmartAction" (`leeo@smartaction.com`) appears to be the ODM
behind the board**, distinct from HiBy themselves — four modules carry their name. Any
GPL source request probably has two addressees, not one.

## The board is fully documented in the firmware

The single most useful discovery. HiBy configures this board **entirely through module
parameters**, not a device tree — so `/module_driver/*.sh` is, in effect, the DTS in
plain text. Everything a new port needs to know about the wiring is already in hand:

```sh
# the DAC
insmod codec_cs43131.ko cs43131_i2c_bus_num=3 cs43131_pwr_gpio=PB02 \
       cs43131_pwr_en_level=1 cs43131_rst_gpio=PB21 cs43131_rst_en_level=1

# the panel (SPI-bitbanged init)
insmod lcd_lg35583.ko gpio_lcd_rst=PA31 gpio_spi_cs=PA30 gpio_spi_sck=PA00 \
       gpio_spi_mosi=PA01 spi_bus_num=5 vcc_regulator_name=bldo1 \
       vccio_regulator_name=bldo2

# touch
insmod cst8xx_touch.ko cst_i2c_bus_num=1 cst_i2c_addr=0x15 \
       cst_x_coords_max=480 cst_y_coords_max=800 \
       cst_reset_gpio=PA17 cst_irq_gpio=PA16 cst_regulator_name=aldo2

# the inline-remote thresholds (this is what BG-era remote work was reverse-engineering)
insmod sa_earpods_adc.ko sea_adc_channel=2 sea_pwr_name=aldo4 \
       sea_play_pause_min=0   sea_play_pause_max=10 \
       sea_volume_up_min=50   sea_volume_up_max=250 \
       sea_volume_down_min=350 sea_volume_down_max=550
```

Plus the complete AXP2101 rail map with voltages, the CW2015 battery model curve (64
bytes of it), and the WiFi/BT GPIO wiring. `driver_default_init_script.sh` gives the
exact load order.

**Panel timings are recoverable too.** Not in the scripts, but present as a
`fb_videomode` struct at offset `0x2488` in `lcd_lg35583.ko`: **480×800, left/right
margins 24/24, upper/lower 8/14, hsync 24, vsync 5, ~62 Hz** — which works out to a
~28.3 MHz pixel clock, internally consistent. The SPI init command sequence is in the
same binary as a register/value table.

So the classic hard part of a board port — working out how the thing is wired — is
already done. What's missing is driver *code*, for nine small modules, five of which are
thin glue.

## The two hard constraints

**1. A stock 6.6 kernel does not fit.** From Ingenic's own prebuilt demo images for this
board:

| Kernel | Image size | vs. the R1's 5,242,880-byte `mtd1` |
|---|---|---|
| 4.4.94 (halley6.v10, nand) | 4,149,334 | fits, 79% full |
| 6.6 (halley6.v20, nand) | **5,533,357** | **5.5% over — does not fit** |

+33%. Trimmable (the demo config carries cameras, GMAC and more the R1 has no use for),
but the stock build can't go into the stock layout, and the A/B scheme (`kernel` +
`kernel2`, 5 MB each) means growing the partition means redoing the map.

**2. Memory is the tightest resource here.** 56,936 kB usable of the 64 MB, **~1.5 MB
free at rest**, and this project already has an OOM-kill in its history. The 4.4.94
kernel occupies ~7.7 MB resident (code 5.6 + data 2.1, from `/proc/iomem`). Mainline has
not shrunk between 4.4 and 6.6. **A newer kernel plausibly makes this device worse.**

## What's genuinely easier than expected

- **The DAC is upstream.** Mainline's `sound/soc/codecs/cs43130.c` — present in the 6.6
  tree — lists `cirrus,cs43131` explicitly in both its `of_device_id` and `i2c_device_id`
  tables. The scariest driver has a working upstream starting point. Caveat: HiBy's own
  20 KB version may implement R1-specific behaviour (hardware volume, DSD, filter modes)
  that mainline doesn't expose identically, and that is exactly where this device's
  audio reputation lives.
- **Ingenic kept fbdev.** Their 6.6 SDK still ships `drivers/video/fbdev/ingenic/` with
  ~30 `panel-*.c` drivers rather than moving to DRM. Mainline's `ingenic-drm` covers
  X1000/X1830 but **not** X1600, so this matters a lot: `/dev/fb0` and `FBIOPAN_DISPLAY`
  — which both `hiby_player` and our Library app depend on — keep working as now. (Note
  the 6.6 fb tree is `fb_stage` plus a `fb_stage_wip` marked work-in-progress with a
  `todo/` directory, so it's less settled than 4.4.94's.)
- **Userspace survives.** Linux's syscall ABI is stable; glibc 2.22 binaries including
  `hiby_player` would run on a 6.6 kernel unchanged. Only the modules need rebuilding —
  there is no ABI compatibility for those, ever, so it's all 28 or nothing.
- ~~**exFAT comes free**~~ — **wrong, we already have it.** See "The payoff is empty"
  below.
- **Bricking is recoverable**: the bundle ships Ingenic's USB `cloner` tooling and the
  X1600 boot ROM enumerates over USB.

## Could we just rewrite the nine ourselves?

Asked directly, so answered directly, from the binaries rather than by feel.
**Short version: seven are genuinely easy, one shouldn't be rewritten at all, and one is
awkward — but only if we keep `hiby_player`.** The nine modules are not the long pole in
this project.

### Don't rewrite (1)

**`codec_cs43131`** — use mainline `cs43130.c` instead. Checked its capability against
HiBy's: mainline is a **functional superset**. HiBy's binary exposes four combined filter
names (`fast_rolloff_low_latency`, `fast_rolloff_phase_compensated`, and the slow pair);
mainline exposes the same space as two orthogonal controls — `PCM Filter Speed` and
`PCM Phase Compensation` — *plus* `Master Playback Volume`, `Master DSD Playback Volume`,
`PCM Nonoversample Emulate`, `PCM High-pass Filter`, `PCM De-emphasis Filter`,
`DSD Phase Modulation`, and a full DSD DAPM graph (83 DSD references, 2,708 lines against
HiBy's 20 KB). The worry that HiBy's driver holds secret sauce doesn't survive contact
with the binary — theirs is the simpler one.

### Easy, because the firmware already tells us everything (6)

| Module | Why it's easy | Effort |
|---|---|---|
| `x1600_hiby_r1_sound_card` | The binary names every link: card `hiby-sound-card`, CPU DAI `x1600-i2s`, platform `x1600-i2s-pcm`, codec `cs43131.3-0030` (i²c-3, addr 0x30), codec DAI `cs43131-hifi`, one control `Output Port Switch`. The 6.6 SDK ships `boards/halley6.c` as a working template. | ~50 lines, hours |
| `sa_earpods_adc` | Every threshold is in the `.sh`: channel 2, rail aldo4, play/pause 0–10, vol-up 50–250, vol-down 350–550, 10 ms poll, 500 ms hold, 200 ms repeat | hours |
| `sa_sound_switch` | Headset detect on ADC ch1, window 2800–3300, 200 ms debounce; balance/line-out all disabled (`-1`) | hours |
| `keyboard_adc_multifunc` | Full ADC ladder known (ch0, deviation 100: key1 164/165 @0, key2 114/115 @1000, key3 115/114 @1650). Mainline `adc-keys` gets most of the way; only the "reuse code" long-press behaviour is extra, and we already understand it from the app side | hours |
| `tcs1421_add` | USB Type-C DRP controller, two GPIOs (PA09/PB24), four modes (Sink/Source/StrongDRP/NormalDRP), one sysfs attribute | hours |
| `sa_config_module` | Tiny misc device (`/dev/sa-config`), one ioctl handler whose own error string is "no support other cmd" — so very few commands. Needs the ioctl numbers recovering, but **we hold both ends**: `hiby_player` is the only caller | ~1 day |

### The genuinely fiddly two

**`lcd_lg35583`** — the panel. Better than feared: `.text` is only 6,672 bytes and
`.rodata` just 256, so there is **no large init table** to recover. Timings are already
extracted (480×800, margins 24/24/8/14, hsync 24, vsync 5, ~62 Hz) and the wiring is in
the `.sh` (reset PA31, bit-banged SPI on PA30/PA00/PA01, rails bldo1/bldo2). What's
missing is the short SPI init command sequence, sitting in `.text` as immediates — the
only true disassembly job in the whole set. Ingenic ships ~30 `panel-*.c` examples
(including SPI ones like `panel-st7701s.c`) to model against. **Days, not weeks.**

**`sa_hgl_dma`** — the awkward one, and the reason to think about scope. It's a misc
device (`/dev/sa_hgl_dma`) exposing open/mmap/write, allocating contiguous memory and
driving `dmaengine_prep_interleaved_dma` — a 2D strided blitter. And
**`hiby_player` depends on it**: its own strings include `(hgl_fb: E) Access hgl dma
failed`, `Map hgl dma memory failed`, `write hgl dma error`. Rewriting it means matching
the exact `write()` payload struct that feeds the DMA template.

**But our Library app doesn't use it at all** — we mmap `/dev/fb0` and page-flip with
`FBIOPAN_DISPLAY` directly. So on a firmware that runs *our* app instead of
`hiby_player`, `sa_hgl_dma` and `sa_config_module` both become unnecessary, and the
bespoke set drops from nine to seven with the two hardest removed.

### So what actually is the long pole?

Not the drivers. Ranked honestly:

1. **Bring-up debugging.** The kernel command line says `console=ttyS2`, and nobody has
   confirmed that UART is reachable on accessible pads. Bringing up a panel and a codec
   on a board where a failed boot gives you a black screen and no log is the hard part of
   this whole exercise — far harder than writing any of the seven easy modules. Partly
   mitigated: the flash layout is A/B (`kernel`+`kernel2`, `rootfs`+`rootfs2`) and
   Ingenic's USB `cloner` recovery exists, so mistakes are recoverable rather than fatal.
2. **The 5 MB kernel partition**, which a stock 6.6 build overshoots by 5.5%.
3. **Memory**, where 6.6 makes an already-tight 64 MB device tighter.
4. **The nine modules** — which, per the above, are mostly a few days each.

And the payoff at the end is still exFAT, a newer WiFi stack, and security fixes on a
device with essentially no attack surface.

## What about 5.10 instead of 6.6?

Asked separately, and it's the right question — 5.10 nearly fixes the size problem. But
it breaks the only rationale that was still standing.

Measured the same way, from Ingenic's own prebuilt demo images for this board:

| Kernel | Image size | vs. the 5,242,880-byte partition |
|---|---|---|
| 4.4.94 (current) | 4,149,334 | fits, 79% full |
| **5.10.186** | **5,290,726** | **over by 47,846 — 0.9%** |
| 6.6 | 5,533,357 | over by 290,477 — 5.5% |

**0.9% is nothing** — dropping one unused subsystem recovers 48 KB. Where 6.6 needs real
defconfig work to fit, 5.10 essentially fits already.

And 5.10 keeps everything that made 6.6 attractive:

- **Full X1600/Halley6 support**: `halley6_v20.dts`, `halley6_v10.dts`, `x1600.dtsi`,
  `x1600-pinctrl.dtsi`, `boards/halley6.c` — and an `x1600_module_base.dts`, which is
  close to what the R1 actually reports (`x1600_halley6_module_base`).
- **The same DAC driver**: 5.10's `cs43130.c` is 2,706 lines to 6.6's 2,708, with an
  identical `of_device_id` table including `cirrus,cs43131`. No loss there.
- **A materially easier port**, if HiBy ever hands over source for the bespoke nine —
  4.4 → 5.10 is far less API churn than 4.4 → 6.6.

Two things go the other way:

- **The touch controller regresses from "have source" to "bespoke".** Mainline's
  `hynitron_cstxxx.c` landed in ~6.3, so it is *not* in 5.10. On 5.10 the CST8xx driver
  has to come from Hynitron's vendor code (obtainable, but another dependency), making it
  **ten** bespoke modules rather than nine.
- **5.10 is at end of life.** Upstream support runs out in **December 2026 — about four
  months from this evaluation.** 6.6 is supported to December 2029.

That last point is decisive, because of *what survives* as a reason to upgrade at all.
Both user-visible benefits evaporated (below): exFAT is already present and the WiFi is
capped by its radio. The **only** remaining rationale was ongoing security fixes — and
porting onto a kernel that stops receiving them in four months defeats exactly that.

**So: 5.10 is the easier target and the wrong one.** If this were ever done, 6.6 is
correct despite the extra 240 KB of trimming, because you do not undertake a multi-week
driver port onto an LTS branch that expires before you'd finish it.

## The payoff is empty — both claimed wins checked and neither survives

I originally listed exFAT and a newer WiFi stack as the benefits. Challenged on both,
I checked, and **both were wrong**.

**exFAT: the R1 already has it.** The kernel is a `uImage` whose header claims "Not
compressed", which is misleading — the payload is a self-decompressing image (entropy
7.86 bits/byte, gzip at offset 16992). Decompressed to 8,143,180 bytes, the filesystem
list is unambiguous:

```
exfat  ✓    vfat  ✓    msdos  ✓    ntfs  ✓    fuse  ✓
squashfs ✓  ubifs ✓    iso9660 ✓
ext2/3/4 ✗  f2fs ✗     udf ✗
```

Not a stub either — it's a full backported driver, with its own inode cache, sysfs
version attributes, read-only fallback on error, and proper `exFAT-fs (%s[%d:%d])`
logging. Nothing to gain here.

*(Method note for anyone rechecking: `strings` on the raw `xImage` finds none of these —
it only sees the decompressor stub. The payload must be decompressed first. My first
pass reported "no filesystems at all", including `squashfs`, which is impossible for a
device whose rootfs is squashfs — that impossibility is what exposed the bad method.)*

**WiFi: capped by the radio, and WPA3 is already there.** The firmware blobs name the
part — `cyw43438-7.46.58.35.bin`, `fw_bcm43438a1.bin`. **BCM43438** is single-band
**2.4 GHz 802.11b/g/n, 1×1** (the Raspberry Pi 3 / Zero W part). No 5 GHz, no 802.11ac,
~72 Mbps PHY ceiling. **No kernel driver can change any of that** — it's silicon.

And the one thing a stack upgrade might plausibly have bought, WPA3, is already present
and lives in *userspace* anyway: `/usr/sbin/wpa_supplicant` is **v2.9**, with SAE and OWE
compiled in. If a WPA3-only network were ever the problem, the fix is a userspace
binary, not a kernel.

So the realistic gain from moving to 6.6 is: driver security fixes, on a device whose
only untrusted input is podcast RSS and internet radio, and which has no exposed
services. Against that: a kernel that doesn't fit the partition, and less free RAM on a
device that already OOMs.

**The payoff column is empty.** Not thin — empty. That changes the recommendation from
"probably not worth it" to "don't, unless the goal is the exercise itself."

## Recommendation

The feasibility question and the value question have opposite answers, so keep them
apart.

**Feasibility: better than expected.** 19 of 28 modules have source, the board wiring is
fully documented in the firmware's own insmod scripts, panel timings are recovered, the
DAC is handled by a more capable mainline driver, and seven of the nine bespoke modules
are hours-to-days of work each. If someone wants to do this as a project, it is a real,
tractable project — and dropping `hiby_player` in favour of our own app removes the two
hardest modules outright.

**Value: nil.** Both claimed benefits evaporated on inspection. Nothing this project has
wanted to fix was kernel-limited — BG44, the worst bug of the session (93% of a core,
continuous audio dropouts), was pure userspace arithmetic, fixed for a 23.5× win with the
kernel never entering into it.

If a GPL source request to HiBy and SmartAction is cheap to send, send it — it costs
nothing and it keeps the door open. But there is no reason to walk through that door
today.

### Superseded: the original recommendation

Kept for the record, since it was written before the payoff claims were checked. **Request GPL source
for the nine modules in group C** — from HiBy (`ringsd@hiby.com` is a named author) and
from SmartAction, who wrote four of them. That's a specific, small, provably-GPL list
rather than a vague request for "the source", which makes it far likelier to succeed.

With those nine, this becomes a normal port: 19 modules already have source, the board
wiring is fully documented, the panel timings are recovered, and the SoC is a supported
reference design. The remaining work is API churn (4.4 → 6.6 is ten years of
clk/regulator/ASoC change) plus shrinking a defconfig by ~300 KB.

Without them, group C has to be rewritten from disassembly — and while five of the nine
are small glue, `codec_cs43131` is the one that decides whether the device still sounds
like an R1.

**And the honest counterweight:** nothing this project has wanted to fix was
kernel-limited. BG44 — the worst bug found this session, 93% of a core and continuous
audio dropouts — was pure userspace arithmetic on a soft-float core, fixed for a 23.5×
win with the kernel never entering into it. A kernel upgrade buys exFAT and a newer WiFi
stack, at the cost of memory this device does not have.
