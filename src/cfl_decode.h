// SPDX-License-Identifier: GPL-2.0
//
// Coffee Lake / Skylake client IMC: read configuration and decode
// physical addresses back to (channel, rank, bank, row, col).

#ifndef CFL_DECODE_H
#define CFL_DECODE_H

#include <stdint.h>
#include <stdbool.h>

struct mc_dimm {
    bool    populated;
    uint8_t ranks;          // 1 or 2
    uint8_t width;          // 8 or 16
    uint16_t size_mb;       // per DIMM
};

struct mc_channel {
    bool    populated;
    struct  mc_dimm dimm[2];   // up to 2 DIMM slots per channel
    uint32_t mad_dimm_raw;     // raw register dump for debug
};

struct mc_config {
    bool     valid;
    uint8_t  channels_populated;
    uint8_t  chip_width;       // 8 or 16 (vote across populated DIMMs)
    uint64_t mchbar_base;
    struct   mc_channel channel[2];

    // Interleave / hash register dumps (for debug + decode).
    uint32_t mad_chnl;
    uint32_t chan_hash;
    uint32_t chan_ehash;
    uint32_t mad_intra_ch0;
    uint32_t mad_intra_ch1;
};

struct pa_decoded {
    bool    valid;
    uint8_t channel;
    uint8_t rank;
    bool    rank_valid;     // true when rank is known (1R channel or decoded)
    bool    rank_speculative; // rank_valid but from unvalidated MAD_INTRA decode
    uint8_t bank_group;
    uint8_t bank;
    uint32_t row;
    uint16_t col;
};

// Probe MCHBAR, read all relevant regs. Idempotent. Returns NULL if
// CPU is not Coffee Lake/Skylake client family.
const struct mc_config *cfl_mc_config(void);

// Force a fresh read (for calibration tool).
void cfl_mc_config_refresh(void);

// Decode a physical address. Confidence may be partial — `valid`
// refers to channel only for now. Rank/bank/row/col best-effort.
struct pa_decoded cfl_decode_pa(uint64_t pa);

// Dump all IMC-relevant MCHBAR registers to the screen (calibration mode).
void cfl_dump_mchbar(void);

// Same, but prints to fixed rows [row_first..row_last] with no scrolling.
void cfl_dump_mchbar_at(int row_first, int row_last);

#endif // CFL_DECODE_H
