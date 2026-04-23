// SPDX-License-Identifier: GPL-2.0
//
// badmem_log — accumulate bad physical page addresses during a memtest run,
// then dump them as a pasteable badmem.txt block and/or persist them to NVRAM
// so the next boot's mask-install / mask-shim can read them automatically.
//
// Uses a fixed static array (no heap dependency).  Maximum 4096 unique
// page-aligned ranges — well beyond any realistic BGA failure pattern.

#include "badmem_log.h"

// efi.h is reachable via -I../../boot in the system/board build.
#include "efi.h"

#include "display.h"

extern int scroll_message_row;
extern void scroll(void);

// Accessor provided by patches/0008-expose-efi-rt.patch applied to hwctrl.c.
extern efi_runtime_services_t *hwctrl_get_efi_rt(void);

// ---------------------------------------------------------------------------
// NVRAM vendor GUID shared with mask-shim and mask-install.
// {3E3E9DB2-1A2B-4B5C-9D1E-5F6A7B8C9D0E}
// ---------------------------------------------------------------------------
static const efi_guid_t BRR_GUID = {
    0x3e3e9db2, 0x1a2b, 0x4b5c,
    { 0x9d, 0x1e, 0x5f, 0x6a, 0x7b, 0x8c, 0x9d, 0x0e }
};

// Variable name for the binary bad-pages array.
// UTF-16 literal: L"BrrBadPages"
static const efi_char16_t BRR_VARNAME_BADPAGES[] = {
    'B','r','r','B','a','d','P','a','g','e','s', 0
};

// Variable name for chip designators.  L"BrrBadChips"
static const efi_char16_t BRR_VARNAME_BADCHIPS[] = {
    'B','r','r','B','a','d','C','h','i','p','s', 0
};

// Variable name for state machine.  L"BrrMaskState"
static const efi_char16_t BRR_VARNAME_STATE[] = {
    'B','r','r','M','a','s','k','S','t','a','t','e', 0
};

// Legacy variable names for migration fallback (read-only; we only write Brr* names).
static const efi_char16_t LEGACY_VARNAME_BADPAGES[] = {
    'A','1','9','9','0','B','a','d','P','a','g','e','s', 0
};
static const efi_char16_t LEGACY_VARNAME_BADCHIPS[] = {
    'A','1','9','9','0','B','a','d','C','h','i','p','s', 0
};
static const efi_char16_t LEGACY_VARNAME_STATE[] = {
    'A','1','9','9','0','M','a','s','k','S','t','a','t','e', 0
};

// State strings (ASCII, written to NVRAM as-is).
#define STATE_TRIAL_PENDING_PAGE  "TRIAL_PENDING_PAGE"
#define STATE_TRIAL_PENDING_CHIP  "TRIAL_PENDING_CHIP"

// ---------------------------------------------------------------------------
// SetVariable / GetVariable function-pointer types.
//
// efi_runtime_services_t in memtest86plus/boot/efi.h stores set_variable and
// get_variable as plain `unsigned long` (not typed fn ptrs).  We cast them
// here to avoid requiring changes to the upstream header.
// ---------------------------------------------------------------------------
typedef efi_status_t (efiapi *set_variable_fn)(
    efi_char16_t *name,
    efi_guid_t   *guid,
    uint32_t      attrs,
    uintn_t       data_size,
    void         *data);

typedef efi_status_t (efiapi *get_variable_fn)(
    efi_char16_t *name,
    efi_guid_t   *guid,
    uint32_t     *attrs,
    uintn_t      *data_size,
    void         *data);

// EFI variable attribute bits.
#define EFI_VAR_NV_BS_RT  (0x00000001u | 0x00000002u | 0x00000004u)

