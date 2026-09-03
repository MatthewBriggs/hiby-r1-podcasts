# Optimising the current 4.4.94 kernel

Direction: **rebuild 4.4.94 from Ingenic's SDK and keep HiBy's existing prebuilt modules.**
Evaluated 2026-08-15. Unlike the newer-kernel idea (see
[NOTES-newer-kernel-feasibility.md](NOTES-newer-kernel-feasibility.md), W6), this one has
a real payoff, because RAM is the binding constraint on this device and the kernel image
is the single largest consumer of it.

## The plan is viable, and one finding is why

**`CONFIG_MODVERSIONS` is OFF.** None of the 28 `.ko` files carries a `__versions`
section. That means module loading checks only `vermagic` — not a per-symbol CRC for
every imported function. Had MODVERSIONS been on, our rebuilt kernel would have needed
byte-identical symbol CRCs to HiBy's, which in practice means their exact source and
config, which we don't have. With it off, **any kernel that matches vermagic and still
exports the right symbols will load these modules.**

The second finding sets the boundary: the 28 modules import **431 distinct kernel
symbols** between them (`tools/ko_modsyms.py`, output kept as `kernel_needed_syms.txt`).
That's a tiny surface — a kernel exports 10,000+ — and it's the hard floor. Everything
outside it is potentially removable.

By subsystem, what those 431 pin down as mandatory:

```
MMC/SDIO 30   ALSA-ASoC 23   string/printk 21   regulator 15   i2c 13
timer/rtc 12  workqueue 12   gpio/pinctrl 11    of/DT 10       net(cfg80211) 10
mem/slab 10   power/backlight 9  procfs/sysfs 8  input 7   locking 7
framebuffer 5  DMA engine 4   ... plus regmap-i2c, USB
```

## Hard constraints — get these wrong and nothing loads

The vermagic string is exactly:

```
4.4.94+ preempt mod_unload MIPS32_R2 32BIT
```

which locks the config in four ways:

| Field | Meaning | Must be |
|---|---|---|
| `4.4.94+` | version + the `+` `setlocalversion` appends | reproduce the `+` exactly |
| `preempt` | `CONFIG_PREEMPT=y` | **cannot** switch to `PREEMPT_NONE`/`VOLUNTARY` |
| `mod_unload` | `CONFIG_MODULE_UNLOAD=y` | keep on |
| *(no `SMP`)* | uniprocessor build | `CONFIG_SMP=n` |
| `MIPS32_R2 32BIT` | architecture | unchanged |

Note the second row: **tuning the preemption model is off the table**, because it would
change vermagic and orphan every module.

`CONFIG_MODVERSIONS` must also stay **off** — turning it on would make the kernel demand
`__versions` sections these modules don't have.

**The subtler hazard is struct layout.** vermagic catches SMP and preempt, but *not*
options that silently change the size of structures the modules embed. Do not touch:

- any `CONFIG_DEBUG_*` that is currently off — `DEBUG_SPINLOCK`, `DEBUG_MUTEXES`,
  `DEBUG_LOCK_ALLOC`/`LOCKDEP`, `DEBUG_LIST`, `SLUB_DEBUG_ON` all change core struct sizes
- `CONFIG_TRACING`/`FTRACE`
- **`CONFIG_PM`/`CONFIG_PM_SLEEP`** — these change `struct device`, and several modules
  register suspend/resume callbacks (`sa_config_suspend`, `tcs1421_suspend`, …)

Rule of thumb: **only change leaf options** — individual drivers, filesystems, network
protocols. Never core, debug, or PM options.

## Where the RAM actually goes (measured)

From `/proc/iomem` and `/proc/meminfo` on the live device:

| | Size | Notes |
|---|---|---|
| RAM fitted | 64.0 MB | `mem=64M@0x0` |
| MemTotal | 55.6 MB | what Linux manages |
| **Reserved / kernel image** | **8.4 MB** | the gap — *this is what trimming returns* |
| — kernel code | 5.49 MB | `00010000–0058c707` |
| — kernel data | 2.08 MB | `0058c708–007a1fff` |
| hgl DMA pool | 6.00 MB | `sahd_hgl_mem_size=6291456`, runtime alloc |
| framebuffer | 1.46 MB | 480×800×16bpp × `frame_num=2` |
| **MemFree at rest** | **~1.5 MB** | and we have OOM-killed before |

Two independent levers, and they're worth doing in this order.

## Lever 1 — no rebuild needed, do this first

These are module parameters in `/module_driver/*.sh`. Free, reversible, testable in
minutes, and they don't require solving anything above.

**The 6 MB hgl DMA pool is the single biggest number on the page.** `sa_hgl_dma.sh` sets
`sahd_hgl_mem_size=6291456` — six megabytes, on a device with 1.5 MB free, for a screen
whose entire framebuffer is 768 KB. That's eight screens' worth of scratch. It is very
likely oversized, and the failure mode if you cut too far is loud and harmless:
`hiby_player` logs `(hgl_fb: E) Map hgl dma memory failed`.

**And note what this becomes later**: our Library app's own code never opens
`/dev/sa_hgl_dma` — it mmaps `/dev/fb0` and page-flips directly. But our app runs as a
*hook inside* `hiby_player`, and `hiby_player` holds that fd, so on current firmware the
6 MB is allocated whenever our app runs too. Only a firmware that replaces `hiby_player`
outright would recover it.

### Attempted 2026-08-15 — inconclusive, and why

Measured, and solid:

- The 6 MB is real. Pool open → `MemAvailable` **19,840 kB**; pool closed →
  **26,336 kB**. A **~6.2 MB delta**, reproduced across several boots.
- The holder is `hiby_player` itself (the boot instance, PID 963 in the test), which
  opens the device **once at startup and holds it for its whole lifetime** — including
  while our Library app is in front (verified: `count=1` with our Main Menu on screen).

What failed, and it's a trap worth recording:

