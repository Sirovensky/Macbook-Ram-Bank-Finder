// SPDX-License-Identifier: GPL-2.0
//
// Hosted unit test for the badmem.txt parser.
// Compiled with a normal host C compiler (no EFI environment needed).
// Run via `make test-shim` from the repo root.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

// Pull in the parser under test.
#include "../badmem_parse.h"
#include "../badmem_parse.c"  // single-translation-unit include for simplicity

// ---------------------------------------------------------------------------
// Test framework (minimal)
// ---------------------------------------------------------------------------

static int g_pass;
static int g_fail;

#define ASSERT_EQ(a, b) do {                                            \
    if ((uint64_t)(a) == (uint64_t)(b)) {                              \
        g_pass++;                                                       \
    } else {                                                            \
        fprintf(stderr, "FAIL  %s:%d  expected 0x%llx got 0x%llx\n",  \
                __FILE__, __LINE__,                                     \
                (unsigned long long)(b), (unsigned long long)(a));      \
        g_fail++;                                                       \
    }                                                                   \
} while (0)

#define ASSERT_STR(label) do {                                          \
    printf("  %-40s ... ", label);                                      \
} while (0)

#define PASS()   do { printf("ok\n"); } while (0)
#define FAIL(m)  do { printf("FAIL: %s\n", m); g_fail++; } while (0)

// ---------------------------------------------------------------------------
// Individual test cases
// ---------------------------------------------------------------------------

static void test_empty(void)
{
    printf("test: empty input\n");
    badmem_range_t out[16];
    unsigned n = badmem_parse("", 0, out, 16);
    ASSERT_EQ(n, 0);
}

static void test_comment_only(void)
{
    printf("test: comment-only\n");
    static const char buf[] = "# this is a comment\n# another\n";
    badmem_range_t out[16];
    unsigned n = badmem_parse(buf, sizeof(buf) - 1, out, 16);
    ASSERT_EQ(n, 0);
}

static void test_single_hex(void)
{
    printf("test: single hex range\n");
    static const char buf[] = "0x200000,4096\n";
    badmem_range_t out[16];
    unsigned n = badmem_parse(buf, sizeof(buf) - 1, out, 16);
    ASSERT_EQ(n, 1);
    if (n >= 1) {
        ASSERT_EQ(out[0].start, 0x200000ULL);
        ASSERT_EQ(out[0].len,   4096ULL);
    }
}

static void test_multiple_ranges(void)
{
    printf("test: multiple ranges with comments and blanks\n");
    static const char buf[] =
        "# start\n"
        "\n"
        "0x1000,4096\n"
        "0x2000,8192\n"
        "# trailing comment\n"
        "0x3000,4096\n";
    badmem_range_t out[16];
    unsigned n = badmem_parse(buf, sizeof(buf) - 1, out, 16);
    ASSERT_EQ(n, 3);
    if (n >= 3) {
        ASSERT_EQ(out[0].start, 0x1000ULL);
        ASSERT_EQ(out[0].len,   4096ULL);
        ASSERT_EQ(out[1].start, 0x2000ULL);
        ASSERT_EQ(out[1].len,   8192ULL);
        ASSERT_EQ(out[2].start, 0x3000ULL);
        ASSERT_EQ(out[2].len,   4096ULL);
    }
}

static void test_alignment(void)
{
    printf("test: address/length alignment\n");
    // addr = 0x500 (not page-aligned), len = 1 (rounds up to page)
    // Expected: start = 0x000, len = 4096 (covers 0x000..0xfff)
    static const char buf[] = "0x500,1\n";
    badmem_range_t out[16];
    unsigned n = badmem_parse(buf, sizeof(buf) - 1, out, 16);
    ASSERT_EQ(n, 1);
    if (n >= 1) {
        ASSERT_EQ(out[0].start, 0x000ULL);   // 0x500 rounded down to 0x000
        ASSERT_EQ(out[0].len,   4096ULL);     // 1 + offset(0x500) rounded up
    }
}

static void test_alignment_cross_page(void)
{
    printf("test: length crossing page boundary\n");
    // addr = 0x1800, len = 0x1000 — spans two pages (0x1000..0x2fff)
    static const char buf[] = "0x1800,0x1000\n";
    badmem_range_t out[16];
    unsigned n = badmem_parse(buf, sizeof(buf) - 1, out, 16);
    ASSERT_EQ(n, 1);
    if (n >= 1) {
        ASSERT_EQ(out[0].start, 0x1000ULL);
        ASSERT_EQ(out[0].len,   0x2000ULL); // 2 pages
    }
}

