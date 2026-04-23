// SPDX-License-Identifier: GPL-2.0
//
// brr-entry.efi — pre-ExitBootServices address-entry UI.
//
// Why this exists: post-ExitBootServices writes (NVRAM Runtime SetVariable
// under any GUID, or USB SFS via pre-captured handles) do NOT persist across
// graceful shutdown on Apple T2.  The ONLY reliable persistence path on this
// hardware is pre-EBS SetVariable / file I/O via Boot Services.
//
// Flow: user runs memtest separately, photographs the bad-address list,
// powers off, boots USB, picks the grub "enter addresses" entry which runs
// this tool.  This tool:
//   1. Prompts for comma-separated hex addresses.
//   2. Parses them, builds BrrBadPages NVRAM blob.
//   3. Writes BrrBadPages + BrrMaskState=TRIAL_PENDING_PAGE via Boot Services
//      SetVariable (persists on T2 because BS-phase writes go through the
//      normal firmware commit path).
//   4. Chainloads mask-shim.efi which applies the mask via AllocatePages and
//      boots macOS.
//
// Shim already applies +/-1 MiB padding to each base PA entry, so we store
// the user's addresses raw.

#include "../efi_types.h"
#include "../efi_util.h"
#include "../badmem_parse.h"
#include "../mask-common/mask_ops.h"

// Apple firmware EFI_CONSOLE_CONTROL_PROTOCOL (legacy from EFI 1.x,
// kept by Apple for Mac-specific console switching).  Grub leaves the
// console in graphics mode when it chainloads us; without a switch to
// text mode via THIS protocol, UEFI ConOut OutputString renders to the
// void.  Standard SimpleTextOutput->SetMode does NOT fix this on Apple
// firmware.  This is the same mechanism grub itself uses to display
// its menu / error text.
// GUID: F42F7782-012E-4C12-9956-49F94304F721
static const EFI_GUID CONSOLE_CONTROL_GUID = {
    0xf42f7782, 0x012e, 0x4c12,
    { 0x99, 0x56, 0x49, 0xf9, 0x43, 0x04, 0xf7, 0x21 }
};

typedef enum {
    ECC_SCREEN_TEXT     = 0,
    ECC_SCREEN_GRAPHICS = 1,
    ECC_SCREEN_MAX      = 2
} ECC_SCREEN_MODE;

typedef struct ECC_s {
    EFI_STATUS (EFIAPI *GetMode)(struct ECC_s *this_, ECC_SCREEN_MODE *mode,
                                   BOOLEAN *uga, BOOLEAN *std_in_locked);
    EFI_STATUS (EFIAPI *SetMode)(struct ECC_s *this_, ECC_SCREEN_MODE mode);
    EFI_STATUS (EFIAPI *LockStdIn)(struct ECC_s *this_, CHAR16 *password);
} EFI_CONSOLE_CONTROL_PROTOCOL;

static void force_text_mode(EFI_SYSTEM_TABLE *st)
{
    EFI_CONSOLE_CONTROL_PROTOCOL *cc = NULL;
    EFI_STATUS s = st->BootServices->LocateProtocol(
        (EFI_GUID *)&CONSOLE_CONTROL_GUID, NULL, (void **)&cc);
    if (s == EFI_SUCCESS && cc && cc->SetMode) {
        cc->SetMode(cc, ECC_SCREEN_TEXT);
    }
}

// ---------------------------------------------------------------------------
// Small ASCII helpers (no libc).
// ---------------------------------------------------------------------------

static int is_hex_digit(CHAR16 c)
{
    return (c >= L'0' && c <= L'9') ||
           (c >= L'a' && c <= L'f') ||
           (c >= L'A' && c <= L'F');
}

static unsigned hex_value(CHAR16 c)
{
    if (c >= L'0' && c <= L'9') return (unsigned)(c - L'0');
    if (c >= L'a' && c <= L'f') return (unsigned)(c - L'a') + 10;
    if (c >= L'A' && c <= L'F') return (unsigned)(c - L'A') + 10;
    return 0;
}

