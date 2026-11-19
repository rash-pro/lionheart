#!/bin/sh
# Every rejection must identify the damage introduced into an otherwise valid package.
set -eu
image=$(readlink -f "$1")
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/package-negative.XXXXXX")
trap 'rm -rf "$work"' EXIT
cmake -DPACKAGE="$image" -DWORK="$work/good" -P "$root/tools/check_package.cmake"
offset=$("$image" --appimage-offset)
dd if="$image" of="$work/runtime" bs=1 count="$offset" status=none
for kind in notice library version; do
  cp -a "$work/good/squashfs-root" "$work/$kind"
  case $kind in
    notice) rm "$work/$kind/usr/share/doc/lionheart/licences/AppImage-source.txt"; expected='missing notice AppImage-source.txt' ;;
    library) cp "$work/$kind/usr/lib/libcurl.so.4" "$work/$kind/usr/lib/libunlisted.so.1"; expected='belongs to no known component' ;;
    version) sed -i 's/X-AppImage-Version=.*/X-AppImage-Version=99.99.99/' "$work/$kind/lionheart.desktop"; expected='lacks X-AppImage-Version' ;;
  esac
  nice -n 10 mksquashfs "$work/$kind" "$work/payload" -noappend -no-xattrs -all-root -comp zstd -processors 6 -no-progress -quiet
  mkdir "$work/$kind-package"
  broken=$work/$kind-package/$(basename "$image")
  cat "$work/runtime" "$work/payload" > "$broken"
  chmod +x "$broken"
  if cmake -DPACKAGE="$broken" -DWORK="$work/check" -P "$root/tools/check_package.cmake" > "$work/rejection" 2>&1; then
    echo "FAIL: accepted damaged $kind package" >&2; exit 1
  fi
  grep -q "$expected" "$work/rejection"
  rm "$work/payload"
  echo "PASS: rejected $kind damage ($expected)"
done
