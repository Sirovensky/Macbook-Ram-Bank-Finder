// SPDX-License-Identifier: GPL-2.0
//
// badmem.txt parser — standalone, no libc, no EFI dependency.
// See badmem_parse.h for format description.

#include "badmem_parse.h"

#define PAGE_SIZE   4096ULL
#define PAGE_MASK   (~(PAGE_SIZE - 1))

// ASCII tolower for a single character (letters only).
static char ascii_lower(char c)
{
    if (c >= 'A' && c <= 'Z') return (char)(c + ('a' - 'A'));
    return c;
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Skip whitespace (not newlines).
static const char *skip_sp(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    return p;
}

// Parse a hex number prefixed with "0x"/"0X" or decimal.
// Advances *pp past the number.  Returns 1 on success, 0 on failure.
static int parse_u64(const char **pp, const char *end, uint64_t *out)
{
    const char *p = *pp;
    if (p >= end) return 0;

    uint64_t val = 0;
    int ok = 0;

    if (p + 1 < end && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        while (p < end) {
            char c = *p;
            uint8_t d;
            if (c >= '0' && c <= '9')      d = (uint8_t)(c - '0');
            else if (c >= 'a' && c <= 'f') d = (uint8_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = (uint8_t)(c - 'A' + 10);
            else break;
            val = (val << 4) | d;
            ok = 1;
            p++;
        }
    } else if (*p >= '0' && *p <= '9') {
        while (p < end && *p >= '0' && *p <= '9') {
            val = val * 10 + (uint8_t)(*p - '0');
            ok = 1;
            p++;
        }
    }

    if (!ok) return 0;
    *out = val;
    *pp  = p;
    return 1;
}

// Advance past the current line (up to and including '\n' or end of buffer).
static const char *skip_line(const char *p, const char *end)
{
    while (p < end && *p != '\n') p++;
    if (p < end) p++; // consume '\n'
    return p;
}

// ---------------------------------------------------------------------------
// Chip-directive helper.
// Matches "chip:" (case-insensitive) at position p, returns pointer past
// "chip:" on match, or NULL on mismatch.
// ---------------------------------------------------------------------------

static const char *match_chip_keyword(const char *p, const char *end)
{
    // "chip:" is 5 characters.
    static const char kw[] = "chip:";
    for (int i = 0; i < 5; i++) {
        if (p + i >= end) return NULL;
        if (ascii_lower(p[i]) != kw[i]) return NULL;
    }
    return p + 5;
}

// Parse a chip designator (e.g. "U2410") into dest[].
// Reads alphanumeric + '_' + '-' characters; stops at whitespace or end.
// Returns 1 if at least one character was read and fits in dest, else 0.
static int parse_designator(const char **pp, const char *end,
                             char *dest, unsigned dest_max)
{
    const char *p = *pp;
    unsigned n = 0;
    while (p < end) {
        char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-') {
            if (n + 1 >= dest_max) return 0; // overflow
            dest[n++] = c;
            p++;
        } else {
            break;
        }
    }
    if (n == 0) return 0;
    dest[n] = '\0';
    *pp = p;
    return 1;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void badmem_parse_full(const char *buf, uint64_t file_len,
                       badmem_range_t *ranges, unsigned range_max,
                       badmem_chip_t  *chips,  unsigned chip_max,
                       badmem_result_t *out)
{
    out->ranges   = ranges;
    out->n_ranges = 0;
    out->chips    = chips;
    out->n_chips  = 0;

    const char *p   = buf;
    const char *end = buf + file_len;

    while (p < end) {
        // Skip leading whitespace on line.
        p = skip_sp(p, end);
        if (p >= end) break;

        // Blank line.
        if (*p == '\r' || *p == '\n') { p = skip_line(p, end); continue; }

        // Comment or chip directive.
        if (*p == '#') {
            p++; // consume '#'
            p = skip_sp(p, end);

            // Try "chip:" keyword.
            const char *after_kw = match_chip_keyword(p, end);
            if (after_kw && out->n_chips < chip_max) {
                p = after_kw;
                p = skip_sp(p, end);
                badmem_chip_t *ce = &chips[out->n_chips];
                if (parse_designator(&p, end, ce->designator, BADMEM_DESIG_MAX)) {
                    ce->channel  = 0;
                    ce->rank     = 0;
                    ce->byte_lane = 0;
                    ce->resolved = 0;
                    out->n_chips++;
                }
            }
            p = skip_line(p, end);
            continue;
        }

        // Skip non-range lines if range array full.
        if (out->n_ranges >= range_max) { p = skip_line(p, end); continue; }

        // Expect: 0xADDR,LEN
        uint64_t addr = 0, len = 0;
        if (!parse_u64(&p, end, &addr)) { p = skip_line(p, end); continue; }

        p = skip_sp(p, end);
        if (p >= end || *p != ',')    { p = skip_line(p, end); continue; }
        p++; // consume ','

        p = skip_sp(p, end);
        if (!parse_u64(&p, end, &len)) { p = skip_line(p, end); continue; }

        // Must reach end-of-line (optional trailing comment).
        p = skip_sp(p, end);
        if (p < end && *p != '\r' && *p != '\n' && *p != '#') {
            p = skip_line(p, end);
            continue;
        }
        p = skip_line(p, end);

        // Align: addr down, len up.
        uint64_t aligned_start = addr & PAGE_MASK;
        uint64_t offset        = addr - aligned_start;
        uint64_t aligned_len   = (len + offset + PAGE_SIZE - 1) & PAGE_MASK;
        if (aligned_len == 0) aligned_len = PAGE_SIZE;

        ranges[out->n_ranges].start = aligned_start;
        ranges[out->n_ranges].len   = aligned_len;
        out->n_ranges++;
    }
}

unsigned badmem_parse(const char *buf, uint64_t file_len,
                      badmem_range_t *out, unsigned out_max)
{
    // Chip entries are parsed but discarded (backwards compatibility).
    static badmem_chip_t chip_scratch[BADMEM_MAX_CHIPS];
    badmem_result_t result;
    badmem_parse_full(buf, file_len, out, out_max,
                      chip_scratch, BADMEM_MAX_CHIPS, &result);
    return result.n_ranges;
}
