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
#include "cfl_decode.h"
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

extern void sleep(unsigned int sec);

#define PAUSE_SECONDS 10

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

    // Phase 0 — capture any prior memtest boot output. Don't clear.
    // Just show a fixed countdown in the footer region so the user can
    // photograph whatever is already on screen.
    countdown("boot-log", PAUSE_SECONDS);

    // Phase 1 — IMC configuration. Clear scroll region, print fixed rows.
    clear_phase();
    r = PHASE_ROW_TOP;

    const struct mc_config *mc = cfl_mc_config();
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
    struct pa_decoded probe = cfl_decode_pa(0x40000000ULL);  // 1 GiB mark
    const char *rk_src;
    if (!probe.rank_valid)           rk_src = "ambiguous (hedging both)";
    else if (probe.rank_speculative) rk_src = "speculative (MAD_INTRA)";
    else                             rk_src = "certain (1R channel)";
    printf(r++, 0, "rank decode @ PA=1GiB: ch%u rk%u -- %s",
        probe.channel, probe.rank, rk_src);

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
    cfl_dump_mchbar_at(r, PHASE_ROW_BOT);

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
