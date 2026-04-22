// SPDX-License-Identifier: GPL-2.0
//
// badmem_log — accumulate bad physical addresses during a memtest run and
// dump them as a pasteable badmem.txt block at end of run.

#ifndef BADMEM_LOG_H
#define BADMEM_LOG_H

#include <stdint.h>

// Record one bad physical address.  Internally the address is rounded down
// to a 4096-byte page boundary and stored as a page-sized range.
// Duplicates within the same page are silently dropped.
// Safe to call from board_report_error() (no dynamic allocation — uses a
// fixed static buffer).
void badmem_log_record(uint64_t phys_addr);

// Print all accumulated ranges to the scroll region in badmem.txt format.
// Call at end of test run (e.g. from board_calibrate or a post-test hook).
// Idempotent — safe to call more than once.
void badmem_log_dump(void);

// Write all accumulated bad page addresses to NVRAM as the binary variable
// "BrrBadPages" under the BRR vendor GUID.
// Format: uint32_t version=1, uint32_t count, then count * uint64_t PAs.
// Capped at 4096 entries (32 KiB blob) to stay within NVRAM limits.
// No-op if UEFI Runtime Services are unavailable (BIOS boot, or RT not found).
// Logs "[nvram] saved N pages" or a failure note to the scroll area.
// Safe to call repeatedly; each call overwrites the previous value.
// After flushing pages, sets BrrMaskState to TRIAL_PENDING_PAGE or
// TRIAL_PENDING_CHIP depending on the BRR_FLAG_AUTO_TRIAL_CHIP bit in
// boot_params->brr_flags.
void badmem_log_flush_nvram(void);

// Record a chip designator (e.g. "U2620") identified during a test run.
// Internally maintained as a NUL-separated list (max 256 bytes total,
// including all NUL separators).  Duplicate designators are silently dropped.
// Called from error_hook.c::board_report_error() when a chip is resolved.
// The accumulated list is written to NVRAM as BrrBadChips during
// badmem_log_flush_nvram() when chip-mode is active.
void badmem_log_record_chip(const char *designator);

// Record a bad (channel, rank, bank_group, bank, row) tuple.
// Deduplicates within a run.  Capped at 256 rows.
// Called from error_hook.c when pa.bank_row_valid is true.
void badmem_log_record_row(uint8_t channel, uint8_t rank,
                            uint8_t bg, uint8_t bank, uint32_t row);

// Flush accumulated bad rows to NVRAM as "BrrBadRows".
// Call from the end-of-pass hook after badmem_log_flush_nvram().
// Binary format: [uint32_t version=1][uint32_t count]
//   followed by count tuples of:
//     uint8_t ch + uint8_t rank + uint8_t bg + uint8_t bank + uint32_t row
//   = 8 bytes per tuple.  Cap 256 rows = 2 KiB blob.
// No-op if UEFI Runtime Services are unavailable.
void badmem_log_flush_rows_nvram(void);

#endif /* BADMEM_LOG_H */
