// SPDX-License-Identifier: GPL-2.0
//
// badmem.txt parser — standalone, no libc, no EFI dependency.
// Used by mask-shim (EFI build) and test_parse (hosted build).

#ifndef BADMEM_PARSE_H
#define BADMEM_PARSE_H

#include <stdint.h>
#include <stddef.h>

#define BADMEM_MAX_RANGES   4096
#define BADMEM_MAX_CHIPS    32       // 16-chip boards × 2 channels max
#define BADMEM_DESIG_MAX    16       // max designator string length incl NUL

// Region entry (current, unchanged behavior): a page-aligned PA range.
typedef struct {
    uint64_t start;  // page-aligned physical address
    uint64_t len;    // length in bytes, multiple of 4096
} badmem_range_t;

// Chip entry (new): a bad chip identified by its U-designator.
// The designator is populated at parse time.  channel/rank/byte_lane are
// resolved later by the shim from board_profiles[].
// resolved == 0 until the shim resolves the entry; 1 after successful lookup.
typedef struct {
    char    designator[BADMEM_DESIG_MAX]; // e.g. "U2410"
    uint8_t channel;    // filled by shim after resolution
    uint8_t rank;       // filled by shim after resolution
    uint8_t byte_lane;  // filled by shim after resolution
    uint8_t resolved;   // 0=unresolved, 1=resolved
} badmem_chip_t;

// Full parse result — both region and chip entries.
typedef struct {
    badmem_range_t *ranges;
    unsigned        n_ranges;
    badmem_chip_t  *chips;
    unsigned        n_chips;
} badmem_result_t;

// Parse badmem.txt content from a buffer, returning both region and chip
// entries.  Caller supplies pre-allocated arrays.
// buf:       pointer to file contents (need not be NUL-terminated).
// file_len:  byte count of valid data in buf.
// ranges:    caller array of at least range_max entries.
// range_max: capacity of ranges[].
// chips:     caller array of at least chip_max entries.
// chip_max:  capacity of chips[].
// out:       filled with counts and pointers on return.
// Lines starting with "# chip:" (any whitespace between # and chip:) are chip
// directives.  Any other '#'-prefixed line is a plain comment.
// Malformed and unrecognised lines are silently skipped.
void badmem_parse_full(const char *buf, uint64_t file_len,
                       badmem_range_t *ranges, unsigned range_max,
                       badmem_chip_t  *chips,  unsigned chip_max,
                       badmem_result_t *out);

// Backwards-compatible wrapper: parses region entries only (chip directives
// are silently counted but not returned).
// Returns the number of region entries written to out.
unsigned badmem_parse(const char *buf, uint64_t len,
                      badmem_range_t *out, unsigned out_max);

#endif /* BADMEM_PARSE_H */
