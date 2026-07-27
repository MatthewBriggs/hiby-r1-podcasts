# HiBy R1 Podcasts app — status

A Podcasts app that replaces the **About** launcher tile on the R1, running
in-process inside `hiby_player`. Working proof of concept as of 2026-07-27.

## What works

Tap **About** on the launcher and the app opens. Three screens, all drawn with a
built-in 5x7 font:

- **Feeds** — an `UPDATE FEEDS` bar, then one row per folder under
  `/data/mnt/sd_0/Audiobooks`.
- **Episodes** — audio files in the selected feed, sorted.
- **Now Playing** — feed and episode name, progress bar, elapsed/total,
  `-30 / PAUSE / +30`, and a speed control cycling 1.0/1.25/1.5/1.75/2.0x.

Lists scroll by swiping, with a proportional scrollbar.

**Updating** is manual and in-app: tap `UPDATE FEEDS` and the app forks
`.podsync/podsync_once.sh`, streaming its log to an update screen until it
reports done. Subscriptions are one RSS URL per line in
`/data/mnt/sd_0/.podsync/feeds.txt`, editable by pulling the card or over the
player's WiFi transfer mode. There is no background process and nothing to
schedule. The fetcher stays a shell script because it needs the static `curl`
on the card — busybox `wget` only offers legacy TLS ciphers and every modern
podcast host rejects the handshake. Episode files are named from the RSS
`<title>`, and the list hides the extension.

Audio plays for real: MP3 decoded with minimp3_ex and written to ALSA on a worker
thread. Pause holds the clock, ±30s seeks, and **resume positions persist** — backing
out stores the offset to `.podsync/resume.txt` on the SD card and re-opening the
episode continues from there. Verified end-to-end on firmware 2.0.25; screenshots in
`tools/` (`v2_play.png`, `v2_paused2.png`, `final_resume.png`).

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

## Audio notes

Seeks use `MP3D_SEEK_TO_SAMPLE`. `MP3D_SEEK_TO_BYTE` starts instantly but its
byte estimate overshoots on VBR: seeking 25 min into a 34 min episode returned
success and then read EOF, so resume silently reported FINISHED. The audiobook
mod avoided SEEK_TO_SAMPLE because indexing a 6.6 h book cost ~14.5 MB, but a
podcast episode is an order of magnitude shorter — a 34 min file indexes in ~3 s
and the UI shows `LOADING...` while it does. Very long episodes will feel slow.

HiBy's `libmp3.so` is libmpg123 but unusable — its file readers are stubs and
`mpg123_read` returns garbled PCM for 22050 Hz MPEG-2 files. minimp3_ex (CC0,
vendored) is compiled in instead. Seeks use `MP3D_SEEK_TO_BYTE`: the sample-accurate
mode indexes the whole file and OOMs a 56 MB device. Because that mode resyncs to a
frame boundary rather than tracking an absolute sample index, `cur_sample` is not a
usable clock — the player keeps its own `base_ms + decoded frames` counter. ALSA is
`dlopen`'d and opened as `plughw:0,0` so it resamples to the hardware's rates; hw/sw
params are set by hand because `snd_pcm_set_params` is unreliable here.

**Speed** is real time-stretching, not resampling, so pitch is preserved:
`wsola.c` cross-fades between waveform-similar windows. Measured on device at
1.25x: 35 s of audio in 28 s of wall clock. The subtle bug worth remembering is
that the similarity-search offset must *not* be folded into the read pointer —
doing so makes it accumulate and ran 1.75x when 1.25x was asked for.

## Known gaps

- **Stale frame on exit.** After leaving, our last frame stays until the player
  next repaints. The audiobook mod solves this with a bounded "handoff watcher"
  that surfaces the player's first hidden redraws.
- **MP3 only.** `.m4a/.m4b/.opus` are listed but will fail to decode.
- No episode ordering by date, no played/unplayed state, no cover art.
- Font is uppercase-only 5x7; lowercase folds to caps.

## Recovery

- Delete `/usr/data/libpodcast_hook.so` and reboot to get a stock-behaving device.
- Upstream firmware, verified, is at `~/Git/hiby-fw/r1-audiobooks-2.0.25.upt`
  (SHA256 `844bbe64…`). The consumed card copy was renamed to `r1.upt.installed`.
