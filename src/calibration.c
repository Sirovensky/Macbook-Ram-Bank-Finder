// SPDX-License-Identifier: GPL-2.0
//
// First-boot calibration: dump IMC register state + board detection to
// the scroll region so contributors can photograph the values and feed
// them into new YAML topology files.
//
// A1990 (and any T2 Mac) has no working keyboard input after ExitBootServices,
// so all pauses are time-based countdowns.  The EFI pre-boot menu (efi_menu.c)
// can set flags in boot_params_t::brr_flags before ExitBootServices to:
//   BRR_FLAG_SKIP_COUNTDOWNS -- suppress the timed photo pauses (fast mode)
//   BRR_FLAG_CALIBRATE_ONLY  -- halt after calibration instead of running tests
// Output uses fixed-row printf() rather than display_scrolled_message() so
// important lines stay put -- each phase clears the scroll region and re-prints
// from the top.

#include "stdint.h"
#include "stdbool.h"

#include "display.h"
#include "screen.h"

#include "board_topology.h"
#include "imc_dispatch.h"
#include "efi_menu.h"      /* BRR_FLAG_* definitions */

// boot.h declares boot_params_addr (the linker symbol written by startup*.S).
// bootparams.h declares boot_params_t including the brr_flags field added
// by patch 0006-bootparams-a1990-flags.patch.
// Both headers are reachable via -I../../boot in INC_DIRS.
#include "boot.h"
#include "bootparams.h"

static uint32_t read_brr_flags(void)
{
    if (boot_params_addr == 0) return 0;
    return ((const boot_params_t *)boot_params_addr)->brr_flags;
}

// Look for a token in the kernel cmdline with simple substring match.
//
// IMPORTANT: memtest's own parse_command_line() (app/config.c) runs
// BEFORE board_calibrate() and REPLACES every space with '\0' in the
// cmdline buffer.  So by the time we look, the buffer is NUL-separated
// tokens (one option per run of bytes), not a single space-separated
// string.  A naive "scan until *p" stops at the first NUL and never
// sees tokens beyond BOOT_IMAGE=/path.
//
// We iterate up to cmd_line_size instead of first-NUL, treating any
// byte sequence (including embedded NULs) as the search domain.
// Good enough for our needle set (brr_fast, brr_auto_page, brr_auto_chip)
// which all start with a unique "brr_" prefix and appear as their own
// tokens with no substring collisions against BOOT_IMAGE paths.
static int cmdline_contains(const char *needle)
{
    if (boot_params_addr == 0) return 0;
    const boot_params_t *bp = (const boot_params_t *)boot_params_addr;
    if (bp->cmd_line_ptr == 0) return 0;
    const char *hay = (const char *)(uintptr_t)bp->cmd_line_ptr;

    unsigned nlen = 0;
    while (needle[nlen]) nlen++;
    if (nlen == 0) return 0;

    unsigned hlen = bp->cmd_line_size;
    if (hlen == 0 || hlen > 4096) hlen = 4096;  // safety cap

    if (nlen > hlen) return 0;

    for (unsigned i = 0; i + nlen <= hlen; i++) {
        unsigned j = 0;
        while (j < nlen && hay[i + j] == needle[j]) j++;
        if (j == nlen) return 1;
    }
    return 0;
}

// BRR debug: `brr_fast` in cmdline narrows the test list to the 3
// most informative fast tests — walking-ones addr (catches stuck bits),
// moving-inversions 1s/0s (classic), and moving-inversions random
// (catches bit-interference).  Cuts pass runtime from ~30-60 min to
// ~5-10 min on 32 GiB.  For iteration during development only.
//
// Declared via tests.h (test_pattern_t + NUM_TEST_PATTERNS).
#include "tests.h"
static void apply_test_filter(void)
{
    if (!cmdline_contains("brr_fast")) {
        display_scrolled_message(0, "[brr] fast mode NOT active (cmdline missing `brr_fast`)");
        scroll();
        return;
    }

    // Enable only tests 0, 3, 5.  Everything else disabled.
    static const int keep[] = { 0, 3, 5 };
    for (int i = 0; i < NUM_TEST_PATTERNS; i++) {
        int k = 0;
        for (unsigned j = 0; j < sizeof(keep)/sizeof(keep[0]); j++) {
            if (keep[j] == i) { k = 1; break; }
        }
        test_list[i].enabled = k;
    }
    display_scrolled_message(0,
        "[brr] fast mode ACTIVE — enabled tests only: 0, 3, 5");
    scroll();
    display_scrolled_message(0,
        "[brr] all other tests disabled (test_list[].enabled=false)");
    scroll();
}