**`killall hiby_player` cannot be used to re-test this.** After a restart the player
*never* reopens the pool — the fd count stays 0 permanently. Almost certainly the known
graphics-DMA leak: the old channel is never released, so `dma_request_channel` fails on
reopen (one of the module's own error strings) and `hgl_fb` silently falls back. So the
restart path **structurally cannot allocate the pool at any size**, which is why no
`hgl` error ever appeared in the log — 3 MB was never actually exercised. Repeated
restarts then wedged the UI, the same lockup this dev loop produces generally; a reboot
cleared it and nothing persisted, since the parameter lives only in sysfs.

**So the value must be set before `hiby_player` starts at boot.** Two ways, both needing
a flash: edit `sahd_hgl_mem_size` in `sa_hgl_dma.sh` in the firmware image, or add an
init script that writes the sysfs value between `S11module_driver_default` and
`S92_03_start_music_player` — the same injection mechanism `patch_firmware.py` already
uses for `S90adb`. Until then, **3 MB remains untested, not validated**.

## Lever 2 — the rebuild, and what to cut

Confirmed compiled in, and confirmed **unreachable from this device's userspace**:

| Subsystem | Evidence it's dead | Rough text saving |
|---|---|---|
| **mac80211** | The WiFi driver `cywdhd` is **FullMAC** — the modules import 26 `cfg80211_*`/`ieee80211_*` symbols and **zero** mac80211-only ones (no `ieee80211_rx`, `_alloc_hw`, `_register_hw`). Yet minstrel rate-control, mesh paths and IBSS are all present in the image | ~300–400 KB |
| **NFS / CIFS / SMB** | 251 string hits, and **no** `mount.nfs`, `mount.cifs`, `smbd` or `nmbd` anywhere in the rootfs, and no `/etc` references | ~400–600 KB |
| **netfilter / iptables** | 117 hits, and no `iptables`/`ip6tables`/`nft` binary exists | ~150–250 KB |
| **IPv6** | present; nothing in userspace configures it | ~150–250 KB |
| **NTFS** | 83 hits. *Judgement call* — plausibly wanted for USB drives, though the R1's USB is device-mode | ~100 KB |

**Keep, definitely**: cfg80211 (WiFi needs it), the in-kernel Bluetooth stack (89 hits —
bluealsa and `hci_uart` depend on it), USB gadget (adb/MTP), exFAT/vfat/NTFS-if-kept,
squashfs, ubifs.

**Honest caveat on those savings**: they are *estimates from typical MIPS build sizes*,
not measurements. I tried to recover `kallsyms` from the decompressed image to size each
subsystem exactly, and the extraction failed — the 4.4 MIPS layout doesn't match the
token-table heuristic I wrote. The only way to get real numbers is to build it twice and
diff. What *is* measured is that all five are present and that four of the five have no
userspace consumer at all.

Plausible total: **~1–1.5 MB off a 5.49 MB text segment**, i.e. roughly a 20% kernel
shrink, returned directly to `MemTotal` — against 1.5 MB of current headroom. That would
roughly double the free memory on the device.

## Performance

Less to win here than on RAM, and worth saying plainly:

- **Preemption model is locked** by vermagic, so the usual first lever is unavailable.
- **CPU frequency scaling is effectively absent** — `cpufreq` appears twice in the image
  but there are no governors (`ondemand`, `conservative`, `governor`, `cpuidle` all zero
  hits). The core appears to run at a fixed clock. That's already the fast choice; it
  costs battery, not speed.
- **The I/O tunables are already done** — `read_ahead_kb` and `vfs_cache_pressure` were
  ported from bidhata's work and are in `INSERT_CONFIG` in `patch_firmware.py`.
- **A smaller kernel is itself a performance win** on this class of chip: less text means
  better I-cache behaviour, and this CPU is in-order with a small cache.
- **Build flags** are worth an experiment: if the SDK defaults to `-Os`, trying `-O2` for
  the kernel is the same trade `build.sh` already documents for our decoder, and for the
  same reason.

The honest framing: this project's measured wins have all been userspace algorithmic ones
(BG44's 23.5×), and the kernel is not where the CPU time goes.

## De-risking

This is much safer than it sounds:

1. **Module loading fails cleanly.** A vermagic or symbol mismatch makes `insmod` print an
   error and refuse — it doesn't corrupt anything. You find out immediately.
2. **The flash layout is A/B** — `kernel`+`kernel2`, `rootfs`+`rootfs2`. Flash the new
   kernel to the spare slot.
3. **USB recovery exists** — the X1600 boot ROM enumerates over USB and Ingenic's
   `cloner` tooling is in the SDK bundle, so a bad image is recoverable.
4. **Start from the SDK's own `halley6` defconfig** (the 4.4.94 v6.0 drop is the closest
   match to what HiBy forked), confirm vermagic matches *before* changing anything, and
   only then start removing leaf options — one subsystem per build, so a regression is
   attributable.

## Suggested order

1. Shrink `sahd_hgl_mem_size` and measure. No build required, biggest single number.
2. Build a stock SDK 4.4.94 halley6 kernel; confirm vermagic string matches exactly;
   confirm all 28 modules `insmod` and the device boots. **This is the milestone** — it
   proves the whole approach before any optimisation work.
3. Then trim, one subsystem per build, in descending order of confidence:
   mac80211 → NFS/CIFS → netfilter → IPv6.
4. Re-measure `MemFree` at rest after each.

## Attempted 2026-08-20 — built, vermagic matched, still bricked on first boot

Built from the SDK's own `x1600_halley6_module_base_linux_sfc_nand_defconfig`,
two leaf-level differences from stock: `CONFIG_NFS_FS`/`CONFIG_CIFS` off,
`CONFIG_CC_OPTIMIZE_FOR_SIZE` off (`-O2` instead of `-Os`). Confirmed clean
against every hard constraint this file documents — `CONFIG_PREEMPT=y`, no
`SMP`, no `MODVERSIONS`, `PM`/`PM_SLEEP` untouched, all the `DEBUG_*`/
`TRACING`/`FTRACE` options off, matching stock. Built a trivial out-of-tree
test module against the finished kernel and diffed its `vermagic` against a
real stock `.ko` (`codec_cs43131.ko`) byte-for-byte: identical, `4.4.94+
preempt mod_unload MIPS32_R2 32BIT`. Also caught and fixed a real mismatch
before flashing: the standard `make uImage` target uses a fixed generic
load address (`0x80010000`), not the board's own
`CONFIG_XIMAGE_LDADDR=0x80F00000` from the defconfig — Ingenic's SDK has
its own `make xImage` target (`arch/mips/boot/zcompressed/Makefile`) that
uses the right one, confirmed matching stock exactly (`Load Address:
80f00000`) before repacking.

**Flashed anyway, and the device didn't come up — black screen, completely
unresponsive to the power button.** Recovered via the documented failsafe
(swap the SD card's `r1.upt` back to a known-good stock-kernel build, then
hold Volume Up + Power until the HIBY logo appears) — no data loss, no
lasting damage, the A/B-adjacent recovery path this file's own "de-risking"
section counted on worked exactly as expected.

**Root cause not yet isolated, and the two config differences are not the
leading suspect.** The build that actually succeeded used a *substitute*
toolchain, not Ingenic's own: every attempt with the SDK's real
`prebuilts/toolchains/mips-gcc720-glibc229` (gcc 7.2.0, x86_64-hosted)
crashed with a segfault inside `as`/`cc1` on a different, effectively
random file each time — tried under QEMU usermode emulation (Docker's
default cross-arch path on Apple Silicon), a real x86_64 full-system VM
(`colima --arch x86_64`, TCG), multiple parallelism levels (`-j4`/`-j2`/
`-j1`) and VM memory sizes (8/10/12 GB) — all failed the same way. That
pattern (different file each run, both under usermode *and* full-system
emulation, unaffected by parallelism or memory) looks like host-side
emulation instability with this specific old toolchain binary rather than
a resource limit. The build that finally worked switched to Debian's
`gcc-mipsel-linux-gnu` (10.2.1), which runs natively on arm64 — no
emulation needed at all, since the kernel doesn't link against target
glibc regardless of which cross-gcc compiled it. Vermagic and the
requested config diff both matched exactly, but a different compiler
version *could* still miscompile board-specific hardware bring-up code
(clock tree, DDR/memory controller, timing-sensitive init) in a way that
hangs before any console output ever appears, which fits the symptom
better than either of the two Kconfig changes (neither touches boot-path
code at all).

**Not yet tried, worth trying first before touching the two config
differences again**: get Ingenic's own toolchain running reliably instead
of substituting Debian's. Rosetta 2 (Apple's own x86_64-on-arm64
translator, not QEMU's software TCG) is a plausible fix for the emulation
instability seen above, if Docker/colima can be made to use it instead of
QEMU for this workload. If a build with Ingenic's real toolchain succeeds
and boots, that isolates the compiler substitution as the actual cause. If
it still fails to boot even with the right toolchain, the two config
differences become suspects after all, and the next step is a build with
neither one changed at all — the "stock rebuild, no differences yet"
milestone this file already called out as step 2 above, never actually
completed on a byte-for-byte-original toolchain.

### Attempt 2, same day — Rosetta fixed the toolchain, the device still didn't boot

Rosetta did solve the toolchain instability completely: `colima start
--vz-rosetta`, then a `--platform linux/amd64` container, runs Ingenic's
own x86_64 `mips-linux-gnu-gcc` 7.2.0 with **zero** segfaults where both
QEMU usermode and a TCG full-system x86_64 VM had failed on a random file
every time. That part of the theory was right, and it's the setup to keep.

The rebuilt kernel (Ingenic's real 7.2.0, same four config changes as
attempt 1, `xImage` target, load address again confirmed `0x80F00000`,
3,682,368 bytes → `xImage-r1-ingenic-toolchain`, packed as
`hiby-r1-kernel-r1b.upt`) **flashed cleanly but hung at the HiBy logo** —
recovered the same way (SD swap + Volume Up + Power).

**That's a different symptom from attempt 1's black screen, and the
difference is the useful part.** The logo is drawn before the kernel takes
over, so attempt 2 got measurably further than attempt 1 — consistent with
the compiler substitution having been a real defect that Rosetta fixed, and
with a *second*, separate reason the kernel still can't bring the board up.
It also means the compiler is no longer a live suspect for what remains.

Since attempts 1 and 2 differ in *both* compiler and config, neither one
isolates anything on its own. Two candidates are left, and only one
experiment separates them:

- the four Kconfig changes (`NFS_FS`/`CIFS`/`NETFILTER`/
  `CC_OPTIMIZE_FOR_SIZE`), or
- the public SDK tree simply not being what HiBy actually ships from —
  a stock `halley6` reference-board defconfig is not the R1, and any
  vendor board patches (PMIC sequencing, panel/backlight bring-up,
  bootargs, DDR timings) would be absent from a public SDK drop.

**Next: the zero-change control build** — pristine
`x1600_halley6_module_base_linux_sfc_nand_defconfig`, no edits whatsoever,
on Ingenic's own toolchain under Rosetta. This is step 2 of "Suggested
order" above, finally run properly, and it's decisive either way: if it
boots, the config changes are the cause and can be reintroduced one at a
time; if it *also* hangs, the SDK tree cannot produce a bootable R1 kernel
at all and no amount of config work will change that — which retires the
whole custom-kernel idea rather than leaving it to be re-attempted.

Build-environment note for whoever runs this next: build on
**container-local storage, not the virtiofs bind mount**. `make mrproper`
alone did not finish a delete pass in 10 minutes over virtiofs; copying
the 1 GB kernel tree and 1 GB toolchain into the container first and
building there is far faster overall than building on the mount.

### Attempt 3 (2026-08-22) — the zero-change control, built clean, NOT yet flashed

Built exactly as prescribed above: pristine
`x1600_halley6_module_base_linux_sfc_nand_defconfig`, no edits at all,
Ingenic's own gcc 7.2.0 under Rosetta, `xImage` target, on container-local
disk. Script kept at `~/Git/hiby-kernel-build/kbuild-vanilla.sh`; output
`~/Git/hiby-kernel-build/xImage-r1-vanilla`, config saved alongside as
`config-vanilla-control.txt`.

Verified before considering a flash: `Image Name: Linux-4.4.94+`,
`Load Address: 80f00000`, `Entry Point: 80f00000`, and every
vermagic-determining option matching stock (`CONFIG_PREEMPT=y`,
`CONFIG_MODULE_UNLOAD=y`, `CONFIG_CPU_MIPS32_R2=y`, `CONFIG_32BIT=y`,
`MODVERSIONS`/`SMP` off) — i.e. the same
`4.4.94+ preempt mod_unload MIPS32_R2 32BIT` the 28 closed-source modules
require. Ingenic's toolchain again ran flawlessly under Rosetta.

**Correction to the record above: attempts 1 and 2 changed only TWO
things, not four.** Diffing the pristine defconfig proves
`CONFIG_CC_OPTIMIZE_FOR_SIZE` and `CONFIG_CIFS` are *already* off in it,
so those two "changes" were no-ops that never entered any build. The real
deltas were only `CONFIG_NETFILTER=y → off` and `CONFIG_NFS_FS=y → off`
(plus their dependents: `NET_INGRESS`, `NFS_COMMON`, `ROOT_NFS`,
`NFS_SWAP`). So the `-Os`→`-O2` change that was a stated goal of this
whole exercise **never actually happened** — the defconfig builds `-Os`
already, and flipping it is still untried.

**A new signal worth weighing before flashing anything:** this
zero-change build is **3,891,264 bytes against stock's 3,731,520** —
160 KB *larger* than what HiBy actually ships, despite being the
"reference" config. (Attempt 2, with netfilter+NFS removed, came to
3,682,368.) HiBy's shipped kernel is therefore demonstrably not this
defconfig — it's a different, somewhat slimmer configuration. That is
consistent with, though not proof of, the vendor-patches theory: the
public SDK's halley6 reference config is a near neighbour of the R1's
kernel, not the thing itself.

**Deliberately not flashed at build time.** Each of the two previous
flashes cost a physical SD-card pull plus the Volume Up + Power failsafe
to recover, and this build is purely diagnostic — it contains no
improvement whatsoever, so the only thing flashing buys is the answer to
"can this tree boot the R1 at all". Worth paying that price once,
knowingly, rather than by habit.

Packaged on request as
`~/Git/hiby-kernel-build/hiby-r1-kernel-vanilla.upt` (41,492,480 bytes),
against the pristine stock OTA_V0 base `~/Downloads/r1.upt`, with the
usual rootfs patch set (DEV_HOOK wiring, noatime, BT HFP, S90adb, radio,
About) and `usr/resource/kernel_build_id` stamped `4.4.94_vanilla` — so
the About screen identifies which kernel is actually running, which is
the whole point when the two candidates look identical from userspace.

`verify_firmware.py` passes: both images' sizes, manifest md5s and every
chunk digest (8 kernel + 72 rootfs) OK. Structure checked explicitly
against the stock base as this file's trap section demands —
Joliet True/True, Rock Ridge True/True, first-entry extent 46 == 46,
entry count 84 == 84. Structurally this is as close to stock as the
previous *successful* flashes were, so a failure to boot would be
attributable to the kernel itself rather than to image packaging.

**Correction worth keeping — a second `.upt` on the card does NOT avoid
the card pull.** `hiby_player` builds the update path as the format
string `%s/sd_0/%s.upt`, i.e. one fixed filename derived from the model,
not a scan of the directory — and the Volume Up + Power failsafe reads
that same name. So at the moment of a bad flash the broken image *is*
`r1.upt`, and with a black screen there is no ADB to rename anything.
Keeping a known-good image on the card is still worth doing, but only
because it turns recovery into a **rename on the Mac** rather than a
41 MB copy over a card reader — it does not make recovery cardless.

Staged 2026-08-22, both md5-verified on the card after `sync`:
`r1.upt` = the vanilla control (`0db31b67…`, 41,492,480 B) and
`recovery-0.29.upt` = the known-good stock-kernel build (`b3f1cc72…`,
41,332,736 B). 0.29 chosen over newer staged releases deliberately: it is
the image that actually recovered this device twice on 2026-08-20, and
for a recovery file "proven to boot on this hardware" beats "newer".

**Result: froze at the HiBy logo, same as attempt 2.** That is the
decisive outcome the control was built to produce, and it clears the
whole suspect list this file had accumulated: not the two Kconfig changes
(this build had none), not the compiler substitution (Ingenic's own
toolchain), not the load address, not vermagic, not image packaging.

### What the stock kernel itself says (2026-08-22, offline autopsy)

Nothing above had ever actually looked *inside* stock's `xImage`. Doing so
(strip the 64-byte u-boot header, inflate the single gzip member at
offset 16928) answers several questions at once. Reproduce with the
snippets in this session; artefacts left in the scratchpad.

**Stock was built with a different compiler than the SDK ships.**

```
stock: Linux version 4.4.94+ (zcz@androidserver3)
       (gcc version 5.2.0 (Ingenic r3.2.1-gcc520 2017.12-15)) #12 PREEMPT Mon Dec 29
ours : Linux version 4.4.94+ (root@…)
       (gcc version 7.2.0 (Ingenic Linux-Release5.1.4.1-…))
```

HiBy built with Ingenic's **gcc 5.2.0** (`r3.2.1-gcc520`, 2017.12). The
v6.0 SDK drop we have ships only `mips-gcc720-glibc229` — gcc **7.2.0**.
So "use Ingenic's own toolchain" was satisfied in letter but not in
spirit: we used *an* Ingenic toolchain, not *the* one. That toolchain is
not in the SDK bundle and would have to be sourced separately.

**The device tree is built in, and HiBy's differs from the SDK's.** Both
images embed exactly one DTB (`0xd00dfeed`), both declaring the same board
(`ingenic,x1600_halley6_module_base`) — so the board family is right and
the defconfig choice was never the problem. But `dtc`-decompiling both and
diffing gives 31 lines of real differences:

| node | stock | ours |
|---|---|---|
| `ingenic,sfc-max-frequency` | 400000000 | 200000000 |
| `sfc_ce1_pb4`, `sfc_ce1_pb31` pinmux | absent | **present** |
| `uart1-pb-sample` pinmux | present | absent |
| USB vbus | `ingenic,drvvbus-gpio` | `external-vbus-detect` |
| USB id | — | `external-id-pin` |
| DPU compatible | `ingenic,x1600-dpu` | `ingenic,dpu` |

These are exactly the "vendor board patches" hypothesised earlier, now
concrete. The SFC entries matter most: the SPI-flash controller is where
the kernel and rootfs live, and ours mux two extra chip-select pins
(`PB4`, `PB31`) that stock never mux. On a board that uses those pins for
something else, that is a plausible early-boot hang with the display left
holding u-boot's splash — which is precisely the observed symptom.

Note the compatible-string difference is probably *not* itself a fault:
each kernel carries its own matching DTB, so each is internally
consistent. It is evidence of SDK-version drift, not a binding failure.

**The single most valuable thing nobody has measured yet:** whether USB/ADB
enumerates while the device sits at the frozen logo. Attempt 1's black
screen was checked (no USB at all); the two logo freezes never were. If
ADB *does* come up, the kernel is booting and only display/init is
broken — a completely different and far more tractable problem than an
early hang. Plug the cable in and run `adb devices` *before* recovering,
next time.

### Attempt 4 (2026-08-22) — stock's own device tree, built and staged

The device tree turns out to be recoverable from the stock image without
HiBy's source at all, which makes this cheap to test. Decompiling stock's
built-in DTB and recompiling it round-trips **byte-identical**
(`e5c1217c3bc7b517bf1152d2bf4b36f0`, 15,833 B), so it can simply be
dropped in as the source `.dts`: `CONFIG_BUILTIN_DTB=y` with
`CONFIG_DT_X1600_MODULE_BASE_DTS_FILE="x1600_halley6_module_base.dts"`
means replacing that one file is the whole change.

Everything else held constant against the control build — same pristine
defconfig, no Kconfig edits, same Ingenic gcc 7.2.0 under Rosetta — so
the DTB is the single variable. Script: `kbuild-stockdtb.sh`; the SDK's
original dts is saved beside it as `dts-sdk-original.dts.bak`.

Built clean, and **the resulting kernel's embedded DTB is byte-identical
to stock's** (verified by re-extracting it from the finished `xImage`).
Load address `0x80F00000`, `Linux-4.4.94+`. Packaged as
`hiby-r1-kernel-stockdtb.upt` (`4c61139e…`, 41,492,480 B), build-id
`4.4.94_stockdtb`, `verify_firmware.py` clean, written to the card as
`r1.upt` and md5-checked after `sync`; `recovery-0.29.upt` left in place
and re-verified intact.

### SOLVED (2026-08-22) — it was never a boot hang, and the ADB check is what proved it

Attempt 4 flashed, showed the HiBy logo, and **looked** identical to the two
previous failures. It was not. Attaching ADB at the frozen logo — the check
this file had been recommending and nobody had done — showed the device was
fully up:

```
# cat /proc/version
Linux version 4.4.94+ (root@cab77fe6b53d) (gcc version 7.2.0 …) #1 PREEMPT Sat Aug 22 18:08:07 UTC 2026
# cat /usr/resource/kernel_build_id
4.4.94_stockdtb
```

Our own kernel, running. UBIFS mounted and recovered, USB gadget up, adbd
serving, and `hiby_player` alive at pid 892 — not crash-looping. **Three
"failed" flashes were very probably this same state**, misread as a hang
because the only visible surface is a display that never gets driven.

**Root cause: five of the 28 vendor modules can't load, because the kernel
doesn't export symbols they link against.** 23 of 28 loaded; the rest:

| module | undefined symbols | needs |
|---|---|---|
| `axp2101` (PMIC) | `power_supply_{register,unregister,changed}`, `regmap_{add,del}_irq_chip` | `POWER_SUPPLY`, `REGMAP_IRQ` |
| `cw2015` (fuel gauge) | `power_supply_{class,register,get_drvdata,changed}` | `POWER_SUPPLY` |
| `sa_sound_switch` | `switch_dev_register`, `switch_set_state` | `SWITCH` |
| `sa_earpods_adc` | `get_switch_status` | (exported by `sa_sound_switch`) |
| `leds_pwm_add` | `led_classdev_{register,unregister}` | `NEW_LEDS`, `LEDS_CLASS` |

All five are off or absent in the halley6 defconfig. The failure chain is
then mechanical: no `axp2101` → the PMIC's regulator rails never register →
the boot log shows `bldo1`, `bldo2`, `aldo2` and `vcc_i2c` each "not found,
using dummy regulator" → the panel and touch controller (i2c `1-0015`) are
never actually powered → nothing ever drives the display → u-boot's splash
stays on screen forever.

So the real answer to "why won't an SDK-built kernel run the R1" is neither
the compiler nor vendor board patches nor the device tree in isolation: the
reference defconfig simply omits peripheral *classes* that HiBy's
closed-source modules link against. The DTB substitution (attempt 4) was
still necessary and is kept.

**The lesson worth carrying:** "frozen at the logo" on this device is not
evidence of a boot failure. The display is the *last* thing to come up and
depends on the PMIC, so almost any peripheral-level breakage looks exactly
like an early hang. Always attach ADB before concluding anything.

### Attempt 5 — the fix, plus a deliberate proof change

Stock DTB (unchanged from attempt 4) plus `POWER_SUPPLY`, `NEW_LEDS`,
`LEDS_CLASS`, `SWITCH` and `REGMAP_IRQ` enabled. `REGMAP_IRQ` is a
promptless bool normally turned on by a `select` from an in-tree MFD driver;
since the driver needing it (`axp2101`) is out-of-tree, nothing selects it,
so `kbuild-fixed.sh` gives that symbol its own one-line prompt rather than
enabling an unrelated PMIC purely for the side-effect.

Also carries one deliberate change — `CONFIG_NFS_FS` off — to prove custom
config actually takes effect and survives: a leaf option, nothing here
mounts NFS (root is UBIFS), and independently checkable afterwards in
`/proc/filesystems`.

Build verifies the previously-undefined symbols are now present in
`System.map` before the image is ever packaged — a pre-flight that would
have caught this whole class of fault without a single flash.

Flashed clean: all 28 modules, `nfs` confirmed gone from `/proc/filesystems`
(proof change verified), display untouched by any of this (expected —
display was always a separate fault, chased below).

### The display fault, isolated (2026-08-22/23)

With the boot/module fault solved, `hiby_player` ran healthily — correct
threads, correct framebuffer state — and simply never painted, leaving
u-boot's own splash on screen. Static analysis of the DMA driver produced
two wrong turns before disassembly gave the real answer.

**Wrong turn 1: channel exhaustion / DMA_PRIVATE.** `dmaengine`'s
`private_candidate()` refuses an *entire* multi-channel controller, before
the filter even runs, if the controller lacks `DMA_PRIVATE` and any one
channel is already claimed. Measured live: 32 channels, 6 held by flash/MMC.
Plausible mechanism, so `dma_cap_set(DMA_PRIVATE, ...)` was added and
flashed. **Did not fix it** — `sahd_open: dma_request_channel error`
persisted identically. This ruled the theory out cleanly, at the cost of one
flash cycle that need not have been spent guessing.

**The actual answer, from the module's own disassembly** (`objdump -d` on
`sa_hgl_dma.ko`, no toolchain needed beyond what was already on hand):

