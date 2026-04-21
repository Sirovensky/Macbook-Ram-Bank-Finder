// SPDX-License-Identifier: GPL-2.0
//
// Coffee Lake / Skylake client IMC decoder.
//
// Source offsets validated from:
//   memtest86plus/system/imc/x86/intel_skl.c
//   coreboot sandybridge/haswell raminit (for bit layouts — must be
//   re-validated on Coffee Lake hardware).
//
// PA->DRAM decode is best-effort. There is no upstream open-source
// decoder for this IMC family; values below are derived from Haswell
// and must be sanity-checked against a live A1990 dump.

#include "stdint.h"
#include "stdbool.h"

#include "cfl_decode.h"
#include "cpuid.h"
#include "pci.h"
#include "vmem.h"

// MCHBAR PCI offsets
#define MCHBAR_LO_REG    0x48
#define MCHBAR_HI_REG    0x4C
#define MCHBAR_ENABLE    0x1
#define MCHBAR_MASK      0x7FFFFF8000ULL
#define MCHBAR_WINDOW    (1UL << 15)

// MCHBAR-relative offsets (Skylake-client family; Coffee Lake inherits)
#define MCHBAR_MAD_CHNL       0x5000
#define MCHBAR_MAD_INTRA_CH0  0x5004   // tentative
#define MCHBAR_MAD_INTRA_CH1  0x5008   // tentative
#define MCHBAR_MAD_DIMM_CH0   0x500C
#define MCHBAR_MAD_DIMM_CH1   0x5010
#define MCHBAR_CHANNEL_HASH   0x5024   // tentative, Haswell location
#define MCHBAR_CHANNEL_EHASH  0x5028   // tentative, Haswell location

// MAD_DIMM bit layout — Haswell layout used as starting point.
// Validated fields will migrate to proper Skylake+ positions after
// first calibration run on an A1990.
#define MAD_DIMM_A_SIZE_SHIFT    0
#define MAD_DIMM_A_SIZE_MASK     0xFF
#define MAD_DIMM_B_SIZE_SHIFT    8
#define MAD_DIMM_B_SIZE_MASK     0xFF
#define MAD_DIMM_A_RANKS_BIT     (1 << 17)
#define MAD_DIMM_B_RANKS_BIT     (1 << 18)
#define MAD_DIMM_A_WIDTH_BIT     (1 << 19)   // 0=x8, 1=x16
#define MAD_DIMM_B_WIDTH_BIT     (1 << 20)
#define MAD_DIMM_SIZE_UNIT_MB    256

static struct mc_config g_config;
static bool             g_config_cached;

static bool cpu_is_coffee_lake_family(void)
{
    // Intel family 6. Coffee Lake = display model 0x9E, Kaby Lake mobile
    // = 0x8E (same IMC). CFL-H uses 0x9E. Display model is
    // (extendedModel << 4) | model.
    if (cpuid_info.vendor_id.str[0] != 'G') return false;   // "GenuineIntel"
    if (cpuid_info.version.family != 6) return false;
    unsigned model = cpuid_info.version.model
                   | (cpuid_info.version.extendedModel << 4);
    return model == 0x8E || model == 0x9E;
}

static uint64_t read_mchbar_base(void)
{
    uint32_t lo = pci_config_read32(0, 0, 0, MCHBAR_LO_REG);
    if (!(lo & MCHBAR_ENABLE)) {
        pci_config_write32(0, 0, 0, MCHBAR_LO_REG, lo | MCHBAR_ENABLE);
        lo = pci_config_read32(0, 0, 0, MCHBAR_LO_REG);
        if (!(lo & MCHBAR_ENABLE)) return 0;
    }
    uint32_t hi = pci_config_read32(0, 0, 0, MCHBAR_HI_REG);
    return (((uint64_t)hi << 32) | lo) & MCHBAR_MASK;
}

static uint32_t mch_read32(uintptr_t mch, uint32_t off)
{
    return *(volatile uint32_t *)(mch + off);
}

static void decode_mad_dimm(uint32_t raw, struct mc_channel *ch)
{
    ch->mad_dimm_raw = raw;
    uint32_t size_a = (raw >> MAD_DIMM_A_SIZE_SHIFT) & MAD_DIMM_A_SIZE_MASK;
    uint32_t size_b = (raw >> MAD_DIMM_B_SIZE_SHIFT) & MAD_DIMM_B_SIZE_MASK;

    ch->dimm[0].populated = size_a > 0;
    ch->dimm[0].size_mb   = size_a * MAD_DIMM_SIZE_UNIT_MB;
    ch->dimm[0].ranks     = (raw & MAD_DIMM_A_RANKS_BIT) ? 2 : 1;
    ch->dimm[0].width     = (raw & MAD_DIMM_A_WIDTH_BIT) ? 16 : 8;

    ch->dimm[1].populated = size_b > 0;
    ch->dimm[1].size_mb   = size_b * MAD_DIMM_SIZE_UNIT_MB;
    ch->dimm[1].ranks     = (raw & MAD_DIMM_B_RANKS_BIT) ? 2 : 1;
    ch->dimm[1].width     = (raw & MAD_DIMM_B_WIDTH_BIT) ? 16 : 8;

    ch->populated = ch->dimm[0].populated || ch->dimm[1].populated;
}

