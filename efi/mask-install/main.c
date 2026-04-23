// SPDX-License-Identifier: GPL-2.0
//
// mask-install — EFI application that installs or removes the mask-shim
// on the internal ESP for persistent (PERMANENT) masking.
//
// Usage:
//   install.efi          — install shim + badmem.txt to internal ESP,
//                          register BootNNNN, prepend to BootOrder, reboot.
//   install.efi --uninstall — remove everything, restore BootOrder, reboot.
//
// The actual install/uninstall logic lives in efi/mask-common/mask_ops.c
// and is shared with mask-shim and revert.efi.

#include "../efi_types.h"
#include "../efi_util.h"
#include "../badmem_parse.h"
#include "../mask-common/mask_ops.h"

// ---------------------------------------------------------------------------
// Confirmation prompt helper (install).
// ---------------------------------------------------------------------------
static CHAR16 install_prompt_yesno(EFI_SYSTEM_TABLE *st, unsigned timeout_s)
{
    efi_print(st, L"About to install the BizarreRamRepair bad-memory mask PERMANENTLY.\r\n");
    efi_print(st, L"This will:\r\n");
    efi_print(st, L"  - Copy mask-shim.efi and badmem.txt to internal ESP (\\EFI\\BRR\\)\r\n");
    efi_print(st, L"  - Add a new EFI BootNNNN entry\r\n");
    efi_print(st, L"  - Modify BootOrder (original saved for revert)\r\n");
    efi_print(st, L"  (mask-shim also reads BrrBadPages from NVRAM automatically)\r\n");
    efi_print(st, L"\r\n");
    efi_print(st, L"Press [Y] to proceed, any other key or 30 s timeout = cancel.\r\n");
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
            efi_print(st, L"\r\n[install] Cancelled.\r\n");
            return 0;
        }
        efi_stall_ms(st, 100);
    }
    efi_print(st, L"\r\n[install] Timed out — cancelled.\r\n");
    return 0;
}

// Second install confirmation — only the "really?" text.
static CHAR16 install_prompt_yesno_2(EFI_SYSTEM_TABLE *st, unsigned timeout_s)
{
    efi_print(st, L"[2/2] Really install? This modifies your EFI boot configuration.\r\n");
    efi_print(st, L"      Your original BootOrder is saved and can be restored via --uninstall.\r\n");
    efi_print(st, L"\r\n");
    efi_print(st, L"Press [Y] to confirm, any other key or 30 s timeout = cancel.\r\n");
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
            efi_print(st, L"\r\n[install] Cancelled at second prompt.\r\n");
            return 0;
        }
        efi_stall_ms(st, 100);
    }
    efi_print(st, L"\r\n[install] Timed out at second prompt — cancelled.\r\n");
    return 0;
}

static CHAR16 uninstall_prompt_yesno(EFI_SYSTEM_TABLE *st, unsigned timeout_s)
{
    efi_print(st, L"About to UNINSTALL the BizarreRamRepair bad-memory mask.\r\n");
    efi_print(st, L"This will:\r\n");
    efi_print(st, L"  - Delete \\EFI\\BRR\\mask-shim.efi and badmem.txt from internal ESP\r\n");
    efi_print(st, L"  - Restore original BootOrder\r\n");
    efi_print(st, L"  - Remove EFI BootNNNN entry and all BRR NVRAM variables\r\n");
    efi_print(st, L"\r\n");
    efi_print(st, L"Press [Y] to proceed, any other key or 30 s timeout = cancel.\r\n");
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
            efi_print(st, L"\r\n[install] Cancelled.\r\n");
            return 0;
        }
        efi_stall_ms(st, 100);
    }
    efi_print(st, L"\r\n[install] Timed out — cancelled.\r\n");
    return 0;
}

// ---------------------------------------------------------------------------
// EFI entry point.
// ---------------------------------------------------------------------------

