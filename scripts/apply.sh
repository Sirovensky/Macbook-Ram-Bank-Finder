#!/usr/bin/env bash
# Copy src/ + generated topology into memtest86plus and wire into build.
# Idempotent.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mt="$here/memtest86plus"

[[ -d "$mt" ]] || { echo "error: $mt not found"; exit 1; }

# 1. Generate board_table.c from topology/*.yaml.
gen="$here/scripts/gen-topology.py"
out="$here/src/board_table.c"
if [[ -x "$gen" ]]; then
    echo "==> generating $out from topology/"
    "$gen" "$here/topology" "$out"
elif command -v python3 >/dev/null; then
    echo "==> generating $out from topology/"
    python3 "$gen" "$here/topology" "$out"
else
    echo "error: python3 required for topology generation"
    exit 1
fi

# 2. Sync into memtest86plus/system/board/.
dst="$mt/system/board"
echo "==> syncing src/ -> $dst/"
mkdir -p "$dst"
cp "$here"/src/*.c "$here"/src/*.h "$dst/"

# Rename smbios.c to avoid linker clash with memtest86plus's own system/smbios.c.
[[ -f "$dst/smbios.c" ]] && mv "$dst/smbios.c" "$dst/smbios_wrap.c"

# 3. Wire object files into Makefile.
mk="$mt/build/x86_64/Makefile"
objs=(
    "board/board_topology.o"
    "board/board_table.o"
    "board/cfl_decode.o"
    "board/error_hook.o"
    "board/badmem_log.o"
    "board/smbios_wrap.o"
    "board/calibration.o"
    "board/efi_menu.o"
)

if ! grep -q "board/board_topology.o" "$mk"; then
    echo "==> patching Makefile SYS_OBJS"
    inject=""
    for o in "${objs[@]}"; do inject+="           system/${o} \\\\\n"; done
    awk -v inject="$inject" '
        /system\/x86\/vmem\.o/ && !done {
            # Ensure line ends with continuation backslash.
            line = $0
            sub(/[[:space:]]+$/, "", line)
            if (substr(line, length(line)) != "\\") line = line " \\"
            print line
            printf "%s", inject
            done = 1; next
        }
        { print }
    ' "$mk" > "$mk.new" && mv "$mk.new" "$mk"
    sed -i.bak 's|-I../../tests|-I../../system/board -I../../tests|' "$mk"
    rm -f "$mk.bak"
else
    # Makefile already has the initial board/ objects; additive injection for
    # any object added later (idempotent: grep before inserting).
    for o in "${objs[@]}"; do
        if ! grep -q "system/${o}" "$mk"; then
            echo "==> injecting system/${o} into Makefile SYS_OBJS"
            # Insert after the last "           system/board/..." continuation
            # line (the SYS_OBJS entries use exactly 11 leading spaces).
            awk -v obj="           system/${o} \\" '
                /^           system\/board\/.*\.o/ { last = NR }
                { lines[NR] = $0 }
                END {
                    for (i = 1; i <= NR; i++) {
                        print lines[i]
                        if (i == last) print obj
                    }
                }
            ' "$mk" > "$mk.new" && mv "$mk.new" "$mk"
        fi
    done
fi

# 3b. Add a pattern rule for system/board/%.o (upstream's system/%.o rule
# only mkdirs `system`, not `system/board`).
if ! grep -q "^system/board/%.o:" "$mk"; then
    echo "==> adding system/board pattern rule"
    awk '
        /^system\/%\.o: \.\.\/\.\.\/system\/%\.c/ && !done {
            done = 1
            print "system/board/%.o: ../../system/board/%.c"
            print "\t@mkdir -p system/board"
            print "\t$(CC) -c $(CFLAGS) $(OPT_SMALL) $(INC_DIRS) -o $@ $< -MMD -MP -MT $@ -MF $(@:.o=.d)"
            print ""
        }
        { print }
    ' "$mk" > "$mk.new" && mv "$mk.new" "$mk"
fi

# 3c. Stage custom GRUB cfg (forces larger gfxmode so text is readable on Retina).
if [[ -d "$here/grub" ]]; then
    echo "==> staging grub/ -> $mt/grub/"
    cp "$here"/grub/a1990-*.cfg "$mt/grub/"
fi

# 4. Apply source patches.
apply_patch() {
    local pf="$1" marker="$2" file="$3"
    if grep -q "$marker" "$mt/$file" 2>/dev/null; then
        echo "==> patch already applied: $pf"
    else
        echo "==> applying $pf"
        (cd "$mt" && patch -p1 < "$here/patches/$pf")
    fi
}

apply_patch 0001-hook-error-reporter.patch     "board_report_error"    "app/error.c"
apply_patch 0002-startup-calibration.patch     "board_calibrate"       "app/main.c"
apply_patch 0003-font-scale.patch              "lfb_scale"             "system/screen.c"
apply_patch 0004-smbios3.patch                 "parse_smbios3_anchor"  "system/smbios.c"
apply_patch 0005-badmem-log-dump.patch         "badmem_log_dump"       "app/main.c"
# Track B: EFI pre-boot menu + boot_params flags
apply_patch 0006-bootparams-a1990-flags.patch  "a1990_flags"           "boot/bootparams.h"
apply_patch 0007-efi-pre-boot-menu.patch       "efi_menu"              "boot/efisetup.c"
# Track C: NVRAM auto-save and auto-reboot
apply_patch 0008-expose-efi-rt.patch           "hwctrl_get_efi_rt"     "system/x86/hwctrl.c"
apply_patch 0009-nvram-auto-reboot.patch       "badmem_log_flush_nvram" "app/main.c"

echo "==> ready. build: cd $mt/build/x86_64 && make"
