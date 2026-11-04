#!/bin/sh
# Regenerates the committed SPIR-V fallbacks; CMake compiles the same sources when glslc exists.
set -e
cd "$(dirname "$0")"
mkdir -p prebuilt
for shader in crt_fullscreen.vert crt_lines.frag crt_beam.frag crt_glow.frag crt_final.frag osd.frag; do
    glslc -O -mfmt=c -o "prebuilt/$shader.inc" "$shader"
done
