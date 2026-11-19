#!/bin/sh
# Prepares a fresh Ubuntu container, as root, to build a release package:
#   packaging/build-environment.sh linux     in ubuntu:22.04 (the AppImage)
#   packaging/build-environment.sh windows   in ubuntu:24.04 (the Windows zip)
# The release workflow runs it in pinned images, and anyone can run it in the
# same images to rebuild a release. The package archive is read from a dated
# snapshot, so the compilers and tools are the same on every run: with the
# pinned sources, that is what makes two builds give the same bytes.
set -eu
SNAPSHOT=20261101T000000Z
case "${1:-}" in
  linux)
    release=jammy
    # SDL2 is built against the headers of every video, audio and input
    # backend it loads at run time; it links none of them.
    packages="build-essential cmake pkg-config git ca-certificates xz-utils squashfs-tools
      libvulkan-dev libdbus-1-dev libssl-dev
      libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxinerama-dev libxfixes-dev
      libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libdrm-dev libgbm-dev
      libasound2-dev libpulse-dev libpipewire-0.3-dev libudev-dev" ;;
  windows)
    release=noble
    packages="build-essential cmake pkg-config git ca-certificates xz-utils make libvulkan-dev zlib1g-dev" ;;
  *)
    echo "usage: $0 linux|windows" >&2
    exit 2 ;;
esac
# shellcheck source=/dev/null
. /etc/os-release
if [ "$VERSION_CODENAME" != "$release" ]; then
  echo "$1 packages are built on Ubuntu $release, not $VERSION_CODENAME" >&2
  exit 2
fi
export DEBIAN_FRONTEND=noninteractive
# The snapshot service answers over HTTPS only. Certificates enter no package.
if [ ! -f /etc/ssl/certs/ca-certificates.crt ]; then
  apt-get update
  apt-get install -y --no-install-recommends ca-certificates
fi
rm -f /etc/apt/sources.list.d/ubuntu.sources
cat > /etc/apt/sources.list <<EOF
deb https://snapshot.ubuntu.com/ubuntu/$SNAPSHOT $release main universe
deb https://snapshot.ubuntu.com/ubuntu/$SNAPSHOT $release-updates main universe
deb https://snapshot.ubuntu.com/ubuntu/$SNAPSHOT $release-security main universe
EOF
apt-get update
# shellcheck disable=SC2086
apt-get install -y --no-install-recommends $packages