// Readback verification: immediately GetVariable after SetVariable and
// compare size.  If GetVariable returns non-success or size=0, the write
// was silently rejected (T2 Full Security namespace filter, per-GUID
// policy, etc.) and the shim will never see the state on next boot.
// Prints a loud warning so the user knows why the flow is stuck.
static int verify_readback(efi_runtime_services_t *rt,
                            const efi_char16_t *name,
                            const char *label,
                            uintn_t expected_min_size)
{
    if (!rt) return 0;
    get_variable_fn get_var = (get_variable_fn)(uintptr_t)rt->get_variable;
    if (!get_var) return 0;

    static uint8_t probe[4096];
    uintn_t sz = sizeof(probe);
    uint32_t attrs = 0;
    efi_status_t gs = get_var((efi_char16_t *)name,
                              (efi_guid_t *)&BRR_GUID,
                              &attrs, &sz, probe);
    if (gs == EFI_SUCCESS && sz >= expected_min_size) {
        display_scrolled_message(0, "[nvram]   readback %s OK (%u bytes, attrs=%x)",
                                 (uintptr_t)label, (uintptr_t)sz, (uintptr_t)attrs);
        scroll();
        return 1;
    }
    display_scrolled_message(0, "[nvram]   READBACK FAILED %s (get_var status=%x sz=%u)",
                             (uintptr_t)label, (uintptr_t)gs, (uintptr_t)sz);
    scroll();
    display_scrolled_message(0, "[nvram]   -> T2 silently rejected write. Lower Secure Boot to No Security.");
    scroll();
    return 0;
}

// ---------------------------------------------------------------------------
// Binary blob layout written to NVRAM.
// ---------------------------------------------------------------------------
typedef struct {
    uint32_t version;   // = 1
    uint32_t count;     // number of uint64_t PA entries that follow
    // uint64_t pages[count] follows immediately.
} badpages_hdr_t;

// ---------------------------------------------------------------------------
// Page accumulator.
// ---------------------------------------------------------------------------

#define PAGE_SIZE       4096ULL
#define PAGE_MASK       (~(PAGE_SIZE - 1))
#define MAX_ENTRIES     4096

static uint64_t log_pages[MAX_ENTRIES];
static unsigned log_count;

// Accessor for error_hook.c's board_decode_pass() — returns internal
// array pointer + count.  Caller must not mutate.  Single-threaded access
// expected at pass boundary (no locking).
uint64_t *badmem_log_entries(unsigned *out_count)
{
    if (out_count) *out_count = log_count;
    return log_pages;
}

// Current count of logged bad pages, for auto-bail threshold checks.
unsigned badmem_log_count(void)
{
    return log_count;
}

// ---------------------------------------------------------------------------
// Chip designator accumulator.
//
// BrrBadChips NVRAM layout: comma-separated designator strings in a flat
// C-string buffer, e.g. "U2620,U2310" (NUL-terminated at end only).
// Total size capped at 256 bytes including the terminating NUL.  The
// consumer (mask-shim) splits on commas, tolerates leading/trailing spaces.
// Comma chosen over NUL so the reader can treat the whole blob as one
// C string without needing to know the underlying byte length.
// ---------------------------------------------------------------------------

#define CHIPS_BUF_SIZE  256

static char  chip_buf[CHIPS_BUF_SIZE];
static unsigned chip_buf_used;   // bytes written so far (excludes final NUL)

void badmem_log_record_chip(const char *designator)
{
    if (!designator || !designator[0]) return;

    // Compute length of incoming designator.
    unsigned dlen = 0;
    while (designator[dlen]) dlen++;

    // Need room for separator (if any), designator, and terminating NUL.
    unsigned sep = (chip_buf_used > 0) ? 1 : 0;
    if (chip_buf_used + sep + dlen + 1 > CHIPS_BUF_SIZE) return;

    // Deduplicate: scan existing comma-separated entries.
    unsigned off = 0;
    while (off < chip_buf_used) {
        unsigned elen = 0;
        while (off + elen < chip_buf_used && chip_buf[off + elen] != ',') elen++;
        if (elen == dlen) {
            unsigned match = 1;
            for (unsigned i = 0; i < elen; i++) {
                if (chip_buf[off + i] != designator[i]) { match = 0; break; }
            }
            if (match) return;  // already recorded
        }
        off += elen;
        if (off < chip_buf_used && chip_buf[off] == ',') off++;
    }

    // Append: optional comma, then designator, then NUL-terminate.
    if (sep) chip_buf[chip_buf_used++] = ',';
    for (unsigned i = 0; i < dlen; i++)
        chip_buf[chip_buf_used++] = designator[i];
    chip_buf[chip_buf_used] = '\0';  // terminator; not counted in used
}