```
180:  sw    zero,16(sp)      ; mask = 0
18c:  ins   v1,v0,0xd,0x1    ; set bit 13
198:  move  a2,zero          ; fn_param = NULL
1a0:  move  a1,zero          ; filter fn = NULL
1a8:  jalr  v0               ; __dma_request_channel(&mask, NULL, NULL)
```

`sa_hgl_dma` requests exactly one capability — bit 13, `DMA_INTERLEAVE` —
with **no filter at all**. Every filter-based, channel-count-based, or
reservation-based theory was chasing code that never executes; the request
dies in `__dma_device_satisfies_mask()` before any channel is examined. The
public SDK's `ingenic_dma.c` has **zero** interleaved-DMA support — no
`device_prep_interleaved_dma`, not even a stub. HiBy's own tree evidently
has this; it simply is not in the public v6.0 drop.

**Lesson for next time something like this happens:** when static analysis
of open-source code stops converging on a closed-source caller, go straight
to the caller's disassembly. Ten minutes of `objdump -d` settled in one pass
what several rounds of plausible-but-wrong kernel-side reasoning could not.

**Implementation** (`interleaved.c`, spliced into `ingenic_dma.c` right
before `ingenic_dma_prep_dma_sg`, which it deliberately mirrors): an
interleaved transfer is `numf` frames of `frame_size` chunks with an
inter-chunk gap, which decomposes onto the same linked hardware-descriptor
chain `build_dma_sg_desc()` already builds — `DCM_LINK` plus the next
descriptor's DMA address to chain, `DCM_TIE` on the last for the completion
IRQ. `dmaengine_get_src_icg()`/`get_dst_icg()` (already in tree, unused by
anything else in this driver) do the gap arithmetic; `src_inc`/`dst_inc`
control whether each side walks at all.

