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
  cp "$wrapper" "$work/bin/zig"
  driver=$work/bin/zig
  path=/usr/bin:/bin
else
  real=$(command -v clang || command -v gcc)
  cp "$wrapper" "$work/bin/clang"
  path=$work/bin:/usr/bin:/bin
  driver=$work/bin/clang
fi
: > "$work/empty.ini"
set -- HOME="$work/home" XDG_DATA_HOME="$work/data" XDG_CONFIG_HOME="$work/config" \
  PATH="$path" SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy LANG=C.UTF-8 \
  TMPDIR="${TMPDIR:-/tmp}" LD_LIBRARY_PATH=/lionheart-user-library-path \
  COMPILER_ENV_LOG="$work/env" REAL_COMPILER="$real" LIONHEART_ENV_PROBE=compiler-package
if [ -n "${APPIMAGE_EXTRACT_AND_RUN:-}" ]; then set -- "$@" APPIMAGE_EXTRACT_AND_RUN="$APPIMAGE_EXTRACT_AND_RUN"; fi
printf '%s\n' "$@" | sort > "$work/launch.env"
rm -f "$work/stopped"
env -i "$@" "$image" --config "$work/empty.ini" \
  --headless --native-compiler "$driver" --native-build-only --native-status --native-workers 6 --disc "$disc" \
  > "$work/out.txt" 2> "$work/err.txt" &
launcher=$!
# Signal the player itself: killing an AppImage launcher can kill its child
# before the player records the final native-build counters.
player=
tries=0
while [ -z "$player" ] && [ "$tries" -lt 300 ]; do
  for proc in /proc/[0-9]*; do
    case "$(readlink "$proc/exe" 2>/dev/null)" in
      */usr/bin/lionheart)
        if tr "\0" "\n" < "$proc/environ" 2>/dev/null | grep -qx "COMPILER_ENV_LOG=$work/env"; then
          player=${proc#/proc/}
          break
        fi ;;
    esac
  done
  tries=$((tries + 1))
  [ -n "$player" ] || sleep 0.1
done
if [ -z "$player" ]; then wait "$launcher" || true; cat "$work/err.txt" >&2; exit 1; fi
(sleep "${COMPILER_TEST_SECONDS:-600}"; : > "$work/stopped"; kill -TERM "$player" 2>/dev/null || true) &
timer=$!
status=0
wait "$launcher" || status=$?
kill "$timer" 2>/dev/null || true
wait "$timer" 2>/dev/null || true
# A timed stop is sufficient only after the real first tier has completed.
test "$status" = 0 || { test -f "$work/stopped" && test "$status" = 1; }
grep -Eq 'first tier (0[.][1-9]|[1-9][0-9]*[.])' "$work/err.txt"
if grep -Eq "native: .* [1-9][0-9]* failed" "$work/err.txt"; then
  echo "FAIL: a native set failed to build" >&2
  exit 1
fi
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
