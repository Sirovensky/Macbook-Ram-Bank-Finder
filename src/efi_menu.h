// SPDX-License-Identifier: GPL-2.0
#ifndef EFI_MENU_H
#define EFI_MENU_H
/**
 * \file
 *
 * Pre-ExitBootServices interactive menu for A1990 (T2 Mac).
 *
 * Must be called while UEFI boot services are still active so that
 * SystemTable->ConIn is functional.  On T2 MacBooks the internal
 * keyboard is proxied through a vendor USB HID path that the
 * memtest86plus xHCI driver cannot reach post-ExitBootServices; the
 * UEFI firmware's own HID stack (exposed via ConIn) is the only
 * reliable input path.
 *
 * Returns a bitmask of BRR_FLAG_* values to be stored in
 * boot_params->brr_flags before ExitBootServices() is called.
 *//*
 * Copyright (C) 2024 A1990-memtest contributors.
 */

#include <stdint.h>

/*
 * Flag bits stored in boot_params_t::brr_flags and tested by the
 * application after ExitBootServices.
 */
#define BRR_FLAG_SKIP_COUNTDOWNS        (1u << 0)   /* skip timed photo pauses        */
#define BRR_FLAG_CALIBRATE_ONLY         (1u << 1)   /* halt after calibration         */
#define BRR_FLAG_AUTO_REBOOT_AFTER_PASS (1u << 2)   /* save NVRAM + reboot after pass */
#define BRR_FLAG_AUTO_TRIAL_CHIP        (1u << 3)   /* chip-mode trial (vs page-mode) */

/*
 * Convenience single-bit flags for the two auto-trial modes.  Both imply
 * SKIP_COUNTDOWNS | AUTO_REBOOT_AFTER_PASS; the callers can check these
 * independently of the lower-level bits.
 *
 *   TRIAL_PAGE  — page-granularity mask: flush BrrBadPages, set
 *                 BrrMaskState = "TRIAL_PENDING_PAGE", reboot.
 *   TRIAL_CHIP  — chip-level mask: flush BrrBadChips (from
 *                 badmem_log_record_chip()), set
 *                 BrrMaskState = "TRIAL_PENDING_CHIP", reboot.
 */
#define BRR_FLAG_TRIAL_PAGE             (1u << 4)   /* [P] page-mode auto-trial       */
#define BRR_FLAG_TRIAL_CHIP             (1u << 5)   /* [C] chip-mode auto-trial       */

/**
 * efi_menu() - Display the pre-boot menu and collect a keypress.
 *
 * @sys_table: pointer to the UEFI system table (must be non-NULL).
 *
 * Displays a 30-second countdown menu via ConOut / ConIn.  Returns a
 * bitmask of zero or more BRR_FLAG_* bits according to user choice.
 * On timeout the menu acts as if Enter was pressed (run all tests,
 * no special flags).
 *
 * Call this BEFORE set_efi_info_and_exit_boot_services().
 */
uint32_t efi_menu(void *sys_table, void *image_handle);

#endif /* EFI_MENU_H */
