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
