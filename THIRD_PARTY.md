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
refuses to run if the caller's idea of the layout disagrees. Per the IJG
licence, this software is based in part on the work of the Independent JPEG
Group.

## Credits

**[yetisoldier/Hiby-R1-Audiobook-Mod](https://github.com/yetisoldier/Hiby-R1-Audiobook-Mod)**
— this app runs on top of that firmware mod, and several device findings it
established are load-bearing here: that a launcher tile can be re-pointed by
`LD_PRELOAD`ing into `hiby_player`, that the tile callback must not point into
the injected object itself, and the raw touch-event injection format that
`tools/tap.py` follows. That project carries no licence file; nothing has been
copied from it, but it is where the approach came from.

**[bidhata/Hiby-R1-Mod](https://github.com/bidhata/Hiby-R1-Mod)** — surveyed
early on while working out how the firmware image is packed.

## Not included

HiBy's own resources — the stock launcher icons, the `.ini` string tables, and
the firmware itself — are not redistributed here. The tile's icon is drawn from
scratch by `icon/make_icon.py`, and its label is produced at runtime by
rewriting a single string in whatever `settings.ini` the installed firmware
provides. Nothing in this repository is HiBy's.
