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
#include "decoder_selftest.h"

// For post-EBS log dump from main.c:
#include "display.h"
extern void scroll(void);

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

// EFI Device-Path protocol GUID.
static const efi_guid_t DEVICE_PATH_GUID = {
    0x09576e91, 0x6d3f, 0x11d2,
    { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b }
};

// EFI Simple File System protocol GUID.
static const efi_guid_t SFS_GUID = {
    0x964e5b22, 0x6459, 0x11d2,
    { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b }
};

// Minimal EFI File Protocol definitions — public struct layout is
// sufficient for open/read/write/close.
typedef struct efi_brr_file_t {
    uint64_t revision;
    efi_status_t (efiapi *open)(struct efi_brr_file_t *, struct efi_brr_file_t **,
                                 efi_char16_t *, uint64_t, uint64_t);
    efi_status_t (efiapi *close)(struct efi_brr_file_t *);
    efi_status_t (efiapi *delete_file)(struct efi_brr_file_t *);
    efi_status_t (efiapi *read)(struct efi_brr_file_t *, uintn_t *, void *);
    efi_status_t (efiapi *write)(struct efi_brr_file_t *, uintn_t *, void *);
    efi_status_t (efiapi *get_position)(struct efi_brr_file_t *, uint64_t *);
    efi_status_t (efiapi *set_position)(struct efi_brr_file_t *, uint64_t);
    efi_status_t (efiapi *get_info)(struct efi_brr_file_t *, efi_guid_t *, uintn_t *, void *);
    efi_status_t (efiapi *set_info)(struct efi_brr_file_t *, efi_guid_t *, uintn_t, void *);
    efi_status_t (efiapi *flush)(struct efi_brr_file_t *);
} efi_brr_file_t;

typedef struct efi_brr_sfs_t {
    uint64_t revision;
    efi_status_t (efiapi *open_volume)(struct efi_brr_sfs_t *, efi_brr_file_t **);
} efi_brr_sfs_t;

#define EFI_BRR_FILE_MODE_READ    0x0000000000000001ULL
#define EFI_BRR_FILE_MODE_WRITE   0x0000000000000002ULL
#define EFI_BRR_FILE_MODE_CREATE  0x8000000000000000ULL

// Handles captured pre-ExitBootServices, usable from post-EBS code IF
// Apple T2 firmware does not invalidate the function pointers on EBS.
// Sentinel (uintptr_t)1 is "not captured yet".  NULL (0) would place
// the variable in .bss and get wiped by startup64.S:258-268's BSS
// zero loop (same bug we fought on g_brr_flags_cached) BEFORE main()
// runs -- by the time badmem_log calls us post-EBS the handle would
// already be gone.  Non-zero initializer forces .data placement,
// which survives the BSS zero.
efi_brr_file_t *g_brr_fs_root = (efi_brr_file_t *)(uintptr_t)1;
#define BRR_FS_ROOT_VALID(p) ((p) != 0 && (p) != (efi_brr_file_t *)(uintptr_t)1)

// Pre-ExitBootServices log buffer.  Every con_puts() in efi_menu
// mirrors into this buffer.  After memtest's screen_init, main.c
// dumps the buffer to the blue framebuffer via display_scrolled_message
// so the user can actually see pre-EBS diagnostics -- on this hardware,
// UEFI ConOut text output never reaches the display (grub leaves the
// console in graphics mode, EFI text console goes to the void).
//
// Must be .data, not .bss, or startup64.S zeros it between
// efi_menu(pre-EBS) and main()(post-EBS).  Non-zero first byte +
// explicit section attribute force .data placement.
struct brr_preboot_log {
    unsigned len;
    unsigned magic;
    char     buf[8192];
};
// Pre-seed with a marker so the post-EBS dump can prove the struct
// is reachable and .data-placed even if efi_menu never wrote to it
// (early-return paths, firmware quirks etc).  len=27 matches the
// literal length so the dump prints exactly this line first.
__attribute__((section(".data")))
struct brr_preboot_log g_brr_preboot = {
    27,
    0xB007B007u,
    "[init] preboot buf alive\n\n"
};

