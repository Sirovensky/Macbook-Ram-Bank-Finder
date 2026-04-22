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
#
# Build in two steps so we can inject our EFI helper binaries into the ESP
# image before the ISO is assembled.  Step 1: build the ESP image only.
make -C "$bld" grub-esp.img GRUB_CFG=a1990

# Build EFI helper apps (mask-shim.efi, install.efi).
echo "==> building EFI helper apps"
make -C "$here/efi"

# Inject EFI helper binaries into the ESP image while it is still a loose FAT
# image (before xorrisofs embeds it as an appended partition).
esp_img="$bld/grub-esp.img"
shim_efi="$here/efi/mask-shim/mask-shim.efi"
install_efi="$here/efi/mask-install/install.efi"
revert_efi="$here/efi/revert/revert.efi"

missing=0
for f in "$shim_efi" "$install_efi" "$revert_efi"; do
    [[ -f "$f" ]] || { echo "warning: missing EFI binary: $f" >&2; missing=1; }
done

if [[ $missing -eq 0 ]]; then
    echo "==> injecting mask-shim.efi, install.efi, revert.efi into ESP"
    mcopy -i "$esp_img" "$shim_efi"    ::/EFI/BOOT/mask-shim.efi
    mcopy -i "$esp_img" "$install_efi" ::/EFI/BOOT/install.efi
    mcopy -i "$esp_img" "$revert_efi"  ::/EFI/BOOT/revert.efi
else
    echo "warning: one or more EFI binaries missing; ISO will be incomplete" >&2
fi

# Step 2: assemble the final ISO (uses the now-augmented grub-esp.img).
make -C "$bld" grub-memtest.iso GRUB_CFG=a1990

iso_src="$bld/grub-memtest.iso"
iso_dst="$out/a1990-memtest.iso"
cp "$iso_src" "$iso_dst"

echo
echo "==> wrote $iso_dst"
echo "    flash with balena Etcher, Rufus, or:"
echo "      sudo dd if=$iso_dst of=/dev/diskN bs=4M status=progress"
