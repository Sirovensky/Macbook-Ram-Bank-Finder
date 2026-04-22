// SPDX-License-Identifier: GPL-2.0
//
// Minimal Coffee Lake IMC PA-decode for the EFI mask-shim.
// See cfl_decode_shim.h for interface notes.
//
// Logic is a faithful port of src/cfl_decode.c with the memtest86plus
// host-build dependencies removed.  See that file for register-layout
// comments and calibration notes.

#include "cfl_decode_shim.h"

// ---------------------------------------------------------------------------
// x86 port I/O (inline asm; only linked into x86_64 EFI builds)
// ---------------------------------------------------------------------------

static inline void outl(uint16_t port, uint32_t val)
{
    __asm__ volatile ("outl %0, %w1" : : "a"(val), "Nd"(port) : "memory");
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t val;
    __asm__ volatile ("inl %w1, %0" : "=a"(val) : "Nd"(port) : "memory");
    return val;
}

// PCI config space read via CF8/CFC.
static uint32_t shim_pci_read32(uint8_t bus, uint8_t dev, uint8_t fn,
                                 uint8_t reg)
{
    uint32_t addr = 0x80000000u
                  | ((uint32_t)bus << 16)
                  | ((uint32_t)dev << 11)
                  | ((uint32_t)fn  <<  8)
                  | (reg & 0xfc);
    outl(0xCF8, addr);
    return inl(0xCFC);
}

// ---------------------------------------------------------------------------
// MCHBAR constants (same as src/cfl_decode.c)
// ---------------------------------------------------------------------------

#define MCHBAR_LO_REG           0x48
#define MCHBAR_HI_REG           0x4C
#define MCHBAR_ENABLE           0x1
#define MCHBAR_MASK             0x7FFFFF8000ULL
#define MCHBAR_WINDOW           (1ULL << 15)

#define MCHBAR_MAD_CHNL         0x5000
#define MCHBAR_MAD_INTRA_CH0    0x5004
#define MCHBAR_MAD_INTRA_CH1    0x5008
#define MCHBAR_MAD_DIMM_CH0     0x500C
#define MCHBAR_MAD_DIMM_CH1     0x5010
#define MCHBAR_CHANNEL_HASH     0x5024
#define MCHBAR_CHANNEL_EHASH    0x5028

// MAD_DIMM bit layout (same as src/cfl_decode.c)
#define MAD_DIMM_A_SIZE_SHIFT   0
#define MAD_DIMM_A_SIZE_MASK    0x3F
#define MAD_DIMM_B_SIZE_SHIFT   16
#define MAD_DIMM_B_SIZE_MASK    0x3F
#define MAD_DIMM_A_RANKS_SHIFT  8
#define MAD_DIMM_A_RANKS_MASK   0x3
#define MAD_DIMM_B_RANKS_SHIFT  12
#define MAD_DIMM_B_RANKS_MASK   0x3
#define MAD_DIMM_A_WIDTH_BIT    (1u << 10)
#define MAD_DIMM_B_WIDTH_BIT    (1u << 11)
#define MAD_DIMM_SIZE_UNIT_MB   1024u

#define MAD_INTRA_RANK_IL_EN_BIT    (1u << 8)
#define MAD_INTRA_RANK_BIT_MASK     0x1Fu
#define MAD_INTRA_RANK_BIT_MIN      6u
#define MAD_INTRA_RANK_BIT_MAX      40u

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

struct shim_dimm {
    bool     populated;
    uint8_t  ranks;       // 1 or 2
    uint8_t  width;       // 8 or 16 — per MAD_DIMM width bit
    uint32_t size_mb;
};

struct shim_channel {
    bool            populated;
    struct shim_dimm dimm[2];
};

struct shim_mc {
    bool     valid;
    uint64_t mchbar_base;
    uint8_t  channels_populated;
    uint32_t mad_chnl;
    uint32_t chan_hash;
    uint32_t mad_intra_ch0;
    uint32_t mad_intra_ch1;
    struct shim_channel channel[2];
};

static struct shim_mc  g_mc;
static bool            g_mc_init;

// ---------------------------------------------------------------------------
// MMIO helper — physical address access (identity-mapped in EFI)
// ---------------------------------------------------------------------------