void badmem_log_record(uint64_t phys_addr)
{
    uint64_t page = phys_addr & PAGE_MASK;

    // Linear dedup scan — call frequency is low (one per detected error).
    for (unsigned i = 0; i < log_count; i++) {
        if (log_pages[i] == page)
            return;
    }

    if (log_count < MAX_ENTRIES) {
        log_pages[log_count++] = page;
    }
    // If buffer full, silently drop — user will see truncated list but
    // the addresses already recorded are still valid.
}

// ---------------------------------------------------------------------------
// Skip-list: 1 MiB regions to exclude from further testing this run.
//
// Populated by common_err() when an error burst is detected on a single
// 1 MiB page (see app/error.c / patch 0013).  Queried by
// board_prune_vm_map() which runs after every setup_vm_map() to remove
// recorded regions from the test windows.  Dedupes + merges overlaps.
// Capped at SKIP_MAX — each entry is 16 bytes, so 64 entries = 1 KiB.
// ---------------------------------------------------------------------------

#define SKIP_MAX  64

static struct badmem_skip_range skip_list[SKIP_MAX];
static unsigned skip_list_count;

void badmem_log_add_skip(uint64_t start_pa, uint64_t end_pa)
{
    if (end_pa <= start_pa) return;

    // Dedup / merge: if the new range overlaps or abuts an existing one,
    // grow the existing entry AND bump its hit counter.  Chains of
    // adjacent 1 MiB bursts coalesce into one entry whose hit count
    // reflects how many distinct burst triggers contributed to it.
    for (unsigned i = 0; i < skip_list_count; i++) {
        if (end_pa < skip_list[i].start || start_pa > skip_list[i].end) continue;
        if (start_pa < skip_list[i].start) skip_list[i].start = start_pa;
        if (end_pa   > skip_list[i].end)   skip_list[i].end   = end_pa;
        if (skip_list[i].hits < 0xFFFFFFFFu) skip_list[i].hits++;
        return;
    }

    if (skip_list_count < SKIP_MAX) {
        skip_list[skip_list_count].start = start_pa;
        skip_list[skip_list_count].end   = end_pa;
        skip_list[skip_list_count].hits  = 1;
        skip_list_count++;
    }
    // If the table overflows we've already masked ~64 MiB in 1 MiB chunks —
    // the chip is effectively dead.  The pass-end summary will show this
    // count and the user can escalate to chip-mode manually.
}

const struct badmem_skip_range *badmem_log_skip_list(unsigned *out_count)
{
    if (out_count) *out_count = skip_list_count;
    return skip_list;
}

unsigned badmem_log_skip_count(void)
{
    return skip_list_count;
}

// ---------------------------------------------------------------------------
// Screen dump (original workflow — still used).
// ---------------------------------------------------------------------------

void badmem_log_dump(void)
{
    display_scrolled_message(0, "");
    scroll();
    display_scrolled_message(0, "--- badmem.txt contents (paste into /EFI/BOOT/badmem.txt) ---");
    scroll();

    if (log_count == 0) {
        display_scrolled_message(0, "# (no bad pages detected this run)");
        scroll();
    } else {
        for (unsigned i = 0; i < log_count; i++) {
            // Format: 0xADDR,4096
            display_scrolled_message(0, "0x%x,4096", (uintptr_t)log_pages[i]);
            scroll();
        }
    }

    display_scrolled_message(0, "--- end badmem.txt ---  (%u page(s) recorded)", log_count);
    scroll();
}

