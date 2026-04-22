// SPDX-License-Identifier: GPL-2.0
//
// Decoder self-test harness — validates DRAMDig-prior bank/row XOR masks via
// bank-conflict / row-hit timing probes executed during EFI boot services.
//
// Algorithm overview:
//   1. Allocate a 4 MiB physically-contiguous buffer via AllocatePages.
//   2. Find three PA pairs from within that buffer:
//        R  — same (bg, bank, row), different col  → row-hit (fast)
//        C  — same (bg, bank),     different row   → bank-conflict (slow)
//        D  — different bank                       → different-bank (fast)
//   3. Measure median access latency for each pair over 100 samples using
//      RDTSC + CLFLUSH + MFENCE.
//   4. Pass if C_med/R_med > 1.5 AND C_med/D_med > 1.5.
//   5. Write result + latency snapshot to NVRAM "BrrDecoderStatus" (8 bytes).
//
// Timing budget: capped at 5 s wall-clock (via Stall accounting).
//
// Copyright (C) 2024 A1990-memtest contributors.

#include <stdint.h>
#include <stdbool.h>

#include "efi.h"
#include "decoder_selftest.h"
#include "imc_dispatch.h"

// ---------------------------------------------------------------------------
// Local ConOut pointer. Set at the top of decoder_selftest_run() from the
// passed sys_table. We don't extern the efi_menu.c static — keep modules
// independent so either can be compiled/linked alone.
// ---------------------------------------------------------------------------
static efi_simple_text_out_t *g_con_out;

// Thin inline wrappers so this file can call con_puts / con_put_dec without
// relying on a shared header.
static void st_puts(const char *s)
{
    if (!g_con_out) return;
    efi_char16_t buf[2];
    buf[1] = 0;
    for (; *s; s++) {
        if (*s == '\n') {
            buf[0] = '\r';
            g_con_out->output_string(g_con_out, buf);
        }
        buf[0] = (uint16_t)(unsigned char)*s;
        g_con_out->output_string(g_con_out, buf);
    }
}

static void st_put_dec(unsigned int v)
{
    char tmp[12];
    char *p = &tmp[11];
    *p = '\0';
    do {
        *--p = (char)('0' + (v % 10));
        v /= 10;
    } while (v > 0);
    st_puts(p);
}

// ---------------------------------------------------------------------------
// Stall helper (cast from void * field in efi_boot_services_t).
// ---------------------------------------------------------------------------
typedef efi_status_t (efiapi *efi_stall_fn)(uintn_t microseconds);

// ---------------------------------------------------------------------------
// SetVariable function-pointer type (same as efi_menu.c).
// ---------------------------------------------------------------------------
typedef efi_status_t (efiapi *set_variable_fn)(
    efi_char16_t *name, efi_guid_t *guid,
    uint32_t attrs, uintn_t data_size, void *data);

// ---------------------------------------------------------------------------
// BRR vendor GUID — same as efi_menu.c / badmem_log.c.
// {3E3E9DB2-1A2B-4B5C-9D1E-5F6A7B8C9D0E}
// ---------------------------------------------------------------------------
static const efi_guid_t BRR_GUID_ST = {
    0x3e3e9db2, 0x1a2b, 0x4b5c,
    { 0x9d, 0x1e, 0x5f, 0x6a, 0x7b, 0x8c, 0x9d, 0x0e }
};

// NVRAM variable name: L"BrrDecoderStatus"
static const efi_char16_t BRR_DECODER_VARNAME[] = {
    'B','r','r','D','e','c','o','d','e','r','S','t','a','t','u','s', 0
};

#define EFI_VAR_NV_BS_RT  (0x00000001u | 0x00000002u | 0x00000004u)

// ---------------------------------------------------------------------------
// NVRAM payload — 8 bytes total.
// Layout:
//   [0..3]  uint32_t status  (brr_decoder_status_t value)
//   [4..7]  uint32_t reserved / latency hint:
//             Written as ASCII string to match the shim reader, which
//             uses mask_nvram_get_ascii() + byte-compare.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// RDTSC with serialising LFENCE before.
// ---------------------------------------------------------------------------
static inline uint64_t rdtsc_lfence(void)
{
    uint32_t lo, hi;
    __asm__ volatile(
        "lfence\n\t"
        "rdtsc"
        : "=a"(lo), "=d"(hi)
        :
        : "memory"
    );
    return ((uint64_t)hi << 32) | lo;
}

// CLFLUSH a cache line — volatile barrier so the compiler can't move it.
static inline void clflush_ptr(volatile void *ptr)
{
    __asm__ volatile(
        "clflush (%[p])"
        :
        : [p] "r"(ptr)
        : "memory"
    );
}

// MFENCE: drain store buffer + ensure prior clflushes are globally visible.
static inline void mfence(void)
{
    __asm__ volatile("mfence" ::: "memory");
}