// Write narrow ASCII content to a file on the USB ESP.  Returns:
//    0 = success (write + readback round-trip confirmed)
//   -1 = no SFS handle available
//   -2 = open failed
//   -3 = write failed
//   -4 = readback open failed
//   -5 = readback length mismatch
//   -6 = readback content mismatch
// Return codes are distinct so pass-end screen tells user exactly
// where the failure was.
int brr_fs_write_file(const efi_char16_t *path, unsigned path_chars,
                      const char *content, unsigned len)
{
    (void)path_chars;
    if (!BRR_FS_ROOT_VALID(g_brr_fs_root)) return -1;
    efi_brr_file_t *root = g_brr_fs_root;

    efi_brr_file_t *f = 0;
    efi_status_t s = root->open(root, &f, (efi_char16_t *)path,
        EFI_BRR_FILE_MODE_READ | EFI_BRR_FILE_MODE_WRITE |
        EFI_BRR_FILE_MODE_CREATE, 0);
    if (s != EFI_SUCCESS || !f) return -2;

    f->set_position(f, 0);
    uintn_t wlen = len;
    s = f->write(f, &wlen, (void *)content);
    f->flush(f);
    f->close(f);
    if (s != EFI_SUCCESS) return -3;

    // Read-verify: re-open, read back, byte-compare against original.
    // Catches silent-success writes on read-only / cached mounts.
    efi_brr_file_t *rf = 0;
    s = root->open(root, &rf, (efi_char16_t *)path,
        EFI_BRR_FILE_MODE_READ, 0);
    if (s != EFI_SUCCESS || !rf) return -4;

    // Read into a local buffer (cap at 1 KiB for stack safety; our
    // state/pages files are much smaller than that).
    static char rb[1024];
    uintn_t rlen = sizeof(rb);
    if (rlen > len + 16) rlen = len + 16;
    s = rf->read(rf, &rlen, rb);
    rf->close(rf);
    if (s != EFI_SUCCESS) return -4;
    if (rlen != len) return -5;
    for (unsigned i = 0; i < len; i++) {
        if (rb[i] != content[i]) return -6;
    }
    return 0;
}

