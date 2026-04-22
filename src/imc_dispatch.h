// SPDX-License-Identifier: GPL-2.0
//
// IMC (Integrated Memory Controller) dispatch layer.
// Provides a pluggable per-family driver model; callers use the
// generic imc_* API and never need to know the CPU family.

#ifndef IMC_DISPATCH_H
#define IMC_DISPATCH_H

#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Shared data structures (preserved from cfl_decode.h for ABI compatibility).
// ---------------------------------------------------------------------------

struct mc_dimm {
    bool     populated;
    uint8_t  ranks;       // 1 or 2
    uint8_t  width;       // 8 or 16
    uint32_t size_mb;     // per DIMM, in MB (uint32 lets future 64GB+ DIMMs fit)
};

struct mc_channel {
    bool     populated;
    struct   mc_dimm dimm[2];  // up to 2 DIMM slots per channel
    uint32_t mad_dimm_raw;     // raw register for debug
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

// ---------------------------------------------------------------------------
// PA decode result — superset of old cfl_decode.h struct pa_decoded.
// New fields have zero/false defaults so existing callers are unaffected.
// ---------------------------------------------------------------------------

struct pa_decoded {
    bool     valid;
    uint8_t  channel;
    uint8_t  rank;
    bool     rank_valid;           // true when rank is known
    bool     rank_speculative;     // rank_valid but from unvalidated decode
    uint8_t  bank_group;
    uint8_t  bank;
    uint32_t row;                  // up to 18 bits
    uint16_t col;                  // up to 12 bits
    bool     bank_row_valid;       // true if bank/row/col decoded
    bool     bank_row_speculative; // true = based on prior, unverified
};

// ---------------------------------------------------------------------------
// Per-family operations table.
// ---------------------------------------------------------------------------

struct imc_ops {
    const char *family_name;

    // CPUID probe; returns true if this family matches the running CPU.
    bool (*detect)(void);

    // Read MCHBAR, populate and cache mc_config; returns NULL on failure.
    const struct mc_config *(*config)(void);

    // Invalidate config cache (calibration mode).
    void (*config_refresh)(void);

    // Decode a physical address to channel/rank/bank/row/col.
    struct pa_decoded (*decode_pa)(uint64_t pa);

    // Number of row-address bits derived from MAD_DIMM chip size.
    unsigned (*row_bits)(void);

    // Enumerate up to cap PAs hitting (ch, rank, bg, bank, row).
    // Walks PA space 0..total_memory() step 4096, filters via decode_pa().
    // Returns number of matching PAs written to out[].
    unsigned (*enumerate_row)(uint8_t ch, uint8_t rank,
                              uint8_t bg, uint8_t bank, uint32_t row,
                              uint64_t *out, unsigned cap);

    // Total installed DRAM in bytes (sum across all populated DIMMs).
    uint64_t (*total_memory)(void);

    // Diagnostics (may be NULL).
    void (*dump_mchbar)(void);
    void (*dump_mchbar_at)(int row_first, int row_last);
};

// ---------------------------------------------------------------------------
// Public API — dispatches to the detected family.
// ---------------------------------------------------------------------------

// Returns the active imc_ops, or NULL if no family matched.
// Result is cached after the first successful detect().
const struct imc_ops *imc_active(void);

// Convenience wrappers (each calls imc_active() internally).
const struct mc_config *imc_config(void);
struct pa_decoded       imc_decode_pa(uint64_t pa);
void                    imc_config_refresh(void);
void                    imc_dump_mchbar(void);
void                    imc_dump_mchbar_at(int row_first, int row_last);

#endif // IMC_DISPATCH_H