void cfl_mc_config_refresh(void)
{
    g_config_cached = false;
    (void)cfl_mc_config();
}

const struct mc_config *cfl_mc_config(void)
{
    if (g_config_cached) return g_config.valid ? &g_config : NULL;
    g_config_cached = true;
    g_config.valid  = false;

    if (!cpu_is_coffee_lake_family()) return NULL;

    uint64_t base = read_mchbar_base();
    if (!base) return NULL;

    uintptr_t mch = map_region(base, MCHBAR_WINDOW, false);
    if (!mch) return NULL;

    g_config.mchbar_base = base;
    g_config.mad_chnl      = mch_read32(mch, MCHBAR_MAD_CHNL);
    g_config.mad_intra_ch0 = mch_read32(mch, MCHBAR_MAD_INTRA_CH0);
    g_config.mad_intra_ch1 = mch_read32(mch, MCHBAR_MAD_INTRA_CH1);
    g_config.chan_hash     = mch_read32(mch, MCHBAR_CHANNEL_HASH);
    g_config.chan_ehash    = mch_read32(mch, MCHBAR_CHANNEL_EHASH);

    uint32_t raw0 = mch_read32(mch, MCHBAR_MAD_DIMM_CH0);
    uint32_t raw1 = mch_read32(mch, MCHBAR_MAD_DIMM_CH1);
    decode_mad_dimm(raw0, &g_config.channel[0]);
    decode_mad_dimm(raw1, &g_config.channel[1]);

    // Population count + chip-width vote
    g_config.channels_populated = 0;
    unsigned votes_x8 = 0, votes_x16 = 0;
    for (int c = 0; c < 2; c++) {
        if (g_config.channel[c].populated) g_config.channels_populated++;
        for (int d = 0; d < 2; d++) {
            const struct mc_dimm *dd = &g_config.channel[c].dimm[d];
            if (!dd->populated) continue;
            if (dd->width == 8)  votes_x8++;
            if (dd->width == 16) votes_x16++;
        }
    }
    g_config.chip_width = (votes_x16 > votes_x8) ? 16 : 8;

    g_config.valid = g_config.channels_populated > 0;
    return g_config.valid ? &g_config : NULL;
}

// ---------------- PA decode ----------------
//
// Current implementation: channel only (best signal, cheapest win).
// Rank/bank/row/col stubbed out pending A1990 calibration.
//
// Channel selection modes on client IMC:
//   1. Symmetric + hash disabled:  PA[6] selects channel.
//   2. Symmetric + hash enabled:   ch = popcount(PA & HASH_MASK) & 1.
//   3. Asymmetric / ECM:           per-channel PA ranges (MAD_CHNL regs).
// Only (1) and (2) are implemented here. ECM left as TODO.

static uint8_t decode_channel(uint64_t pa, const struct mc_config *mc)
{
    // Only 1 channel populated -> trivial.
    if (mc->channels_populated == 1) {
        return mc->channel[0].populated ? 0 : 1;
    }

    // Hash enable bit: Haswell CHANNEL_HASH bit 0. Validate on A1990.
    bool hash_enabled = (mc->chan_hash & 0x1);
    if (!hash_enabled) {
        return (uint8_t)((pa >> 6) & 0x1);   // cache-line interleave
    }

    // Hash mask in Haswell bits [19:6]; apply as XOR fold.
    uint64_t mask = (mc->chan_hash >> 6) & 0x3FFF;
    uint64_t bits = (pa >> 6) & mask;
    uint8_t parity = 0;
    while (bits) { parity ^= (bits & 1); bits >>= 1; }
    return parity;
}

struct pa_decoded cfl_decode_pa(uint64_t pa)
{
    struct pa_decoded d = { 0 };
    const struct mc_config *mc = cfl_mc_config();
    if (!mc) return d;

    d.channel = decode_channel(pa, mc);
    d.valid   = true;   // channel decode only — rank/bank/row/col unknown

    // TODO: rank decode (MAD_DIMM rank-interleave bit + low PA bit).
    // TODO: bank / row / col decode (requires MAD_INTRA understanding).

    return d;
}

// --------- calibration helper ---------
// Full 0x5000..0x5100 MCHBAR dump. Uses scroll area so lines accumulate.
extern int scroll_message_row;
extern void scroll(void);
#include "display.h"

void cfl_dump_mchbar(void)
{
    const struct mc_config *mc = cfl_mc_config();
    if (!mc) return;

    uintptr_t mch = map_region(mc->mchbar_base, MCHBAR_WINDOW, false);
    if (!mch) return;

    for (uint32_t off = 0x5000; off < 0x5100; off += 16) {
        display_scrolled_message(0, "%04x: %08x %08x %08x %08x", off,
            mch_read32(mch, off + 0),
            mch_read32(mch, off + 4),
            mch_read32(mch, off + 8),
            mch_read32(mch, off + 12));
        scroll();
    }
}