extern void sleep(unsigned int sec);

#define PAUSE_SECONDS 5

// Phase uses the whole scroll region (rows 12..23 inclusive = 12 rows).
#define PHASE_ROW_TOP    ROW_SCROLL_T
#define PHASE_ROW_BOT    ROW_SCROLL_B
#define STATUS_ROW       (ROW_SCROLL_B)   // countdown overwrites last row

static void clear_phase(void)
{
    clear_screen_region(PHASE_ROW_TOP, 0, PHASE_ROW_BOT, SCREEN_WIDTH - 1);
}

static void countdown(const char *tag, unsigned seconds)
{
    // If SKIP_COUNTDOWNS was requested from the EFI menu, suppress pauses.
    if (read_brr_flags() & BRR_FLAG_SKIP_COUNTDOWNS) {
        clear_screen_region(STATUS_ROW, 0, STATUS_ROW, SCREEN_WIDTH - 1);
        return;
    }
    for (unsigned s = seconds; s > 0; s--) {
        clear_screen_region(STATUS_ROW, 0, STATUS_ROW, SCREEN_WIDTH - 1);
        printf(STATUS_ROW, 0, "--- %s --- photo NOW --- %u s ---", tag, s);
        sleep(1);
    }
    clear_screen_region(STATUS_ROW, 0, STATUS_ROW, SCREEN_WIDTH - 1);
}