**First cut: one descriptor per row. Flashed, and it was a genuinely new
result** — `sahd_open` succeeded (the capability fix was right), but
`sahd_write: dmaengine_prep_interleaved_dma error` appeared on nearly every
frame, and the framebuffer capture showed the status bar and menu text
rendering while the body of the screen stayed black. Root cause: the
channel's descriptor pool is exactly one page —
`hdesc_max = PAGE_SIZE / sizeof(struct hdma_desc)` = 4096/32 = **128**
descriptors. A full-height 800-row blit needs 800; small text blits fit
under 128 and rendered.

**The fix: coalesce.** `DTC_TC_MSK` (0xffffff, ~16 MB) is far larger than
one descriptor needs to hold, so whenever consecutive chunks are physically
adjacent on both sides (both inter-chunk gaps are zero — the case for any
full-width blit) they merge into a single descriptor instead of one per
row. A full 480x800x16bpp screen (768,000 bytes) then costs exactly **one**
descriptor. Genuinely strided blits still cost one per row and remain
bounded by the same 128 pool. Implemented as two passes over the same
logic — count first (`sdesc == NULL`), then build — so the count and the
build can never disagree, the same two-mode shape `build_dma_sg_desc()`
already uses.

**Result: clean boot, zero SAUD errors, UI renders correctly.** Confirmed
by framebuffer capture (a fully-formed About screen — row layout, dividers,
right-aligned values) and, decisively, by the screen itself showing live
device state: `Kernel version 4.4.94_il_coalesce`, `SD card 317.3 GB free`
(the exFAT fix, end to end), `Memory 19 MB free`. `dmesg` clean of every
error this session chased. The app `.so` on `/usr/data` survived every
flash untouched (md5 matched the latest local build throughout), so every
BG/R fix built during the display chase (BG79, BG80, BG85-89, R58, R59,
R60) is live on this boot without any further action.

