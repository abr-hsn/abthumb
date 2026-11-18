#!/bin/sh
# Usage: sh tests/fuzz/make_seeds.sh   (from repo root). Writes build/fuzz/seeds/<target>/.
R="$(cd "$(dirname "$0")/../.." && pwd)"; G="$R/tests/corpus/generated"; S="$R/build/fuzz/seeds"
rm -rf "$S"; mkdir -p "$S/probe" "$S/psd" "$S/qoi"
cp_small() { # src dst
  sz=$(wc -c < "$1"); if [ "$sz" -lt 300000 ]; then cp "$1" "$2"; else head -c 262144 "$1" > "$2"; fi; }
for d in ai eps pdf psd psb; do
  n=0; for f in "$G/$d"/*; do [ -f "$f" ] || continue; n=$((n+1)); [ $n -gt 12 ] && break
    cp_small "$f" "$S/probe/${d}_$(basename "$f")"; done; done
for d in psd psb; do for f in "$G/$d"/*; do [ -f "$f" ] || continue
  cp_small "$f" "$S/psd/${d}_$(basename "$f")"; done; done
# tiny QOI seed: 2x2 header + RGBA run
printf 'qoif\0\0\0\2\0\0\0\2\4\0\xff\x00\x00\xff\x55\xc1\0\0\0\0\0\0\0\1' >"$S/qoi/tiny.qoi"
