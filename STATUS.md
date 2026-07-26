# HiBy R1 Podcasts app — status

A Podcasts app that replaces the **About** launcher tile on the R1, running
in-process inside `hiby_player`. Working proof of concept as of 2026-07-27.

## What works

Tap **About** on the launcher and a custom full-screen UI appears: purple header,
`EXIT` affordance, and one row per folder under `/data/mnt/sd_0/Audiobooks`, drawn
with a built-in 5x7 font. Tapping a row highlights it; tapping the header exits and
hands the display back to the player. Verified on firmware 2.0.25 with screenshots
in `tools/` (`app4.png`, `app_sel2.png`).

## How it hooks in

**Tile route.** Launcher tiles are 96-byte records in `.data`, but a tile's *name
string* and its *callback* belong to different records — for a name at `S`, the
callback is at `S + 0x48`. This cost the most time to work out; getting it wrong
silently disables a tile instead of failing loudly.

| tile | name string | callback | stock value |
|---|---|---|---|
| ebook (= Audiobooks) | `0x00891FE8` | `0x00892030` | `0x0075DAEC` (audiobook mod's cave) |
| sysset (= System) | `0x008920A8` | `0x008920F0` | `0x0053BBE0` |
| **about** | `0x00892108` | **`0x00892150`** | `0x0053BC20` |
| **about (live one)** | `0x00892528` | **`0x00892570`** | `0x0053BC20` |

`0x00892570` is the record the launcher actually dispatches; both are patched.

**A callback may not point into our `.so`** — the launcher then fails to render at
all. It must be an address inside the executable image, which is why the audiobook
mod uses a code cave. We claim unused zeroed space in `.rodata` at `0x0075E400`
(clear of the mod's `0x35DAEC` / `0x35DBC0` / `0x35DF40`), write a MIPS trampoline
(`lui t9 / addiu t9 / jr t9 / nop`), and point the tile there. Cave and `.data`
edits happen at runtime from the `LD_PRELOAD` constructor, before the launcher
instantiates its heap copy of the records — so no binary patching.

**Rendering.** The tile callback runs on the player's UI thread, so blocking there
stops the player's render loop and nothing is ever panned. The app therefore owns
the frame loop while open: `mmap` `/dev/fb0`, draw, `FBIOPAN_DISPLAY`, flip page,
~30 fps. It also takes `EVIOCGRAB` on `/dev/input/event1` so taps don't leak
through to the launcher underneath.

## Dev loop (no reflashing)

The firmware at `~/Git/hiby-fw/work/r1-podcast-dev-2.0.25.upt` is stock 2.0.25 with
**one file changed** (`/usr/bin/hiby_player.sh`) so the supervisor also preloads
`/usr/data/libpodcast_hook.so` and runs `/usr/data/init.sh` at boot. It drops the
dev hook after 2 consecutive crashes, so a bad build cannot bootloop the device.

```sh
cd app && ./build.sh
adb push libpodcast_hook.so /usr/data/libpodcast_hook.so
adb shell 'sync; reboot'
```

**Reboot between iterations — do not `killall hiby_player`.** The HGL graphics DMA
pool is never reclaimed when the player dies; after a few restarts allocation fails
with `[SAUD]sahd_open: malloc hgl memory error` and the UI freezes silently (process
alive, clock stopped, taps ignored). A frozen clock across `fbgrab` captures is the
tell. This cost two device lock-ups before it was understood.

Observe and drive the device without touching it:

```sh
python3 tools/fbgrab.py shot.png   # /dev/fb0 -> PNG
python3 tools/tap.py 360 640       # inject a tap (About tile)
```

Both move data as base64 through `adb shell`: this adbd predates `exec-out`, and
plain `shell` corrupts binary with LF translation.

## Known gaps

- **Nothing plays yet.** Rows are folders; selecting one only highlights it. No
  episode list, no audio. Playback is the next real chunk of work — the audiobook
  mod already does resume/speed/chapters for anything under `/Audiobooks`, so the
  cheapest route may be to hand off to it rather than reimplement.
- **Stale frame on exit.** After leaving, our last frame stays until the player
  next repaints. The audiobook mod solves this with a bounded "handoff watcher"
  that surfaces the player's first hidden redraws.
- **No feed fetching wired in.** `~/Git/hiby-podsync` downloads episodes and works,
  but its autostart was lost with the firmware change; point it at
  `/data/mnt/sd_0/Audiobooks/<feed>/` and start it from `/usr/data/init.sh`.
- Font is uppercase-only 5x7; lowercase folds to caps.
- Only the first ~13 feeds fit; no scrolling.

## Recovery

- Delete `/usr/data/libpodcast_hook.so` and reboot to get a stock-behaving device.
- Upstream firmware, verified, is at `~/Git/hiby-fw/r1-audiobooks-2.0.25.upt`
  (SHA256 `844bbe64…`). The consumed card copy was renamed to `r1.upt.installed`.
