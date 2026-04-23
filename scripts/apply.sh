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
mkdir -p "$dst/imc"
cp "$here"/src/*.c "$here"/src/*.h "$dst/"
cp "$here"/src/imc/*.c "$dst/imc/"

# Rename smbios.c to avoid linker clash with memtest86plus's own system/smbios.c.
[[ -f "$dst/smbios.c" ]] && mv "$dst/smbios.c" "$dst/smbios_wrap.c"

# 3. Wire object files into Makefile.
mk="$mt/build/x86_64/Makefile"
objs=(
    "board/board_topology.o"
    "board/board_table.o"
    "board/cfl_decode.o"
    "board/imc_dispatch.o"
    "board/imc/cfl_skl_kbl.o"
    "board/error_hook.o"
    "board/badmem_log.o"
    "board/smbios_wrap.o"
    "board/calibration.o"
    "board/efi_menu.o"
    "board/decoder_selftest.o"
    "board/skip.o"
    "board/beeper.o"
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

# 3b. Add pattern rules for system/board/%.o and system/board/imc/%.o
# (upstream's system/%.o rule only mkdirs `system`, not the subdirs).
#
# IMPORTANT: inject BOTH rules in a single awk pass, before the upstream
# `system/%.o: ../../system/%.c` rule.  Earlier versions injected the imc
# rule by matching the already-injected board rule and splicing in between
# the board rule's target line and its body — which orphaned the body and
# left `system/board/%.o:` with no recipe (so .d files failed to write
# into a non-existent directory).  Keep both rules adjacent and ordered.
if ! grep -q "^system/board/%.o:" "$mk"; then
    echo "==> adding system/board + system/board/imc pattern rules"
    awk '
        /^system\/%\.o: \.\.\/\.\.\/system\/%\.c/ && !done {
            done = 1
            print "system/board/%.o: ../../system/board/%.c"
            print "\t@mkdir -p system/board"
            print "\t$(CC) -c $(CFLAGS) $(OPT_SMALL) $(INC_DIRS) -o $@ $< -MMD -MP -MT $@ -MF $(@:.o=.d)"
            print ""
            print "system/board/imc/%.o: ../../system/board/imc/%.c"
            print "\t@mkdir -p system/board/imc"
            print "\t$(CC) -c $(CFLAGS) $(OPT_SMALL) $(INC_DIRS) -o $@ $< -MMD -MP -MT $@ -MF $(@:.o=.d)"
            print ""
        }
        { print }
    ' "$mk" > "$mk.new" && mv "$mk.new" "$mk"
elif ! grep -q "^system/board/imc/%.o:" "$mk"; then
    # Backward-compat: older Makefile already had board rule but not imc.
    # Inject imc rule immediately after the board rule body (detect end of
    # body via blank line following the $(CC) line).
    echo "==> adding system/board/imc pattern rule (board rule already present)"
    awk '
        /^system\/board\/%\.o: \.\.\/\.\.\/system\/board\/%\.c/ { in_board = 1 }
        in_board && /^$/ && !done {
            print $0
            print "system/board/imc/%.o: ../../system/board/imc/%.c"
            print "\t@mkdir -p system/board/imc"
            print "\t$(CC) -c $(CFLAGS) $(OPT_SMALL) $(INC_DIRS) -o $@ $< -MMD -MP -MT $@ -MF $(@:.o=.d)"
            print ""
            done = 1
            in_board = 0
            next
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
apply_patch 0006-bootparams-a1990-flags.patch  "brr_flags"             "boot/bootparams.h"
apply_patch 0007-efi-pre-boot-menu.patch       "efi_menu"              "boot/efisetup.c"
# Track C: NVRAM auto-save and auto-reboot
apply_patch 0008-expose-efi-rt.patch           "hwctrl_get_efi_rt"     "system/x86/hwctrl.c"
apply_patch 0009-nvram-auto-reboot.patch       "badmem_log_flush_nvram" "app/main.c"
# T2 auto-reboot: memtest's panic handler waits for keypress that never
# comes on A1990 (T2 blocks post-EBS keyboard).  Patch adds 60 s timer.
apply_patch 0010-panic-auto-reboot.patch       "auto-reboot in 60 s"   "app/x86/interrupt.c"
# T2 non-blocking scroll: upstream scroll() blocks on <Enter> if scroll_lock
# flips ON (stray space-bar event from T2 HID proxy).  Patch short-circuits
# the wait loop.  See patch 0011 header comment for full rationale.
apply_patch 0011-scroll-never-block.patch      "BRR: non-blocking scroll" "app/display.c"
# T2 common_err() deadlock: check_input() inside common_err (under
# error_mutex) would open config_menu() on a stray '1' keycode from the
# T2 HID proxy, freezing the test.  Drop the call — do_tick's own BSP-
# only check_input (outside the mutex) still handles ESC.
apply_patch 0012-no-check-input-in-common-err.patch "BRR: do NOT call check_input" "app/error.c"
# Early-bail on error burst.  Detects contiguous-address error run
# (indicates a single bad region saturating the CPU) AND absolute error
# count cap.  Flipping `bail` exits current test, main loop advances
# to next.  The bad region is still recorded in BrrBadPages; the shim
# expands each logged PA +/- 1 MiB on the next boot so the masked
# range covers the full failing chip region without ever re-running
# the problematic test pattern against it.
apply_patch 0013-early-bail-on-many-errors.patch "BRR: fail-safe error-burst detector" "app/error.c"
# Hook board_prune_vm_map() into setup_vm_map() so each new window is
# pruned against the accumulated skip list before tests run on it.
apply_patch 0015-vm-map-prune-hook.patch "board_prune_vm_map" "app/main.c"
# Pass-progress bar: switch from tick-ratio (breaks when prune splits
# vm_map entries) to completed-test-count metric.  Bounded at 100%
# by definition.
apply_patch 0016-pass-progress-test-count.patch "BRR: pass% = completed tests" "app/display.c"
# Suppress FAIL banner on A1990 (T2 keyboard can't dismiss it; overlay
# covers end-of-pass summary + NVRAM messages + auto-reboot countdown).
apply_patch 0017-no-fail-banner.patch "BRR: suppress the FAIL banner" "app/display.c"
# T2-hardened reboot: try UEFI ResetSystem WARM+COLD, cf9, KB-ctrl,
# then triple-fault as guaranteed last resort.  Default reboot()
# fell through on A1990 because T2 blocks every soft-reset path.
apply_patch 0018-reboot-t2-hardened.patch "BRR: verbose reboot diagnostics" "system/x86/hwctrl.c"
# Re-cache brr_flags after startup64.S BSS zero: efi_menu() wrote
# g_brr_flags_cached pre-ExitBootServices but the asm BSS-zero loop
# (startup64.S:258-268, triggered on first_boot=1) wipes it before
# main() runs.  Copy bp->brr_flags (UEFI-pool mem, not in BSS, still
# pristine here) into the BSS cache as the first act of global_init.
apply_patch 0019-brr-flags-recache.patch "BRR: re-cache brr_flags post-BSS-zero" "app/main.c"

echo "==> ready. build: cd $mt/build/x86_64 && make"