### Summary of the whole chain, start to finish

1. SDK reference defconfig != HiBy's own kernel config (never had HiBy's;
   no `/proc/config.gz` on stock, no `IKCFG_ST`).
2. Device tree: SDK's differs from stock's in 31 lines (SFC pinmux,
   USB vbus/id, DPU compatible string). Fixed by decompiling stock's own
   built-in DTB and recompiling it as the source `.dts` — verified
   byte-identical round-trip, no HiBy source needed.
3. Five vendor `.ko`s failed to load (`POWER_SUPPLY`, `NEW_LEDS`/
   `LEDS_CLASS`, `SWITCH`, `REGMAP_IRQ` all missing from defconfig) —
   found by attaching ADB at what looked like a boot hang and discovering
   the kernel was actually up. This was the single most valuable move of
   the whole session: three flashes had been mis-read as failures before
   this.
4. SD card is exFAT with `codepage=cp936`; `EXFAT_FS`/`NLS_UTF8`/
   `NLS_CODEPAGE_936` all missing. Filesystem parity taken to match stock's
   full list (`+MSDOS_FS +ISO9660_FS +NTFS_FS +FUSE_FS`), confirmed against
   stock's actual `mount` output, not guessed.
5. Display: `sa_hgl_dma` needs `DMA_INTERLEAVE`, absent from the public
   SDK's DMA driver entirely. Implemented from scratch, informed by the
   module's own disassembly rather than guessed from the capability name
   alone; coalesced to fit the 128-descriptor pool limit.