// Create directory if missing; silent no-op if present or error.
// Separate from brr_fs_write_file because mkdir has to be idempotent
// and the attribute flag differs.
static void brr_fs_mkdir(const efi_char16_t *path)
{
    if (!BRR_FS_ROOT_VALID(g_brr_fs_root)) return;
    efi_brr_file_t *f = 0;
    efi_status_t s = g_brr_fs_root->open(g_brr_fs_root, &f,
        (efi_char16_t *)path,
        EFI_BRR_FILE_MODE_READ | EFI_BRR_FILE_MODE_WRITE |
        EFI_BRR_FILE_MODE_CREATE, 0x10 /* EFI_FILE_DIRECTORY */);
    if (s == EFI_SUCCESS && f) f->close(f);
}

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
// Also mirror to g_brr_preboot so memtest's post-EBS main() can dump
// the same text to its visible blue framebuffer (UEFI ConOut is
// effectively write-to-void on A1990 + grub graphics-mode console).
static void con_puts(const char *s)
{
    efi_char16_t buf[2];
    buf[1] = 0;
    for (; *s; s++) {
        // Mirror to buffer (guarded for size).
        if (g_brr_preboot.len < sizeof(g_brr_preboot.buf) - 1) {
            g_brr_preboot.buf[g_brr_preboot.len++] = *s;
            g_brr_preboot.buf[g_brr_preboot.len] = 0;
        }
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
// Return total byte length of a device path including its End node.
static uintn_t dp_total_len_menu(const uint8_t *dp)
{
    uintn_t total = 0;
    for (;;) {
        uint16_t node_len = (uint16_t)dp[2] | ((uint16_t)dp[3] << 8);
        total += node_len;
        if (dp[0] == 0x7f && dp[1] == 0xff) break;
        dp += node_len;
    }
    return total;
}

static efi_status_t chainload(efi_handle_t image_handle,
                               const efi_char16_t *path,
                               unsigned path_chars)  // includes trailing NUL
{
    if (!g_bs || !image_handle) return 0x8000000000000002ULL; // EFI_INVALID_PARAMETER

    // 1. Get Loaded-Image protocol on our image to find the source device.
    efi_loaded_image_t *li = 0;
    efi_status_t s = g_bs->handle_protocol(
        image_handle, (efi_guid_t *)&LOADED_IMAGE_GUID, (void **)&li);
    if (s != EFI_SUCCESS || !li) return s;

    // 2. Get the hardware device path of our device so we can prefix it.
    //    This is the pattern from mask-shim/main.c::build_file_device_path():
    //    copy all hardware nodes (strip trailing End), append MediaFilePath,
    //    append End.  On T2, a bare FilePath node without the hardware prefix
    //    causes LoadImage to fail silently.
    uint8_t *hw_dp = 0;
    uintn_t prefix_len = 0;
    if (li->device_handle) {
        s = g_bs->handle_protocol(
            li->device_handle, (efi_guid_t *)&DEVICE_PATH_GUID,
            (void **)&hw_dp);
        if (s == EFI_SUCCESS && hw_dp) {
            uintn_t total = dp_total_len_menu(hw_dp);
            prefix_len = (total >= 4) ? (total - 4) : 0; // strip End node
        } else {
            hw_dp = 0;
        }
    }

    // 3. Build:  [hardware prefix] + MediaFilePath(path) + End
    //    path_chars includes the trailing NUL, so path_bytes covers the NUL too.
    unsigned path_bytes = path_chars * sizeof(efi_char16_t);
    uintn_t fp_sz    = 4 + path_bytes;          // FilePath node
    uintn_t end_sz   = 4;                        // End node
    uintn_t total_sz = prefix_len + fp_sz + end_sz;

    // dp_buf must hold the full path.  512 bytes is safe for any real-world
    // combination of hardware prefix + file path on this machine.
    static uint8_t dp_buf[512];
    _Static_assert(sizeof(dp_buf) >= 512,
                   "dp_buf too small for hardware prefix + file path + end node");
    if (total_sz > sizeof(dp_buf)) return 0x8000000000000002ULL; // EFI_INVALID_PARAMETER

    uint8_t *p = dp_buf;
    // Copy hardware prefix (no End node).
    if (prefix_len > 0) {
        for (uintn_t i = 0; i < prefix_len; i++) p[i] = hw_dp[i];
        p += prefix_len;
    }
    // MediaFilePath (Type=4, SubType=4).
    p[0] = 0x04; p[1] = 0x04;
    p[2] = (uint8_t)(fp_sz & 0xff);
    p[3] = (uint8_t)(fp_sz >> 8);
    for (unsigned i = 0; i < path_chars; i++) {
        efi_char16_t c = path[i];
        p[4 + i * 2]     = (uint8_t)(c & 0xFF);
        p[4 + i * 2 + 1] = (uint8_t)((c >> 8) & 0xFF);
    }
    p += fp_sz;
    // End-of-Entire-Device-Path node.
    p[0] = 0x7f; p[1] = 0xff; p[2] = 4; p[3] = 0;

    // 4. LoadImage with BootPolicy=FALSE and the full device path.
    load_image_fn load_img = (load_image_fn)(uintptr_t)g_bs->load_image;
    start_image_fn start_img = (start_image_fn)(uintptr_t)g_bs->start_image;
    if (!load_img || !start_img) return 0x8000000000000002ULL;

    efi_handle_t new_image = 0;
    s = load_img(0, image_handle, dp_buf, 0, 0, &new_image);
    if (s != EFI_SUCCESS) return s;

    // 5. StartImage — binary reboots on success, so this should not return.
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

// Case-sensitive substring search — stdlib unavailable.
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

// BRR: global flag cache in memtest-BSS.
//
// Two distinct corruption sources at pass end on A1990:
//   (1) boot_params->brr_flags is UEFI-pool memory but its region stays
//       in pm_map (map_region only maps, doesn't reserve), so memory
//       tests overwrite it with test patterns.  Observed at pass end:
//       0x3b80b56d instead of the expected 0x15.
//   (2) .bss variables like this one DO survive memory tests (the
//       active image region is preserved across relocations) — but
//       startup64.S:258-268 zeros .bss[_bss.._end] on first boot AFTER
//       efi_setup() returns (which is where efi_menu() runs) and
//       BEFORE main() runs.  So anything we write here pre-EBS gets
//       wiped before main() can read it.
//
// Fix lives in two places:
//   - efi_menu() wrapper below writes the cache pre-EBS (doomed but
//     useful for a future refactor where startup BSS-zero is moved).
//   - main.c::global_init() re-populates the cache from bp->brr_flags
//     as the first act after boot_params is mapped — that read is made
//     before any tests have run, so bp is still pristine, and the BSS
//     cache then survives for the rest of the run.
uint32_t g_brr_flags_cached = 0;

static uint32_t efi_menu_impl(void *sys_table_arg, void *image_handle_arg, const char *cmdline);

// Direct-to-buffer log (bypasses con_puts so early returns still
// leave a breadcrumb for the post-EBS dump).
static void brr_buf_log(const char *s)
{
    while (*s && g_brr_preboot.len < sizeof(g_brr_preboot.buf) - 1) {
        g_brr_preboot.buf[g_brr_preboot.len++] = *s++;
    }
    g_brr_preboot.buf[g_brr_preboot.len] = 0;
}

uint32_t efi_menu(void *sys_table_arg, void *image_handle_arg, const char *cmdline)
{
    brr_buf_log("[wrap] efi_menu() entered\n");
    uint32_t f = efi_menu_impl(sys_table_arg, image_handle_arg, cmdline);
    brr_buf_log("[wrap] efi_menu_impl returned\n");
    g_brr_flags_cached = f;
    return f;
}

static uint32_t efi_menu_impl(void *sys_table_arg, void *image_handle_arg, const char *cmdline)
{
    brr_buf_log("[impl] entered\n");
    efi_system_table_t *st = (efi_system_table_t *)sys_table_arg;
    efi_handle_t image_handle = (efi_handle_t)image_handle_arg;
    if (!st) { brr_buf_log("[impl] st NULL, early-return\n"); return 0; }

    g_con_out = st->con_out;
    g_con_in  = st->con_in;
    g_bs      = st->boot_services;
    g_rs      = st->runtime_services;
    brr_buf_log("[impl] got con_out/in/bs/rs pointers\n");
    if (!g_con_out) brr_buf_log("[impl] WARNING g_con_out is NULL\n");
    if (!g_con_in)  brr_buf_log("[impl] WARNING g_con_in is NULL\n");
    if (!g_bs)      brr_buf_log("[impl] WARNING g_bs is NULL\n");
    if (!g_rs)      brr_buf_log("[impl] WARNING g_rs is NULL\n");

    if (!g_con_out || !g_con_in || !g_bs) {
        brr_buf_log("[impl] critical protocol NULL, early-return 0\n");
        return 0;
    }
    brr_buf_log("[impl] past NULL checks, proceeding\n");

    // Helper macro: pause N ms via BS Stall.  Used between pre-EBS
    // diagnostic lines so the user can actually read them before the
    // screen clears into memtest's framebuffer.
    efi_stall_fn brr_stall = (efi_stall_fn)(uintptr_t)g_bs->stall;
#define BRR_PAUSE_MS(ms) do { if (brr_stall) brr_stall((ms) * 1000u); } while (0)

    // BRR: capture Simple File System root handle pre-ExitBootServices.
    // Used by badmem_log to write state/pages files post-EBS.  This is
    // only safe if T2 firmware leaves the function pointers valid after
    // ExitBootServices; if it zeroes / unmaps them, the post-EBS call
    // will fault.  We also write a "brr-boot.txt" marker here pre-EBS
    // so the user can confirm from macOS that file-write is working at
    // all.  Reset to 0 before capture (pre-capture sentinel value is
    // (void *)1 to force .data placement; 0 means "capture attempted
    // and failed" -- distinguishable post-EBS).
    g_brr_fs_root = 0;

    // Probe: iterate every handle that supports SimpleFileSystem, try
    // writing a tiny test file, read it back in the same boot.  The
    // handle where write+readback round-trips is the one we keep.
    // LoadedImage->device_handle is tried FIRST (grub-loaded memtest
    // usually gets the ESP) but if that fails we fall through to the
    // full locate_handle enumeration.
    {
        // locate_handle with EFI_LOCATE_BY_PROTOCOL
        typedef efi_status_t (efiapi *locate_handle_fn)(
            int search_type, efi_guid_t *proto, void *key,
            uintn_t *buffer_size, efi_handle_t *buffer);
        locate_handle_fn locate_h = (locate_handle_fn)(uintptr_t)g_bs->locate_handle;

        // Get list of all SFS-supporting handles.
        uintn_t hsz = 0;
        efi_guid_t sfs_g = SFS_GUID;
        if (locate_h) locate_h(2 /* ByProtocol */, &sfs_g, 0, &hsz, 0);

        // Allocate handle buffer via allocate_pool.
        efi_handle_t *handles = 0;
        if (hsz > 0) {
            g_bs->allocate_pool(2 /* EFI_LOADER_DATA */, hsz, (void **)&handles);
            if (handles) locate_h(2, &sfs_g, 0, &hsz, handles);
        }
        unsigned nh = (unsigned)(hsz / sizeof(efi_handle_t));

        // Prefer LoadedImage->device_handle order: move to front.
        efi_loaded_image_t *li = 0;
        efi_handle_t preferred = 0;
        if (g_bs->handle_protocol(image_handle,
            (efi_guid_t *)&LOADED_IMAGE_GUID, (void **)&li) == EFI_SUCCESS && li) {
            preferred = li->device_handle;
        }

        con_puts("  [fs] SFS-handle scan: ");
        con_put_dec(nh);
        con_puts(" candidates\r\n");

        for (unsigned order = 0; order < nh + 1; order++) {
            efi_handle_t h;
            if (order == 0) {
                if (!preferred) continue;
                h = preferred;
                con_puts("  [fs]  try (preferred/LoadedImage) ");
            } else {
                if (!handles) break;
                h = handles[order - 1];
                if (h == preferred) continue;  // already tried
                con_puts("  [fs]  try handle#");
                con_put_dec(order - 1);
                con_puts(" ");
            }

            efi_brr_sfs_t *fs = 0;
            efi_status_t s = g_bs->handle_protocol(h,
                (efi_guid_t *)&SFS_GUID, (void **)&fs);
            if (s != EFI_SUCCESS || !fs) { con_puts("no SFS\r\n"); continue; }

            efi_brr_file_t *root = 0;
            s = fs->open_volume(fs, &root);
            if (s != EFI_SUCCESS || !root) { con_puts("open_volume fail\r\n"); continue; }

            // Create \EFI\BRR\ subdirectory.  Grub lives in \EFI\BOOT\
            // so we avoid that path (memtest/shim binaries could clash).
            static efi_char16_t dpath[] = {
                '\\','E','F','I','\\','B','R','R', 0
            };
            {
                efi_brr_file_t *d = 0;
                efi_status_t sd = root->open(root, &d, dpath,
                    EFI_BRR_FILE_MODE_READ | EFI_BRR_FILE_MODE_WRITE |
                    EFI_BRR_FILE_MODE_CREATE, 0x10 /* directory */);
                if (sd == EFI_SUCCESS && d) d->close(d);
            }

            // Write test marker to \EFI\BRR\boot.txt.  Dedicated subdir
            // nothing else (grub, memtest, shim) touches.
            static efi_char16_t tpath[] = {
                '\\','E','F','I','\\','B','R','R','\\','b','o','o','t','.','t','x','t', 0
            };
            efi_brr_file_t *f = 0;
            s = root->open(root, &f, tpath,
                EFI_BRR_FILE_MODE_READ | EFI_BRR_FILE_MODE_WRITE |
                EFI_BRR_FILE_MODE_CREATE, 0);
            if (s != EFI_SUCCESS || !f) {
                root->close(root);
                con_puts("open FAIL\r\n");
                continue;
            }
            static const char msg[] = "BRR-OK\n";
            f->set_position(f, 0);
            uintn_t wlen = sizeof(msg) - 1;
            s = f->write(f, &wlen, (void *)msg);
            f->flush(f);
            f->close(f);
            if (s != EFI_SUCCESS) {
                root->close(root);
                con_puts("write FAIL\r\n");
                continue;
            }

            // Immediate readback in the same boot to verify SFS layer
            // actually stored the bytes (not a silent-success on a
            // read-only volume).
            efi_brr_file_t *f2 = 0;
            s = root->open(root, &f2, tpath, EFI_BRR_FILE_MODE_READ, 0);
            if (s != EFI_SUCCESS || !f2) {
                root->close(root);
                con_puts("readback open FAIL\r\n");
                continue;
            }
            char rb[16] = {0};
            uintn_t rlen = sizeof(rb) - 1;
            s = f2->read(f2, &rlen, rb);
            f2->close(f2);
            if (s != EFI_SUCCESS || rlen < 6 ||
                rb[0] != 'B' || rb[1] != 'R' || rb[2] != 'R' ||
                rb[3] != '-' || rb[4] != 'O' || rb[5] != 'K') {
                root->close(root);
                con_puts("readback mismatch\r\n");
                continue;
            }

            // Round-trip passed.  Keep this root for post-EBS use.
            g_brr_fs_root = root;
            con_puts("ROUND-TRIP OK  <== using this handle\r\n");
            break;
        }

        if (handles) g_bs->free_pool(handles);

        if (!BRR_FS_ROOT_VALID(g_brr_fs_root)) {
            con_puts("  [fs] NO HANDLE round-tripped -- file persistence broken\r\n");
        }
    }
    BRR_PAUSE_MS(2000);

    // ---------------------------------------------------------------------------
    // Grub unattended mode: `brr_auto_page` or `brr_auto_chip` in the
    // kernel cmdline forces the corresponding automatic-trial flags and
    // skips the interactive menu entirely.  This is how grub entry 1
    // drives the full detect → trial → permanent flow without needing
    // the user to press any keys during the first boot.  NVRAM state
    // machine still takes over on subsequent boots (TRIAL_PENDING →
    // chainload shim, TRIAL_BOOTED → Y/Y prompt which DOES require
    // keyboard input — ConIn works at that point because we're still
    // pre-ExitBootServices).
    // ---------------------------------------------------------------------------
    // ---------------------------------------------------------------------------
    // Phase A (ORDER MATTERS): check NVRAM state machine FIRST, BEFORE
    // honouring cmdline auto-flags.  If a previous memtest pass wrote
    // TRIAL_PENDING_* to BrrMaskState, we need to chainload mask-shim.efi
    // this boot — not re-run memtest.  grub entry 1 unconditionally
    // passes `brr_auto_page`; if we returned early on that flag the
    // state machine would loop detect->detect->detect and the shim
    // would never run.
    // ---------------------------------------------------------------------------
    {
        char state_probe[64] = {0};
        unsigned probe_sz = read_state(state_probe, sizeof(state_probe) - 1);

        // Always-on diagnostic: shows on every boot whether NVRAM
        // persistence across the T2 graceful-shutdown path is actually
        // working.  On the detect->shutdown->boot cycle we expect to see
        // "BrrMaskState sz=18 val=TRIAL_PENDING_PAGE" here; if we see
        // sz=0 the write did not survive and the shim chain won't fire.
        con_puts("\r\n  [probe] BrrMaskState sz=");
        con_put_dec(probe_sz);
        if (probe_sz > 0) {
            con_puts(" val=\"");
            for (unsigned i = 0; i < probe_sz && i < 40; i++) {
                unsigned char ch = (unsigned char)state_probe[i];
                char buf[2] = { (ch >= 0x20 && ch < 0x7f) ? (char)ch : '.', 0 };
                con_puts(buf);
            }
            con_puts("\"");
        } else {
            con_puts(" (variable not found -- T2 did NOT persist write)");
        }
        con_puts("\r\n");

        BRR_PAUSE_MS(1500);

        // Also probe the USB ESP files written by the prior boot.
        // Reads happen pre-EBS via the same SimpleFileSystem root we
        // just opened.  If \brr-state.txt shows TRIAL_PENDING_PAGE,
        // the post-EBS file-write path works on this T2 and we can
        // drop NVRAM entirely.
        if (BRR_FS_ROOT_VALID(g_brr_fs_root)) {
            static const efi_char16_t files[][32] = {
                { '\\','E','F','I','\\','B','R','R','\\','b','o','o','t','.','t','x','t', 0 },
                { '\\','E','F','I','\\','B','R','R','\\','s','t','a','t','e','.','t','x','t', 0 },
                { '\\','E','F','I','\\','B','R','R','\\','p','a','g','e','s','.','t','x','t', 0 },
            };
            static const char *labels[] = { "brr-boot", "brr-state", "brr-pages" };
            for (int idx = 0; idx < 3; idx++) {
                efi_brr_file_t *f = 0;
                efi_status_t s = g_brr_fs_root->open(g_brr_fs_root, &f,
                    (efi_char16_t *)files[idx], EFI_BRR_FILE_MODE_READ, 0);
                con_puts("  [file] ");
                con_puts(labels[idx]);
                con_puts(".txt ");
                if (s != EFI_SUCCESS || !f) {
                    con_puts("NOT FOUND (status 0x");
                    con_put_dec((unsigned)(s & 0xFFFFFFFFu));
                    con_puts(")\r\n");
                    BRR_PAUSE_MS(1500);
                    continue;
                }
                static char rbuf[128];
                uintn_t rlen = sizeof(rbuf) - 1;
                s = f->read(f, &rlen, rbuf);
                f->close(f);
                if (s != EFI_SUCCESS) {
                    con_puts("READ FAILED (status 0x");
                    con_put_dec((unsigned)(s & 0xFFFFFFFFu));
                    con_puts(")\r\n");
                    BRR_PAUSE_MS(1500);
                    continue;
                }
                rbuf[rlen < sizeof(rbuf) ? rlen : sizeof(rbuf) - 1] = 0;
                con_puts("bytes=");
                con_put_dec((unsigned)rlen);
                con_puts(" first40=\"");
                for (unsigned i = 0; i < rlen && i < 40; i++) {
                    unsigned char ch = (unsigned char)rbuf[i];
                    char buf[2] = { (ch >= 0x20 && ch < 0x7f) ? (char)ch :
                                     (ch == '\n' ? '|' : '.'), 0 };
                    con_puts(buf);
                }
                con_puts("\"\r\n");
                BRR_PAUSE_MS(1500);
            }
        } else {
            con_puts("  [file] SFS root unavailable, cannot probe files\r\n");
            BRR_PAUSE_MS(2000);
        }

        // Final pause so user can see overall probe output before tests
        // start (or chainload fires).
        con_puts("\r\n  [probe] done -- continuing in 5 s ...\r\n");
        BRR_PAUSE_MS(5000);

        if (probe_sz == 0) {
            // No existing state — this is a fresh NONE boot; run the
            // decoder self-test once so page-mode can later upgrade to
            // row-mode in the shim.
            decoder_selftest_run(st);
        }
    }

    uint32_t auto_flags = check_nvram_state(image_handle);
    if (auto_flags != 0xFFFFFFFFu) {
        // State said: chainload shim (TRIAL_PENDING_*) or prompt for
        // permanent (TRIAL_BOOTED).  Either way, the function already
        // did the chainload — the returned flags tell the caller what
        // to do if the chainload failed.  Return now; cmdline flags
        // are irrelevant once the state machine owns the boot.
        return auto_flags;
    }

    // ---------------------------------------------------------------------------
    // Phase B: no pending state.  Honour cmdline auto-flags to drive a
    // fresh detect run unattended.  grub entry 1 sets brr_auto_page,
    // entry 2 sets brr_auto_chip, entry 7 adds brr_fast.  Tests run,
    // errors get logged, BrrBadPages + BrrMaskState=TRIAL_PENDING_PAGE
    // written at pass end, auto-reboot — and THIS function runs again
    // on the next boot, goes through Phase A, and chainloads the shim.
    // ---------------------------------------------------------------------------
    if (cmdline_has(cmdline, "brr_auto_page")) {
        con_puts("\r\n  [auto] brr_auto_page in cmdline -> page-mask mode\r\n\r\n");
        return BRR_FLAG_SKIP_COUNTDOWNS | BRR_FLAG_AUTO_REBOOT_AFTER_PASS
             | BRR_FLAG_TRIAL_PAGE;
    }
    if (cmdline_has(cmdline, "brr_auto_chip")) {
        con_puts("\r\n  [auto] brr_auto_chip in cmdline -> chip-mask mode\r\n\r\n");
        return BRR_FLAG_SKIP_COUNTDOWNS | BRR_FLAG_AUTO_REBOOT_AFTER_PASS
             | BRR_FLAG_AUTO_TRIAL_CHIP | BRR_FLAG_TRIAL_CHIP;
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
                // Using 4-arg UEFI spec signature.
                typedef void (efiapi *reset_fn)(int, efi_status_t, uintn_t, void *);
                reset_fn do_reset = (reset_fn)(uintptr_t)g_rs->reset_system;
                do_reset(EFI_RESET_COLD, 0, 0, (void *)0);
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

// BRR: dump the pre-EBS con_puts mirror buffer to memtest's blue
// framebuffer so the user can see diagnostic output that never
// reached UEFI ConOut (grub-graphics-mode console is write-to-void
// on A1990).  Called from main.c::global_init after screen_init.
void brr_preboot_log_dump(void)
{
    // Unconditional header: tells us at a glance whether the struct
    // is reachable in .data (magic should be 0xB007B007 always) and
    // whether efi_menu added anything on top of the init marker.
    display_scrolled_message(0, "=== pre-EBS log: magic=%x len=%u ===",
                              (uintptr_t)g_brr_preboot.magic,
                              (uintptr_t)g_brr_preboot.len);
    scroll();

    if (g_brr_preboot.len == 0) {
        display_scrolled_message(0, "    (empty -- efi_menu did not run or returned before any con_puts)");
        scroll();
        return;
    }

    char line[128];
    unsigned i = 0;
    while (i < g_brr_preboot.len) {
        unsigned j = 0;
        while (i < g_brr_preboot.len && j < sizeof(line) - 1) {
            char c = g_brr_preboot.buf[i++];
            if (c == '\r') continue;
            if (c == '\n') break;
            line[j++] = c;
        }
        line[j] = 0;
        if (j > 0) {
            display_scrolled_message(0, "%s", (uintptr_t)(uintptr_t)line);
            scroll();
        }
    }
    display_scrolled_message(0, "=== end pre-EBS log ===");
    scroll();
}
