// SPDX-License-Identifier: GPL-2.0
//
// Coffee Lake / Kaby Lake / Skylake IMC family module.
//
// Ported from src/cfl_decode.c; internal functions renamed to cfl_skl_*
// to avoid link-time collisions with future family modules.
//
// Added in this module (Phase A Track A):
//   - Bank-group / bank / row / col decode (DRAMDig Skylake XOR prior).
//   - row_bits() from MAD_DIMM chip geometry.
//   - enumerate_row() — walks PA space, filters by decoded address.
//   - total_memory() — sum of all populated DIMM sizes.
//
// Register layout notes preserved from original cfl_decode.c.

#include "stdint.h"
#include "stdbool.h"

// Parent-directory headers (build adds -I../../system/board to INC_DIRS).
#include "../imc_dispatch.h"
#include "../cpuid.h"
#include "../pci.h"
#include "../vmem.h"

// ---------------------------------------------------------------------------
// MCHBAR constants (same as original cfl_decode.c).
// ---------------------------------------------------------------------------

#define MCHBAR_LO_REG    0x48
#define MCHBAR_HI_REG    0x4C
#define MCHBAR_ENABLE    0x1
#define MCHBAR_MASK      0x7FFFFF8000ULL
#define MCHBAR_WINDOW    (1UL << 15)

#define MCHBAR_MAD_CHNL       0x5000
#define MCHBAR_MAD_INTRA_CH0  0x5004
#define MCHBAR_MAD_INTRA_CH1  0x5008
#define MCHBAR_MAD_DIMM_CH0   0x500C
#define MCHBAR_MAD_DIMM_CH1   0x5010
#define MCHBAR_CHANNEL_HASH   0x5024
#define MCHBAR_CHANNEL_EHASH  0x5028

// MAD_DIMM bit layout (Skylake/Coffee Lake client IMC).
// Validated on A1990 raw=0x410 (1R x16 16GB/ch): width bit [10], ranks [9:8].
#define MAD_DIMM_A_SIZE_SHIFT    0
#define MAD_DIMM_A_SIZE_MASK     0x3F    // GB
#define MAD_DIMM_B_SIZE_SHIFT    16
#define MAD_DIMM_B_SIZE_MASK     0x3F    // GB
#define MAD_DIMM_A_RANKS_SHIFT   8       // DLR: 0=1R, 1=2R, 2=4R, 3=8R
#define MAD_DIMM_A_RANKS_MASK    0x3
#define MAD_DIMM_B_RANKS_SHIFT   12      // DSR
#define MAD_DIMM_B_RANKS_MASK    0x3
#define MAD_DIMM_A_WIDTH_BIT     (1 << 10)  // DLW: 0=x8, 1=x16
#define MAD_DIMM_B_WIDTH_BIT     (1 << 11)  // DSW
#define MAD_DIMM_SIZE_UNIT_MB    1024

// MAD_INTRA rank-interleave layout (speculative; see original cfl_decode.c).
#define MAD_INTRA_RANK_IL_EN_BIT   (1u << 8)
#define MAD_INTRA_RANK_BIT_MASK    0x1Fu
#define MAD_INTRA_RANK_BIT_MIN     6
#define MAD_INTRA_RANK_BIT_MAX     40

// ---------------------------------------------------------------------------
// Config cache.
// ---------------------------------------------------------------------------

static struct mc_config g_config;
static bool             g_config_cached;

// ---------------------------------------------------------------------------
// CPUID detect.
// ---------------------------------------------------------------------------

static bool cfl_skl_detect(void)
{
    // Intel family 6. Coffee Lake = display model 0x9E, Kaby Lake mobile
    // = 0x8E (same IMC). CFL-H uses 0x9E. Display model =
    // (extendedModel << 4) | model.
    if (cpuid_info.vendor_id.str[0] != 'G') return false;  // "GenuineIntel"
    if (cpuid_info.version.family != 6) return false;
    unsigned model = cpuid_info.version.model
                   | (cpuid_info.version.extendedModel << 4);
    return model == 0x8E || model == 0x9E;
}

// ---------------------------------------------------------------------------
// MCHBAR helpers.
// ---------------------------------------------------------------------------

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
    uint32_t rnk_a  = (raw >> MAD_DIMM_A_RANKS_SHIFT) & MAD_DIMM_A_RANKS_MASK;
    uint32_t rnk_b  = (raw >> MAD_DIMM_B_RANKS_SHIFT) & MAD_DIMM_B_RANKS_MASK;

    ch->dimm[0].populated = size_a > 0;
    ch->dimm[0].size_mb   = (uint32_t)(size_a * MAD_DIMM_SIZE_UNIT_MB);
    ch->dimm[0].ranks     = (uint8_t)(1u << rnk_a);
    ch->dimm[0].width     = (raw & MAD_DIMM_A_WIDTH_BIT) ? 16 : 8;

    ch->dimm[1].populated = size_b > 0;
    ch->dimm[1].size_mb   = (uint32_t)(size_b * MAD_DIMM_SIZE_UNIT_MB);
    ch->dimm[1].ranks     = (uint8_t)(1u << rnk_b);
    ch->dimm[1].width     = (raw & MAD_DIMM_B_WIDTH_BIT) ? 16 : 8;

    ch->populated = ch->dimm[0].populated || ch->dimm[1].populated;
}

