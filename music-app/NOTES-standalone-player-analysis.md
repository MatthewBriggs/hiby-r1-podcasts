# open_hiby_player: source analysis

Analysis of `github.com/hiby-modding/open_hiby_player`, cloned 2026-08-14 at
commit `8b37dbe`. Read-only: the source was cloned to a scratch directory and
read, never built, never run, never put on the device.

This supersedes an earlier version of this file, which was written from
`strings` analysis of a *compiled binary* (`~/Downloads/hiby_player`, build
stamp `2026-07-24`) before the source was available. Several inferences in
that version are corrected below.

## Licence — read this first

**The repo is GPLv3. This project is MIT. Do not copy code from it.**
Techniques, facts, sysfs paths and hardware behaviour are not copyrightable
and can be used freely; source lines cannot. Everything below is deliberately
written as *facts about the hardware and about what their code does*, not as
code to lift.

One exception, and it is worth knowing: `src/system/decode/stb_vorbis.c` is
Sean Barrett's, dual-licensed public-domain/MIT-0, so it is not encumbered by
the repo's GPL. If it is ever wanted, take it from upstream `nothings/stb`
rather than from this tree, so the provenance is unambiguous.

## What it is, and what it is not

- **Standalone, not a hook.** It kills `hiby_player.sh` and `hiby_player` and
  owns the device outright. LVGL over plain `/dev/fb0`
  (`lv_linux_fbdev_create()`, `main.c:83-88`), statically linked.
- **Confirmed: no HGL anywhere.** Grepping the whole tree finds no `hgl`,
  `hgl_fb` or DMA-pool reservation of any kind. My earlier strings-based
  inference — that it bypasses HiBy's private graphics library entirely and
  goes straight at the kernel fbdev node — was right. This is the second
  independent project to do so (Nanowave, Rust/Slint, is the other, via KMS),
  which is a real signal for RP1.
- **It targets the R3Pro II, not the R1.** "As I only have an R3Pro II,
  that's what's currently supported"; *add R1 support* is an open item in
  `TODO.md`. Every sysfs path below is therefore R3Pro II-confirmed and
  R1-**unverified** — see the checklist at the end.
- **The public repo is well behind the binary I analysed earlier.** The
  binary had a Subsonic client, parametric EQ with `.peq` profiles,
  Opus/ALAC/APE/DSD, crossfade, Wi-Fi/BT pairing UI, and "Audio Books coming
  soon" stubs. **None of that exists in this source tree** — grep finds no
  `subsonic`, no `opus`, no `crossfade`, no PEQ. The binary was a private
  fork or a much later build ("Jose Garita's private build" appears in its
  strings). Do not treat the feature list in my earlier notes as describing
  this repo.

## The important caveat: we already have most of this

