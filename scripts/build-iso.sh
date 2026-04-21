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

echo "==> make -C $bld iso"
make -C "$bld" iso

iso_src="$bld/memtest.iso"
iso_dst="$out/a1990-memtest.iso"
cp "$iso_src" "$iso_dst"

echo
echo "==> wrote $iso_dst"
echo "    flash with balena Etcher, Rufus, or:"
echo "      sudo dd if=$iso_dst of=/dev/diskN bs=4M status=progress"