// Parse a single 0x-prefixed or bare hex number.  Returns address, leaves
// *pp pointing past the last hex digit.
static UINT64 parse_hex(CHAR16 **pp)
{
    CHAR16 *p = *pp;
    UINT64 v = 0;
    // Skip spaces.
    while (*p == L' ' || *p == L'\t') p++;
    // Optional 0x / 0X prefix.
    if (*p == L'0' && (p[1] == L'x' || p[1] == L'X')) p += 2;
    while (is_hex_digit(*p)) {
        v = (v << 4) | hex_value(*p);
        p++;
    }
    *pp = p;
    return v;
}

// ---------------------------------------------------------------------------
// Line editor: read a line from ConIn into a CHAR16 buffer.  Echoes keys to
// ConOut.  Supports backspace.  Enter submits.  ESC cancels (returns 0).
// ---------------------------------------------------------------------------
static unsigned read_line(EFI_SYSTEM_TABLE *st, CHAR16 *buf, unsigned cap)
{
    unsigned n = 0;
    for (;;) {
        EFI_INPUT_KEY key = {0, 0};
        EFI_STATUS s = st->ConIn->ReadKeyStroke(st->ConIn, &key);
        if (s == EFI_NOT_READY) {
            efi_stall_ms(st, 10);
            continue;
        }
        if (s != EFI_SUCCESS) continue;

        if (key.ScanCode == 0x17 /* ESC */) {
            return 0;
        }
        CHAR16 c = key.UnicodeChar;
        if (c == L'\r' || c == L'\n') {
            buf[n] = 0;
            efi_newline(st);
            return n;
        }
        if (c == 0x08 /* BS */ || c == 0x7F /* DEL */) {
            if (n > 0) {
                n--;
                CHAR16 erase[4] = { 0x08, L' ', 0x08, 0 };
                efi_print(st, erase);
            }
            continue;
        }
        if (c >= L' ' && c < 0x7F && n + 1 < cap) {
            buf[n++] = c;
            CHAR16 echo[2] = { c, 0 };
            efi_print(st, echo);
        }
    }
}

// ---------------------------------------------------------------------------
// Parse the line into up to cap addresses.  Expects comma-separated hex
// (0x-prefix optional).  Returns number parsed.
// ---------------------------------------------------------------------------
static unsigned parse_addresses(CHAR16 *line, UINT64 *out, unsigned cap)
{
    unsigned n = 0;
    CHAR16 *p = line;
    while (*p && n < cap) {
        while (*p == L' ' || *p == L'\t' || *p == L',') p++;
        if (!*p) break;
        UINT64 v = parse_hex(&p);
        // Page-align down to 4 KiB.
        v &= ~(UINT64)0xFFFu;
        if (v != 0) out[n++] = v;
    }
    return n;
}

// ---------------------------------------------------------------------------
// Write BrrBadPages blob via SetVariable (Boot Services phase -- persists
// on T2 because firmware commits these to flash on ExitBootServices).
// ---------------------------------------------------------------------------
static EFI_STATUS write_bad_pages(EFI_SYSTEM_TABLE *st, UINT64 *pas, unsigned n)
{
    // Header format (matches src/badmem_log.c and mask-shim reader):
    //   uint32_t version = 1;
    //   uint32_t count   = n;
    //   uint64_t pas[n];
    static UINT8 blob[8 + 4096 * 8];
    if (n > 4096) n = 4096;
    UINTN blob_sz = 8 + (UINTN)n * 8;

    blob[0] = 1; blob[1] = 0; blob[2] = 0; blob[3] = 0;  // version = 1 (LE)
    blob[4] = (UINT8)(n & 0xff);
    blob[5] = (UINT8)((n >> 8) & 0xff);
    blob[6] = (UINT8)((n >> 16) & 0xff);
    blob[7] = (UINT8)((n >> 24) & 0xff);
    UINT64 *pa_arr = (UINT64 *)(blob + 8);
    for (unsigned i = 0; i < n; i++) pa_arr[i] = pas[i];

    return st->RuntimeServices->SetVariable(
        (CHAR16 *)BRR_VARNAME_BADPAGES, (EFI_GUID *)&BRR_GUID,
        EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS |
            EFI_VARIABLE_RUNTIME_ACCESS,
        blob_sz, blob);
}