static void test_malformed_lines(void)
{
    printf("test: malformed lines skipped\n");
    static const char buf[] =
        "not-a-number\n"
        "0x1000\n"          // missing ,len
        "0x1000,\n"         // missing len value
        "0x2000,4096\n"     // valid
        "garbage here\n";
    badmem_range_t out[16];
    unsigned n = badmem_parse(buf, sizeof(buf) - 1, out, 16);
    ASSERT_EQ(n, 1);
    if (n >= 1) {
        ASSERT_EQ(out[0].start, 0x2000ULL);
        ASSERT_EQ(out[0].len,   4096ULL);
    }
}

static void test_inline_comment(void)
{
    printf("test: inline comment after value\n");
    static const char buf[] = "0x4000,4096  # bad page\n";
    badmem_range_t out[16];
    unsigned n = badmem_parse(buf, sizeof(buf) - 1, out, 16);
    ASSERT_EQ(n, 1);
    if (n >= 1) {
        ASSERT_EQ(out[0].start, 0x4000ULL);
        ASSERT_EQ(out[0].len,   4096ULL);
    }
}

static void test_decimal_len(void)
{
    printf("test: decimal length\n");
    static const char buf[] = "0x100000,8192\n";
    badmem_range_t out[16];
    unsigned n = badmem_parse(buf, sizeof(buf) - 1, out, 16);
    ASSERT_EQ(n, 1);
    if (n >= 1) {
        ASSERT_EQ(out[0].start, 0x100000ULL);
        ASSERT_EQ(out[0].len,   8192ULL);
    }
}

static void test_cap_at_max(void)
{
    printf("test: output capped at out_max\n");
    // Generate 10 valid lines.
    char buf[256];
    int pos = 0;
    for (int i = 0; i < 10; i++) {
        pos += sprintf(buf + pos, "0x%x000,4096\n", i + 1);
    }
    badmem_range_t out[5];
    unsigned n = badmem_parse(buf, (uint64_t)pos, out, 5);
    ASSERT_EQ(n, 5); // capped
}

static void test_no_trailing_newline(void)
{
    printf("test: no trailing newline\n");
    static const char buf[] = "0x8000,4096";
    badmem_range_t out[4];
    unsigned n = badmem_parse(buf, sizeof(buf) - 1, out, 4);
    ASSERT_EQ(n, 1);
    if (n >= 1) ASSERT_EQ(out[0].start, 0x8000ULL);
}

// ---------------------------------------------------------------------------
// Chip-mode test cases
// ---------------------------------------------------------------------------

static void test_chip_basic(void)
{
    printf("test: chip directive basic\n");
    static const char buf[] =
        "# chip: U2410\n"
        "# chip: U2300\n";
    badmem_range_t ranges[16];
    badmem_chip_t  chips[16];
    badmem_result_t r;
    badmem_parse_full(buf, sizeof(buf) - 1, ranges, 16, chips, 16, &r);
    ASSERT_EQ(r.n_ranges, 0);
    ASSERT_EQ(r.n_chips, 2);
    if (r.n_chips >= 2) {
        ASSERT_STR("designator[0] = U2410");
        if (strcmp(chips[0].designator, "U2410") == 0) PASS();
        else FAIL("got wrong designator");

        ASSERT_STR("designator[1] = U2300");
        if (strcmp(chips[1].designator, "U2300") == 0) PASS();
        else FAIL("got wrong designator");

        ASSERT_EQ(chips[0].resolved, 0); // not resolved by parser
    }
}