static inline uint32_t mmio_read32(uint64_t phys)
{
    return *(volatile uint32_t *)(uintptr_t)phys;
}

// ---------------------------------------------------------------------------
// CPUID family/model check (Coffee Lake = family 6, model 0x8E or 0x9E)
// ---------------------------------------------------------------------------

static bool cpu_is_cfl(void)
{
    uint32_t eax = 0;
    __asm__ volatile ("cpuid" : "=a"(eax) : "0"(1u) : "ebx", "ecx", "edx");
    uint32_t family   = (eax >> 8) & 0xf;
    uint32_t model    = ((eax >> 4) & 0xf) | (((eax >> 16) & 0xf) << 4);
    return family == 6 && (model == 0x8E || model == 0x9E);
}

// ---------------------------------------------------------------------------
// Initialise — read MCHBAR registers once
// ---------------------------------------------------------------------------

static void decode_mad_dimm(uint32_t raw, struct shim_channel *ch)
{
    uint32_t size_a = (raw >> MAD_DIMM_A_SIZE_SHIFT) & MAD_DIMM_A_SIZE_MASK;
    uint32_t size_b = (raw >> MAD_DIMM_B_SIZE_SHIFT) & MAD_DIMM_B_SIZE_MASK;
    uint32_t rnk_a  = (raw >> MAD_DIMM_A_RANKS_SHIFT) & MAD_DIMM_A_RANKS_MASK;
    uint32_t rnk_b  = (raw >> MAD_DIMM_B_RANKS_SHIFT) & MAD_DIMM_B_RANKS_MASK;

    ch->dimm[0].populated = size_a > 0;
    ch->dimm[0].size_mb   = size_a * MAD_DIMM_SIZE_UNIT_MB;
    ch->dimm[0].ranks     = (uint8_t)(1u << rnk_a);
    ch->dimm[0].width     = (raw & MAD_DIMM_A_WIDTH_BIT) ? 16 : 8;

    ch->dimm[1].populated = size_b > 0;
    ch->dimm[1].size_mb   = size_b * MAD_DIMM_SIZE_UNIT_MB;
    ch->dimm[1].ranks     = (uint8_t)(1u << rnk_b);
    ch->dimm[1].width     = (raw & MAD_DIMM_B_WIDTH_BIT) ? 16 : 8;

    ch->populated = ch->dimm[0].populated || ch->dimm[1].populated;
}

bool shim_cfl_init(void)
{
    if (g_mc_init) return g_mc.valid;
    g_mc_init = true;
    g_mc.valid = false;

    if (!cpu_is_cfl()) return false;

    // Read MCHBAR from host bridge PCI config (bus 0, dev 0, fn 0).
    uint32_t lo = shim_pci_read32(0, 0, 0, MCHBAR_LO_REG);
    if (!(lo & MCHBAR_ENABLE)) {
        // Try to enable.
        // In EFI pre-ExitBootServices the firmware has already set this;
        // if it's off we can't write PCI config safely — just bail.
        return false;
    }
    uint32_t hi = shim_pci_read32(0, 0, 0, MCHBAR_HI_REG);
    uint64_t base = (((uint64_t)hi << 32) | lo) & MCHBAR_MASK;
    if (!base) return false;

    g_mc.mchbar_base   = base;
    g_mc.mad_chnl      = mmio_read32(base + MCHBAR_MAD_CHNL);
    g_mc.mad_intra_ch0 = mmio_read32(base + MCHBAR_MAD_INTRA_CH0);
    g_mc.mad_intra_ch1 = mmio_read32(base + MCHBAR_MAD_INTRA_CH1);
    g_mc.chan_hash     = mmio_read32(base + MCHBAR_CHANNEL_HASH);

    uint32_t raw0 = mmio_read32(base + MCHBAR_MAD_DIMM_CH0);
    uint32_t raw1 = mmio_read32(base + MCHBAR_MAD_DIMM_CH1);
    decode_mad_dimm(raw0, &g_mc.channel[0]);
    decode_mad_dimm(raw1, &g_mc.channel[1]);

    g_mc.channels_populated = 0;
    for (int c = 0; c < 2; c++)
        if (g_mc.channel[c].populated) g_mc.channels_populated++;

    g_mc.valid = g_mc.channels_populated > 0;
    return g_mc.valid;
}