// ---------------------------------------------------------------------------
// Chainload mask-shim.efi.  Iterate all handles that support
// SimpleFileSystem; for each, try to open \EFI\BOOT\mask-shim.efi.
// The handle where the file exists is the USB ESP we want to LoadImage
// from -- build a hardware-prefix + FilePath device path against that
// handle, then LoadImage.  This matches the pattern mask-shim itself
// uses (successfully) to chainload macOS's boot.efi, and avoids the
// Apple-T2 failure mode where a device path built from LoadedImage
// ->DeviceHandle resolves to an ISO9660 view instead of the FAT ESP.
// ---------------------------------------------------------------------------
static EFI_STATUS chainload_shim(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st)
{
    static const EFI_GUID LOADED_IMAGE_GUID_L = {
        0x5b1b31a1, 0x9562, 0x11d2,
        { 0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b }
    };
    static const EFI_GUID DEVICE_PATH_GUID_L = {
        0x09576e91, 0x6d3f, 0x11d2,
        { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b }
    };
    static const EFI_GUID SFS_GUID_L = {
        0x0964e5b22, 0x6459, 0x11d2,
        { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b }
    };
    static const CHAR16 SHIM_PATH[] = L"\\EFI\\BOOT\\mask-shim.efi";
    UINTN path_chars = 0;
    while (SHIM_PATH[path_chars]) path_chars++;
    path_chars++;  // include trailing NUL

    // Enumerate all SimpleFileSystem handles.
    UINTN handles_sz = 0;
    EFI_STATUS s = st->BootServices->LocateHandle(
        EFI_LOCATE_BY_PROTOCOL, (EFI_GUID *)&SFS_GUID_L, NULL, &handles_sz, NULL);
    if (s != EFI_BUFFER_TOO_SMALL && s != EFI_SUCCESS) return s;

    EFI_HANDLE *handles = NULL;
    s = st->BootServices->AllocatePool(EfiLoaderData, handles_sz, (void **)&handles);
    if (s != EFI_SUCCESS) return s;
    s = st->BootServices->LocateHandle(
        EFI_LOCATE_BY_PROTOCOL, (EFI_GUID *)&SFS_GUID_L, NULL, &handles_sz, handles);
    if (s != EFI_SUCCESS) {
        st->BootServices->FreePool(handles);
        return s;
    }
    UINTN nh = handles_sz / sizeof(EFI_HANDLE);

    // Find the handle whose root contains \EFI\BOOT\mask-shim.efi.
    EFI_HANDLE device = NULL;
    for (UINTN i = 0; i < nh; i++) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs = NULL;
        if (st->BootServices->HandleProtocol(
                handles[i], (EFI_GUID *)&SFS_GUID_L, (void **)&sfs) != EFI_SUCCESS)
            continue;
        EFI_FILE_PROTOCOL *root = NULL;
        if (sfs->OpenVolume(sfs, &root) != EFI_SUCCESS) continue;
        EFI_FILE_PROTOCOL *f = NULL;
        EFI_STATUS fs_s = root->Open(root, &f, (CHAR16 *)SHIM_PATH,
                                      EFI_FILE_MODE_READ, 0);
        root->Close(root);
        if (fs_s == EFI_SUCCESS) {
            f->Close(f);
            device = handles[i];
            break;
        }
    }
    st->BootServices->FreePool(handles);

    if (!device) return EFI_NOT_FOUND;

    // Build hardware-prefix + FilePath device path against `device`.
    UINT8 *hw_dp = NULL;
    UINTN prefix_len = 0;
    s = st->BootServices->HandleProtocol(
        device, (EFI_GUID *)&DEVICE_PATH_GUID_L, (void **)&hw_dp);
    if (s == EFI_SUCCESS && hw_dp) {
        UINT8 *p = hw_dp;
        for (;;) {
            UINT16 node_len = (UINT16)p[2] | ((UINT16)p[3] << 8);
            if (p[0] == 0x7f && p[1] == 0xff) break;
            p += node_len;
        }
        prefix_len = (UINTN)(p - hw_dp);
    } else {
        hw_dp = NULL;
    }

    UINTN fp_sz = 4 + path_chars * sizeof(CHAR16);
    UINTN total_sz = prefix_len + fp_sz + 4;

    static UINT8 dp_buf[512];
    if (total_sz > sizeof(dp_buf)) return EFI_INVALID_PARAMETER;

    UINT8 *dp = dp_buf;
    if (prefix_len > 0) {
        for (UINTN i = 0; i < prefix_len; i++) dp[i] = hw_dp[i];
        dp += prefix_len;
    }
    dp[0] = 0x04; dp[1] = 0x04;
    dp[2] = (UINT8)(fp_sz & 0xff);
    dp[3] = (UINT8)(fp_sz >> 8);
    for (UINTN i = 0; i < path_chars; i++) {
        CHAR16 c = SHIM_PATH[i];
        dp[4 + i * 2]     = (UINT8)(c & 0xFF);
        dp[4 + i * 2 + 1] = (UINT8)((c >> 8) & 0xFF);
    }
    dp += fp_sz;
    dp[0] = 0x7f; dp[1] = 0xff; dp[2] = 4; dp[3] = 0;

    EFI_HANDLE new_image = NULL;
    s = st->BootServices->LoadImage(0, image_handle, (void *)dp_buf, NULL, 0,
                                     &new_image);
    if (s != EFI_SUCCESS) return s;

    return st->BootServices->StartImage(new_image, NULL, NULL);
    (void)LOADED_IMAGE_GUID_L;  // silence unused -- kept for future extensions
}

