// SPDX-License-Identifier: GPL-2.0
//
// Pre-ExitBootServices interactive menu for A1990 (T2 Mac).
//
// Root cause: memtest86plus calls ExitBootServices() early, which destroys
// UEFI ConIn.  The T2 security chip proxies the internal keyboard through a
// vendor-specific USB HID handoff that memtest's own xHCI driver cannot
// navigate -- so both the internal keyboard and external USB-C keyboards are
// dead during test runs.  The only working input path is UEFI ConIn, which
// must be used while boot services are still alive.
//
// This module presents a 30-second countdown menu before ExitBootServices.
// The chosen action is encoded as BRR_FLAG_* bits returned to the caller
// for storage in boot_params_t::brr_flags.
//
// State-machine aware: if BrrMaskState NVRAM var is set to
// TRIAL_PENDING_PAGE or TRIAL_PENDING_CHIP (written by a previous memtest
// pass), the menu is bypassed and the appropriate flags are returned
// automatically.  If state is TRIAL_BOOTED or PERMANENT_UNCONFIRMED, the
// shim handles prompts; this menu just runs normally.
//
// Compilation: this file lives in system/board/ and is compiled with the same
// INC_DIRS as the rest of the build (-I../../boot is in scope), so it can
// include efi.h and bootparams.h directly.
//
// Copyright (C) 2024 A1990-memtest contributors.

#include <stdint.h>

// efi.h defines efi_system_table_t, efi_simple_text_in_t, efi_boot_services_t,
// efi_runtime_services_t, efi_input_key_t, EFI_SUCCESS, EFI_NOT_READY, etc.
// It lives in memtest86plus/boot/ and is reachable via -I../../boot.
#include "efi.h"

#include "efi_menu.h"

// ---------------------------------------------------------------------------
// efi_boot_services_t in efi.h declares stall as `void *` (since the caller
// only uses it via efi_call_bs macro which is not available here).  Cast it
// to the correct function pointer type when invoking.
// ---------------------------------------------------------------------------
typedef efi_status_t (efiapi *efi_stall_fn)(uintn_t microseconds);

// SetVariable / GetVariable fn-pointer types (fields are stored as unsigned long
// in memtest's efi_runtime_services_t).
typedef efi_status_t (efiapi *set_variable_fn)(
    efi_char16_t *name, efi_guid_t *guid,
    uint32_t attrs, uintn_t data_size, void *data);

typedef efi_status_t (efiapi *get_variable_fn)(
    efi_char16_t *name, efi_guid_t *guid,
    uint32_t *attrs, uintn_t *data_size, void *data);

// ---------------------------------------------------------------------------
// NVRAM vendor GUID shared with mask-shim / mask-install / badmem_log.
// {3E3E9DB2-1A2B-4B5C-9D1E-5F6A7B8C9D0E}
// ---------------------------------------------------------------------------
static const efi_guid_t BRR_GUID = {
    0x3e3e9db2, 0x1a2b, 0x4b5c,
    { 0x9d, 0x1e, 0x5f, 0x6a, 0x7b, 0x8c, 0x9d, 0x0e }
};

// NVRAM variable name: L"BrrMaskState"
static const efi_char16_t BRR_VARNAME_STATE[] = {
    'B','r','r','M','a','s','k','S','t','a','t','e', 0
};

// Legacy NVRAM variable name for migration: L"A1990MaskState"
static const efi_char16_t LEGACY_VARNAME_STATE[] = {
    'A','1','9','9','0','M','a','s','k','S','t','a','t','e', 0
};

// EFI variable attribute bits (NV + BS + RT).
#define EFI_VAR_NV_BS_RT  (0x00000001u | 0x00000002u | 0x00000004u)

// State strings for comparison (ASCII, matched byte-by-byte against NVRAM).
#define STATE_TRIAL_PENDING_PAGE  "TRIAL_PENDING_PAGE"
#define STATE_TRIAL_PENDING_CHIP  "TRIAL_PENDING_CHIP"
#define STATE_TRIAL_BOOTED        "TRIAL_BOOTED"

