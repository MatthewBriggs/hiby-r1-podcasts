# HiBy R1 Podcasts app — status

A Podcasts app that takes over the **About** launcher tile on the R1 — retitled
"Podcasts" with a microphone icon — running in-process inside `hiby_player`.

## What works

Tap **About** on the launcher and the app opens. Three screens, all drawn with a
built-in 5x7 font:

- **Feeds** — an `UPDATE FEEDS` bar, then one row per folder under
  `/data/mnt/sd_0/Audiobooks`.
- **Episodes** — audio files in the selected feed, newest first, each with a
  completion figure (a percentage while part-played, DONE when finished).
- **Now Playing** — feed cover art, episode name, progress bar, elapsed/total,
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

Ordering is by publication date. The parser converts the RSS `pubDate` to a form
busybox `touch -d` accepts and the fetcher stamps it onto each file, so the
file's mtime *is* its publication date. Sorting by download time would be
backwards: the fetcher walks the feed newest-first, so the newest episode is
written first and would sort last.

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

## Output routing and volume

Bluetooth and wired need entirely different handling.

**Bluetooth** goes through BlueALSA, not the DAC. A connected A2DP sink shows up
in `bluealsa-cli list-pcms` as a path ending `/sink`; when one is present the
player opens the predefined `bluealsa` PCM (which auto-selects the most recent
sink and converts rate/format) instead of `plughw:0,0`, falling back to wired
rather than going silent. BlueALSA exposes a real mixer element named after the
device, so volume there is a normal `amixer -D bluealsa` set.

**Wired volume has to be done in software.** The CS43131's volume registers are
not wired up on this hardware: `amixer sset Left 40` reports success and reads
back 0, and nothing audible changes — bidhata's mod notes the same thing
(`USE_VOLUME_CHIP` "forces volume writes to unwired CS43131 registers"). The
stock player scales samples itself, and so does this one, just before handing
PCM to ALSA, with a squared curve so low settings stay usable. The level is kept
in `.podsync/volume.txt`. Without this, wired playback is silent.

Volume keys are on **`event2`** (the ADC keypad), not `event0`. Both key nodes are
grabbed while the app is open, or the stock player consumes the presses and moves
its own volume behind us.

## Audio notes

Playback does not use `mp3dec_ex_*` at all. Both of its seek modes were dead
ends: `MP3D_SEEK_TO_BYTE` reports success then reads EOF on a far seek (25 min
into a 34 min episode), and `MP3D_SEEK_TO_SAMPLE` is exact but indexes the whole
file. Worse, `mp3dec_ex_open` computes duration by scanning every frame either
way, so opening an episode took ~4.8 s.

Instead the worker drives `mp3dec_decode_frame` over an mmap'd file and gets
duration from `mp3meta.c`, which reads the Xing/Info header (exact frame count,
plus a seek TOC when present) or falls back to the CBR bitrate. That needs only
the first few hundred bytes: **opening dropped from 4788 ms to 291 ms**. Seeking
is a byte offset from the TOC or a linear interpolation, then a resync to the
next frame header.

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
- Cover art shows only on Now Playing, not in the lists.
- Font is uppercase-only 5x7; lowercase folds to caps.

## Tile icon and label

The tile is renamed and re-iconed by shadowing three read-only resources with
`mount --bind`, done from the `LD_PRELOAD` constructor rather than a boot script:
the constructor runs before `main()`, and the player reads its string table
during startup, a race a backgrounded boot script loses (it won for the icon,
which is loaded later, but not for the label).

The label is `<about>` in **`str/english/settings.ini`** — not `<abo_dev>` in
`launcher.ini`, which drives a different menu. The giveaway was that the System
tile reads "System" while `launcher.ini` says "System settings".

`icon/make_icon.py` draws the 140x140 RGBA microphone with 4x supersampling and
writes the PNG via zlib, so there is no image-library dependency.

## Cover art

`cover.c` decodes a feed's `cover.jpg` with the device's `libjpeg.so.9`, which is
`dlopen`'d so a missing library degrades to no cover. libjpeg has no opaque-handle
API, so the struct layout has to match: libjpeg 9's own headers are vendored,
because Homebrew ships version 80 (jpeg-turbo) and 100 (IJG) and both disagree
with the device's 90. `jpeg_CreateDecompress` validates the struct size, so a
mismatch fails safely rather than corrupting memory. Decoding uses `scale_denom`
to downscale during decode and caches the result next to the cover as raw RGB565.

## Recovery

- Delete `/usr/data/libpodcast_hook.so` and reboot to get a stock-behaving device.
- Upstream firmware, verified, is at `~/Git/hiby-fw/r1-audiobooks-2.0.25.upt`
  (SHA256 `844bbe64…`). The consumed card copy was renamed to `r1.upt.installed`.