// ---------------------------------------------------------------------------
// Measure a single access latency (cycles) for ptr2 after evicting ptr1.
//
// Pattern:
//   open ptr1 (row-activate), flush ptr1 (force next access to DRAM),
//   access ptr2 (incurs bank-conflict or row-hit depending on PA relation),
//   measure with RDTSC around ptr2 only.
// ---------------------------------------------------------------------------
static uint64_t measure_pair(volatile uint8_t *ptr1, volatile uint8_t *ptr2)
{
    volatile uint8_t dummy;

    // Touch ptr1 to open its row in the DRAM row buffer.
    dummy = *ptr1;
    (void)dummy;

    // Evict ptr1 from all caches so the next access goes to DRAM.
    clflush_ptr((volatile void *)ptr1);

    // Also evict ptr2 so its access is a fresh DRAM load.
    clflush_ptr((volatile void *)ptr2);
    mfence();

    uint64_t t0 = rdtsc_lfence();
    dummy = *ptr2;
    uint64_t t1 = rdtsc_lfence();

    (void)dummy;
    return t1 - t0;
}

// ---------------------------------------------------------------------------
// Sorting helper (insertion sort — 100 elements, in-place).
// ---------------------------------------------------------------------------
static void sort_u64(uint64_t *a, unsigned n)
{
    for (unsigned i = 1; i < n; i++) {
        uint64_t key = a[i];
        unsigned j = i;
        while (j > 0 && a[j - 1] > key) {
            a[j] = a[j - 1];
            j--;
        }
        a[j] = key;
    }
}

// ---------------------------------------------------------------------------
// Collect SAMPLES latency readings for (ptr1, ptr2) and return the median.
// Stall budget_us is decremented by STALL_PER_SAMPLE_US per sample so the
// caller can cap total wall-clock time.
// ---------------------------------------------------------------------------
#define SAMPLES              100u
#define STALL_PER_SAMPLE_US  10u   // pessimistic; each sample is ~few us

static uint64_t median_latency(volatile uint8_t *ptr1, volatile uint8_t *ptr2,
                                uint32_t *budget_us,
                                efi_stall_fn do_stall)
{
    static uint64_t samples[SAMPLES]; // static to avoid stack pressure in EFI
    for (unsigned i = 0; i < SAMPLES; i++) {
        samples[i] = measure_pair(ptr1, ptr2);
        // Alternate roles to force row cycling and flush residual state.
        clflush_ptr((volatile void *)ptr1);
        clflush_ptr((volatile void *)ptr2);
        mfence();

        if (do_stall && *budget_us >= STALL_PER_SAMPLE_US) {
            do_stall(STALL_PER_SAMPLE_US);
            *budget_us -= STALL_PER_SAMPLE_US;
        } else if (*budget_us == 0) {
            // Time budget exhausted; return 0 as sentinel.
            return 0;
        }
    }
    sort_u64(samples, SAMPLES);
    return samples[SAMPLES / 2]; // median
}

// ---------------------------------------------------------------------------
// Walk the 4 MiB buffer looking for candidate PA pairs.
//
// Strategy: step through the buffer in 4 KiB pages, decode each PA, look for
// pages whose decoded (bg, bank, row) relationships match the required class.
// Return false if no suitable pair found within the buffer.
//
// pa_base: physical address of the start of the buffer (from AllocatePages).
// buf:     virtual pointer to the buffer (same mapping assumed 1:1 in EFI).
// buf_pages: total number of 4096-byte pages in the buffer (= 1024 for 4 MiB).
// ---------------------------------------------------------------------------
#define BUF_PAGES   1024u  // 4 MiB / 4 KiB
#define PAGE_BYTES  4096u

// Find two PAs: same (bg, bank, row), different col — "row hit".
static bool find_row_hit(uint64_t pa_base, volatile uint8_t *buf,
                         volatile uint8_t **out1, volatile uint8_t **out2)
{
    struct pa_decoded first = {0};
    uint64_t first_pa = 0;
    bool have_first = false;

    for (unsigned i = 0; i < BUF_PAGES; i++) {
        uint64_t pa = pa_base + (uint64_t)i * PAGE_BYTES;
        struct pa_decoded d = imc_decode_pa(pa);
        if (!d.valid || !d.bank_row_valid) continue;

        if (!have_first) {
            first = d;
            first_pa = pa;
            have_first = true;
            continue;
        }

        // Same bank-group, bank, row; must differ in col or PA (different page
        // = different column address within the row).
        if (d.bank_group == first.bank_group &&
            d.bank       == first.bank       &&
            d.row        == first.row        &&
            pa           != first_pa) {
            *out1 = buf + (first_pa - pa_base);
            *out2 = buf + (pa       - pa_base);
            return true;
        }
    }
    return false;
}

