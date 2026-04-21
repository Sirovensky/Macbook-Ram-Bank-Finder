// SPDX-License-Identifier: GPL-2.0
//
// Error hook called from memtest86plus app/error.c::common_err().
// Decodes (addr, xor) using generic IMC decode + optional board overlay.
//
// Output tiers:
//   T1 (always): channel, byte lane(s), chip width  (generic, works everywhere)
//   T2 (overlay): BGA designator + location hint    (requires YAML overlay)

#include "stdint.h"
#include "stdbool.h"

#include "display.h"

#include "board_topology.h"
#include "cfl_decode.h"

extern int scroll_message_row;
extern void scroll(void);

void board_report_error(uint64_t addr, uint64_t xor_bits)
{
    struct pa_decoded pa = cfl_decode_pa(addr);
    if (!pa.valid) return;

    uint8_t lane_mask = 0;
    for (int bit = 0; bit < 64; bit++) {
        if (xor_bits & (1ULL << bit)) lane_mask |= 1 << (bit / 8);
    }
    if (!lane_mask && xor_bits != 0) lane_mask = 0x01;
    if (!lane_mask) return;

    const board_profile_t *p = board_detect();
    uint8_t ranks = p ? p->ranks_per_channel : 1;
    const char *qual = (ranks > 1) ? "?" : "";

    display_scrolled_message(0, "[mem] ch%u addr=%016llx xor=%016llx",
        pa.channel, (unsigned long long)addr, (unsigned long long)xor_bits);
    scroll();

    // T1 line: byte lanes always
    display_scrolled_message(0, "[mem] lanes:");
    int col_off = 13;
    for (uint8_t lane = 0; lane < 8; lane++) {
        if (!(lane_mask & (1 << lane))) continue;
        display_scrolled_message(col_off, "D%u", lane);
        col_off += 4;
    }
    scroll();

    // T2 line: overlay designators (if board matched)
    if (!p) return;
    display_scrolled_message(0, "[mem] suspect chips:");
    col_off = 22;
    for (uint8_t lane = 0; lane < 8; lane++) {
        if (!(lane_mask & (1 << lane))) continue;
        for (uint8_t r = 0; r < ranks; r++) {
            const board_package_t *pk = board_lookup(p, pa.channel, r, lane);
            if (!pk) continue;
            display_scrolled_message(col_off, "%s%s", pk->designator, qual);
            col_off += 8;
        }
    }
    scroll();
}
