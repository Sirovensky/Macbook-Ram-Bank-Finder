// SPDX-License-Identifier: GPL-2.0
//
// Pre-ExitBootServices boot-mode selector for A1990 (T2 Mac).
//
// This translation unit is invoked once from boot/efisetup.c (via patch
// 0007) while UEFI Boot Services are still alive.  It inspects the grub
// kernel cmdline and returns a bitmask of BRR_FLAG_* bits that memtest
// reads from boot_params_t::brr_flags.
//
// In the current 3-entry grub flow the only token that matters is
// "brr_fast", which maps to BRR_FLAG_SKIP_COUNTDOWNS (used by the photo-
// pause countdowns in src/calibration.c).  Everything else — interactive
// menu, NVRAM state auto-chainload, per-mode trial flags — lived here
// in earlier revisions and has been removed; see the git history for
// the old code if you need to resurrect any of it.
//
// Compilation: lives in system/board/ via scripts/apply.sh and builds
// with the same INC_DIRS as the rest of the tree.  We can include efi.h
// from memtest86plus/boot/ directly.
//
// Copyright (C) 2024 A1990-memtest contributors.

#include <stdint.h>

#include "efi.h"        /* efi_system_table_t — kept for future expansion */
#include "efi_menu.h"

// Case-sensitive substring search — stdlib is unavailable this early in
// the boot path.  Safe against NULL inputs.
static int cmdline_has(const char *hay, const char *needle)
{
    if (!hay || !needle) return 0;
    for (const char *p = hay; *p; p++) {
        const char *a = p, *b = needle;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

uint32_t efi_menu(void *sys_table_arg, void *image_handle_arg, const char *cmdline)
{
    (void)sys_table_arg;
    (void)image_handle_arg;

    if (cmdline_has(cmdline, "brr_fast"))
        return BRR_FLAG_SKIP_COUNTDOWNS;

    return 0;
}