// Polling constants (also used by the main countdown below).
#define STALL_US          10000u
#define TICKS_PER_SECOND  (1000000u / STALL_US)
#define TIMEOUT_SECONDS   30u

// EFI Loaded-Image protocol GUID (for discovering USB device handle).
static const efi_guid_t LOADED_IMAGE_GUID = {
    0x5b1b31a1, 0x9562, 0x11d2,
    { 0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b }
};

// LoadImage / StartImage fn-pointer types (fields are `void *` in efi.h).
typedef efi_status_t (efiapi *load_image_fn)(
    unsigned char boot_policy, efi_handle_t parent,
    void *file_path, void *source_buffer, uintn_t source_size,
    efi_handle_t *image_handle);

typedef efi_status_t (efiapi *start_image_fn)(
    efi_handle_t image_handle, uintn_t *exit_data_size, efi_char16_t **exit_data);

// Minimal EFI_DEVICE_PATH for a MediaFilePath node followed by End-of-Path.
// Built on the stack with enough room for a ~64-char filename.
typedef struct {
    uint8_t  type;
    uint8_t  sub_type;
    uint16_t length;
} __attribute__((packed)) efi_dp_hdr_t;

// ---------------------------------------------------------------------------
// Helpers: output via ConOut
// ---------------------------------------------------------------------------

static efi_simple_text_out_t  *g_con_out;
static efi_simple_text_in_t   *g_con_in;
static efi_boot_services_t    *g_bs;
static efi_runtime_services_t *g_rs;

static void con_out_str(const efi_char16_t *str)
{
    g_con_out->output_string(g_con_out, (efi_char16_t *)str);
}

// Output a narrow ASCII string one character at a time via ConOut.
static void con_puts(const char *s)
{
    efi_char16_t buf[2];
    buf[1] = 0;
    for (; *s; s++) {
        if (*s == '\n') {
            buf[0] = '\r';
            g_con_out->output_string(g_con_out, buf);
        }
        buf[0] = (uint16_t)(unsigned char)*s;
        g_con_out->output_string(g_con_out, buf);
    }
}

// Print an unsigned decimal number via ConOut.
static void con_put_dec(unsigned int v)
{
    char tmp[12];
    char *p = &tmp[11];
    *p = '\0';
    do {
        *--p = (char)('0' + (v % 10));
        v /= 10;
    } while (v > 0);
    con_puts(p);
}

// Overwrite current line in-place using CR; show countdown value.
static void con_countdown_tick(unsigned int secs)
{
    static const efi_char16_t cr[2] = { '\r', 0 };
    con_out_str(cr);
    con_puts("  Countdown: ");
    con_put_dec(secs);
    con_puts(" s )  ");
}

// ---------------------------------------------------------------------------
// Simple ASCII strcmp helper.
// ---------------------------------------------------------------------------
static int ascii_eq_n(const char *buf, const char *ref, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        if (buf[i] != ref[i]) return 0;
    }
    return 1;
}

// ---------------------------------------------------------------------------
// Read one key from ConIn (non-blocking). Returns 0 if no key ready.
// ---------------------------------------------------------------------------
static efi_char16_t con_read_key(void)
{
    efi_input_key_t key;
    efi_status_t s = g_con_in->read_key_stroke(g_con_in, &key);
    if (s == EFI_SUCCESS) return key.ch;
    return 0;
}

