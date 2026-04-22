// SPDX-License-Identifier: GPL-2.0
//
// board_prune_vm_map() — remove recorded skip regions from memtest's
// per-window vm_map after setup_vm_map() has populated it.
//
// Called from app/main.c at the end of setup_vm_map() so that tests
// see a vm_map that excludes the failing 1 MiB regions flagged by
// the burst detector in common_err().  Entries that overlap a skip
// range are split into one or two non-overlapping remnants; entries
// fully inside a skip range are dropped.
//
// The skip list itself lives in badmem_log.c and is accumulated
// during the run whenever a 1 MiB page produces an error burst.
// board_prune_vm_map() is idempotent: calling it on an already-pruned
// map is a no-op (overlaps with the new skip set, not the old one).

#include "stdint.h"
#include "stdbool.h"

#include "pmem.h"   // for MAX_MEM_SEGMENTS
#include "test.h"   // for vm_map_t + vm_map[] + vm_map_size
#include "badmem_log.h"

// Scratch buffer for the rebuilt map — static to avoid EFI-time stack
// pressure.  MAX_MEM_SEGMENTS is small enough that 2× capacity covers
// the worst case (every entry split in two by skip ranges).
static vm_map_t scratch[MAX_MEM_SEGMENTS * 2];

// Display hook (optional — prints a one-line notice each time we prune).
#include "display.h"
extern int scroll_message_row;
extern void scroll(void);

void board_prune_vm_map(void)
{
    unsigned nskip = 0;
    const struct badmem_skip_range *skips = badmem_log_skip_list(&nskip);
    if (nskip == 0) return;

    unsigned out = 0;
    bool any_pruned = false;

    typedef struct { uintptr_t s; uintptr_t e; } remnant_t;

    for (int i = 0; i < vm_map_size; i++) {
        uintptr_t seg_s = (uintptr_t)vm_map[i].start;
        uintptr_t seg_e = (uintptr_t)vm_map[i].end;

        // Apply each skip range in turn.  Each skip may split the
        // current remnant into up to two non-overlapping pieces; we
        // keep them on a small local work list and iterate.
        remnant_t work[8];
        unsigned nwork = 0;
        work[nwork].s = seg_s; work[nwork].e = seg_e; nwork++;

        for (unsigned k = 0; k < nskip && nwork > 0; k++) {
            uintptr_t sk_s = (uintptr_t)skips[k].start;
            uintptr_t sk_e = (uintptr_t)skips[k].end;

            unsigned new_nwork = 0;
            remnant_t new_work[8];

            for (unsigned w = 0; w < nwork; w++) {
                uintptr_t a = work[w].s;
                uintptr_t b = work[w].e;
                if (sk_e <= a || sk_s >= b) {
                    // No overlap — keep as-is.
                    new_work[new_nwork].s = a;
                    new_work[new_nwork].e = b;
                    new_nwork++;
                    continue;
                }
                any_pruned = true;
                // Left piece.
                if (sk_s > a && new_nwork < 8) {
                    new_work[new_nwork].s = a;
                    new_work[new_nwork].e = sk_s;
                    new_nwork++;
                }
                // Right piece.
                if (sk_e < b && new_nwork < 8) {
                    new_work[new_nwork].s = sk_e;
                    new_work[new_nwork].e = b;
                    new_nwork++;
                }
                // Else: remnant fully swallowed by skip range — drop.
            }
            // Copy new_work back to work.
            for (unsigned w = 0; w < new_nwork; w++) work[w] = new_work[w];
            nwork = new_nwork;
        }

        // Emit surviving remnants into the scratch map.
        for (unsigned w = 0; w < nwork && out < sizeof(scratch)/sizeof(scratch[0]); w++) {
            scratch[out] = vm_map[i];  // copy base fields (pm_base_addr, proximity)
            scratch[out].start = (testword_t *)work[w].s;
            scratch[out].end   = (testword_t *)work[w].e;
            out++;
        }
    }

    // Capture pre/post sizes for the progress-bar compensation hook.
    int pre_size = vm_map_size;

    if (any_pruned) {
        for (unsigned i = 0; i < out && (int)i < MAX_MEM_SEGMENTS; i++) {
            vm_map[i] = scratch[i];
        }
        vm_map_size = (int)(out > MAX_MEM_SEGMENTS ? MAX_MEM_SEGMENTS : out);

        // Only announce when the skip-list size has grown since the last
        // prune — a new window may re-trigger the split logic against the
        // same list and we don't want to spam the scroll area.  memtest's
        // printf only knows %i %u %x (no %d), so use %i for signed ints.
        static unsigned last_announced_nskip = 0;
        if (nskip != last_announced_nskip) {
            last_announced_nskip = nskip;
            display_scrolled_message(0,
                "[skip] pruned vm_map to %i entries (%u MiB skipped total)",
                vm_map_size, (uintptr_t)nskip);
            scroll();
        }
    }
    (void)pre_size;
}

// Accessor for the progress-bar compensation hook in app/display.c.
//
// The test% bar was calibrated against an UNPRUNED vm_map during the
// dummy run.  After pruning splits vm_map entries, each test's outer
// loop `for (i=0; i<vm_map_size; i++)` iterates more times than the
// dummy predicted — test_ticks overshoots ticks_per_test, bar locks
// at 100% mid-test.
//
// We expose the current vm_map_size via this function and the
// patched do_tick() in app/display.c applies the ratio
// (ticks_per_test * current_size / baseline_size) in the %
// calculation so the bar tracks real progress.
//
// Returns current vm_map_size.  Baseline (pre-prune) size is not
// tracked — callers use live vm_map_size; the test% patch below
// compensates in proportion to how many skip ranges are active.
int board_current_vm_map_size(void)
{
    return vm_map_size;
}

// Total number of skip entries currently active.  Used by display.c's
// do_tick() to estimate how much the vm_map grew relative to dummy.
// Each skip entry splits at most one vm_map entry in two, so the
// approximate growth factor is (1 + nskip / baseline).  For our
// typical case (baseline ~1-2 entries), growth ~= 1 + nskip.
unsigned board_skip_count_public(void)
{
    extern unsigned badmem_log_skip_count(void);
    return badmem_log_skip_count();
}