Since the earlier version of this file was written, `c054fdb` ("Cover art
fallback (BG25), power management, Now Playing redesign") landed a full
suspend implementation in `music_hook.c`. Comparing honestly, **our
implementation is ahead of theirs on the parts that matter most**:

| | open_hiby_player | ours (`music_hook.c`) |
|---|---|---|
| Screen off via `/sys/class/graphics/fb0/blank` | yes (`0`/`1`) | yes (writes `4`, `FB_BLANK_POWERDOWN`) |
| Suspend via `echo mem > /sys/power/state` | yes, blocks until resume | yes, same |
| Bluetooth teardown around suspend | **no** | **yes** — `/usr/bin/bt_suspend` before, `bt_resume` after |
| RTC wake backstop | **no** | **yes** — `wakealarm` at +900 s so a failed button press can't leave it dead in a pocket |
| Restores prior BT power state | **no** | **yes** |
| Idle suspend enabled by default | **no** — disabled, wake source unconfirmed | yes, shipped and working |

Their `TODO.md` is explicit that idle suspend is off by default "until
KEY_POWER is confirmed as a kernel wake source". We got past that and know
the `bt_suspend → blank → echo mem → bt_resume` sequence is load-bearing —
something their code does not do at all and would presumably have to
rediscover. So this is not a case of finding a solved problem; on power
management specifically, we are the more advanced implementation.

## The one genuinely new, actionable thing

Two details in their `power.c` address a failure mode we hit and worked
around rather than fixed:

1. **A 60 ms settle after unblank, before touching brightness.**
   `SCREEN_ON_SETTLE_US = 60000` (`power.c:46`), applied at `power.c:198`.
   Their reasoning: the fb blank notifier re-inits the panel and re-powers
   the backlight on unblank, and "a brightness write that lands mid-reinit
   can be swallowed, leaving the screen dark."

2. **The scan-out has to be explicitly re-armed after an unblank.** They
   enable a one-shot `force_refresh` around the wake repaint
   (`power.c:217-227`), which makes the fbdev flush issue
   `FBIOPUT_VSCREENINFO` with `ACTIVATE_NOW | FORCE`. Their comment: the
   blank "tears down the panel's scan-out; on this hardware the controller
   won't re-present the framebuffer just because we memcpy fresh pixels into
   it — it must be explicitly kicked."

**Why this matters to us.** `music_hook.c:3214-3217` carries a comment
describing what reads like exactly this symptom: *"restoring brightness to a
blanked panel leaves a black screen that no amount of pressing will bring
back."* Our fix was to unblank unconditionally on the way out — a workaround
that avoids the state rather than repairing it. Their two details suggest the
underlying cause is a torn-down scan-out plus a swallowed brightness write,
and give a specific remedy for each. It also plausibly explains the
`EBUSY`-on-`FBIOPAN_DISPLAY` behaviour the audiobook mod documented and that
we inherited a workaround for.

Neither `FBIOPUT_VSCREENINFO` nor a post-unblank settle appears anywhere in
our code today (grep: zero hits for both).

**Transferability — better than it first looks.** The obvious objection is
that their fix lives inside LVGL's fbdev backend and we don't use LVGL. But
the LVGL wrapper is incidental; the mechanism is a raw ioctl. And we are not
a bystander to the framebuffer: `music_hook.c` opens `/dev/fb0` itself
(`:3461`), keeps the fd in `g_fbfd`, and already issues `FBIOGET_VSCREENINFO`
(`:3466`), `FBIOPAN_DISPLAY` (`:4498`, `:4545`) and `FBIOBLANK` (`:3212`,
`:3218`, `:4464`) on it. Adding one more ioctl on an fd we already own and
already drive is a small, in-idiom change — not the architectural leap it
would be if we were purely a guest on someone else's display stack.

The real risk is different and worth stating: `hiby_player`'s own HGL
render thread is panning the same device concurrently, so forcing a mode
re-activation could disturb *its* state, not just ours. That argues for
trying it narrowly — on the wake path only, once, exactly as they do — and
watching for the BG6 watchdog fighting it, rather than adopting it globally.

## What does not transfer

- **Pausing the render loop while dark** (`lv_timer_pause` on the display
  refresh timer, `power.c:177-179`). Trivial for them because they own the
  loop. We own our frame loop only while our app is open, and `hiby_player`'s
  own render thread keeps panning regardless of what we do. Not applicable in
  the same form, though "stop *our* redraw while locked" may be worth its own
  look for BG9.
- **Their overall power state machine** (250 ms tick, 30 s screen-off, 120 s
  idle-suspend defaults, `RESUME_GUARD_MS = 600` to absorb the wake press
  arriving as a normal input event). We already have equivalents. The
  600 ms resume guard is the one small idea worth comparing against ours if
  we ever see a double-toggle on wake.

## What it does not help with at all

- **BG12 (audio distortion + UI freeze over USB-C, thermal).** Nothing in
  this codebase touches thermal management, CPU frequency/governor, USB power
  delivery, or DAC thermal limits. Their only power lever is full
  suspend-to-RAM.
- **BG9 (battery drain over USB-C).** Same — no USB power handling, no
  `cpufreq`/`cpuidle` control. Their code cannot explain why we drain faster
  than stock while playing.
- **W2 (SoC suspend / C-states).** They suspend or they don't; there is no
  finer-grained idle-state work to learn from.

## Verify on the R1 before relying on any of it

Every path below is R3Pro II-confirmed and R1-unverified. The device was
disconnected when this was written, so none of these were run.

```bash
adb shell 'ls -l /sys/class/graphics/fb0/blank; cat /sys/class/graphics/fb0/blank'
```

```bash
adb shell 'cat /sys/class/backlight/*/max_brightness; cat /sys/class/backlight/*/brightness'
```

```bash
adb shell 'cat /sys/power/state; cat /sys/kernel/debug/wakeup_sources 2>/dev/null | head'
```

Also worth confirming: their `BRIGHTNESS_MIN = 1` with a comment that `0` "is
not a valid off value" on their panel. Ours writes `0` to the backlight
directly (`set_locked`, `music_hook.c:3198`) and that demonstrably works
here, so this is a genuine R3Pro II/R1 hardware difference rather than
something to copy — noted only so nobody "fixes" our working `0` to match
their code.
