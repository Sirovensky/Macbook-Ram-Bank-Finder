#!/usr/bin/env bash
# Copy our src/ into memtest86plus and wire it into the build.
# Idempotent.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mt="$here/memtest86plus"

[[ -d "$mt" ]] || { echo "error: $mt not found"; exit 1; }

dst="$mt/system/a1990"
echo "==> syncing src/ -> $dst/"
mkdir -p "$dst"
cp "$here"/src/*.c "$here"/src/*.h "$dst/"

# Rename smbios.c to avoid linker clash with memtest86plus's own system/smbios.c.
[[ -f "$dst/smbios.c" ]] && mv "$dst/smbios.c" "$dst/smbios_wrap.c"

mk="$mt/build/x86_64/Makefile"

if ! grep -q "a1990/a1990_topology.o" "$mk"; then
    echo "==> patching Makefile SYS_OBJS"
    awk '
        /system\/x86\/vmem\.o/ && !done {
            print $0
            print "           system/a1990/a1990_topology.o \\"
            print "           system/a1990/cfl_decode.o \\"
            print "           system/a1990/error_hook.o \\"
            print "           system/a1990/smbios_wrap.o \\"
            print "           system/a1990/calibration.o \\"
            done = 1; next
        }
        { print }
    ' "$mk" > "$mk.new" && mv "$mk.new" "$mk"
    sed -i.bak 's|-I../../tests|-I../../system/a1990 -I../../tests|' "$mk"
    rm -f "$mk.bak"
elif ! grep -q "a1990/calibration.o" "$mk"; then
    echo "==> adding calibration.o to SYS_OBJS"
    awk '
        /a1990\/smbios_wrap\.o/ && !done {
            print $0
            print "           system/a1990/calibration.o \\"
            done = 1; next
        }
        { print }
    ' "$mk" > "$mk.new" && mv "$mk.new" "$mk"
fi

apply_patch() {
    local pf="$1" marker="$2" file="$3"
    if grep -q "$marker" "$mt/$file" 2>/dev/null; then
        echo "==> patch already applied: $pf"
    else
        echo "==> applying $pf"
        (cd "$mt" && patch -p1 < "$here/patches/$pf")
    fi
}

apply_patch 0001-hook-error-reporter.patch     "a1990_report_error"    "app/error.c"
apply_patch 0002-startup-calibration.patch     "a1990_calibrate"       "app/main.c"

echo "==> ready. build: cd $mt/build/x86_64 && make"