// ---------------------------------------------------------------------------
// PA decode — mirrors src/cfl_decode.c logic faithfully
// ---------------------------------------------------------------------------

static uint8_t decode_channel(uint64_t pa)
{
    if (g_mc.channels_populated == 1)
        return g_mc.channel[0].populated ? 0 : 1;

    bool hash_enabled = (g_mc.chan_hash & 0x1);
    if (!hash_enabled)
        return (uint8_t)((pa >> 6) & 0x1);

    uint64_t mask = (g_mc.chan_hash >> 6) & 0x3FFF;
    uint64_t bits = (pa >> 6) & mask;
    uint8_t parity = 0;
    while (bits) { parity ^= (bits & 1); bits >>= 1; }
    return parity;
}

static uint8_t channel_max_ranks(const struct shim_channel *ch)
{
    uint8_t m = 0;
    for (int d = 0; d < 2; d++) {
        if (!ch->dimm[d].populated) continue;
        if (ch->dimm[d].ranks > m) m = ch->dimm[d].ranks;
    }
    return m;
}

static uint8_t decode_rank(uint64_t pa, uint8_t ch_idx, bool *out_valid)
{
    *out_valid = false;
    const struct shim_channel *ch = &g_mc.channel[ch_idx];
    uint8_t max_ranks = channel_max_ranks(ch);

    if (max_ranks <= 1) {
        *out_valid = true;
        return 0;
    }

    uint32_t intra = (ch_idx == 0) ? g_mc.mad_intra_ch0 : g_mc.mad_intra_ch1;
    if (!(intra & MAD_INTRA_RANK_IL_EN_BIT)) return 0;

    unsigned rank_bit = intra & MAD_INTRA_RANK_BIT_MASK;
    if (rank_bit < MAD_INTRA_RANK_BIT_MIN ||
        rank_bit > MAD_INTRA_RANK_BIT_MAX) return 0;

    *out_valid = true;
    return (uint8_t)((pa >> rank_bit) & 0x1);
}

// ---------------------------------------------------------------------------
// Bank-group / bank / row / col decode — DRAMDig Skylake XOR prior.
// Mirrors cfl_skl_kbl.c logic for the EFI shim context.
//
// Reference: DRAMDig Table 2 (Skylake client, dual-channel DDR4 x16):
//   BG0 = PA[7]  ^ PA[14]
//   BG1 = PA[8]  ^ PA[9]  ^ PA[12] ^ PA[13] ^ PA[18] ^ PA[19]
//   BA0 = PA[15] ^ PA[18]
//   BA1 = PA[16] ^ PA[19]
//   ROW = PA[18 .. 18+row_bits-1]
// ---------------------------------------------------------------------------

static inline unsigned shim_bit(uint64_t pa, unsigned n)
{
    return (unsigned)((pa >> n) & 1u);
}

// Derive row address bit-width from DIMM geometry — match cfl_skl_row_bits().
// Width read from real MAD_DIMM bits (fixes audit C3: shim and memtest must
// use identical formulae or rows recorded vs enumerated diverge).
static unsigned shim_row_bits(void)
{
    if (!g_mc.valid) return 0;

    // Find first populated DIMM — all channels use same geometry on
    // memory-down boards; SODIMM setups read first populated.
    unsigned size_mb = 0;
    unsigned ranks   = 1;
    unsigned width   = 8;   // real x8/x16 from MAD_DIMM

    for (int c = 0; c < 2 && size_mb == 0; c++) {
        if (!g_mc.channel[c].populated) continue;
        for (int d = 0; d < 2 && size_mb == 0; d++) {
            if (!g_mc.channel[c].dimm[d].populated) continue;
            size_mb = g_mc.channel[c].dimm[d].size_mb;
            ranks   = g_mc.channel[c].dimm[d].ranks;
            width   = g_mc.channel[c].dimm[d].width;
        }
    }
    if (size_mb == 0 || ranks == 0 || width == 0) return 0;

    // chips_per_rank = channel_width / chip_width = 64 / width.
    unsigned chips_per_rank = (width == 0) ? 8 : (64u / width);
    if (chips_per_rank == 0) return 0;

    // bytes per chip row = 1024 cols * (width / 8) bytes.
    uint64_t bytes_per_row = 1024ULL * ((uint64_t)width / 8ULL);
    if (bytes_per_row == 0) return 0;

    uint64_t dimm_bytes    = (uint64_t)size_mb * 1024ULL * 1024ULL;
    uint64_t chip_bytes    = dimm_bytes / ((uint64_t)chips_per_rank * ranks);
    uint64_t rows_per_bank = chip_bytes / bytes_per_row / 16ULL;  // 16 banks
    if (rows_per_bank == 0) return 0;

    unsigned bits = 0;
    uint64_t v = rows_per_bank;
    while (v > 1) { v >>= 1; bits++; }
    return bits;
}