// ---------------------------------------------------------------------------
// Entry point.
// ---------------------------------------------------------------------------
EFI_STATUS efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st)
{
    // Apple-specific: switch console from grub's graphics mode back
    // to text mode via EFI_CONSOLE_CONTROL_PROTOCOL.  Without this,
    // text rendered via ConOut is invisible on A1990 after grub
    // chainloads us.
    force_text_mode(st);

    // Force a real text mode -- grub leaves the console in gfxterm
    // graphics mode, and UEFI ConOut text output renders invisibly
    // there on A1990.  Query available text modes, pick the largest
    // one (so we get decent density on a Retina panel), reset the
    // input buffer, then clear.
    if (st->ConOut && st->ConOut->SetMode) {
        // Try common text-mode indices from largest to smallest.
        // EFI defines mode 0 = 80x25 (required), others vendor-defined.
        // Apple firmware typically has 0=80x25, 1=80x50, higher numbers
        // for larger text consoles.
        UINTN cols = 0, rows = 0;
        UINTN best_mode = 0;
        UINTN best_area = 0;
        for (UINTN m = 0; m < 8; m++) {
            EFI_STATUS q = st->ConOut->QueryMode(st->ConOut, m, &cols, &rows);
            if (q != EFI_SUCCESS) continue;
            UINTN area = cols * rows;
            if (area > best_area) { best_area = area; best_mode = m; }
        }
        st->ConOut->SetMode(st->ConOut, best_mode);
        if (st->ConOut->SetAttribute)
            st->ConOut->SetAttribute(st->ConOut, 0x07);  // light-gray on black
        if (st->ConOut->ClearScreen)
            st->ConOut->ClearScreen(st->ConOut);
    }
    if (st->ConIn && st->ConIn->Reset)
        st->ConIn->Reset(st->ConIn, 0);

    efi_print(st, L"\r\n");
    efi_print(st, L"  ============================================\r\n");
    efi_print(st, L"        APPLY MASK -- enter bad addresses\r\n");
    efi_print(st, L"  ============================================\r\n");
    efi_print(st, L"\r\n");

    // Check if NVRAM already has a saved mask from a previous run.  If
    // so, offer a quick-retry path -- user just presses Y and we chain
    // straight to mask-shim without re-typing addresses.  Covers the
    // case where shim chainload failed (e.g. boot.efi not found) and
    // the user wants to retry without reentering data.
    {
        static UINT8 existing[8 + 4096 * 8];
        UINTN ex_sz = sizeof(existing);
        UINT32 ex_attrs = 0;
        EFI_STATUS sg = st->RuntimeServices->GetVariable(
            (CHAR16 *)BRR_VARNAME_BADPAGES, (EFI_GUID *)&BRR_GUID,
            &ex_attrs, &ex_sz, existing);
        if (sg == EFI_SUCCESS && ex_sz >= 8) {
            UINT32 n_existing = *(UINT32 *)(existing + 4);
            if (n_existing > 0 && n_existing <= 4096 &&
                ex_sz >= 8 + n_existing * 8) {
                UINT64 *pa_arr = (UINT64 *)(existing + 8);
                efi_print(st, L"  Existing saved mask in NVRAM:\r\n");
                efi_print(st, L"\r\n");
                for (UINT32 i = 0; i < n_existing && i < 8; i++) {
                    efi_print(st, L"    ");
                    efi_print_dec(st, (UINTN)(i + 1));
                    efi_print(st, L". ");
                    efi_print_hex(st, pa_arr[i]);
                    efi_print(st, L"\r\n");
                }
                if (n_existing > 8) {
                    efi_print(st, L"    ... and ");
                    efi_print_dec(st, (UINTN)(n_existing - 8));
                    efi_print(st, L" more\r\n");
                }
                efi_print(st, L"\r\n");
                efi_print(st, L"  Y = apply this mask + boot macOS\r\n");
                efi_print(st, L"  N = discard and enter new addresses\r\n");
                efi_print(st, L"  ESC = cancel (reboot)\r\n");
                efi_print(st, L"\r\n");

                for (;;) {
                    CHAR16 k = efi_readkey(st);
                    if (k == L'Y' || k == L'y') {
                        efi_newline(st);
                        efi_print(st, L"  Applying mask + booting macOS ...\r\n");
                        efi_stall_ms(st, 800);
                        EFI_STATUS cs = chainload_shim(image_handle, st);
                        efi_print(st, L"\r\n");
                        efi_print(st, L"  Could not chainload mask-shim (status=");
                        efi_print_hex(st, (UINT64)cs);
                        efi_print(st, L").\r\n");
                        efi_print(st, L"  NVRAM is still saved.  Reboot + retry entry 3,\r\n");
                        efi_print(st, L"  or hold Option at boot and pick macOS manually.\r\n");
                        efi_stall_ms(st, 15000);
                        st->RuntimeServices->ResetSystem(
                            EFI_RESET_WARM, EFI_SUCCESS, 0, NULL);
                        for (;;) { __asm__ __volatile__("hlt"); }
                    }
                    if (k == 0x1B) {
                        efi_print(st, L"\r\n  Cancelled -- rebooting.\r\n");
                        efi_stall_ms(st, 1500);
                        st->RuntimeServices->ResetSystem(
                            EFI_RESET_WARM, EFI_SUCCESS, 0, NULL);
                        for (;;) { __asm__ __volatile__("hlt"); }
                    }
                    if (k == L'N' || k == L'n') {
                        efi_newline(st);
                        break;  // fall through to address-entry prompt
                    }
                    efi_stall_ms(st, 50);
                }
            }
        }
    }

    efi_print(st, L"  Type the bad addresses from the memtest photo.\r\n");
    efi_print(st, L"\r\n");
    efi_print(st, L"  Format:  hex, comma-separated  (0x optional)\r\n");
    efi_print(st, L"  Example: 0xb2100000, 0xb2200000\r\n");
    efi_print(st, L"\r\n");
    efi_print(st, L"  Enter = submit    Backspace = delete    ESC = cancel\r\n");
    efi_print(st, L"\r\n");
    efi_print(st, L"  > ");

    static CHAR16 line[512];
    unsigned n = read_line(st, line, sizeof(line) / sizeof(line[0]));
    if (n == 0) {
        efi_print(st, L"\r\n  Cancelled.\r\n");
        efi_stall_ms(st, 2000);
        return EFI_SUCCESS;
    }

    static UINT64 pas[256];
    unsigned npa = parse_addresses(line, pas, 256);

    if (npa == 0) {
        efi_print(st, L"\r\n  No valid addresses.  Cancelled.\r\n");
        efi_stall_ms(st, 3000);
        return EFI_SUCCESS;
    }

    efi_print(st, L"\r\n");
    efi_print(st, L"  Parsed ");
    efi_print_dec(st, (UINTN)npa);
    efi_print(st, L" address(es):\r\n");
    efi_print(st, L"\r\n");
    for (unsigned i = 0; i < npa; i++) {
        efi_print(st, L"    ");
        efi_print_dec(st, (UINTN)(i + 1));
        efi_print(st, L". ");
        efi_print_hex(st, pas[i]);
        efi_print(st, L"  -> masking ");
        efi_print_hex(st, pas[i] - 0x100000ULL);
        efi_print(st, L" .. ");
        efi_print_hex(st, pas[i] + 0x100000ULL);
        efi_print(st, L"  (2 MiB)\r\n");
    }
    efi_print(st, L"\r\n");
    efi_print(st, L"  Type Y to apply, any other key to cancel.\r\n");
    efi_print(st, L"\r\n");

    for (;;) {
        CHAR16 k = efi_readkey(st);
        if (k == L'Y' || k == L'y') { efi_newline(st); break; }
        if (k != 0) {
            efi_print(st, L"\r\n  Cancelled.\r\n");
            efi_stall_ms(st, 2000);
            return EFI_SUCCESS;
        }
        efi_stall_ms(st, 50);
    }

    efi_print(st, L"  Writing to NVRAM   ... ");
    EFI_STATUS s = write_bad_pages(st, pas, npa);
    if (s != EFI_SUCCESS) {
        efi_print(st, L"FAILED (status=");
        efi_print_hex(st, (UINT64)s);
        efi_print(st, L")\r\n");
        efi_print(st, L"  Check T2 security mode: must allow external boot.\r\n");
        efi_stall_ms(st, 10000);
        return s;
    }
    efi_print(st, L"OK\r\n");

    efi_print(st, L"  Saving state       ... ");
    s = mask_nvram_set_ascii(st, BRR_VARNAME_STATE, BRR_STATE_TRIAL_PENDING_PAGE);
    if (s != EFI_SUCCESS) {
        efi_print(st, L"FAILED (status=");
        efi_print_hex(st, (UINT64)s);
        efi_print(st, L")\r\n");
        efi_stall_ms(st, 10000);
        return s;
    }
    efi_print(st, L"OK\r\n");

    efi_print(st, L"\r\n");
    efi_print(st, L"  Applying mask + booting macOS ...\r\n");
    efi_print(st, L"\r\n");
    efi_stall_ms(st, 1000);

    s = chainload_shim(image_handle, st);
    // If we return here, chainload failed but NVRAM state is committed.
    // A power-cycle + USB boot will re-run this tool and pick up the
    // already-saved state (addresses auto-applied without retyping).
    efi_print(st, L"\r\n");
    efi_print(st, L"  Could not chainload mask-shim (status=");
    efi_print_hex(st, (UINT64)s);
    efi_print(st, L").\r\n");
    efi_print(st, L"  The mask state is already saved.  Rebooting in 10 s --\r\n");
    efi_print(st, L"  at the grub menu pick entry 3 again to retry, or just\r\n");
    efi_print(st, L"  power off; the next boot from USB will pick up the mask.\r\n");
    efi_stall_ms(st, 10000);

    st->RuntimeServices->ResetSystem(EFI_RESET_WARM, EFI_SUCCESS, 0, NULL);
    for (;;) { __asm__ __volatile__("hlt"); }
    return EFI_SUCCESS;
}