// ---------------------------------------------------------------------------
// Read a line of ASCII text from ConIn with basic backspace support.
// Echoes characters as typed.  Returns number of chars written (excl NUL).
// buf must be at least cap bytes.
// ---------------------------------------------------------------------------
static unsigned con_read_line(char *buf, unsigned cap)
{
    unsigned n = 0;
    efi_stall_fn do_stall = (efi_stall_fn)(g_bs->stall);

    while (1) {
        efi_char16_t ch = con_read_key();
        if (ch == 0) {
            do_stall(5000); // 5 ms poll
            continue;
        }
        if (ch == '\r' || ch == '\n') {
            break;
        }
        if ((ch == 0x08 || ch == 0x7f) && n > 0) {
            // Backspace
            n--;
            // Erase last character on screen: BS + space + BS
            static const efi_char16_t bs_seq[4] = { 0x08, ' ', 0x08, 0 };
            con_out_str(bs_seq);
            continue;
        }
        if (ch < 0x20 || ch > 0x7e) continue; // ignore non-printable
        if (n + 1 >= cap) continue;            // buffer full
        buf[n++] = (char)(unsigned char)ch;
        // Echo the character.
        efi_char16_t echo[2] = { ch, 0 };
        con_out_str(echo);
    }
    buf[n] = '\0';
    con_puts("\n");
    return n;
}

// ---------------------------------------------------------------------------
// Check NVRAM state — if TRIAL_PENDING_* is set, chainload mask-shim.efi so
// the shim applies the trial mask and advances state to TRIAL_BOOTED; if
// TRIAL_BOOTED, prompt user and optionally chainload install.efi.
// Returns 0xFFFFFFFF if state is absent or doesn't require special action.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Read BrrMaskState from NVRAM, with legacy A1990MaskState fallback.
// Returns length read or 0 on any failure.
// ---------------------------------------------------------------------------
static unsigned read_state(char *out, unsigned cap)
{
    if (!g_rs) return 0;
    get_variable_fn get_var =
        (get_variable_fn)(uintptr_t)g_rs->get_variable;
    if (!get_var) return 0;

    // Try canonical name first.
    uintn_t sz = cap;
    uint32_t attrs = 0;
    efi_status_t s = get_var(
        (efi_char16_t *)BRR_VARNAME_STATE,
        (efi_guid_t *)&BRR_GUID,
        &attrs, &sz, out);
    if (s == EFI_SUCCESS) return (unsigned)sz;

    // Fallback: legacy variable name from older installs.
    sz = cap;
    attrs = 0;
    s = get_var(
        (efi_char16_t *)LEGACY_VARNAME_STATE,
        (efi_guid_t *)&BRR_GUID,
        &attrs, &sz, out);
    if (s != EFI_SUCCESS) return 0;
    return (unsigned)sz;
}

// ---------------------------------------------------------------------------
// chainload() — generic chainload of an EFI binary from our USB device.
// Builds a MediaFilePath device-path node for the given UCS-2 path and
// dispatches via LoadImage + StartImage.
// Returns 0 on success dispatch (never returns if binary runs ok), or
// non-zero EFI_STATUS on failure.
// ---------------------------------------------------------------------------
static efi_status_t chainload(efi_handle_t image_handle,
                               const efi_char16_t *path,
                               unsigned path_chars)  // includes trailing NUL
{
    if (!g_bs || !image_handle) return 0x8000000000000002ULL; // EFI_INVALID_PARAMETER

    // 1. Get Loaded-Image protocol on our image.
    efi_loaded_image_t *li = 0;
    efi_status_t s = g_bs->handle_protocol(
        image_handle, (efi_guid_t *)&LOADED_IMAGE_GUID, (void **)&li);
    if (s != EFI_SUCCESS || !li) return s;

    // 2. Build a MediaFilePath node + End-of-Path.
    //    MediaFilePath: type=4 (MEDIA_DEVICE_PATH), sub=4 (FILE_PATH).
    //    End: type=0x7F, sub=0xFF, length=4.
    unsigned path_bytes = path_chars * 2;

    // Max path: ~64 chars * 2 = 128 + 4 hdr + 4 end = 136. Use 160 for safety.
    static uint8_t dp_buf[160];
    efi_dp_hdr_t *h = (efi_dp_hdr_t *)dp_buf;
    h->type = 0x04;           // MEDIA_DEVICE_PATH
    h->sub_type = 0x04;       // FILEPATH
    h->length = (uint16_t)(sizeof(*h) + path_bytes);
    for (unsigned i = 0; i < path_chars; i++) {
        efi_char16_t c = path[i];
        dp_buf[sizeof(*h) + i * 2]     = (uint8_t)(c & 0xFF);
        dp_buf[sizeof(*h) + i * 2 + 1] = (uint8_t)((c >> 8) & 0xFF);
    }
    efi_dp_hdr_t *e = (efi_dp_hdr_t *)(dp_buf + h->length);
    e->type = 0x7F;           // END_DEVICE_PATH
    e->sub_type = 0xFF;
    e->length = 4;

    // 3. LoadImage with BootPolicy=TRUE lets firmware search on the parent device.
    load_image_fn load_img = (load_image_fn)(uintptr_t)g_bs->load_image;
    start_image_fn start_img = (start_image_fn)(uintptr_t)g_bs->start_image;
    if (!load_img || !start_img) return 0x8000000000000002ULL;

    efi_handle_t new_image = 0;
    s = load_img(1, image_handle, dp_buf, 0, 0, &new_image);
    if (s != EFI_SUCCESS) return s;

    // 4. StartImage — binary reboots on success, so this should not return.
    return start_img(new_image, 0, 0);
}