Every one of these was either measured on a live device or read directly
out of a binary — the two wrong turns in the whole session (memory
pressure as a cause; DMA_PRIVATE as the fix) were exactly the two moments
reasoning got ahead of evidence, and both cost a flash cycle to rule out.
The lesson generalises: on this device, a black screen or a stuck logo is
*never* diagnostic on its own — attach ADB before concluding anything,
every time.

### Follow-up (2026-08-23): the interleaved-DMA fix worked, but only for
### one screen -- the 128-descriptor pool was the real remaining limit

The coalesced interleaved-DMA build rendered a genuinely correct About
screen (framebuffer-captured and confirmed), but stock's own native
screens — "System" settings, reached via a tile our own hook doesn't
replace — corrupted, and the same error resurfaced:
`sahd_write: dmaengine_prep_interleaved_dma error`, hundreds per minute
while scrolling. Diagnosed by instrumenting the actual failure point
(`kbuild-dbg.sh`, rate-limited `pr_err` at every rejection inside
`ingenic_dma_prep_interleaved_dma`) rather than theorising again — this
was the second time in the session a plausible-sounding guess (this time:
maybe the coalescing logic still has a bug) would have cost a flash to
rule out for nothing.

**Measured, live:**
```
IL_DBG: alloc_swdesc failed: nr=662 hdesc_num=0 hdesc_max=128 pool=82ce7200 numf=662 frame_size=1
IL_DBG: alloc_swdesc failed: nr=534 hdesc_num=0 hdesc_max=128 pool=82ce7200 numf=534 frame_size=1
```
`hdesc_num=0` on every single failure ruled out a pool leak outright.
`nr == numf` exactly, every time, meant the coalescing logic was doing
precisely what it was designed to do — it simply had nothing to coalesce.
Stock's scrolling settings list redraws rows with real per-row gaps
(rounded-card margins), so consecutive rows are never physically adjacent
in memory and can never merge into one descriptor. Genuinely strided
work, needing up to 662 descriptors against a 128 cap.

**The fix, not a workaround: the cap was arbitrary, not a hardware
limit.** `ingenic_dma_alloc_chan_resources()` sets
`hdesc_max = PAGE_SIZE / sizeof(struct hdma_desc)` = 4096/32 = 128 — but
read `dma_pool_create()`'s own implementation
(`mm/dmapool.c`, present in this exact source tree): its `boundary`
argument is a per-object alignment constraint, not a total-capacity
limit, and `dma_pool_alloc()` transparently grows across as many backing
pages as it needs. The one-page ceiling was a number the driver's
original author picked for whatever workload they had in mind, not
something the pool itself, or the hardware, enforces. Raised to
`(PAGE_SIZE * 8) / sizeof(struct hdma_desc)` = 1024 — comfortable
headroom over the largest live-observed request (662) with room for a
full-height scroll or a larger screen.

`interleaved.c` itself needed no further change — the coalescing logic
was correct all along, it just wasn't the binding constraint for this
class of request. Kept anyway: it still turns any genuinely contiguous
blit (a full-width redraw, which is most of what this hook's own screens
do — see below) into a single descriptor instead of hundreds.

**Also settled while chasing this, from reading `music_hook.c` directly
rather than assuming: this hook's own screens never touch HGL or
interleaved DMA at all.** `music_entry()` (`music_hook.c` ~line 6404)
opens and `mmap`s `/dev/fb0` itself — a plain double-buffered mmap,
independent of `hiby_player`'s own framebuffer handle and of
`sa_hgl_dma` entirely. Confirmed two ways: none of the corrupted
screen's strings ("Backlight settings", "USB working mode", "Idle
shutdown", "Shortcut menu", "Certification information") exist anywhere
in `music_hook.c`, and the hook's `draw_ui()`/`blit_*` functions write
straight into the mmap'd buffer with no call into HGL anywhere in that
path. So every screen this hook actually built — Menu, Library,
Podcasts, its own Settings, its own Now Playing — was never at risk from
this bug, in either its broken or fixed state. Only stock's own
untouched native UI (System settings, and whatever native screen
"Music" in the original report referred to) exercises HGL at all.

This matters directly for the "drop `hiby_player`" plan: a standalone
build that owns `/dev/fb0` the way `music_entry()` already does needs
none of this interleaved-DMA work in the first place. `S11jpeg_display_
shell`'s `cmd_jpeg_display` (runs before a single vendor `.ko` loads,
writes the boot logo straight to `/dev/fb0`) is stock's own existence
proof that a minimal process can draw on this hardware with zero HGL
dependency.

