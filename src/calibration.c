// SPDX-License-Identifier: GPL-2.0
//
// First-boot calibration: dump IMC register state + board detection to
// the scroll area so contributors can photograph the values and feed
// them into new YAML topology files.

#include "stdint.h"
#include "stdbool.h"

#include "display.h"

#include "board_topology.h"
#include "cfl_decode.h"
#include "smbios.h"

extern int scroll_message_row;
extern void scroll(void);

#define LOG(...) do {                                    \
    display_scrolled_message(0, __VA_ARGS__);            \
    scroll();                                            \
} while (0)

void board_calibrate(void)
{
    LOG("=== BOARD-MEMTEST CALIBRATION ===");

    const char *prod = smbios_board_id();
    LOG("SMBIOS product: %s", prod ? prod : "(unknown)");

    const struct mc_config *mc = cfl_mc_config();
    if (!mc) {
        LOG("Coffee Lake IMC not detected. CPU outside supported family.");
        return;
    }

    LOG("MCHBAR base: %016llx", (unsigned long long)mc->mchbar_base);
    LOG("channels populated=%u  chip_width=x%u",
        mc->channels_populated, mc->chip_width);

    for (int c = 0; c < 2; c++) {
        const struct mc_channel *ch = &mc->channel[c];
        LOG("CH%d raw=%08x populated=%d", c, ch->mad_dimm_raw, ch->populated);
        for (int d = 0; d < 2; d++) {
            const struct mc_dimm *dd = &ch->dimm[d];
            if (!dd->populated) continue;
            LOG("  DIMM%c %u MB  ranks=%u  x%u",
                'A' + d, dd->size_mb, dd->ranks, dd->width);
        }
    }

    LOG("MAD_CHNL=%08x  CH_HASH=%08x  CH_EHASH=%08x",
        mc->mad_chnl, mc->chan_hash, mc->chan_ehash);
    LOG("MAD_INTRA_CH0=%08x  MAD_INTRA_CH1=%08x",
        mc->mad_intra_ch0, mc->mad_intra_ch1);

    cfl_dump_mchbar();

    const board_profile_t *p = board_detect();
    if (p) {
        LOG("Detected board: %s (%u packages x%u, %u ch, %u rk)",
            p->friendly_name, p->package_count, p->chip_width,
            p->channels, p->ranks_per_channel);
    } else {
        LOG("No board overlay match. Generic tier-1 decode only.");
    }

    LOG("=== calibration done; tests starting ===");
}
