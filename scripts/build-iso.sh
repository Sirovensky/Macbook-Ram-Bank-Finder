#!/usr/bin/env bash
# Build hybrid ISO from memtest.efi produced in memtest86plus/build/x86_64.
# Needs: xorriso, grub-mkrescue (from GRUB2). macOS lacks grub-mkrescue
# reliably; run inside Linux or Docker if not found.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mt="$here/memtest86plus"
out="$here/dist"
mkdir -p "$out"

efi="$mt/build/x86_64/memtest.efi"
bin="$mt/build/x86_64/memtest.bin"

if [[ ! -f "$efi" && ! -f "$bin" ]]; then
    echo "error: no memtest built. cd $mt/build/x86_64 && make" >&2
    exit 1
fi

if ! command -v grub-mkrescue >/dev/null; then
    echo "error: grub-mkrescue not found. install grub2 (Linux) or run in Docker." >&2
    echo "       macOS: 'brew install grub' does not supply mkrescue reliably;"
    echo "       easiest: docker run --rm -v \"$here:/work\" -w /work debian:stable bash -c \\"
    echo "                'apt-get update && apt-get install -y grub-common grub-pc-bin grub-efi-amd64-bin xorriso && ./scripts/build-iso.sh'"
    exit 1
fi

staging="$(mktemp -d)"
trap 'rm -rf "$staging"' EXIT

mkdir -p "$staging/boot/grub"
cp "$efi" "$staging/boot/memtest.efi" 2>/dev/null || true
cp "$bin" "$staging/boot/memtest.bin" 2>/dev/null || true

cat > "$staging/boot/grub/grub.cfg" <<'EOF'
set timeout=3
set default=0

menuentry "a1990-memtest (UEFI)" {
    chainloader /boot/memtest.efi
}

menuentry "a1990-memtest (BIOS)" {
    linux16 /boot/memtest.bin
}

menuentry "a1990-memtest (calibration dump mode)" {
    chainloader /boot/memtest.efi opts=dump-imc
}
EOF

iso="$out/a1990-memtest.iso"
grub-mkrescue -o "$iso" "$staging" \
    -- -volid A1990MEMTEST

echo "==> wrote $iso"
echo "    write to USB with:"
echo "      sudo dd if=$iso of=/dev/diskN bs=4M status=progress"
