#!/bin/sh
# Exercise real compiler processes, beyond the no-disc player's environment.
# Usage: test-compiler-environment.sh APPIMAGE DISC.cue WORK WRAPPER [zig]
# WRAPPER is compiler-environment.c compiled for the target distribution.
# The zig mode expects --toolchain-install to have completed in WORK/data.
set -eu
image=$(readlink -f "$1") disc=$(readlink -f "$2") work=$(readlink -f "$3") wrapper=$(readlink -f "$4")
mode=${5:-system}
mkdir -p "$work/bin" "$work/env" "$work/home" "$work/config" "$work/data"
if [ "$mode" = zig ]; then
  real=$(find "$work/data/lionheart/toolchain" -name zig -type f | head -1)
  test -n "$real"
  mv "$real" "$real.real"
  cp "$wrapper" "$real"
  real=$real.real
  path=/usr/bin:/bin
else
  real=$(command -v clang || command -v gcc)
  cp "$wrapper" "$work/bin/clang"
  path=$work/bin:/usr/bin:/bin
fi
: > "$work/empty.ini"
set -- HOME="$work/home" XDG_DATA_HOME="$work/data" XDG_CONFIG_HOME="$work/config" \
  PATH="$path" SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy LANG=C.UTF-8 \
  TMPDIR="${TMPDIR:-/tmp}" LD_LIBRARY_PATH=/lionheart-user-library-path \
  COMPILER_ENV_LOG="$work/env" REAL_COMPILER="$real" LIONHEART_ENV_PROBE=compiler-package
if [ -n "${APPIMAGE_EXTRACT_AND_RUN:-}" ]; then set -- "$@" APPIMAGE_EXTRACT_AND_RUN="$APPIMAGE_EXTRACT_AND_RUN"; fi
printf '%s\n' "$@" | sort > "$work/launch.env"
status=0
env -i "$@" timeout -s TERM "${COMPILER_TEST_SECONDS:-600}" "$image" --config "$work/empty.ini" \
  --headless --native-build-only --native-status --native-workers 6 --disc "$disc" \
  > "$work/out.txt" 2> "$work/err.txt" || status=$?
# A timeout is sufficient only after the real first tier has completed.
test "$status" = 0 || test "$status" = 124
grep -Eq 'first tier [1-9][0-9]*[.]' "$work/err.txt"
# The builder intentionally directs compiler temporary files into its private
# cache; these are the only additions to the user's launch environment.
sed '/^TMPDIR=/d' "$work/launch.env" > "$work/expected.env"
count=0
for file in "$work/env/"*.env; do
  test -f "$file"
  sort "$file" | sed '/^TMPDIR=/d; /^ZIG_GLOBAL_CACHE_DIR=/d; /^ZIG_LOCAL_CACHE_DIR=/d' > "$work/actual.env"
  diff -u "$work/expected.env" "$work/actual.env"
  count=$((count + 1))
done
echo "PASS: $mode compiler, first tier built, $count compiler environments match"