### REVERTED same day: raising the descriptor cap traded a cosmetic bug for a real hang

Flashed `4.4.94_desccap` (`hdesc_max` 128 -> 1024). Within one session of
normal use, `hiby_player`'s main thread went into an **unkillable D-state
block** -- confirmed via `/proc/<pid>/task/*/wchan` reading `sahd_write`,
0% CPU, not spinning, genuinely waiting on a DMA completion that never
arrived. No kernel-side timeout exists anywhere in this path, so nothing
was ever going to recover it -- not a signal, not `kill -9`, nothing short
of a reboot. (One did work over ADB, no physical power-cycle needed --
worth knowing: `reboot` can still succeed with one thread wedged in D
state, at least this time.)

**Root cause of the hang is still genuinely unknown** -- likely either a
real chain-depth limit the X1600's PDMA hardware enforces that the driver
doesn't check for, or a bug in this session's own `DCM_LINK`/`DCM_TIE`
chaining logic that only misbehaves past some length nothing had ever
actually exercised before (every previously-*tested* success was the
About screen's fully-coalesced 1-descriptor case; every multi-hundred-
descriptor request had only ever been rejected at the allocator, never
actually issued to hardware, until this cap raise let one through for the
first time). Both are plausible; nothing here distinguishes them yet.

Reverted `hdesc_max` to its original 128 rather than chasing the real
limit under time pressure with a live, wedged device. This is a
deliberate downgrade back to the earlier, extensively-boot-tested
behaviour (stock's own oversized scroll blits fail to draw and look
corrupt, exactly as seen with the coalescing-only build) -- not a fix,
just the safe default. It costs nothing this app's own UI needs: every
screen this hook actually built draws via a direct `/dev/fb0` mmap (see
above), never through `sa_hgl_dma` at all, so this entire code path --
working, broken, or hung -- only ever touches stock's own untouched
native screens.

**Lesson, stated plainly:** a hang that requires a reboot is categorically
worse than a bug that merely looks wrong on screen, and "the fix removed
the visible error" is not the same claim as "the fix is safe" -- the two
have to be checked separately, and this session checked only the first
one before flashing.

### The 128 cap is EXACT HARDWARE, not arbitrary -- I was wrong

Chased properly afterwards instead of guessing again, and the evidence is
unambiguous:

```c
#define DDA_DOA_SFT  4
#define DDA_DOA_MSK  (0xff << DDA_DOA_SFT)
#define PHY_TO_DESC_DOA(dma)  ((((dma) & DDA_DOA_MSK) >> DDA_DOA_SFT) << 24)
```

The next-descriptor pointer is an **8-bit offset** taken from bits [11:4]
of the physical address: 256 units x 16 bytes = 4096 bytes = exactly one
page. The hardware cannot address a descriptor outside the current page.
`4096 / 32 = 128`. Raising `hdesc_max` put later descriptors in a second
page, where `PHY_TO_DESC_DOA` silently truncated the address and pointed
the engine at the wrong descriptor *within the first page* -- a corrupt
chain, followed into garbage, hence the unrecoverable wedge.

Confirmed independently: **mainline's own `drivers/dma/dma-jz4780.c` has
the identical limit** (`JZ_DMA_MAX_DESC = PAGE_SIZE / sizeof(hwdesc)`) and
documents the field the same way (`@dtc: ... offset of the next descriptor
from the descriptor base address in the upper 8 bits`). Two independent
implementations agreeing is as close to proof as this gets without a
datasheet.

### Why there is no cheap fix, and what the real options are

`DCM_STDE` (BIT(2), "stride enable") and the per-descriptor `sd` field
exist in this hardware and are **never used** -- `desc->sd = 0`
unconditionally in `build_one_desc()`. Mainline defines both and doesn't
use them either. Mainline's struct comment is the only documentation
found anywhere: `@sd: target/source stride difference (in stride transfer
mode)`. Native 2D stride is almost certainly what HiBy's own kernel uses,
since the interleaved support *and* the stride support are missing from
the public SDK **together** -- one feature, one omission.

The SDK ships no register datasheet (`docs/doc/` has X1000/x1830
*software* manuals and an OTA guide; no PDMA register chapter, checked).

Options, with honest costs:

| | approach | cost / risk |
|---|---|---|
| A | leave cap at 128 | zero risk; stock's big scroll blits still corrupt |
| B | multi-batch chaining, <=128 per batch, re-arm on completion IRQ | documented behaviour only, but real work: each batch must sit in one 4KB-aligned region, which `dma_pool` does not guarantee, so it needs its own allocator path plus IRQ-handler state |
| C | implement `STDE` 2D mode | elegant (one descriptor per blit) but undocumented registers, no reference implementation anywhere, no datasheet -- i.e. exactly the guesswork that caused the hang |
| D | CPU `memcpy` fallback when `nr > 128` | no undocumented registers, but needs phys->virt on DMA addresses, only valid for lowmem; a wrong assumption corrupts memory, which is worse than hanging |

### SHELVED (2026-08-23) -- reverted to the stock kernel entirely

Decision: put stock's own kernel back and stop custom-kernel work until
`hiby_player` is gone. The reasoning is that **this entire code path has
exactly one consumer, and it is scheduled for deletion**. This hook's own
screens never touch HGL or interleaved DMA at all -- `music_entry()`
opens and mmaps `/dev/fb0` directly (see the section above, confirmed by
source, not assumed). So every hour spent on options B/C/D above buys
correct rendering for stock's native settings screens, which the
standalone-player plan removes outright.

Flashed `hiby-r1-stockkernel.upt` -- pristine stock base through
`patch_firmware.py` with **no** `--kernel`, so stock's own 3,731,520-byte
kernel is retained and only the usual rootfs patches (DEV_HOOK wiring,
noatime, BT HFP, S90adb, radio, About) are applied. No `kernel_build_id`
stamped, so About correctly falls back to `uname()`. Verified: kernel
size matches stock exactly, all chunk digests OK.

**Nothing is lost.** The complete working custom-kernel chain is preserved
in `~/Git/hiby-kernel-build/`: `kbuild-il2.sh` (the last build that
rendered correctly), `interleaved.c` (the DMA implementation, which was
*correct* -- it was only ever the cap raise that broke things),
`stock-dtb.dts`, `vendor-symbols.py`, and `final/`. Everything needed to
resume is here, plus the five Kconfig fixes, the filesystem parity set,
and the DTB substitution method, all documented above.

**If this is picked up again**, start from `kbuild-il2.sh` (known-good,
renders correctly, no hang -- its only flaw is that stock's >128-descriptor
blits fail to draw), and treat option B as the fix. Do **not** raise
`hdesc_max`; that path is now definitively closed.

### SOLVED — Bluetooth boot time is in `bt_init`, not `cywdhd` (2026-09-02)

**The assumption was wrong.** Both the initial request and `patch_firmware.py`'s own comments assumed `cywdhd` — the WiFi/BT combo module — was what made Bluetooth late. Measurement showed it is not.