static void shim_decode_bank_row(uint64_t pa, struct shim_pa_decoded *d)
{
    d->bank_group = (uint8_t)(
        (shim_bit(pa,  7) ^ shim_bit(pa, 14)) |
        ((shim_bit(pa, 8) ^ shim_bit(pa, 9) ^ shim_bit(pa, 12) ^
          shim_bit(pa, 13) ^ shim_bit(pa, 18) ^ shim_bit(pa, 19)) << 1)
    );

    d->bank = (uint8_t)(
        (shim_bit(pa, 15) ^ shim_bit(pa, 18)) |
        ((shim_bit(pa, 16) ^ shim_bit(pa, 19)) << 1)
    );

    unsigned rb = shim_row_bits();
    if (rb > 0 && rb <= 20) {
        uint64_t row_mask = (1ULL << rb) - 1;
        d->row = (uint32_t)((pa >> 18) & row_mask);
    } else {
        // Fallback: use 17-bit row (conservative for 16 Gbit x16 chips).
        d->row = (uint32_t)((pa >> 18) & 0x1FFFFU);
    }

    d->bank_row_valid = true;
}

struct shim_pa_decoded shim_cfl_decode_pa(uint64_t pa)
{
    struct shim_pa_decoded d = { false, 0, 0, false, 0, 0, 0, false };
    if (!g_mc.valid) return d;

    d.channel    = decode_channel(pa);
    d.rank       = decode_rank(pa, d.channel, &d.rank_valid);
    d.valid      = true;

    shim_decode_bank_row(pa, &d);
    return d;
}

// ---------------------------------------------------------------------------
// Row enumeration — walk PA space and filter by (ch, rank, bg, bank, row).
// ---------------------------------------------------------------------------

unsigned shim_cfl_enumerate_row(uint8_t ch, uint8_t rank,
                                 uint8_t bg, uint8_t bank, uint32_t row,
                                 uint64_t *out, unsigned cap)
{
    if (!g_mc.valid || !out || cap == 0) return 0;

    uint64_t total = shim_cfl_total_memory();
    if (total == 0) return 0;

    unsigned count = 0;
    for (uint64_t pa = 0; pa < total && count < cap; pa += 4096) {
        struct shim_pa_decoded d = shim_cfl_decode_pa(pa);
        if (!d.valid || !d.bank_row_valid) continue;
        if (d.channel != ch)     continue;
        if (d.rank_valid && d.rank != rank) continue;
        if (d.bank_group != bg)  continue;
        if (d.bank != bank)      continue;
        if (d.row  != row)       continue;
        out[count++] = pa;
    }
    return count;
}

// ---------------------------------------------------------------------------
// Total memory in bytes (sum of channel sizes)
// ---------------------------------------------------------------------------

uint64_t shim_cfl_total_memory(void)
{
    if (!g_mc.valid) return 0;
    uint64_t total = 0;
    for (int c = 0; c < 2; c++) {
        if (!g_mc.channel[c].populated) continue;
        for (int d = 0; d < 2; d++) {
            if (!g_mc.channel[c].dimm[d].populated) continue;
            total += (uint64_t)g_mc.channel[c].dimm[d].size_mb * 1024 * 1024;
        }
    }
    return total;
}
