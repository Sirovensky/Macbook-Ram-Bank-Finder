// SPDX-License-Identifier: GPL-2.0
//
// Shared install / uninstall logic used by both mask-install and mask-shim.
//
// Copyright (C) 2024 A1990-memtest contributors.

#ifndef MASK_OPS_H
#define MASK_OPS_H

#include "../efi_types.h"

// ---------------------------------------------------------------------------
// NVRAM vendor GUID shared across all BRR EFI modules.
// {3E3E9DB2-1A2B-4B5C-9D1E-5F6A7B8C9D0E}
// ---------------------------------------------------------------------------
extern const EFI_GUID BRR_GUID;

// ---------------------------------------------------------------------------
// State strings (NUL-terminated ASCII) used as NVRAM values.
// ---------------------------------------------------------------------------
#define BRR_STATE_NONE                    "NONE"
#define BRR_STATE_TRIAL_PENDING_PAGE      "TRIAL_PENDING_PAGE"
#define BRR_STATE_TRIAL_PENDING_CHIP      "TRIAL_PENDING_CHIP"
#define BRR_STATE_TRIAL_BOOTED            "TRIAL_BOOTED"
#define BRR_STATE_PERMANENT_UNCONFIRMED   "PERMANENT_UNCONFIRMED"
#define BRR_STATE_PERMANENT_CONFIRMED     "PERMANENT_CONFIRMED"

// ---------------------------------------------------------------------------
// NVRAM variable names (UTF-16, declared as CHAR16 arrays in mask_ops.c).
// ---------------------------------------------------------------------------
extern const CHAR16 BRR_VARNAME_STATE[];
extern const CHAR16 BRR_VARNAME_BADPAGES[];
extern const CHAR16 BRR_VARNAME_BADCHIPS[];
extern const CHAR16 BRR_VARNAME_ORIGBOOT[];     // "BrrBackupBootOrder" (canonical)
extern const CHAR16 BRR_VARNAME_ORIGBOOT_LEGACY[]; // "A1990MaskOriginalBootOrder" (legacy fallback — keep old name)
extern const CHAR16 BRR_VARNAME_BOOTSLOT[];
extern const CHAR16 BRR_VARNAME_BOOTENTRIES[];  // "BrrBackupBootEntries"

// ---------------------------------------------------------------------------
// install_mask_full()
//
// Performs full EFI install:
//   1. Backup current BootOrder -> BrrBackupBootOrder.
//   2. Enumerate existing Boot* NVRAM slots -> BrrBackupBootEntries.
//   3. Backup existing \EFI\BRR\ contents to \EFI\BRR\backup\ (if present).
//   4. Copy mask-shim.efi and badmem.txt from USB to internal ESP \EFI\BRR\.
//   5. Create BootNNNN entry for \EFI\BRR\mask-shim.efi.
//   6. Prepend to BootOrder.
//   7. Set BrrMaskState = PERMANENT_UNCONFIRMED.
//
// @image_handle: handle of the calling EFI application (used to find USB dev).
// @st:           system table pointer.
// @err_msg:      on error, set to a short description; may be NULL.
//
// Does NOT reboot — caller handles that.
// Returns EFI_SUCCESS on success, EFI error code otherwise.
// ---------------------------------------------------------------------------
EFI_STATUS install_mask_full(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st,
                              const char **err_msg);

// ---------------------------------------------------------------------------
// uninstall_mask_full()
//
// Performs full EFI uninstall / revert:
//   1. Restore BootOrder from BrrBackupBootOrder (falls back to legacy var name).
//   2. Delete our BootNNNN entry from BrrBootSlot (SetVariable size=0).
//   3. Delete all files under \EFI\BRR\ on internal ESP (backup/ subdir too).
//   4. Delete all BRR NVRAM vars:
//        BrrMaskState, BrrBadPages, BrrBadChips,
//        BrrBackupBootOrder, BrrBackupBootEntries, BrrBootSlot,
//        A1990MaskOriginalBootOrder (legacy).
//   5. Print a clear summary of what was restored / deleted.
//
// Idempotent: safe to run even if install was never completed, or run twice.
// Does NOT reboot — caller handles that.
// ---------------------------------------------------------------------------
EFI_STATUS uninstall_mask_full(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st,
                                const char **err_msg);

// ---------------------------------------------------------------------------
// nvram_set_ascii() / nvram_get_ascii() / nvram_delete()
// Convenience wrappers used by mask-shim and mask-common.
// ---------------------------------------------------------------------------
EFI_STATUS mask_nvram_set_ascii(EFI_SYSTEM_TABLE *st, const CHAR16 *name,
                                 const char *val);
EFI_STATUS mask_nvram_get_ascii(EFI_SYSTEM_TABLE *st, const CHAR16 *name,
                                 char *buf, UINTN bufsz);
void       mask_nvram_delete(EFI_SYSTEM_TABLE *st, const CHAR16 *name);

#endif /* MASK_OPS_H */