static void test_chip_mixed(void)
{
    printf("test: chip directives mixed with region entries\n");
    static const char buf[] =
        "# chip: U2410\n"
        "0x200000,4096\n"
        "# chip: U2300\n"
        "0x201000,4096\n"
        "# plain comment — not a chip\n";
    badmem_range_t ranges[16];
    badmem_chip_t  chips[16];
    badmem_result_t r;
    badmem_parse_full(buf, sizeof(buf) - 1, ranges, 16, chips, 16, &r);
    ASSERT_EQ(r.n_ranges, 2);
    ASSERT_EQ(r.n_chips, 2);
    if (r.n_ranges >= 2) {
        ASSERT_EQ(ranges[0].start, 0x200000ULL);
        ASSERT_EQ(ranges[1].start, 0x201000ULL);
    }
    if (r.n_chips >= 2) {
        ASSERT_STR("chip[0] = U2410");
        if (strcmp(chips[0].designator, "U2410") == 0) PASS();
        else FAIL("wrong designator");
        ASSERT_STR("chip[1] = U2300");
        if (strcmp(chips[1].designator, "U2300") == 0) PASS();
        else FAIL("wrong designator");
    }
}

static void test_chip_plain_comment_not_chip(void)
{
    printf("test: lines with '#' but not '# chip:' are plain comments\n");
    static const char buf[] =
        "# chipset comment\n"
        "# This is about chips but not a directive\n"
        "# chip:U2410\n"    // no space after colon: still valid
        "# chip:\n"          // chip: with no designator: ignored
        "# chip: \n";        // chip: with whitespace only: ignored
    badmem_range_t ranges[16];
    badmem_chip_t  chips[16];
    badmem_result_t r;
    badmem_parse_full(buf, sizeof(buf) - 1, ranges, 16, chips, 16, &r);
    ASSERT_EQ(r.n_ranges, 0);
    // Only "# chip:U2410" should produce a chip entry (no space needed)
    ASSERT_EQ(r.n_chips, 1);
    if (r.n_chips >= 1) {
        ASSERT_STR("chip[0] = U2410");
        if (strcmp(chips[0].designator, "U2410") == 0) PASS();
        else FAIL("wrong designator");
    }
}

static void test_chip_cap_at_max(void)
{
    printf("test: chip entries capped at chip_max\n");
    // 5 chip lines, cap at 3.
    static const char buf[] =
        "# chip: U2410\n"
        "# chip: U2420\n"
        "# chip: U2430\n"
        "# chip: U2400\n"
        "# chip: U2330\n";
    badmem_range_t ranges[4];
    badmem_chip_t  chips[3];
    badmem_result_t r;
    badmem_parse_full(buf, sizeof(buf) - 1, ranges, 4, chips, 3, &r);
    ASSERT_EQ(r.n_chips, 3);
}

static void test_chip_compat_old_api(void)
{
    printf("test: backwards-compat badmem_parse ignores chip directives\n");
    static const char buf[] =
        "# chip: U2410\n"
        "0x100000,4096\n"
        "# chip: U2300\n"
        "0x200000,4096\n";
    badmem_range_t out[16];
    unsigned n = badmem_parse(buf, sizeof(buf) - 1, out, 16);
    ASSERT_EQ(n, 2);
    if (n >= 2) {
        ASSERT_EQ(out[0].start, 0x100000ULL);
        ASSERT_EQ(out[1].start, 0x200000ULL);
    }
}

// ---------------------------------------------------------------------------
// BrrBadRows binary blob tests (Track C)
// ---------------------------------------------------------------------------

// Build a minimal BrrBadRows blob in the caller-supplied buffer.
// Layout:
//   uint32_t version = 1
//   uint32_t count
//   [count * { uint8 ch, uint8 rank, uint8 bg, uint8 bank, uint32 row }]
static unsigned build_badrows_blob(uint8_t *buf, unsigned bufsz,
                                    uint8_t ch, uint8_t rank,
                                    uint8_t bg, uint8_t bank, uint32_t row)
{
    uint32_t version = 1, count = 1;
    if (bufsz < 16) return 0;
    // version (4 bytes LE)
    buf[0] = (version >>  0) & 0xff; buf[1] = (version >>  8) & 0xff;
    buf[2] = (version >> 16) & 0xff; buf[3] = (version >> 24) & 0xff;
    // count
    buf[4] = (count >>  0) & 0xff; buf[5] = (count >>  8) & 0xff;
    buf[6] = (count >> 16) & 0xff; buf[7] = (count >> 24) & 0xff;
    // tuple
    buf[8]  = ch;
    buf[9]  = rank;
    buf[10] = bg;
    buf[11] = bank;
    buf[12] = (row >>  0) & 0xff; buf[13] = (row >>  8) & 0xff;
    buf[14] = (row >> 16) & 0xff; buf[15] = (row >> 24) & 0xff;
    return 16;
}