// ---------------------------------------------------------------------------
// NVRAM flush — new automatic workflow.
// ---------------------------------------------------------------------------

// Access the brr_flags stored in boot_params (written by efi_menu before
// ExitBootServices).  The boot_params_t layout is defined in
// memtest86plus/boot/bootparams.h and patched by 0006-bootparams-a1990-flags.
// We forward-declare only the fields we need to avoid a complex include here.
// boot_params_addr is a uintptr_t exported by the memtest86plus boot shim.
extern uintptr_t boot_params_addr;

// Offset of brr_flags in boot_params_t (from patch 0006):
// cmd_line_ptr is at 0x228 (4 bytes), unused6 fills 0x22c-0x238,
// cmd_line_size at 0x238 (4 bytes), then brr_flags at 0x23c.
#define BRR_FLAGS_OFFSET  0x23c

// Flag bit for chip-mode trial (must match efi_menu.h BRR_FLAG_AUTO_TRIAL_CHIP).
#define BRR_FLAG_AUTO_TRIAL_CHIP_BIT  (1u << 3)

void badmem_log_flush_nvram(void)
{
    efi_runtime_services_t *rt = hwctrl_get_efi_rt();
    if (!rt) {
        // BIOS boot or EFI system table not found: NVRAM path unavailable.
        return;
    }

    // Cap entries at MAX_ENTRIES (already enforced during recording, but
    // assert defensively since we compute blob size below).
    unsigned n = log_count;
    if (n > MAX_ENTRIES)
        n = MAX_ENTRIES;

    // Build the binary blob in a static buffer.
    // Header (8 bytes) + n * 8 bytes of PAs = up to 32776 bytes.
    // 4096 entries × 8 = 32768 + 8 header = 32776 — comfortably under the
    // 64 KiB per-variable NVRAM limit enforced by typical UEFI firmware.
    static uint8_t blob[sizeof(badpages_hdr_t) + MAX_ENTRIES * sizeof(uint64_t)];
    badpages_hdr_t *hdr = (badpages_hdr_t *)blob;
    hdr->version = 1;
    hdr->count   = n;

    uint64_t *pa_array = (uint64_t *)(blob + sizeof(badpages_hdr_t));
    for (unsigned i = 0; i < n; i++) {
        pa_array[i] = log_pages[i];
    }

    uintn_t blob_size = sizeof(badpages_hdr_t) + (uintn_t)n * sizeof(uint64_t);

    // Cast the unsigned-long field to the proper function-pointer type.
    set_variable_fn set_var = (set_variable_fn)(uintptr_t)rt->set_variable;

    efi_status_t status = set_var(
        (efi_char16_t *)BRR_VARNAME_BADPAGES,
        (efi_guid_t *)&BRR_GUID,
        EFI_VAR_NV_BS_RT,
        blob_size,
        blob);

    if (status == EFI_SUCCESS) {
        display_scrolled_message(0, "[nvram] saved %u bad page(s) to NVRAM", n);
        scroll();
        verify_readback(rt, BRR_VARNAME_BADPAGES, "BrrBadPages",
                        sizeof(badpages_hdr_t));
    } else {
        // SetVariable failed — NVRAM might be read-only (T2 Medium Security,
        // firmware locked, or BrrBadPages blob exceeded per-variable cap).
        // Print a non-fatal warning and continue; the screen dump is still
        // available for the manual workflow AND we still try to write the
        // chip/row/state variables below (they are smaller and often succeed
        // when BrrBadPages alone is rejected by per-var size limits).
        //
        // NOTE: earlier revisions returned here on the first failure, which
        // lost BrrBadChips AND left the state machine untouched, causing the
        // next boot's efi_menu to re-run memtest instead of chainloading
        // the shim.  Continue through — each subsequent write is independent.
        display_scrolled_message(0,
            "[nvram] SetVariable BrrBadPages failed (status %x) -- continuing with smaller vars",
            (uintptr_t)status);
        scroll();
    }

    // ---------------------------------------------------------------------------
    // Phase B: determine chip-mode flag and conditionally flush BrrBadChips.
    // ---------------------------------------------------------------------------
    // Read from efi_menu.c's BSS-stable cache, NOT from
    // boot_params->brr_flags.  The latter gets clobbered by memory
    // tests writing over the struct (map_region doesn't remove the
    // region from pm_map, so tests write garbage on top of the
    // Linux boot-protocol struct).  Observed corruption: bit 3
    // spuriously set, triggering chip_mode when user asked for page.
    extern uint32_t g_brr_flags_cached;
    uint32_t brr_flags = g_brr_flags_cached;

    int chip_mode = (brr_flags & BRR_FLAG_AUTO_TRIAL_CHIP_BIT) != 0;

    if (chip_mode && chip_buf_used > 0) {
        // Write BrrBadChips: comma-separated designator list (e.g.
        // "U2620,U2310").  Include the trailing NUL so the reader can
        // Write comma-separated list WITHOUT trailing NUL — matches the
        // no-NUL convention used by mask_nvram_set_ascii / state strings,
        // and avoids the off-by-one where a full 256-byte payload would
        // overflow the reader's (bufsz-1)-byte capacity in
        // mask_nvram_get_ascii.  Reader NUL-terminates at buf[sz].
        efi_status_t sc = set_var(
            (efi_char16_t *)BRR_VARNAME_BADCHIPS,
            (efi_guid_t *)&BRR_GUID,
            EFI_VAR_NV_BS_RT,
            (uintn_t)chip_buf_used, chip_buf);

        if (sc == EFI_SUCCESS) {
            display_scrolled_message(0, "[nvram] saved BrrBadChips (%u byte(s))",
                                     chip_buf_used);
            scroll();
        } else {
            display_scrolled_message(0,
                "[nvram] WARNING: could not save BrrBadChips (status %x)",
                (uintptr_t)sc);
            scroll();
        }
    }

    // ---------------------------------------------------------------------------
    // Phase C: set BrrMaskState to TRIAL_PENDING_PAGE or TRIAL_PENDING_CHIP.
    // This tells mask-shim on the next USB boot which mask to apply.
    // ---------------------------------------------------------------------------
    const char *new_state = chip_mode ? STATE_TRIAL_PENDING_CHIP
                                      : STATE_TRIAL_PENDING_PAGE;

    // Compute length of new_state string.
    uintn_t state_len = 0;
    while (new_state[state_len]) state_len++;

    efi_status_t ss = set_var(
        (efi_char16_t *)BRR_VARNAME_STATE,
        (efi_guid_t *)&BRR_GUID,
        EFI_VAR_NV_BS_RT,
        state_len, (void *)new_state);

    if (ss == EFI_SUCCESS) {
        display_scrolled_message(0, "[nvram] state -> %s", new_state);
        scroll();
        verify_readback(rt, BRR_VARNAME_STATE, "BrrMaskState", state_len);
    } else {
        display_scrolled_message(0, "[nvram] WARNING: could not set BrrMaskState (status %x)",
                                 (uintptr_t)ss);
        scroll();
    }

    // Delete legacy A1990* variables to complete migration (best-effort).
    //
    // NOTE — T2 firmware requires NV|BS|RT attributes even for a delete
    // (size=0) call; attrs=0 is rejected with EFI_INVALID_PARAMETER and
    // the variable stays on disk as cruft.  This was originally fixed
    // in mask-shim / mask-install but the memtest-side legacy cleanup
    // still passed attrs=0 and therefore silently did nothing on T2.
    set_var((efi_char16_t *)LEGACY_VARNAME_BADPAGES,
            (efi_guid_t *)&BRR_GUID, EFI_VAR_NV_BS_RT, 0, 0);
    set_var((efi_char16_t *)LEGACY_VARNAME_BADCHIPS,
            (efi_guid_t *)&BRR_GUID, EFI_VAR_NV_BS_RT, 0, 0);
    set_var((efi_char16_t *)LEGACY_VARNAME_STATE,
            (efi_guid_t *)&BRR_GUID, EFI_VAR_NV_BS_RT, 0, 0);
}