// Convenience wrappers for the two fixed paths.
static efi_status_t chainload_install(efi_handle_t image_handle)
{
    static const efi_char16_t path[] = {
        '\\','E','F','I','\\','B','O','O','T','\\',
        'i','n','s','t','a','l','l','.','e','f','i', 0
    };
    return chainload(image_handle, path, sizeof(path) / sizeof(efi_char16_t));
}

static efi_status_t chainload_shim(efi_handle_t image_handle)
{
    static const efi_char16_t path[] = {
        '\\','E','F','I','\\','B','O','O','T','\\',
        'm','a','s','k','-','s','h','i','m','.','e','f','i', 0
    };
    return chainload(image_handle, path, sizeof(path) / sizeof(efi_char16_t));
}

// ---------------------------------------------------------------------------
// Prompt Y/N with a 30-second timeout.  Returns 1 on Y, 0 on N / timeout.
// ---------------------------------------------------------------------------
static int prompt_yn(const char *question)
{
    con_puts("\n");
    con_puts(question);
    con_puts(" [Y/N, 30 s timeout = N]:  ");

    unsigned remaining = 30;
    unsigned ticks     = 0;
    efi_stall_fn do_stall = (efi_stall_fn)(g_bs->stall);

    while (remaining > 0) {
        efi_char16_t ch = con_read_key();
        if (ch == 'y' || ch == 'Y') { con_puts("Y\n"); return 1; }
        if (ch == 'n' || ch == 'N' || ch == 0x1B) { con_puts("N\n"); return 0; }
        do_stall(STALL_US);
        ticks++;
        if (ticks >= TICKS_PER_SECOND) {
            ticks = 0;
            remaining--;
        }
    }
    con_puts("(timeout -> N)\n");
    return 0;
}

// ---------------------------------------------------------------------------
// TRIAL_BOOTED handler: user returned to USB after trial boot.  Prompt twice
// for safety, then chainload install.efi if confirmed.
// ---------------------------------------------------------------------------
static void handle_trial_booted(efi_handle_t image_handle)
{
    con_puts("\n");
    con_puts("========================================================\n");
    con_puts("  Trial mask was applied in previous boot (TRIAL_BOOTED)\n");
    con_puts("========================================================\n");
    con_puts("  If macOS booted + ran correctly, you can install the\n");
    con_puts("  mask PERMANENTLY (writes to internal disk + EFI NVRAM).\n");
    con_puts("\n");

    if (!prompt_yn("  Install mask permanently?")) {
        con_puts("  -> Keeping trial (no changes).  Proceeding to memtest menu.\n\n");
        return;
    }
    if (!prompt_yn("  Really install permanently? EFI will be modified")) {
        con_puts("  -> Cancelled.  Proceeding to memtest menu.\n\n");
        return;
    }

    con_puts("\n  Chain-loading install.efi from USB ...\n");
    efi_status_t s = chainload_install(image_handle);
    // If we reach here, chainload failed.
    con_puts("  [error] install.efi chainload failed (status=0x");
    con_put_dec((unsigned)(s & 0xFFFFFFFF));
    con_puts(")\n");
    con_puts("  Falling back: pick grub entry 4 (Install) manually.\n\n");
}