static void test_rows_blob_single(void)
{
    printf("test: badmem_parse_rows_blob single entry\n");
    uint8_t blob[16];
    unsigned sz = build_badrows_blob(blob, sizeof(blob), 1, 0, 2, 3, 0xABCDU);

    badmem_row_t out[8];
    unsigned n = badmem_parse_rows_blob(blob, sz, out, 8);
    ASSERT_EQ(n, 1);
    if (n >= 1) {
        ASSERT_EQ(out[0].channel,    1);
        ASSERT_EQ(out[0].rank,       0);
        ASSERT_EQ(out[0].bank_group, 2);
        ASSERT_EQ(out[0].bank,       3);
        ASSERT_EQ(out[0].row,        0xABCDU);
    }
}

static void test_rows_blob_bad_version(void)
{
    printf("test: badmem_parse_rows_blob bad version rejected\n");
    uint8_t blob[16];
    build_badrows_blob(blob, sizeof(blob), 0, 0, 0, 0, 0);
    blob[0] = 2;  // version = 2, not supported

    badmem_row_t out[8];
    unsigned n = badmem_parse_rows_blob(blob, 16, out, 8);
    ASSERT_EQ(n, 0);
}

static void test_rows_blob_truncated(void)
{
    printf("test: badmem_parse_rows_blob truncated blob returns 0\n");
    uint8_t blob[16];
    build_badrows_blob(blob, sizeof(blob), 0, 0, 0, 0, 0);

    badmem_row_t out[8];
    // Supply only 10 bytes — header OK but tuple data missing.
    unsigned n = badmem_parse_rows_blob(blob, 10, out, 8);
    ASSERT_EQ(n, 0);
}

static void test_rows_blob_null(void)
{
    printf("test: badmem_parse_rows_blob null blob returns 0\n");
    badmem_row_t out[8];
    unsigned n = badmem_parse_rows_blob(NULL, 16, out, 8);
    ASSERT_EQ(n, 0);
}

static void test_rows_blob_cap(void)
{
    printf("test: badmem_parse_rows_blob capped at out_max\n");
    // Build a blob with 3 tuples.
    uint8_t blob[8 + 3 * 8];
    uint32_t version = 1, count = 3;
    blob[0] = 1; blob[1] = 0; blob[2] = 0; blob[3] = 0;  // version
    blob[4] = 3; blob[5] = 0; blob[6] = 0; blob[7] = 0;  // count
    (void)version; (void)count;
    for (int i = 0; i < 3; i++) {
        int off = 8 + i * 8;
        blob[off+0] = (uint8_t)i;  // ch
        blob[off+1] = 0;  // rank
        blob[off+2] = 0;  // bg
        blob[off+3] = 0;  // bank
        blob[off+4] = (uint8_t)i; blob[off+5] = 0; blob[off+6] = 0; blob[off+7] = 0;  // row
    }
    badmem_row_t out[2];
    unsigned n = badmem_parse_rows_blob(blob, sizeof(blob), out, 2);
    ASSERT_EQ(n, 2);  // capped at out_max=2
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(void)
{
    printf("==> test-shim: badmem parser unit tests\n\n");

    test_empty();
    test_comment_only();
    test_single_hex();
    test_multiple_ranges();
    test_alignment();
    test_alignment_cross_page();
    test_malformed_lines();
    test_inline_comment();
    test_decimal_len();
    test_cap_at_max();
    test_no_trailing_newline();

    printf("\n--- chip-mode tests ---\n\n");

    test_chip_basic();
    test_chip_mixed();
    test_chip_plain_comment_not_chip();
    test_chip_cap_at_max();
    test_chip_compat_old_api();

    printf("\n--- BrrBadRows binary blob tests ---\n\n");

    test_rows_blob_single();
    test_rows_blob_bad_version();
    test_rows_blob_truncated();
    test_rows_blob_null();
    test_rows_blob_cap();

    printf("\n");
    if (g_fail == 0) {
        printf("==> PASS  (%d assertions)\n", g_pass);
        return 0;
    } else {
        printf("==> FAIL  (%d passed, %d failed)\n", g_pass, g_fail);
        return 1;
    }
}
