#!/bin/sh
# Cross-compile the podcast hook for the R1 (MIPS32 little-endian, glibc 2.22).
set -e
zig cc -target mipsel-linux-gnueabihf.2.22 \
  -shared -fPIC -Os -s -fvisibility=hidden -fno-common \
  -o libpodcast_hook.so podcast_hook.c audio.c wsola.c cover.c -lpthread
ls -la libpodcast_hook.so