static uint32_t check_nvram_state(efi_handle_t image_handle)
{
    char state[64] = {0};
    unsigned sz = read_state(state, sizeof(state) - 1);
    if (sz == 0) return 0xFFFFFFFFu;
    state[sz] = '\0';

    unsigned len_page   = sizeof(STATE_TRIAL_PENDING_PAGE) - 1;
    unsigned len_chip   = sizeof(STATE_TRIAL_PENDING_CHIP) - 1;
    unsigned len_booted = sizeof(STATE_TRIAL_BOOTED) - 1;

    if (sz == len_page && ascii_eq_n(state, STATE_TRIAL_PENDING_PAGE, len_page)) {
        con_puts("\n  [auto] State = TRIAL_PENDING_PAGE -> chainloading mask-shim.efi\n\n");
        efi_status_t s = chainload_shim(image_handle);
        // If we reach here, chainload failed — fall through to normal menu.
        con_puts("  [warn] mask-shim.efi chainload failed (status=0x");
        con_put_dec((unsigned)(s & 0xFFFFFFFF));
        con_puts("); continuing to menu.\n\n");
        return 0xFFFFFFFFu;
    }
    if (sz == len_chip && ascii_eq_n(state, STATE_TRIAL_PENDING_CHIP, len_chip)) {
        con_puts("\n  [auto] State = TRIAL_PENDING_CHIP -> chainloading mask-shim.efi\n\n");
        efi_status_t s = chainload_shim(image_handle);
        // If we reach here, chainload failed — fall through to normal menu.
        con_puts("  [warn] mask-shim.efi chainload failed (status=0x");
        con_put_dec((unsigned)(s & 0xFFFFFFFF));
        con_puts("); continuing to menu.\n\n");
        return 0xFFFFFFFFu;
    }
    if (sz == len_booted && ascii_eq_n(state, STATE_TRIAL_BOOTED, len_booted)) {
        handle_trial_booted(image_handle);
        // Fall through to regular menu regardless of outcome.
    }

    return 0xFFFFFFFFu;
}

// ---------------------------------------------------------------------------
// Menu constants
// ---------------------------------------------------------------------------

/* constants moved earlier so helpers above can use them */

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------

