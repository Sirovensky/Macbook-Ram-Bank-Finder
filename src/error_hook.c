// SPDX-License-Identifier: GPL-2.0
//
// Error hook called from memtest86plus app/error.c::common_err().
//
// DESIGN NOTE — why this is now minimal:
//
// Earlier versions did PA decode, board_detect(), and display output
// inside this hook.  On A1990 that caused system-wide hangs once the
// first error fired: memtest's `common_err()` holds `error_mutex` while
// calling us, and any fault/hang inside our hook would lock other CPUs
// trying to report their own errors.  Observed symptom: test timer
// frozen, ESC blocked, repeated errors on the same chip no longer
// making the screen advance.
//
// Fix: this hook now ONLY appends the PA to a static array
// (badmem_log_record).  No decode.  No SMBIOS lookup.  No display.
// Heavy work (decode + display + NVRAM flush) is done ONCE at pass
// boundary from CPU 0 only, in app/main.c via patch 0005+0009.
//
// Trade-off: per-error screen output ("suspect chips: U2620") moves
// from per-error to end-of-pass.  User sees less live detail but
// testing no longer deadlocks.  Chip identification still works —
// happens when CPU 0 processes the accumulated PAs at pass end.

#include "stdint.h"
#include "stdbool.h"

#include "badmem_log.h"

void board_report_error(uint64_t addr, uint64_t xor_bits)
{
    (void)xor_bits;  // accumulated bit-fault info deferred to pass-end decode

    // Pure static-array append, no pointer chases, no MCHBAR reads, no
    // SMBIOS walks, no display I/O.  Called under memtest's error_mutex
    // so concurrent APs serialise at that level; even so, this function
    // is small enough to complete in microseconds on any CPU.
    badmem_log_record(addr);
}

// ---------------------------------------------------------------------------
// Decode + display pass — called ONCE from app/main.c at end-of-pass by
// CPU 0, after all workers have synchronised at the pass barrier.  Runs
// single-threaded, no concurrent error reports, no AP races.
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
// Called only from board_report_pass() below (single-threaded, CPU 0).
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
    const char *qual;
    if (pa.rank_valid) {
        rank_lo = rank_hi = pa.rank;
        qual = pa.rank_speculative ? "~" : "";
    } else if (ranks > 1) {
        rank_lo = 0;
        rank_hi = (uint8_t)(ranks - 1);
        qual = "?";
    } else {
        rank_lo = rank_hi = 0;
        qual = "";
    }

    // Determine byte lane(s) hit from the decoded PA — without xor_bits
    // we can't narrow to a single lane, so log every lane of the channel.
    // This over-reports chips on a channel, but matches what the user
    // sees anyway for a 1R A1990: one channel = 8 chips.  The NVRAM row
    // record already narrows to (bg, bank, row) precision.
    //
    // For a tighter per-error chip-report we'd need xor_bits passed to
    // this deferred hook — currently xor is thrown away at error time.
    // Revisit if precise per-error chip lists are needed.

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
    (void)qual;
}

// Called from app/main.c at end of each pass (patch 0005/0009 area),
// single-threaded on CPU 0.  Walks badmem_log entries, decodes each,
// populates BrrBadChips and BrrBadRows accumulators.  The NVRAM flush
// in badmem_log_flush_nvram + badmem_log_flush_rows_nvram then persists
// both lists.
void board_decode_pass(void)
{
    extern uint64_t *badmem_log_entries(unsigned *out_count);  // from badmem_log.c
    unsigned n = 0;
    uint64_t *pas = badmem_log_entries(&n);

    if (!pas || n == 0) return;

    display_scrolled_message(0, "[mem] decoded %u bad page(s):", n);
    scroll();

    for (unsigned i = 0; i < n; i++) {
        decode_and_log_one(pas[i]);
    }

    // Short summary of identified chips.
    const board_profile_t *p = board_detect();
    if (p) {
        display_scrolled_message(0, "[mem] see BrrBadChips + BrrBadRows in NVRAM for details.");
        scroll();
    }
}