static const EFI_GUID lip_guid_i = EFI_LOADED_IMAGE_PROTOCOL_GUID;

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    st->ConOut->ClearScreen(st->ConOut);
    efi_print(st, L"BizarreRamRepair mask-install v2\r\n");
    efi_print(st, L"=====================================\r\n");

    // Parse load options: look for "--uninstall".
    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    int uninstall = 0;
    EFI_STATUS s = st->BootServices->HandleProtocol(
        image, (EFI_GUID *)&lip_guid_i, (void **)&li);
    if (s == EFI_SUCCESS && li && li->LoadOptions && li->LoadOptionsSize > 0) {
        CHAR16 *opts = (CHAR16 *)li->LoadOptions;
        UINTN  n16   = li->LoadOptionsSize / sizeof(CHAR16);
        static const CHAR16 uninstall_flag[] = L"--uninstall";
        UINTN  flen  = efi_strlen16(uninstall_flag);
        for (UINTN i = 0; i + flen <= n16; i++) {
            UINTN match = 1;
            for (UINTN j = 0; j < flen; j++) {
                if (opts[i+j] != uninstall_flag[j]) { match = 0; break; }
            }
            if (match) { uninstall = 1; break; }
        }
    }

    if (uninstall) {
        efi_print(st, L"Mode: UNINSTALL\r\n\r\n");

        CHAR16 ans = uninstall_prompt_yesno(st, 30);
        if (ans != L'Y') {
            efi_print(st, L"[install] Uninstall cancelled.\r\n");
            efi_stall_ms(st, 2000);
            return EFI_ABORTED;
        }

        const char *err = NULL;
        EFI_STATUS us = uninstall_mask_full(image, st, &err);
        if (us != EFI_SUCCESS) {
            efi_print(st, L"[install] ERROR: ");
            if (err) efi_printa(st, err);
            efi_print(st, L"\r\n");
            efi_stall_ms(st, 5000);
            return us;
        }

        efi_print(st, L"[install] Uninstall complete. Rebooting in 3 s...\r\n");
        efi_stall_ms(st, 3000);
        st->RuntimeServices->ResetSystem(EFI_RESET_COLD, EFI_SUCCESS, 0, NULL);
        return EFI_SUCCESS;

    } else {
        efi_print(st, L"Mode: INSTALL\r\n\r\n");

        // Show BrrBadPages count (informational).
        {
            static const EFI_GUID APPLE_GUID_INST = {
                0x7c436110, 0xab2a, 0x4bbb,
                { 0xa8, 0x80, 0xfe, 0x41, 0x99, 0x5c, 0x9f, 0x82 }
            };
            static UINT8 probe_blob[8];
            UINTN probe_sz = sizeof(probe_blob);
            UINT32 probe_attrs = 0;
            EFI_STATUS sv = st->RuntimeServices->GetVariable(
                (CHAR16 *)BRR_VARNAME_BADPAGES, (EFI_GUID *)&BRR_GUID,
                &probe_attrs, &probe_sz, probe_blob);
            if (sv != EFI_SUCCESS) {
                // Apple-GUID fallback (T2 commits those).
                probe_sz = sizeof(probe_blob);
                probe_attrs = 0;
                sv = st->RuntimeServices->GetVariable(
                    (CHAR16 *)BRR_VARNAME_BADPAGES,
                    (EFI_GUID *)&APPLE_GUID_INST,
                    &probe_attrs, &probe_sz, probe_blob);
            }
            if (sv == EFI_SUCCESS && probe_sz >= 8) {
                UINT32 n_pages = ((UINT32)probe_blob[4]       |
                                  ((UINT32)probe_blob[5] << 8)  |
                                  ((UINT32)probe_blob[6] << 16) |
                                  ((UINT32)probe_blob[7] << 24));
                efi_print(st, L"[install] NVRAM: BrrBadPages contains ");
                efi_print_dec(st, (UINTN)n_pages);
                efi_print(st, L" page(s) from last memtest run.\r\n\r\n");
            } else {
                efi_print(st, L"[install] NVRAM: no BrrBadPages found.\r\n");
                efi_print(st, L"  Supply badmem.txt on USB, or run memtest with [P]/[C].\r\n\r\n");
            }
        }

        // Pre-flight NVRAM write test.
        {
            static const CHAR16 test_var[] = L"BrrMaskProbe";
            UINT8 probe_data = 0x42;
            EFI_STATUS sp = st->RuntimeServices->SetVariable(
                (CHAR16 *)test_var, (EFI_GUID *)&BRR_GUID,
                EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS |
                EFI_VARIABLE_RUNTIME_ACCESS,
                sizeof(probe_data), &probe_data);
            if (sp != EFI_SUCCESS) {
                efi_print(st, L"[install] ERROR: NVRAM writes are denied.\r\n");
                efi_print(st, L"  Check T2 Startup Security (must allow external boot).\r\n");
                efi_stall_ms(st, 5000);
                return sp;
            }
            // Delete probe — T2 rejects attrs=0, must pass NV|BS|RT to
            // match the variable's stored attributes.  See mask_ops.c:88.
            st->RuntimeServices->SetVariable(
                (CHAR16 *)test_var, (EFI_GUID *)&BRR_GUID,
                EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS |
                    EFI_VARIABLE_RUNTIME_ACCESS,
                0, NULL);
        }

        // First confirmation.
        CHAR16 ans1 = install_prompt_yesno(st, 30);
        if (ans1 != L'Y') {
            efi_print(st, L"[install] Install cancelled — no changes made.\r\n");
            efi_stall_ms(st, 2000);
            return EFI_ABORTED;
        }

        // Second confirmation (only reached if first was Y).
        efi_print(st, L"\r\n");
        CHAR16 ans2 = install_prompt_yesno_2(st, 30);
        if (ans2 != L'Y') {
            efi_print(st, L"[install] Install cancelled — no changes made.\r\n");
            efi_stall_ms(st, 2000);
            return EFI_ABORTED;
        }

        const char *err = NULL;
        EFI_STATUS is = install_mask_full(image, st, &err);
        if (is != EFI_SUCCESS) {
            efi_print(st, L"[install] ERROR: ");
            if (err) efi_printa(st, err);
            efi_print(st, L"\r\n");
            efi_stall_ms(st, 5000);
            return is;
        }

        efi_print(st, L"[install] Install complete. Rebooting in 3 s...\r\n");
        efi_stall_ms(st, 3000);
        st->RuntimeServices->ResetSystem(EFI_RESET_COLD, EFI_SUCCESS, 0, NULL);
        return EFI_SUCCESS;
    }
}
