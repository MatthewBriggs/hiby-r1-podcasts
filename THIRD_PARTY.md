# Third-party code and credits

## Vendored sources

| Path | Project | Licence |
|---|---|---|
| `app/vendor/minimp3.h`, `app/vendor/minimp3_ex.h` | [minimp3](https://github.com/lieff/minimp3) by lieff | CC0-1.0 (public domain) |
| `app/vendor/stb_truetype.h` | [stb](https://github.com/nothings/stb) by Sean Barrett | Public domain / MIT |
| `app/vendor/jpeg9/*.h` | [libjpeg 9](https://ijg.org/) by the Independent JPEG Group | IJG licence |

The libjpeg headers are **headers only**. No IJG code is compiled into or
shipped with this project — the R1's firmware already provides
`/usr/lib/libjpeg.so.9`, and the library is `dlopen`'d at runtime. The headers
are vendored because the device ships the library without them, and
`jpeg_CreateDecompress` validates `sizeof(struct jpeg_decompress_struct)` and
refuses to run if the caller's idea of the layout disagrees.

**This software is based in part on the work of the Independent JPEG Group.**

Those headers say "for conditions of distribution and use, see the accompanying
README file", and that README is not vendored here, so the conditions are
reproduced instead:

> The authors make NO WARRANTY or representation, either express or implied,
> with respect to this software, its quality, accuracy, merchantability, or
> fitness for a particular purpose. This software is provided "AS IS", and you,
> its user, assume the entire risk as to its quality and accuracy.
>
> Permission is hereby granted to use, copy, modify, and distribute this
> software (or portions thereof) for any purpose, without fee, subject to these
> conditions: (1) If any part of the source code for this software is
> distributed, then this README file must be included, with this copyright and
> no-warranty notice unaltered; and any additions, deletions, or changes to the
> original files must be clearly indicated in accompanying documentation.
> (2) If only executable code is distributed, then the accompanying
> documentation must state that "this software is based in part on the work of
> the Independent JPEG Group". (3) Permission for use of this software is
> granted only if the user accepts full responsibility for any undesirable
> consequences; the authors accept NO LIABILITY for damages of any kind.

The vendored headers are unmodified from libjpeg 9.

## Not shipped, but required at runtime

- **A CA bundle.** The installation instructions point at Mozilla's, as
  published by the curl project (MPL 2.0). It is downloaded by the user, not
  redistributed here.
- **A static `curl`.** Supplied by the user; not redistributed here.
- **`msyh.ttf`** (Microsoft YaHei), read from the device's own firmware at
  runtime for text rendering. It is proprietary and is deliberately **not**
  copied into this repository or any release.

## Credits

**[yetisoldier/Hiby-R1-Audiobook-Mod](https://github.com/yetisoldier/Hiby-R1-Audiobook-Mod)**
— this app runs on top of that firmware mod, and several device findings it
established are load-bearing here: that a launcher tile can be re-pointed by
`LD_PRELOAD`ing into `hiby_player`, and that the tile callback must not point
into the injected object itself.

`tools/tap.py` deserves a more precise statement than "inspired by". It was
written against that project's `tools/adb_inject_touch_event.py` and follows its
structure: the same `abs_frame` decomposition and the same eight-frame press.
The `struct.pack("<llHHl", …)` layout is the Linux `input_event` ABI and the
frame count was determined empirically, so both are facts rather than authorship
— but the shape of the code is theirs, and it is fairer to call it derived than
independent.

**That project ships no licence file, so its terms are unstated.** Nothing here
redistributes any of its code or firmware, and `tap.py` is a development helper
rather than part of the app, but anyone reusing that file should know where it
came from. If yetisoldier would like it removed, rewritten, or licensed
differently, please open an issue and it will be dealt with.

**[bidhata/Hiby-R1-Mod](https://github.com/bidhata/Hiby-R1-Mod)** — surveyed
early on while working out how the firmware image is packed.

## Not included

HiBy's own resources — the stock launcher icons, the `.ini` string tables, and
the firmware itself — are not redistributed here. The tile's icon is drawn from
scratch by `icon/make_icon.py`, and its label is produced at runtime by
rewriting a single string in whatever `settings.ini` the installed firmware
provides. Nothing in this repository is HiBy's.
