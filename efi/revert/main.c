// SPDX-License-Identifier: GPL-2.0
//
// revert.efi — EFI application that performs a full A1990 mask uninstall
// and reboots.  Calls uninstall_mask_full() from mask-common which:
//   - deletes \EFI\BRR\ contents from the internal ESP
//   - restores pre-install BootOrder (BrrBackupBootOrder -> BootOrder)
//   - deletes BRR NVRAM variables (BrrMaskState, BrrBadPages, etc.)
//
// Idempotent: safe to run from any state, including NONE.
//
// Status (2026-04): NOT currently wired into any grub entry — the simple
// 3-entry flow (full test / fast test / brr-entry) doesn't expose revert.
// The binary is still staged to \EFI\BOOT\revert.efi on the USB ESP so
// an advanced user can invoke it via the UEFI Shell if needed.  A grub
// entry will be added when the permanent-install milestone lands.
//
// Copyright (C) 2024 A1990-memtest contributors.

#include "../efi_types.h"
#include "../efi_util.h"
#include "../mask-common/mask_ops.h"

// Confirmation prompt: prints lines, waits up to timeout_s for Y.
// Returns L'Y' on yes, 0 otherwise.
static CHAR16 revert_prompt(EFI_SYSTEM_TABLE *st, unsigned timeout_s)
{
    efi_print(st, L"\r\n");
    efi_print(st, L"  About to REVERT all BRR mask changes.\r\n");
    efi_print(st, L"  This will:\r\n");
    efi_print(st, L"    - Delete \\EFI\\BRR\\mask-shim.efi and badmem.txt from internal ESP\r\n");
    efi_print(st, L"    - Restore pre-install BootOrder\r\n");
    efi_print(st, L"    - Remove all BRR (and legacy A1990) NVRAM variables\r\n");
    efi_print(st, L"\r\n");
    efi_print(st, L"  Press [Y] to proceed, any other key or 30 s = cancel.\r\n");
    efi_print(st, L"\r\n");

    unsigned ticks = timeout_s * 10;
    for (unsigned t = 0; t < ticks; t++) {
        if (t % 10 == 0) {
            efi_print(st, L"  Time remaining: ");
            efi_print_dec(st, (UINTN)(timeout_s - t / 10));
            efi_print(st, L" s  \r");
        }
        CHAR16 k = efi_readkey(st);
        if (k == L'Y' || k == L'y') { efi_newline(st); return L'Y'; }
        if (k != 0) {
            efi_print(st, L"\r\n[revert] Cancelled.\r\n");
            return 0;
        }
        efi_stall_ms(st, 100);
    }
    efi_print(st, L"\r\n[revert] Timed out — cancelled.\r\n");
    return 0;
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    st->ConOut->ClearScreen(st->ConOut);
    efi_print(st, L"A1990 revert v1\r\n");
    efi_print(st, L"=====================================\r\n");

    CHAR16 ans = revert_prompt(st, 30);
    if (ans != L'Y') {
        efi_print(st, L"[revert] No changes made.\r\n");
        efi_stall_ms(st, 3000);
        return EFI_ABORTED;
    }

    const char *err = NULL;
    EFI_STATUS s = uninstall_mask_full(image, st, &err);
    if (s != EFI_SUCCESS) {
        efi_print(st, L"[revert] ERROR: ");
        if (err) efi_printa(st, err);
        efi_print(st, L"\r\n");
        efi_stall_ms(st, 5000);
        return s;
    }

    efi_print(st, L"[revert] Done. Rebooting in 3 s...\r\n");
    efi_stall_ms(st, 3000);
    st->RuntimeServices->ResetSystem(EFI_RESET_COLD, EFI_SUCCESS, 0, NULL);
    return EFI_SUCCESS;
}