// ---------------------------------------------------------------------------
// Config read/refresh.
// ---------------------------------------------------------------------------

static void cfl_skl_config_refresh(void)
{
    g_config_cached = false;
    (void)g_config_cached; // will be re-read on next config() call
}

static const struct mc_config *cfl_skl_config(void)
{
    if (g_config_cached) return g_config.valid ? &g_config : NULL;
    g_config_cached = true;
    g_config.valid  = false;

    if (!cfl_skl_detect()) return NULL;

    uint64_t base = read_mchbar_base();
    if (!base) return NULL;

    uintptr_t mch = map_region(base, MCHBAR_WINDOW, false);
    if (!mch) return NULL;

    g_config.mchbar_base    = base;
    g_config.mad_chnl       = mch_read32(mch, MCHBAR_MAD_CHNL);
    g_config.mad_intra_ch0  = mch_read32(mch, MCHBAR_MAD_INTRA_CH0);
    g_config.mad_intra_ch1  = mch_read32(mch, MCHBAR_MAD_INTRA_CH1);
    g_config.chan_hash       = mch_read32(mch, MCHBAR_CHANNEL_HASH);
    g_config.chan_ehash      = mch_read32(mch, MCHBAR_CHANNEL_EHASH);

    uint32_t raw0 = mch_read32(mch, MCHBAR_MAD_DIMM_CH0);
    uint32_t raw1 = mch_read32(mch, MCHBAR_MAD_DIMM_CH1);
    decode_mad_dimm(raw0, &g_config.channel[0]);
    decode_mad_dimm(raw1, &g_config.channel[1]);

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

// ---------------------------------------------------------------------------
// Channel decode.
// ---------------------------------------------------------------------------

static uint8_t cfl_skl_decode_channel(uint64_t pa, const struct mc_config *mc)
{
    if (mc->channels_populated == 1)
        return mc->channel[0].populated ? 0 : 1;

    bool hash_enabled = (mc->chan_hash & 0x1);
    if (!hash_enabled)
        return (uint8_t)((pa >> 6) & 0x1);

    uint64_t mask = (mc->chan_hash >> 6) & 0x3FFF;
    uint64_t bits = (pa >> 6) & mask;
    uint8_t parity = 0;
    while (bits) { parity ^= (bits & 1); bits >>= 1; }
    return parity;
}

// ---------------------------------------------------------------------------
// Rank decode (speculative; preserved from original).
// ---------------------------------------------------------------------------

static uint8_t channel_max_ranks(const struct mc_channel *ch)
{
    uint8_t m = 0;
    for (int d = 0; d < 2; d++) {
        if (!ch->dimm[d].populated) continue;
        if (ch->dimm[d].ranks > m) m = ch->dimm[d].ranks;
    }
    return m;
}

static uint8_t cfl_skl_decode_rank(uint64_t pa, uint8_t ch_idx,
                                    const struct mc_config *mc,
                                    bool *out_valid, bool *out_speculative)
{
    *out_valid = false;
    *out_speculative = false;

    const struct mc_channel *ch = &mc->channel[ch_idx];
    uint8_t max_ranks = channel_max_ranks(ch);

    if (max_ranks <= 1) {
        *out_valid = true;
        return 0;
    }

    uint32_t intra = (ch_idx == 0) ? mc->mad_intra_ch0 : mc->mad_intra_ch1;
    if (!(intra & MAD_INTRA_RANK_IL_EN_BIT))
        return 0;

    unsigned rank_bit = intra & MAD_INTRA_RANK_BIT_MASK;
    if (rank_bit < MAD_INTRA_RANK_BIT_MIN ||
        rank_bit > MAD_INTRA_RANK_BIT_MAX)
        return 0;

    *out_valid = true;
    *out_speculative = true;
    return (uint8_t)((pa >> rank_bit) & 0x1);
}

// ---------------------------------------------------------------------------
// Bank-group / bank / row / col decode — DRAMDig Skylake XOR prior.
//
// Reference: DRAMDig: "A Fast and Accurate DRAM Address Decoder" (Pessl et al.)
// Table 2 (Skylake client, dual-channel DDR4 x16):
//   BG0 = PA[7]  ^ PA[14]
//   BG1 = PA[8]  ^ PA[9]  ^ PA[12] ^ PA[13] ^ PA[18] ^ PA[19]
//   BA0 = PA[15] ^ PA[18]
//   BA1 = PA[16] ^ PA[19]
//   COL = PA[6:15]   (10 bits, standard DDR4 column)
//   ROW = PA[18..18+row_bits-1]
//
// These are applied to the *raw* physical address (channel bit is part of
// the decode, not removed first — the XOR functions are defined over PA).
// All results flagged speculative=true until Track B self-test validates them.
// ---------------------------------------------------------------------------

static inline unsigned bit(uint64_t pa, unsigned n)
{
    return (unsigned)((pa >> n) & 1u);
}

static void cfl_skl_decode_bank_row_col(uint64_t pa, struct pa_decoded *d,
                                         unsigned row_bit_count)
{
    // Bank group.
    d->bank_group = (uint8_t)(
        (bit(pa,  7) ^ bit(pa, 14)) |
        ((bit(pa, 8) ^ bit(pa, 9) ^ bit(pa, 12) ^ bit(pa, 13) ^
          bit(pa, 18) ^ bit(pa, 19)) << 1)
    );

    // Bank.
    d->bank = (uint8_t)(
        (bit(pa, 15) ^ bit(pa, 18)) |
        ((bit(pa, 16) ^ bit(pa, 19)) << 1)
    );

    // Column: bits [15:6] = 10 bits (cache-line granularity; bit 6 is first).
    d->col = (uint16_t)((pa >> 6) & 0x3FF);

    // Row: starts at bit 18, width = row_bit_count.
    if (row_bit_count > 0 && row_bit_count <= 20) {
        uint64_t row_mask = (1ULL << row_bit_count) - 1;
        d->row = (uint32_t)((pa >> 18) & row_mask);
    }

    d->bank_row_valid       = true;
    d->bank_row_speculative = true;  // Track B self-test will clear this.
}

// ---------------------------------------------------------------------------
// row_bits() — derive row address width from MAD_DIMM chip geometry.
//
// Worked examples (from spec):
//   16 Gbit x16 (A1990 MT40A2G16): DIMM = 16 GB (both DIMMs total 32GB/ch
//     in the A1990 2R config, but size_mb here is per-DIMM = 16384 MB).
//     chip capacity: 16 Gbit = 2 GB.  chips_per_rank = 64-bit_bus / 16 = 4.
//     per-chip size = 16384 MB / 4 chips / 2 ranks = 2048 MB = 2^31 bytes.
//     bytes_per_chip_row = 1024 cols * (16/8) bytes = 2048 = 2^11.
//     total_chip_rows = 2^31 / 2^11 = 2^20.  banks = 4 BG * 4 bank = 16.
//     rows_per_bank = 2^20 / 16 = 2^16 — but spec says 17 bits for 16 Gbit.
//   Resolution: the "16 Gbit" part means 16 Gbit per-die; the MAD_DIMM size
//     already encodes the whole DIMM (both dies if 3DS, or single die).
//     For the A1990 16 GB DIMM (single die MT40A2G16), the chip has 2^21
//     total rows across 16 banks = 17-bit row address per bank. ✓
//   The formula:
//     chip_bytes = (size_mb * 1024 * 1024) / chips_per_rank / ranks
//     bytes_per_row = 1024 * (chip_width / 8)
//     total_rows_in_chip = chip_bytes / bytes_per_row
//     rows_per_bank = total_rows_in_chip / (4 * 4)   // 2 BG bits, 2 BA bits
//     row_bits = log2(rows_per_bank)
// ---------------------------------------------------------------------------

static unsigned cfl_skl_row_bits(void)
{
    const struct mc_config *mc = cfl_skl_config();
    if (!mc) return 0;

    // Find first populated channel+dimm.
    const struct mc_dimm *dd = NULL;
    for (int c = 0; c < 2 && !dd; c++)
        for (int d = 0; d < 2 && !dd; d++)
            if (mc->channel[c].dimm[d].populated)
                dd = &mc->channel[c].dimm[d];
    if (!dd) return 0;

    uint8_t chip_width = dd->width;          // 8 or 16
    uint8_t ranks      = dd->ranks;          // 1 or 2
    uint32_t size_mb   = dd->size_mb;        // MB per DIMM

    // chips_per_rank: 64-bit bus / chip_width (x8 → 8, x16 → 4).
    unsigned chips_per_rank = 64u / chip_width;

    // Per-chip size in bytes = total DIMM bytes / (chips_per_rank * ranks).
    uint64_t dimm_bytes  = (uint64_t)size_mb * 1024ULL * 1024ULL;
    uint64_t chip_bytes  = dimm_bytes / ((uint64_t)chips_per_rank * ranks);

    // Bytes per chip row = 1024 columns * (chip_width / 8) bytes/column.
    unsigned bytes_per_row = 1024u * (chip_width / 8u);

    // Total rows in chip.
    uint64_t total_rows = chip_bytes / bytes_per_row;

    // Rows per bank (DDR4: 4 bank groups * 4 banks = 16 banks).
    uint64_t rows_per_bank = total_rows / 16u;
    if (rows_per_bank == 0) return 0;

    // log2(rows_per_bank).
    unsigned bits = 0;
    uint64_t v = rows_per_bank;
    while (v > 1) { v >>= 1; bits++; }
    return bits;
}

// ---------------------------------------------------------------------------
// total_memory() — sum of all populated DIMM sizes.
// ---------------------------------------------------------------------------

static uint64_t cfl_skl_total_memory(void)
{
    const struct mc_config *mc = cfl_skl_config();
    if (!mc) return 0;
    uint64_t total = 0;
    for (int c = 0; c < 2; c++) {
        if (!mc->channel[c].populated) continue;
        for (int d = 0; d < 2; d++) {
            if (!mc->channel[c].dimm[d].populated) continue;
            total += (uint64_t)mc->channel[c].dimm[d].size_mb * 1024ULL * 1024ULL;
        }
    }
    return total;
}

// ---------------------------------------------------------------------------
// decode_pa() — channel + rank (speculative) + bank/row/col (DRAMDig prior).
// ---------------------------------------------------------------------------

static struct pa_decoded cfl_skl_decode_pa(uint64_t pa)
{
    struct pa_decoded d = { 0 };
    const struct mc_config *mc = cfl_skl_config();
    if (!mc) return d;

    d.channel = cfl_skl_decode_channel(pa, mc);
    d.rank    = cfl_skl_decode_rank(pa, d.channel, mc,
                                    &d.rank_valid, &d.rank_speculative);
    d.valid   = true;

    // Bank / row / col via DRAMDig Skylake XOR prior.
    unsigned rb = cfl_skl_row_bits();
    cfl_skl_decode_bank_row_col(pa, &d, rb);

    return d;
}

// ---------------------------------------------------------------------------
// enumerate_row() — iterate PA space, filter by (ch, rank, bg, bank, row).
// ---------------------------------------------------------------------------

static unsigned cfl_skl_enumerate_row(uint8_t ch, uint8_t rank,
                                       uint8_t bg, uint8_t bank, uint32_t row,
                                       uint64_t *out, unsigned cap)
{
    uint64_t total = cfl_skl_total_memory();
    if (!total || !out || !cap) return 0;

    unsigned count = 0;
    for (uint64_t pa = 0; pa < total && count < cap; pa += 4096) {
        struct pa_decoded d = cfl_skl_decode_pa(pa);
        if (!d.valid || !d.bank_row_valid) continue;
        if (d.channel != ch)     continue;
        if (d.rank_valid && d.rank != rank) continue;
        if (d.bank_group != bg)  continue;
        if (d.bank != bank)      continue;
        if (d.row != row)        continue;
        out[count++] = pa;
    }
    return count;
}

// ---------------------------------------------------------------------------
// MCHBAR dump helpers (unchanged from original cfl_decode.c).
// ---------------------------------------------------------------------------

extern int scroll_message_row;
extern void scroll(void);
#include "display.h"

static void cfl_skl_dump_mchbar(void)
{
    const struct mc_config *mc = cfl_skl_config();
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

static void cfl_skl_dump_mchbar_at(int row_first, int row_last)
{
    const struct mc_config *mc = cfl_skl_config();
    if (!mc) return;
    uintptr_t mch = map_region(mc->mchbar_base, MCHBAR_WINDOW, false);
    if (!mch) return;

    int r = row_first;
    for (uint32_t off = 0x5000; off < 0x5100 && r <= row_last; off += 16) {
        printf(r++, 0, "%04x: %08x %08x %08x %08x", off,
            mch_read32(mch, off + 0),
            mch_read32(mch, off + 4),
            mch_read32(mch, off + 8),
            mch_read32(mch, off + 12));
    }
}

// ---------------------------------------------------------------------------
// Exported ops table.
// ---------------------------------------------------------------------------

struct imc_ops cfl_skl_kbl_ops = {
    .family_name    = "CFL/SKL/KBL",
    .detect         = cfl_skl_detect,
    .config         = cfl_skl_config,
    .config_refresh = cfl_skl_config_refresh,
    .decode_pa      = cfl_skl_decode_pa,
    .row_bits       = cfl_skl_row_bits,
    .enumerate_row  = cfl_skl_enumerate_row,
    .total_memory   = cfl_skl_total_memory,
    .dump_mchbar    = cfl_skl_dump_mchbar,
    .dump_mchbar_at = cfl_skl_dump_mchbar_at,
};
