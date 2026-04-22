#!/usr/bin/env bash
# Delegate to upstream memtest86plus's own `make iso` target and stage
# the result in dist/. Needs: xorriso, mtools, dosfstools, grub tools.
# Use `make docker` on macOS.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mt="$here/memtest86plus"
bld="$mt/build/x86_64"
out="$here/dist"
mkdir -p "$out"

if [[ ! -x "$bld/mt86plus" && ! -f "$bld/mt86plus" ]]; then
    echo "error: mt86plus not built. run 'make build' first." >&2
    exit 1
fi

if ! command -v xorrisofs >/dev/null; then
    echo "error: xorrisofs not found. install xorriso/mtools/dosfstools,"
    echo "       or use 'make docker' on macOS."
    exit 1
fi

echo "==> make -C $bld grub-memtest.iso GRUB_CFG=a1990"
# grub-memtest.iso uses GRUB as intermediate bootloader so we can force
# gfxmode (Retina defaults to native 2880x1800 which makes text unreadable).
# GRUB_CFG=a1990 pulls in grub/a1990-{efi,legacy}.cfg staged by apply.sh.
make -C "$bld" grub-memtest.iso GRUB_CFG=a1990

iso_src="$bld/grub-memtest.iso"
iso_dst="$out/a1990-memtest.iso"
cp "$iso_src" "$iso_dst"

echo
echo "==> wrote $iso_dst"
echo "    flash with balena Etcher, Rufus, or:"
echo "      sudo dd if=$iso_dst of=/dev/diskN bs=4M status=progress"