// ---------------------------------------------------------------------------
// Row accumulator (Track C).
// ---------------------------------------------------------------------------

// Variable name for bad-row tuples.  L"BrrBadRows"
static const efi_char16_t BRR_VARNAME_BADROWS[] = {
    'B','r','r','B','a','d','R','o','w','s', 0
};

#define MAX_BAD_ROWS  256

// Packed tuple: ch, rank, bg, bank (1 byte each) + row (4 bytes) = 8 bytes.
// Shim reads byte-for-byte via cfl_decode_shim.c; __attribute__((packed))
// + _Static_assert guards against future field additions breaking the ABI.
typedef struct __attribute__((packed)) {
    uint8_t  ch;
    uint8_t  rank;
    uint8_t  bg;
    uint8_t  bank;
    uint32_t row;
} bad_row_t;
_Static_assert(sizeof(bad_row_t) == 8,
               "BrrBadRows on-disk layout must stay 8 bytes/tuple");

static bad_row_t row_log[MAX_BAD_ROWS];
static unsigned  row_count;

void badmem_log_record_row(uint8_t channel, uint8_t rank,
                            uint8_t bg, uint8_t bank, uint32_t row)
{
    // Deduplicate: scan existing entries.
    for (unsigned i = 0; i < row_count; i++) {
        if (row_log[i].ch   == channel &&
            row_log[i].rank == rank    &&
            row_log[i].bg   == bg      &&
            row_log[i].bank == bank    &&
            row_log[i].row  == row)
            return;  // already recorded
    }

    if (row_count >= MAX_BAD_ROWS) return;  // cap

    row_log[row_count].ch   = channel;
    row_log[row_count].rank = rank;
    row_log[row_count].bg   = bg;
    row_log[row_count].bank = bank;
    row_log[row_count].row  = row;
    row_count++;
}