uint32_t efi_menu(void *sys_table_arg, void *image_handle_arg)
{
    efi_system_table_t *st = (efi_system_table_t *)sys_table_arg;
    efi_handle_t image_handle = (efi_handle_t)image_handle_arg;
    if (!st) return 0;

    g_con_out = st->con_out;
    g_con_in  = st->con_in;
    g_bs      = st->boot_services;
    g_rs      = st->runtime_services;

    if (!g_con_out || !g_con_in || !g_bs) return 0;

    // ---------------------------------------------------------------------------
    // Phase B: check if NVRAM has a pending trial state — if so, chainload
    // mask-shim.efi (TRIAL_PENDING_*) or run the permanent-install prompt
    // (TRIAL_BOOTED, may chainload install.efi and never return).
    // ---------------------------------------------------------------------------
    uint32_t auto_flags = check_nvram_state(image_handle);
    if (auto_flags != 0xFFFFFFFFu) {
        return auto_flags;
    }

    // Clear screen (ANSI ESC[2J + ESC[H as UCS-2).
    static const efi_char16_t clrscr[] = {
        0x001B, '[', '2', 'J', 0x001B, '[', 'H', 0
    };
    con_out_str(clrscr);

    con_puts("\r\n");
    con_puts("  A1990 Memtest -- press a key (timeout ");
    con_put_dec(TIMEOUT_SECONDS);
    con_puts(" s -> Run tests)\r\n");
    con_puts("\r\n");
    con_puts("    [Enter]  Run all tests (no auto-apply)\r\n");
    con_puts("    [P]      Automatic: page-mask (1 pass, NVRAM save, reboot)\r\n");
    con_puts("    [C]      Automatic: chip-mask (enter chip designators, then same)\r\n");
    con_puts("    [T]      Fast mode (skip countdowns, run all tests, no auto-apply)\r\n");
    con_puts("    [R]      Reboot\r\n");
    con_puts("\r\n");

    // Initial countdown display.
    unsigned int remaining = TIMEOUT_SECONDS;
    unsigned int ticks     = 0;
    con_countdown_tick(remaining);

    efi_stall_fn do_stall = (efi_stall_fn)(g_bs->stall);

    while (remaining > 0) {
        efi_char16_t ch = con_read_key();

        if (ch != 0) {
            // Enter (0x000D) or carriage return.
            if (ch == 0x000D || ch == 0x000A) {
                con_puts("\r\n  -> Run all tests (no auto-apply)\r\n\r\n");
                return 0;
            }
            // 'p' / 'P' -- automatic page-mask
            if (ch == 'p' || ch == 'P') {
                con_puts("\r\n  -> Automatic: page-mask, 1 pass, NVRAM save, reboot\r\n\r\n");
                return BRR_FLAG_SKIP_COUNTDOWNS | BRR_FLAG_AUTO_REBOOT_AFTER_PASS |
                       BRR_FLAG_TRIAL_PAGE;
            }
            // 'c' / 'C' -- automatic chip-mask: designators collected by
            // error_hook via badmem_log_record_chip() during the test run.
            // No up-front designator prompt: user picks chip mode here, then
            // the test identifies the chips automatically.
            if (ch == 'c' || ch == 'C') {
                con_puts("\r\n");
                con_puts("  -> Automatic chip-mask: will identify chips during test,\r\n");
                con_puts("     save BrrBadChips to NVRAM, set TRIAL_PENDING_CHIP, reboot.\r\n\r\n");
                return BRR_FLAG_SKIP_COUNTDOWNS | BRR_FLAG_AUTO_REBOOT_AFTER_PASS |
                       BRR_FLAG_AUTO_TRIAL_CHIP | BRR_FLAG_TRIAL_CHIP;
            }
            // 'r' / 'R' -- reboot
            if (ch == 'r' || ch == 'R') {
                con_puts("\r\n  -> Rebooting...\r\n");
                // Match 3-arg typedef in memtest86plus/boot/efi.h (upstream).
                // UEFI spec is 4-arg (DataSize + ResetData) but memtest's own
                // hwctrl.c also uses 3-arg across platforms without issue --
                // firmware only reads ResetData when DataSize > 0.
                g_rs->reset_system(EFI_RESET_COLD, 0, 0);
                // Should not return; spin if firmware is broken.
                while (1) {
#if defined(__x86_64__) || defined(__i386__)
                    __asm__ __volatile__("hlt");
#endif
                }
            }
            // 't' / 'T' -- fast mode (skip countdowns, no auto-apply)
            if (ch == 't' || ch == 'T') {
                con_puts("\r\n  -> Fast mode (skip countdowns, no auto-apply)\r\n\r\n");
                return BRR_FLAG_SKIP_COUNTDOWNS;
            }
            // Any other key: treat as Enter.
            con_puts("\r\n  -> Run all tests\r\n\r\n");
            return 0;
        }

        // Stall 10 ms, then advance countdown.
        do_stall(STALL_US);
        ticks++;
        if (ticks >= TICKS_PER_SECOND) {
            ticks = 0;
            remaining--;
            con_countdown_tick(remaining);
        }
    }

    // Timeout -- treat as Enter.
    con_puts("\r\n  -> Timeout: running all tests\r\n\r\n");
    return 0;
}
