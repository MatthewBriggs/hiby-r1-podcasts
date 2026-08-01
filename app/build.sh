#!/bin/sh
# Cross-compile the podcast hook for the R1 (MIPS32 little-endian, glibc 2.22).
set -e
script_dir="$(dirname "$0")"
common_dir="$script_dir/../../common"
zig cc -target mipsel-linux-gnueabihf.2.22 \
  -shared -fPIC -Os -s -fvisibility=hidden -fno-common \
  -I. -I"$common_dir" \
  -o libpodcast_hook.so podcast_hook.c audio.c wsola.c mp3meta.c \
  "$common_dir/cover.c" "$common_dir/text.c" -lpthread
ls -la libpodcast_hook.so