// Find two PAs: same (bg, bank), different row — "bank conflict".
static bool find_bank_conflict(uint64_t pa_base, volatile uint8_t *buf,
                               volatile uint8_t **out1, volatile uint8_t **out2)
{
    struct pa_decoded first = {0};
    uint64_t first_pa = 0;
    bool have_first = false;

    for (unsigned i = 0; i < BUF_PAGES; i++) {
        uint64_t pa = pa_base + (uint64_t)i * PAGE_BYTES;
        struct pa_decoded d = imc_decode_pa(pa);
        if (!d.valid || !d.bank_row_valid) continue;

        if (!have_first) {
            first = d;
            first_pa = pa;
            have_first = true;
            continue;
        }

        if (d.bank_group == first.bank_group &&
            d.bank       == first.bank       &&
            d.row        != first.row) {
            *out1 = buf + (first_pa - pa_base);
            *out2 = buf + (pa       - pa_base);
            return true;
        }
    }
    return false;
}

// Find two PAs: different bank (bank_group or bank differs) — "different bank".
static bool find_diff_bank(uint64_t pa_base, volatile uint8_t *buf,
                           volatile uint8_t **out1, volatile uint8_t **out2)
{
    struct pa_decoded first = {0};
    uint64_t first_pa = 0;
    bool have_first = false;

    for (unsigned i = 0; i < BUF_PAGES; i++) {
        uint64_t pa = pa_base + (uint64_t)i * PAGE_BYTES;
        struct pa_decoded d = imc_decode_pa(pa);
        if (!d.valid || !d.bank_row_valid) continue;

        if (!have_first) {
            first = d;
            first_pa = pa;
            have_first = true;
            continue;
        }

        if (d.bank_group != first.bank_group || d.bank != first.bank) {
            *out1 = buf + (first_pa - pa_base);
            *out2 = buf + (pa       - pa_base);
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Public entry point.
// ---------------------------------------------------------------------------

brr_decoder_status_t decoder_selftest_run(void *sys_table_arg)
{
    efi_system_table_t *st = (efi_system_table_t *)sys_table_arg;
    if (!st) return BRR_DECODER_UNKNOWN;

    g_con_out = st->con_out;                 // file-local; st_puts uses it
    efi_boot_services_t    *bs = st->boot_services;
    efi_runtime_services_t *rs = st->runtime_services;
    if (!bs) return BRR_DECODER_UNKNOWN;

    efi_stall_fn do_stall = (efi_stall_fn)(uintptr_t)bs->stall;
    if (!do_stall) return BRR_DECODER_UNKNOWN;

    // -----------------------------------------------------------------------
    // Check that the active IMC family has a bank/row decoder.
    // -----------------------------------------------------------------------
    const struct imc_ops *ops = imc_active();
    if (!ops || !ops->decode_pa) {
        st_puts("[decoder] No IMC decoder available — skipping self-test.\n");
        return BRR_DECODER_NOT_APPLICABLE;
    }

    // Quick probe: does decode_pa give bank_row_valid for PA=0?
    {
        struct pa_decoded probe = ops->decode_pa(0);
        if (!probe.bank_row_valid) {
            st_puts("[decoder] Decoder has no bank/row output — NOT_APPLICABLE.\n");
            return BRR_DECODER_NOT_APPLICABLE;
        }
    }

    st_puts("[decoder] Starting bank/row decoder self-test...\n");

    // -----------------------------------------------------------------------
    // Allocate 4 MiB physically-contiguous buffer via AllocatePages.
    // AllocateAnyPages = 0, EFI_LOADER_DATA = 2.
    // -----------------------------------------------------------------------
    efi_phys_addr_t buf_pa = 0;
    uintn_t pages = BUF_PAGES; // 1024 * 4096 = 4 MiB
    efi_status_t s = bs->allocate_pages(0 /*AllocateAnyPages*/,
                                         EFI_LOADER_DATA,
                                         pages, &buf_pa);
    if (s != EFI_SUCCESS || !buf_pa) {
        st_puts("[decoder] AllocatePages failed — returning UNKNOWN.\n");
        return BRR_DECODER_UNKNOWN;
    }

    volatile uint8_t *buf = (volatile uint8_t *)(uintptr_t)buf_pa;

    // Initialise buffer (touch all pages so physical backing is established).
    for (unsigned i = 0; i < BUF_PAGES; i++) {
        buf[i * PAGE_BYTES] = (uint8_t)i;
    }
    mfence();

    // -----------------------------------------------------------------------
    // Time budget: 5 000 000 us = 5 s total.  We decrement it as we go.
    // -----------------------------------------------------------------------
    uint32_t budget_us = 5000000u;

    // -----------------------------------------------------------------------
    // Find candidate pointer pairs.
    // -----------------------------------------------------------------------
    volatile uint8_t *r1 = 0, *r2 = 0; // row-hit pair
    volatile uint8_t *c1 = 0, *c2 = 0; // bank-conflict pair
    volatile uint8_t *d1 = 0, *d2 = 0; // different-bank pair

    bool ok_r = find_row_hit     (buf_pa, buf, &r1, &r2);
    bool ok_c = find_bank_conflict(buf_pa, buf, &c1, &c2);
    bool ok_d = find_diff_bank   (buf_pa, buf, &d1, &d2);

    if (!ok_r || !ok_c || !ok_d) {
        st_puts("[decoder] Could not find required PA pairs in buffer — UNKNOWN.\n");
        bs->free_pages(buf_pa, pages);
        return BRR_DECODER_UNKNOWN;
    }

    st_puts("[decoder] PA pairs found. Measuring latencies...\n");

    // -----------------------------------------------------------------------
    // Measure median latencies.
    // -----------------------------------------------------------------------
    uint64_t r_med = median_latency(r1, r2, &budget_us, do_stall);
    uint64_t c_med = median_latency(c1, c2, &budget_us, do_stall);
    uint64_t d_med = median_latency(d1, d2, &budget_us, do_stall);

    // Free buffer before reporting (even if result is bad).
    bs->free_pages(buf_pa, pages);

    if (r_med == 0 || c_med == 0 || d_med == 0) {
        // Time budget exhausted during measurement.
        st_puts("[decoder] Timing budget exhausted — returning UNKNOWN.\n");
        return BRR_DECODER_UNKNOWN;
    }

    // -----------------------------------------------------------------------
    // Evaluate pass criteria.
    //   C/R > 1.5  →  conflict clearly slower than row-hit
    //   C/D > 1.5  →  conflict clearly slower than diff-bank
    // We use integer arithmetic: X/Y > 1.5 iff 2*X > 3*Y.
    // -----------------------------------------------------------------------
    bool pass_cr = (2 * c_med) > (3 * r_med);
    bool pass_cd = (2 * c_med) > (3 * d_med);

    brr_decoder_status_t result =
        (pass_cr && pass_cd) ? BRR_DECODER_VALIDATED : BRR_DECODER_FAILED;

    // -----------------------------------------------------------------------
    // Print result.
    // -----------------------------------------------------------------------
    if (result == BRR_DECODER_VALIDATED) {
        uint64_t diff = (c_med > r_med) ? (c_med - r_med) : 0;
        st_puts("[decoder] Bank/row decoder VALIDATED via bank-conflict timing (~");
        st_put_dec((unsigned int)(diff > 0xFFFFFFFFu ? 0xFFFFFFFFu : diff));
        st_puts(" cycles diff).\n");
    } else {
        st_puts("[decoder] Bank/row decoder FAILED (timing test inconclusive). "
                "Will fall back to full channel mask.\n");
        st_puts("[decoder]   rowhit=");
        st_put_dec((unsigned int)(r_med > 0xFFFFFFFFu ? 0xFFFFFFFFu : r_med));
        st_puts("  conflict=");
        st_put_dec((unsigned int)(c_med > 0xFFFFFFFFu ? 0xFFFFFFFFu : c_med));
        st_puts("  diffbank=");
        st_put_dec((unsigned int)(d_med > 0xFFFFFFFFu ? 0xFFFFFFFFu : d_med));
        st_puts("\n");
    }

    // -----------------------------------------------------------------------
    // Write result to NVRAM: "BrrDecoderStatus" as an ASCII string.
    // The shim (efi/mask-shim/main.c) reads it via mask_nvram_get_ascii()
    // and byte-compares to "VALIDATED" / "FAILED" / "UNKNOWN" /
    // "NOT_APPLICABLE".  Keep the NUL terminator out of DataSize so the
    // shim's size-sensitive compare works correctly.
    // -----------------------------------------------------------------------
    (void)c_med; (void)r_med;  // latency hint dropped in favor of simple string
    if (rs) {
        set_variable_fn set_var =
            (set_variable_fn)(uintptr_t)rs->set_variable;
        if (set_var) {
            const char *str;
            unsigned     len;
            switch (result) {
                case BRR_DECODER_VALIDATED:
                    str = "VALIDATED"; len = 9; break;
                case BRR_DECODER_FAILED:
                    str = "FAILED"; len = 6; break;
                case BRR_DECODER_NOT_APPLICABLE:
                    str = "NOT_APPLICABLE"; len = 14; break;
                default:
                    str = "UNKNOWN"; len = 7; break;
            }
            set_var(
                (efi_char16_t *)BRR_DECODER_VARNAME,
                (efi_guid_t *)&BRR_GUID_ST,
                EFI_VAR_NV_BS_RT,
                (uintn_t)len,
                (void *)str);
        }
    }

    return result;
}
