// SPDX-License-Identifier: GPL-2.0
//
// Error hook called from memtest86plus app/error.c::common_err().
//
// DESIGN NOTE — why this is now ZERO-display:
//
// Earlier versions did PA decode, board_detect(), and display output
// inside this hook.  On A1990 that caused system-wide hangs:
//
//   1. `common_err()` holds `error_mutex` across the whole hook call.
//   2. Any call to scroll() can enter a blocking wait if scroll_lock is
//      ON and the message area is full (see app/display.c:scroll()).
//   3. The T2 keyboard path post-ExitBootServices can inject stray
//      keypresses; the space-bar toggle on scroll_lock is one event
//      away from turning the lock ON even though we never intended it.
//   4. Once scroll_lock is ON + screen full, the next scroll() inside
//      any CPU's error path blocks forever waiting for <Enter>, which
//      the user cannot reliably send on T2 post-EBS.  error_mutex is
//      held the entire time → BSP cannot progress → timer freezes,
//      ESC appears dead, test pattern text is frozen.
//
// Fix: this hook now ONLY appends the PA to a static array
// (badmem_log_record) and returns.  No decode.  No SMBIOS lookup.
// No display.  No scroll.  Nothing that can ever block.
//
// All heavy work (decode + summary display + NVRAM flush) happens ONCE
// at pass boundary from CPU 0 only, via board_decode_pass() invoked
// from app/main.c (patch 0009).  There is no per-error screen output
// during the test; the user sees errors only in the pass-end summary
// and as the total error counter in the upper pinned area (which
// memtest86plus maintains itself via its own per-error display path,
// already guarded by error_mutex + ERROR_LIMIT).

#include "stdint.h"

#include "badmem_log.h"

void board_report_error(uint64_t addr, uint64_t xor_bits)
{
    // Pure fast path — called from common_err() under error_mutex.  Must
    // NEVER call scroll(), display_scrolled_message(), board_detect(),
    // imc_decode_pa(), or anything that could block or fault.  Just
    // append the PA to the static log and return.
    (void)xor_bits;  // retained for API compat; decode uses PA only
    badmem_log_record(addr);
}

// ---------------------------------------------------------------------------
// Decode + display pass — called ONCE from app/main.c at end-of-pass by
// CPU 0, after all workers have synchronised at the pass barrier.  Runs
// single-threaded, no concurrent error reports, no AP races.  Safe to
// call scroll()/display_scrolled_message() here because error_mutex is
// not held and no AP is contending.
//
// Iterates the accumulated bad-PA array, decodes each via imc_decode_pa,
// records (ch, rank, bg, bank, row) for BrrBadRows NVRAM save, records
// (designator) for BrrBadChips NVRAM save, and prints a one-line
// summary of suspect chips per bad page.
// ---------------------------------------------------------------------------

#include "display.h"
#include "board_topology.h"
#include "imc_dispatch.h"

extern int scroll_message_row;
extern void scroll(void);

static uint8_t channel_ranks(const struct mc_config *mc, uint8_t ch_idx)
{
    if (!mc || ch_idx > 1) return 1;
    uint8_t m = 0;
    for (int d = 0; d < 2; d++) {
        const struct mc_dimm *dd = &mc->channel[ch_idx].dimm[d];
        if (!dd->populated) continue;
        if (dd->ranks > m) m = dd->ranks;
    }
    return m ? m : 1;
}

// Process one bad PA: decode, record row/chip, print suspect-chip line.
// Called only from board_decode_pass() below (single-threaded, CPU 0).
static void decode_and_log_one(uint64_t addr)
{
    struct pa_decoded pa = imc_decode_pa(addr);
    if (!pa.valid) return;

    // Record bad row for NVRAM row-mode.
    if (pa.bank_row_valid) {
        badmem_log_record_row(pa.channel, pa.rank,
                              pa.bank_group, pa.bank, pa.row);
    }

    const struct mc_config *mc = imc_config();
    const board_profile_t *p   = board_detect();
    if (!p) return;

    uint8_t ranks = channel_ranks(mc, pa.channel);

    uint8_t rank_lo, rank_hi;
    if (pa.rank_valid) {
        rank_lo = rank_hi = pa.rank;
    } else if (ranks > 1) {
        rank_lo = 0;
        rank_hi = (uint8_t)(ranks - 1);
    } else {
        rank_lo = rank_hi = 0;
    }

    // Accumulate unique chip designators across every lane of the
    // channel — for a 1R A1990 that's the 8 chips sharing the DQ bus.
    const char *seen[BOARD_MAX_PACKAGES] = { 0 };
    unsigned seen_count = 0;

    for (uint8_t r = rank_lo; r <= rank_hi; r++) {
        for (uint8_t lane = 0; lane < 8; lane++) {
            const board_package_t *pk = board_lookup(p, pa.channel, r, lane);
            if (!pk) continue;
            bool dup = false;
            for (unsigned i = 0; i < seen_count; i++) {
                if (seen[i] == pk->designator) { dup = true; break; }
            }
            if (dup) continue;
            if (seen_count < BOARD_MAX_PACKAGES) {
                seen[seen_count++] = pk->designator;
            }
            badmem_log_record_chip(pk->designator);
        }
    }
}

// Called from app/main.c at end of each pass (patch 0005/0009 area),
// single-threaded on CPU 0.  Walks badmem_log entries, decodes each,
// populates BrrBadChips and BrrBadRows accumulators, and prints a
// one-line human-readable summary.  The NVRAM flush in
// badmem_log_flush_nvram + badmem_log_flush_rows_nvram then persists
// both lists.
void board_decode_pass(void)
{
    extern uint64_t *badmem_log_entries(unsigned *out_count);  // from badmem_log.c
    unsigned n = 0;
    uint64_t *pas = badmem_log_entries(&n);

    display_scrolled_message(0, "");
    scroll();
    display_scrolled_message(0, "=== End-of-pass summary ===");
    scroll();

    if (!pas || n == 0) {
        display_scrolled_message(0, "[mem] no bad pages recorded this pass.");
        scroll();
        return;
    }

    display_scrolled_message(0, "[mem] decoded %u bad page(s):", n);
    scroll();

    // Print each bad PA on its own line so the user can see the full list.
    for (unsigned i = 0; i < n; i++) {
        display_scrolled_message(0, "  PA %016x", (uintptr_t)pas[i]);
        scroll();
        decode_and_log_one(pas[i]);
    }

    display_scrolled_message(0, "[mem] see NVRAM BrrBadChips + BrrBadRows for decoded detail.");
    scroll();
}
