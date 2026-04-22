// SPDX-License-Identifier: GPL-2.0
//
// Error hook called from memtest86plus app/error.c::common_err().
// Decodes (addr, xor) into concrete BGA chip designators.
//
// Output tiers:
//   T1 (always): channel, byte lane(s), chip width  (generic, works everywhere)
//   T2 (overlay): BGA designator + location hint    (requires YAML overlay)
//
// Goal: report ONE chip per failing byte lane when possible. Ranks used:
//   - IMC-detected per-channel rank count (not static profile — profile
//     is for max-config and misreports on reduced-memory SKUs).
//   - decoded rank when pa.rank_valid; otherwise both ranks listed with
//     a "?" qualifier so user knows it is a guess.

#include "stdint.h"
#include "stdbool.h"

#include "display.h"

#include "board_topology.h"
#include "imc_dispatch.h"
#include "badmem_log.h"

extern int scroll_message_row;
extern void scroll(void);

// memtest86plus's printf supports only %c %s %i %u %x %k. No %l/%ll length
// modifiers. uintptr_t is 64-bit on x86_64 so plain %x handles 64-bit vals.

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

void board_report_error(uint64_t addr, uint64_t xor_bits)
{
    // Accumulate for badmem.txt dump at end of run (always, regardless of
    // whether the full topology decode succeeds).
    badmem_log_record(addr);

    struct pa_decoded pa = imc_decode_pa(addr);
    if (!pa.valid) return;

    // Track C: record bad row tuple for row-level NVRAM masking.
    if (pa.bank_row_valid) {
        badmem_log_record_row(pa.channel, pa.rank,
                              pa.bank_group, pa.bank, pa.row);
    }

    uint8_t lane_mask = 0;
    for (int bit = 0; bit < 64; bit++) {
        if (xor_bits & (1ULL << bit)) lane_mask |= 1 << (bit / 8);
    }
    if (!lane_mask) return;

    const struct mc_config *mc = imc_config();
    const board_profile_t *p   = board_detect();
    uint8_t ranks = channel_ranks(mc, pa.channel);

    // Header line: address + xor + decoded ch/rank.
    if (pa.rank_valid) {
        const char *mark = pa.rank_speculative ? "~" : "";
        display_scrolled_message(0,
            "[mem] ch%u rk%u%s addr=%016x xor=%016x",
            pa.channel, pa.rank, mark,
            (uintptr_t)addr, (uintptr_t)xor_bits);
    } else {
        display_scrolled_message(0,
            "[mem] ch%u rk=? addr=%016x xor=%016x",
            pa.channel, (uintptr_t)addr, (uintptr_t)xor_bits);
    }
    scroll();

    // T1 line: byte lanes (always).
    display_scrolled_message(0, "[mem] lanes:");
    int col_off = 13;
    for (uint8_t lane = 0; lane < 8; lane++) {
        if (!(lane_mask & (1 << lane))) continue;
        display_scrolled_message(col_off, "D%u", lane);
        col_off += 4;
    }
    scroll();

    // T2 line: overlay designators (if board matched).
    if (!p) return;

    // Decide which ranks to emit:
    //   - rank decoded (or 1R channel): exactly 1 rank.
    //   - rank ambiguous and 2R channel: list both with "?" qualifier.
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

    display_scrolled_message(0, "[mem] suspect chips:");
    col_off = 22;

    // Dedupe: an x16 chip covers a lane pair, so both lanes map to the
    // same designator. Emit each (rank, designator) only once.
    const char *seen[BOARD_MAX_PACKAGES] = { 0 };
    unsigned seen_count = 0;

    for (uint8_t r = rank_lo; r <= rank_hi; r++) {
        for (uint8_t lane = 0; lane < 8; lane++) {
            if (!(lane_mask & (1 << lane))) continue;
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

            // Side-channel: record chip designator for chip-mode NVRAM save.
            // badmem_log_record_chip() deduplicates across the full run so
            // repeated errors on the same chip don't inflate the list.
            badmem_log_record_chip(pk->designator);

            display_scrolled_message(col_off, "%s%s", pk->designator, qual);
            col_off += 8;
        }
    }
    scroll();

    // Confidence legend (only on first speculative/ambiguous report each
    // test pass would be ideal, but common_err() has no pass state. Keep
    // it one line per error for clarity.)
    if (qual[0] == '?') {
        display_scrolled_message(0,
            "[mem] '?' = rank unknown; both rank candidates listed.");
        scroll();
    } else if (qual[0] == '~') {
        display_scrolled_message(0,
            "[mem] '~' = rank from speculative MAD_INTRA decode.");
        scroll();
    }
}