// Binary blob header written to BrrBadRows.
typedef struct {
    uint32_t version;   // = 1
    uint32_t count;     // number of bad_row_t tuples that follow
} badrows_hdr_t;

void badmem_log_flush_rows_nvram(void)
{
    if (row_count == 0) return;

    efi_runtime_services_t *rt = hwctrl_get_efi_rt();
    if (!rt) return;

    unsigned n = row_count;
    if (n > MAX_BAD_ROWS) n = MAX_BAD_ROWS;

    // blob: 8-byte header + n * 8-byte tuples = max 2056 bytes.
    static uint8_t blob[sizeof(badrows_hdr_t) + MAX_BAD_ROWS * sizeof(bad_row_t)];
    badrows_hdr_t *hdr = (badrows_hdr_t *)blob;
    hdr->version = 1;
    hdr->count   = n;

    bad_row_t *dst = (bad_row_t *)(blob + sizeof(badrows_hdr_t));
    for (unsigned i = 0; i < n; i++) dst[i] = row_log[i];

    uintn_t blob_size = sizeof(badrows_hdr_t) + (uintn_t)n * sizeof(bad_row_t);

    set_variable_fn set_var = (set_variable_fn)(uintptr_t)rt->set_variable;

    efi_status_t s = set_var(
        (efi_char16_t *)BRR_VARNAME_BADROWS,
        (efi_guid_t *)&BRR_GUID,
        EFI_VAR_NV_BS_RT,
        blob_size, blob);

    if (s == EFI_SUCCESS) {
        display_scrolled_message(0, "[nvram] saved %u bad row(s) to BrrBadRows", n);
        scroll();
    } else {
        display_scrolled_message(0,
            "[nvram] WARNING: could not save BrrBadRows (status %x)",
            (uintptr_t)s);
        scroll();
    }
}
