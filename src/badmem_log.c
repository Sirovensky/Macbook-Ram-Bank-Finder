// SPDX-License-Identifier: GPL-2.0
//
// badmem_log — accumulate bad physical page addresses + chip designators
// during a memtest run, then dump them on-screen as a copy-pasteable block
// at pass end.  No NVRAM writes: post-ExitBootServices Runtime SetVariable
// does not persist on Apple T2, so persistence happens later in
// brr-entry.efi (pre-EBS) after the user photographs this block.
//
// Uses fixed static arrays (no heap dependency).  Limits:
//   MAX_ENTRIES = 4096 unique page-aligned bad addresses
//   CHIPS_BUF_SIZE = 256 bytes of comma-separated chip designators
//   MAX_BAD_ROWS = 256 bank/rank/row tuples (row-mode accumulator)
//   SKIP_MAX = 64 error-burst regions excluded mid-test
//
// Earlier revisions called Runtime SetVariable at pass end to flush these
// structures to NVRAM.  That code was removed; see git history if you
// want to resurrect it for a non-T2 platform.

#include "badmem_log.h"

#include "display.h"

extern int scroll_message_row;
extern void scroll(void);

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
    display_scrolled_message(0, "  BAD ADDRESSES (%u found) -- photograph this line:",
                              (uintptr_t)log_count);
    scroll();

    if (log_count == 0) {
        display_scrolled_message(0, "    (none detected)");
        scroll();
        return;
    }

    // Print up to ~60 chars per line, comma-separated hex.
    char line[128];
    unsigned line_len = 0;
    line[0] = 0;
    for (unsigned i = 0; i < log_count; i++) {
        char abuf[24];
        unsigned pos = 0;
        abuf[pos++] = '0'; abuf[pos++] = 'x';
        uint64_t v = log_pages[i];
        int started = 0;
        for (int shift = 60; shift >= 0; shift -= 4) {
            unsigned nib = (unsigned)((v >> shift) & 0xFu);
            if (!started && nib == 0 && shift > 0) continue;
            started = 1;
            abuf[pos++] = nib < 10 ? ('0' + nib) : ('a' + nib - 10);
        }
        if (!started) abuf[pos++] = '0';
        abuf[pos] = 0;

        unsigned alen = pos;
        unsigned comma = (i + 1 < log_count) ? 2u : 0u;
        if (line_len + alen + comma > 60) {
            display_scrolled_message(0, "    %s", (uintptr_t)line);
            scroll();
            line_len = 0;
            line[0] = 0;
        }
        for (unsigned k = 0; k < alen; k++) line[line_len++] = abuf[k];
        if (i + 1 < log_count) { line[line_len++] = ','; line[line_len++] = ' '; }
        line[line_len] = 0;
    }
    if (line_len > 0) {
        display_scrolled_message(0, "    %s", (uintptr_t)line);
        scroll();
    }
}


// ---------------------------------------------------------------------------
// Row accumulator (Track C): records bank/rank/row tuples for chip-level
// diagnostics.  NVRAM persistence for rows was removed (post-EBS runtime
// SetVariable does not persist on Apple T2; brr-entry.efi handles
// persistence pre-EBS on a per-page basis).  The record function stays
// because error_hook.c calls it during tests.
// ---------------------------------------------------------------------------

#define MAX_BAD_ROWS  256

typedef struct __attribute__((packed)) {
    uint8_t  ch;
    uint8_t  rank;
    uint8_t  bg;
    uint8_t  bank;
    uint32_t row;
} bad_row_t;
_Static_assert(sizeof(bad_row_t) == 8, "row tuple layout");

static bad_row_t row_log[MAX_BAD_ROWS];
static unsigned  row_count;

void badmem_log_record_row(uint8_t channel, uint8_t rank,
                            uint8_t bg, uint8_t bank, uint32_t row)
{
    for (unsigned i = 0; i < row_count; i++) {
        if (row_log[i].ch   == channel &&
            row_log[i].rank == rank    &&
            row_log[i].bg   == bg      &&
            row_log[i].bank == bank    &&
            row_log[i].row  == row)
            return;
    }
    if (row_count >= MAX_BAD_ROWS) return;
    row_log[row_count].ch   = channel;
    row_log[row_count].rank = rank;
    row_log[row_count].bg   = bg;
    row_log[row_count].bank = bank;
    row_log[row_count].row  = row;
    row_count++;
}
