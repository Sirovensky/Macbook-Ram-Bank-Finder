// SPDX-License-Identifier: GPL-2.0
//
// First-boot calibration: dump IMC register state to the scroll area so
// the user can photograph the values and feed them back into the
// topology tables.

#include "stdint.h"
#include "stdbool.h"

#include "display.h"

#include "a1990_topology.h"
#include "cfl_decode.h"
#include "smbios.h"

// memtest86plus scroll helper symbols
extern int scroll_message_row;
extern void scroll(void);

#define LOG(...) do {                                    \
    display_scrolled_message(0, __VA_ARGS__);            \
    scroll();                                            \
} while (0)

void a1990_calibrate(void)
{
    LOG("=== A1990-MEMTEST CALIBRATION ===");

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

    const a1990_topology_t *topo = a1990_detect();
    if (topo) {
        LOG("Detected: %s (%u packages x%u)",
            topo->variant_name, topo->package_count, topo->chip_width);
    } else {
        LOG("No A1990 topology match. Generic decode only.");
    }

    LOG("=== calibration done; tests starting ===");
}
