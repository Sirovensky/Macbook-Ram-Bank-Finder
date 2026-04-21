// SPDX-License-Identifier: GPL-2.0
//
// Error hook called from memtest86plus app/error.c::common_err().
// Decodes (addr, xor) into BGA designator and prints a message in
// the scroll area.

#include "stdint.h"
#include "stdbool.h"

#include "display.h"

#include "a1990_topology.h"
#include "cfl_decode.h"

extern int scroll_message_row;
extern void scroll(void);

void a1990_report_error(uint64_t addr, uint64_t xor_bits)
{
    const a1990_topology_t *topo = a1990_detect();
    if (!topo) return;

    struct pa_decoded pa = cfl_decode_pa(addr);
    if (!pa.valid) return;

    uint8_t lane_mask = 0;
    for (int bit = 0; bit < 64; bit++) {
        if (xor_bits & (1ULL << bit)) lane_mask |= 1 << (bit / 8);
    }
    if (!lane_mask && xor_bits != 0) {
        // xor bits above 64 (32-bit word) — still count
        lane_mask |= 1;
    }
    if (!lane_mask) return;

    uint8_t ranks = (topo->variant == A1990_VARIANT_32GB) ? 2 : 1;
    const char *qual = (ranks > 1) ? "?" : "";

    // First line: channel + where
    display_scrolled_message(0, "[A1990] ch%u addr=%016llx xor=%016llx",
        pa.channel, (unsigned long long)addr, (unsigned long long)xor_bits);
    scroll();

    // Second line: package names
    display_scrolled_message(0, "[A1990] suspect chips:");
    int col_off = 22;
    for (uint8_t lane = 0; lane < 8; lane++) {
        if (!(lane_mask & (1 << lane))) continue;
        for (uint8_t r = 0; r < ranks; r++) {
            const a1990_package_t *pkg =
                a1990_lookup(topo, pa.channel, r, lane);
            if (!pkg) continue;
            display_scrolled_message(col_off, "%s%s", pkg->designator, qual);
            col_off += 8;
        }
    }
    scroll();
}
