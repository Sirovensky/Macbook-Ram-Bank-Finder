#!/usr/bin/env bash
# scan-usb.sh -- find flashed BRR USB, mount ESP, print BRR state files.
#
# macOS cannot auto-mount the hybrid ISO because the partition scanner
# gets confused by the ISO9660+GPT overlay.  This script uses diskutil
# + manual mount of the ESP (partition 2, FAT).  After print it unmounts
# cleanly so the user can safely yank the stick.

set -u

finddev() {
    # Look for MT86PLUS_64 volume label (set by xorriso build).  If not
    # found by label, fall back to any external disk with an appended
    # EFI partition.
    local d
    d=$(diskutil list 2>/dev/null | awk '/MT86PLUS_64/ {print $NF}' | head -1)
    if [[ -z "$d" ]]; then
        # Fallback: look for any external disk with an "EFI" partition
        # (our hybrid ISO appends one at partition 2).
        d=$(diskutil list external 2>/dev/null | awk '
            /^\/dev\/disk/ { cur = $1; sub(":$", "", cur) }
            /EFI/ && cur { print cur "s2"; exit }')
    fi
    echo "$d"
}

DEV=$(finddev)
if [[ -z "$DEV" ]]; then
    echo "no BRR USB detected."
    echo "plug it in, then: diskutil list external"
    echo "to see what identifier it has."
    exit 1
fi

# Normalize to whole-disk identifier for reporting, and slice 2 for mount
# (hybrid ISO appends ESP as partition 2).
WHOLE=${DEV%s*}
ESP="${WHOLE}s2"

echo "==> BRR USB at $WHOLE, ESP at $ESP"

MNT=$(mktemp -d /tmp/brr-scan.XXXXXX)
trap 'umount "$MNT" 2>/dev/null || true; rmdir "$MNT" 2>/dev/null || true' EXIT

# Mount read-only so we don't race with firmware writes in case user
# reboots while we're poking around.
if ! sudo mount -t msdos -o rdonly "$ESP" "$MNT" 2>/dev/null; then
    echo "mount FAILED on $ESP.  try: diskutil list $WHOLE"
    exit 2
fi

echo "==> mounted $ESP on $MNT"
echo

for f in EFI/BRR/boot.txt EFI/BRR/state.txt EFI/BRR/pages.txt; do
    p="$MNT/$f"
    if [[ -f "$p" ]]; then
        sz=$(stat -f %z "$p")
        mt=$(stat -f "%Sm" -t "%Y-%m-%d %H:%M:%S" "$p")
        echo "--- $f ($sz bytes, modified $mt) ---"
        cat "$p"
        echo
    else
        echo "--- $f  MISSING ---"
        echo
    fi
done

echo "==> unmount + cleanup"