**Baseline, cold boot (stock `bt_init`, LEAN_4 kernel):**

| Milestone | Uptime |
|---|---|
| `adb` responsive | 3.81 s |
| `rfkill0` present (`cywdhd` finished) | 3.88 s |
| `hci0` present | 8.98 s |
| `/tmp/bt_init_ok` (Bluetooth actually usable) | **16.29 s** |

`cywdhd` accounts for only **0.44 s** (`dhd_module_init` in at 1.816 s, out at 2.262 s) — under 3 % of the 16.3 s gap. Deferring or splitting the driver could never have paid for itself.

**Where the 16.3 s actually went — ~12 s of fixed sleeps in `/usr/bin/bt_init`:**

| Step | Sleep |
|---|---|
| after rfkill write | `sleep 1` |
| after `brcm_patchram_plus` | `sleep 5` |
| after `hciconfig hci0 up` | `sleep 1` |
| after `hciconfig hci0 reset` | `sleep 1` |
| after `bt-agent` | `sleep 2` |
| before `bt-adapter` calls | `sleep 1` |
| before `sdptool add HIBYLINK_SP` | `sleep 1` |

**Fix 1 — sleep-to-poll rewrite (`patch_bt_init_timing`).** Every sleep became a `bt_wait()` condition poll (50 ms period) with the old timeout as the failure bound. The rfkill step additionally waits for `/sys/class/rfkill/rfkill0` to exist rather than assuming it.

Measured on hardware against a full rfkill power-cycle of the radio:

```
stock:   ~12.4 s
patched:  5.78 s
patched:  5.78 s
```

All milestones reached each time (`bluetoothd`, `bt-agent`, `bluealsa` running, alias set, HibyLink SP registered, same `Powered: 1 -> 0` end state as stock).

Phase breakdown of the 5.86 s instrumented run:

| Phase | Time |
|---|---|
| rfkill on | 0.04 s |
| MAC address resolved | 0.36 s |
| `hci0` appeared (patchram done) | **4.65 s** |
| `hci0` UP RUNNING | 4.67 s |
| bluez on D-Bus | 4.78 s |
| `hci0` UP after reset | 4.91 s |
| `bt-agent` up | 4.96 s |
| `bluealsa` up | 5.07 s |
| adapter configured | 5.69 s |
| done | 5.86 s |

**Finding — the remaining ~4.3 s is `brcm_patchram_plus` and it cannot be shortened.** The `.hcd` firmware is only 46,324 bytes (~0.015 s at 3 Mbaud), so transfer time is not the factor. Varying `--tosleep` produced no change:

| `--tosleep` | Result | Time |
|---|---|---|
| 50000 µs | addr OK, UP RUNNING | 4.26 s |
| 20000 µs | addr OK, UP RUNNING | 4.25 s |
| 10000 µs | addr OK, UP RUNNING | 4.26 s |
| 5000 µs | addr OK, UP RUNNING | 4.26 s |

The delay is the chip's own bring-up plus hardcoded waits inside the closed-source `brcm_patchram_plus`. This cost can only be **started earlier**, not made smaller.

**Fix 2 — start `bt_init` earlier (`hasten_bt_init`): `S80_bt_init` → `S22_bt_init`.** `S80` already ran the script backgrounded (`"$PL01" &`), so nothing downstream blocked on it; it was simply being started late. Real dependencies:

| Dependency | Satisfied by |
|---|---|
| `/usr/data` mounted (`bt_macaddr.txt`, `alsa.conf`) | `S21mount_ubifs` |
| `rfkill0` / `cywdhd` | `S11` (patched script now polls for it) |
| `/dev/ttyS0` | kernel, always present |
| D-Bus | `bt_init` starts its own `dbus-daemon`; does **not** depend on `S30dbus` |

**S22 is therefore the earliest safe slot.** Moving there overlaps the fixed 4.26 s `brcm_patchram_plus` cost with `S30`/`S40`/`S43`/`S50` and the player start instead of paying it after all of them.

**WiFi, "as late as possible".** `defer_wifi_module` backgrounds `cywdhd` in place (moving it to the end was tried before and delayed Bluetooth). With `bt_init` now waiting on `rfkill0` instead of assuming it, backgrounding is safe. `cywdhd`'s 0.44 s therefore comes off the boot critical path entirely.

Going further — not loading `cywdhd` at all until WiFi is switched on — was considered and rejected for now: `cywdhd` is the sole provider of `md_bcmdhd_bt_power`/`rfkill0`, and because it **cannot be reloaded after `rmmod`**, a load-on-demand design has no recovery path if the first attempt fails.

**Constraint discovered the same night: `cywdhd` cannot be reloaded.** Its exit path unregisters the platform driver but not the platform device, so a subsequent `insmod` hits:

```
sysfs: cannot create duplicate filename '/devices/platform/md_bcmdhd_bt_power'
kobject_add_internal failed for md_bcmdhd_bt_power with -EEXIST
```

`insmod` returns `EEXIST`. One attempt per boot; recovery is a reboot.

**Correction to earlier notes:** `defer_wifi_module`'s docstring claimed `cywdhd` init costs "~1.1 s of a ~4 s boot". Measured twice on LEAN_4 it is **0.44 s**. Its other figure — kernel proper done at 0.36 s — remains accurate (measured 0.357 s).

**Result after flashing, measured cold boot:**

| Milestone | Before | After |
|---|---|---|
| `hci0` present | 8.98 s | **7.42 s** |
| `/tmp/bt_init_ok` (Bluetooth usable) | 16.29 s | **9.11 s** |

Verified on the flashed device: `library_standalone` running, `hci0 UP RUNNING
PSCAN ISCAN` with the correct BD address, `bluetoothd`/`bt-agent`/`bluealsa`
all up, `cywdhd` loaded and `wlan0` associated with an IP, SD card mounted.
(`hci0` reads `DOWN` in the instant after `bt_init` finishes — that is stock's
own end state too, since the script closes with `bt-adapter --set Powered Off`;
it goes `UP RUNNING` when the player powers it on.)

**What this is and is not compared against.** Both numbers were measured on the
same `LEAN_4` kernel and the same 0.44-standalone rootfs, so the 7.2 s delta is
a clean like-for-like isolation of this change. It is **not** a measurement
against stock firmware. Two things make it a fair proxy anyway: every `sleep`
removed here was HiBy's own, untouched by this project (the only prior edit to
`bt_init` was the `bluealsa` HFP line), and `S80` ordering was stock too. What
is not controlled for is the kernel — `LEAN_4` is 380,928 bytes smaller than
the stock-config build, which would shift when `S22`/`S80` runs by perhaps
0.1-0.4 s, moving both figures together rather than changing the delta. A true
stock baseline needs a flash of `hiby-r1-STOCKKERNEL-abtest.upt` (or
`0.44-standalone.upt`, which still carries the stock kernel) and has not been
taken.
