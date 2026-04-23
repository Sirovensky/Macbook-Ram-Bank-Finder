// SPDX-License-Identifier: GPL-2.0
#ifndef EFI_MENU_H
#define EFI_MENU_H
/**
 * \file
 *
 * Pre-ExitBootServices boot-mode selector for A1990 (T2 Mac).
 *
 * History: this file once hosted an interactive pre-boot menu with auto-
 * NVRAM-save and chainload-to-shim paths.  The current 3-entry grub flow
 * (full test / fast test / setup NVRAM hook) doesn't need any of that —
 * the memtest entries simply run memtest, and the mask-setup entry is
 * handled by brr-entry.efi (a separate EFI app).  efi_menu() is now a
 * thin cmdline parser that returns BRR_FLAG_SKIP_COUNTDOWNS when grub
 * passed "brr_fast", otherwise 0.
 *
 * Kept as a function rather than inlined because the call site in
 * boot/efisetup.c (patch 0007) already invokes it, and the T2 ConIn
 * access pattern may be useful again if we re-add an interactive path.
 *
 * Returns a bitmask of BRR_FLAG_* values to be stored in
 * boot_params->brr_flags before ExitBootServices() is called.
 *//*
 * Copyright (C) 2024 A1990-memtest contributors.
 */

#include <stdint.h>

/*
 * Flag bits stored in boot_params_t::brr_flags and tested by the
 * application after ExitBootServices.  Only SKIP_COUNTDOWNS and
 * CALIBRATE_ONLY are currently consumed (src/calibration.c).
 */
#define BRR_FLAG_SKIP_COUNTDOWNS        (1u << 0)   /* skip timed photo pauses */
#define BRR_FLAG_CALIBRATE_ONLY         (1u << 1)   /* halt after calibration  */

/**
 * efi_menu() - Parse boot cmdline and return flag bitmask.
 *
 * @sys_table:    UEFI system table pointer (may be NULL; only used for
 *                early console output on error).
 * @image_handle: loaded-image handle (reserved for future chainload use,
 *                currently unused).
 * @cmdline:      ASCII boot argument string from grub (may be NULL).
 *
 * Currently recognised tokens:
 *   "brr_fast"  -> BRR_FLAG_SKIP_COUNTDOWNS
 *
 * Returns 0 for the default full-test flow.
 * Call this BEFORE set_efi_info_and_exit_boot_services().
 */
uint32_t efi_menu(void *sys_table, void *image_handle, const char *cmdline);

#endif /* EFI_MENU_H */
