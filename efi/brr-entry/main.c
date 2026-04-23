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
// Chainload \EFI\BOOT\mask-shim.efi from the same USB we were loaded from.
// Builds a hardware-prefix device path (required by Apple T2 firmware, bare
// MediaFilePath is silently rejected).
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

    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    EFI_STATUS s = st->BootServices->HandleProtocol(
        image_handle, (EFI_GUID *)&LOADED_IMAGE_GUID_L, (void **)&li);
    if (s != EFI_SUCCESS || !li) return s;

    UINT8 *hw_dp = NULL;
    UINTN prefix_len = 0;
    if (li->DeviceHandle) {
        s = st->BootServices->HandleProtocol(
            li->DeviceHandle, (EFI_GUID *)&DEVICE_PATH_GUID_L, (void **)&hw_dp);
        if (s == EFI_SUCCESS && hw_dp) {
            // Walk to end node, total length minus end node (4 bytes).
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
    }

    static const CHAR16 path[] = L"\\EFI\\BOOT\\mask-shim.efi";
    UINTN path_chars = 0;
    while (path[path_chars]) path_chars++;
    path_chars++;  // include trailing NUL

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
        CHAR16 c = path[i];
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
}

// ---------------------------------------------------------------------------
// Entry point.
// ---------------------------------------------------------------------------
EFI_STATUS efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st)
{
    // Clear screen and move cursor home (ANSI escape via CHAR16).
    static const CHAR16 clrscr[] = { 0x001B, L'[', L'2', L'J',
                                       0x001B, L'[', L'H', 0 };
    st->ConOut->OutputString(st->ConOut, (CHAR16 *)clrscr);

    efi_print(st, L"=====================================================\r\n");
    efi_print(st, L"  BRR bad-address entry tool\r\n");
    efi_print(st, L"=====================================================\r\n");
    efi_print(st, L"\r\n");
    efi_print(st, L"Type the bad addresses memtest reported.\r\n");
    efi_print(st, L"Format: hex, comma-separated.  0x prefix optional.\r\n");
    efi_print(st, L"Example:  0xb2100000, 0xb2200000, 0xb2300000\r\n");
    efi_print(st, L"\r\n");
    efi_print(st, L"Each entry gets +/-1 MiB padding applied automatically\r\n");
    efi_print(st, L"by mask-shim when the mask is built.\r\n");
    efi_print(st, L"\r\n");
    efi_print(st, L"ESC cancels.  Enter submits.  Backspace deletes.\r\n");
    efi_print(st, L"\r\n");
    efi_print(st, L"addresses> ");

    static CHAR16 line[512];
    unsigned n = read_line(st, line, sizeof(line) / sizeof(line[0]));
    if (n == 0) {
        efi_print(st, L"\r\n[brr-entry] Cancelled (empty input).\r\n");
        efi_stall_ms(st, 2000);
        return EFI_SUCCESS;
    }

    static UINT64 pas[256];
    unsigned npa = parse_addresses(line, pas, 256);

    efi_print(st, L"\r\n");
    efi_print(st, L"[brr-entry] Parsed ");
    efi_print_dec(st, (UINTN)npa);
    efi_print(st, L" address(es):\r\n");
    for (unsigned i = 0; i < npa; i++) {
        efi_print(st, L"  [");
        efi_print_dec(st, (UINTN)i);
        efi_print(st, L"]  ");
        efi_print_hex(st, pas[i]);
        efi_print(st, L"  -> mask ");
        efi_print_hex(st, pas[i] - 0x100000ULL);
        efi_print(st, L" .. ");
        efi_print_hex(st, pas[i] + 0x100000ULL);
        efi_print(st, L"\r\n");
    }
    efi_print(st, L"\r\n");

    if (npa == 0) {
        efi_print(st, L"[brr-entry] No valid addresses parsed.  Cancelled.\r\n");
        efi_stall_ms(st, 3000);
        return EFI_SUCCESS;
    }

    // Confirm.
    efi_print(st, L"Press [Y] to write NVRAM + chainload mask-shim, any other key = cancel\r\n");
    for (;;) {
        CHAR16 k = efi_readkey(st);
        if (k == L'Y' || k == L'y') { efi_newline(st); break; }
        if (k != 0) {
            efi_print(st, L"\r\n[brr-entry] Cancelled by user.\r\n");
            efi_stall_ms(st, 2000);
            return EFI_SUCCESS;
        }
        efi_stall_ms(st, 50);
    }

    // Write BrrBadPages.
    efi_print(st, L"[brr-entry] Writing BrrBadPages NVRAM variable... ");
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

    // Read back to verify (pre-EBS GetVariable works; T2 commits on EBS).
    {
        static UINT8 rb[8 + 4096 * 8];
        UINTN rb_sz = sizeof(rb);
        UINT32 attrs = 0;
        s = st->RuntimeServices->GetVariable(
            (CHAR16 *)BRR_VARNAME_BADPAGES, (EFI_GUID *)&BRR_GUID,
            &attrs, &rb_sz, rb);
        efi_print(st, L"[brr-entry] Readback: status=");
        efi_print_hex(st, (UINT64)s);
        efi_print(st, L"  bytes=");
        efi_print_dec(st, (UINTN)rb_sz);
        efi_print(st, L"\r\n");
    }

    // Write state.
    efi_print(st, L"[brr-entry] Setting BrrMaskState=TRIAL_PENDING_PAGE... ");
    s = mask_nvram_set_ascii(st, BRR_VARNAME_STATE, BRR_STATE_TRIAL_PENDING_PAGE);
    if (s != EFI_SUCCESS) {
        efi_print(st, L"FAILED (status=");
        efi_print_hex(st, (UINT64)s);
        efi_print(st, L")\r\n");
        efi_stall_ms(st, 10000);
        return s;
    }
    efi_print(st, L"OK\r\n");

    // Chainload mask-shim, which applies the mask and chainloads macOS.
    efi_print(st, L"\r\n[brr-entry] Chainloading mask-shim.efi ...\r\n");
    efi_stall_ms(st, 1500);
    s = chainload_shim(image_handle, st);
    // If we return, chainload failed.
    efi_print(st, L"[brr-entry] Chainload returned status=");
    efi_print_hex(st, (UINT64)s);
    efi_print(st, L"\r\n");
    efi_print(st, L"  NVRAM state is written -- reboot manually and pick grub\r\n");
    efi_print(st, L"  entry 'Boot macOS normally (no mask)' -- mask-shim will\r\n");
    efi_print(st, L"  still run if BootOrder is set up, or use entry for --mask.\r\n");
    efi_stall_ms(st, 30000);
    return s;
}
