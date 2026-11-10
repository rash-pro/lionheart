#!/bin/sh
# Runs a packaged AppImage without discs:
#   packaging/linux/test-appimage.sh Lionheart-VERSION-x86_64.AppImage
# 1. Headless, it must stop at the missing disc, as the plain player does.
# 2. The player must start with exactly the environment it was launched with.
#    Every program it starts (the compiler, a system compiler, a browser)
#    inherits that environment, so anything the AppImage runtime or AppRun
#    added or changed, such as a library path into the package, would reach
#    them. The player's own environment is read from /proc while it waits on
#    its setup screen.
# Without FUSE, set APPIMAGE_EXTRACT_AND_RUN=1; it is passed through.
set -eu
image=$(readlink -f "$1")
work=$(mktemp -d "${TMPDIR:-/tmp}/lionheart-appimage.XXXXXX")
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/home" "$work/data" "$work/config"
: > "$work/empty.ini"
marker="lionheart-env-$$-$(date +%s)"

# The launch environment, written once: the expected set, and the arguments.
set -- HOME="$work/home" XDG_DATA_HOME="$work/data" XDG_CONFIG_HOME="$work/config" PATH=/usr/bin:/bin \
  LANG=C.UTF-8 SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy SDL_RENDER_DRIVER=software \
  LD_LIBRARY_PATH=/lionheart-user-library-path LIONHEART_ENV_PROBE="$marker"
if [ -n "${APPIMAGE_EXTRACT_AND_RUN:-}" ]; then set -- "$@" APPIMAGE_EXTRACT_AND_RUN="$APPIMAGE_EXTRACT_AND_RUN"; fi
if [ -n "${TMPDIR:-}" ]; then set -- "$@" TMPDIR="$TMPDIR"; fi

status=0
env -i "$@" "$image" --headless --config "$work/empty.ini" > "$work/out.txt" 2> "$work/err.txt" || status=$?
if [ "$status" -ne 2 ] || ! grep -q '^Provide FF8 USA disc 1 with --disc PATH' "$work/err.txt"; then
  echo "FAIL: headless start without discs exited $status:" >&2
  cat "$work/err.txt" >&2
  exit 1
fi
echo "PASS: headless start stops at the missing disc"

printf 'wait 1200\nquit\n' > "$work/wait.txt"
env -i "$@" "$image" --config "$work/empty.ini" --gui-test "$work/wait.txt" > "$work/gui-out.txt" 2> "$work/gui-err.txt" &
launcher=$!
player=
tries=0
while [ -z "$player" ] && [ "$tries" -lt 300 ]; do
  for proc in /proc/[0-9]*; do
    case "$(readlink "$proc/exe" 2>/dev/null)" in
      */usr/bin/lionheart)
        if tr '\0' '\n' < "$proc/environ" 2>/dev/null | grep -qx "LIONHEART_ENV_PROBE=$marker"; then
          player=${proc#/proc/}
          tr '\0' '\n' < "$proc/environ" | sort > "$work/player.env"
        fi ;;
    esac
  done
  tries=$((tries + 1))
  [ -n "$player" ] || sleep 0.1
done
kill "$launcher" 2>/dev/null || true
[ -z "$player" ] || kill "$player" 2>/dev/null || true
wait "$launcher" 2>/dev/null || true
if [ -z "$player" ]; then
  echo "FAIL: the player did not start from the AppImage:" >&2
  cat "$work/gui-err.txt" >&2
  exit 1
fi
printf '%s\n' "$@" | sort > "$work/launch.env"
if ! diff -u "$work/launch.env" "$work/player.env" > "$work/env.diff"; then
  echo "FAIL: the player's environment differs from the one it was launched with:" >&2
  cat "$work/env.diff" >&2
  exit 1
fi
echo "PASS: the player and its children see the launch environment unchanged"