void board_calibrate(void)
{
    int r;

    // BRR debug: echo the kernel cmdline so the user can confirm grub
    // actually passed what they expected.
    //
    // Note: memtest's parse_command_line() ran before us and replaced
    // every ' ' with '\0' in the buffer (tokenisation in place).  We
    // iterate up to cmd_line_size and substitute NULs back to spaces
    // in a local copy so the printed line is legible again.
    if (boot_params_addr != 0) {
        const boot_params_t *bp = (const boot_params_t *)boot_params_addr;
        if (bp->cmd_line_ptr != 0 && bp->cmd_line_size > 0) {
            const char *cmd = (const char *)(uintptr_t)bp->cmd_line_ptr;
            char buf[72];
            unsigned cap = sizeof(buf) - 1;
            unsigned sz  = bp->cmd_line_size;
            if (sz > cap) sz = cap;
            for (unsigned j = 0; j < sz; j++) {
                char c = cmd[j];
                buf[j] = (c == '\0') ? ' ' : c;
            }
            buf[sz] = 0;
            // Trim trailing spaces.
            while (sz > 0 && buf[sz - 1] == ' ') { buf[--sz] = 0; }
            display_scrolled_message(0, "[brr] cmdline: %s", (uintptr_t)buf);
            scroll();
        } else {
            display_scrolled_message(0, "[brr] cmdline: (empty)");
            scroll();
        }
    }

    // BRR debug: apply cmdline test-list filter BEFORE dummy_run starts.
    // Runs only 3 tests if `brr_fast` is in the kernel cmdline.
    apply_test_filter();

    // Phase 0 — capture any prior memtest boot output. Don't clear.
    // Just show a fixed countdown in the footer region so the user can
    // photograph whatever is already on screen.
    countdown("boot-log", PAUSE_SECONDS);

    // Phase 1 — IMC configuration. Clear scroll region, print fixed rows.
    clear_phase();
    r = PHASE_ROW_TOP;

    const struct mc_config *mc = imc_config();
    if (!mc) {
        printf(r++, 0, "Coffee Lake IMC not detected. CPU unsupported.");
        countdown("end", PAUSE_SECONDS);
        return;
    }

    printf(r++, 0, "MCHBAR base: %016x",
        (uintptr_t)mc->mchbar_base);
    printf(r++, 0, "channels populated=%u  chip_width=x%u",
        mc->channels_populated, mc->chip_width);
    for (int c = 0; c < 2 && r <= PHASE_ROW_BOT - 1; c++) {
        const struct mc_channel *ch = &mc->channel[c];
        printf(r++, 0, "CH%u raw=%08x populated=%u",
            c, ch->mad_dimm_raw, ch->populated);
        for (int d = 0; d < 2 && r <= PHASE_ROW_BOT - 1; d++) {
            const struct mc_dimm *dd = &ch->dimm[d];
            if (!dd->populated) continue;
            printf(r++, 0, "  DIMM%c %u MB  ranks=%u  x%u",
                'A' + d, dd->size_mb, dd->ranks, dd->width);
        }
    }
    printf(r++, 0, "MAD_CHNL=%08x CH_HASH=%08x CH_EHASH=%08x",
        mc->mad_chnl, mc->chan_hash, mc->chan_ehash);
    printf(r++, 0, "MAD_INTRA_CH0=%08x MAD_INTRA_CH1=%08x",
        mc->mad_intra_ch0, mc->mad_intra_ch1);

    // Rank decode probe: show what decoder would emit on a neutral PA.
    struct pa_decoded probe = imc_decode_pa(0x40000000ULL);  // 1 GiB mark
    const char *rk_src;
    if (!probe.rank_valid)           rk_src = "ambiguous (hedging both)";
    else if (probe.rank_speculative) rk_src = "speculative (MAD_INTRA)";
    else                             rk_src = "certain (1R channel)";
    printf(r++, 0, "rank decode @ PA=1GiB: ch%u rk%u -- %s",
        probe.channel, probe.rank, rk_src);

    // Bank/row/col decode probe (DRAMDig Skylake XOR prior).
    const char *br_src = probe.bank_row_valid
        ? (probe.bank_row_speculative ? "speculative (DRAMDig prior)" : "validated")
        : "unavailable";
    if (r <= PHASE_ROW_BOT - 1)
        printf(r++, 0, "bank/row decode @ PA=1GiB: bg=%u bank=%u row=%x -- %s",
            probe.bank_group, probe.bank, probe.row, br_src);

    countdown("imc-config", PAUSE_SECONDS);

    // Phase 2 — board identification. This is the most important screen.
    clear_phase();
    r = PHASE_ROW_TOP;

    printf(r++, 0, "=== BOARD IDENTIFICATION ===");
    int src = smbios_debug_source();
    printf(r++, 0, "SMBIOS source=%i ep=%016x tlen=%u",
        src, (uintptr_t)smbios_debug_ep_addr(), smbios_debug_table_len());
    const char *prod = smbios_board_id();
    printf(r++, 0, "SMBIOS product: [%s]", prod ? prod : "(unknown)");
    const board_profile_t *p = board_detect();
    if (p) {
        printf(r++, 0, "Overlay: %s", p->friendly_name);
        printf(r++, 0, "  %u packages x%u, %u ch, %u rk",
            p->package_count, p->chip_width,
            p->channels, p->ranks_per_channel);
    } else {
        printf(r++, 0, "Overlay: NO MATCH (generic tier-1 decode only)");
    }

    countdown("board-id", PAUSE_SECONDS);

    // Phase 3 — MCHBAR register dump for new-board calibration.
    clear_phase();
    r = PHASE_ROW_TOP;
    printf(r++, 0, "--- MCHBAR 0x5000..0x5100 dump ---");
    imc_dump_mchbar_at(r, PHASE_ROW_BOT);

    countdown("mchbar", PAUSE_SECONDS);

    clear_phase();

    if (read_brr_flags() & BRR_FLAG_CALIBRATE_ONLY) {
        // User pressed [C] in the EFI menu: halt here after showing results.
        printf(PHASE_ROW_TOP,     0, "=== calibration done; halting (calibrate-only mode) ===");
        printf(PHASE_ROW_TOP + 1, 0, "Power off or reset the machine.");
        // Spin forever -- tests must not start.
        while (1) {
#if defined(__x86_64__) || defined(__i386__)
            __asm__ __volatile__("hlt");
#endif
        }
    }

    printf(PHASE_ROW_TOP, 0, "=== calibration done; tests starting ===");
}
