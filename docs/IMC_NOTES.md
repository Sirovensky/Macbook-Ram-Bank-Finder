# Coffee Lake-H IMC register notes

Target: Intel Coffee Lake-H client CPUs (i7-8750H/8850H/9750H, i9-8950HK/9880H/9980HK).
Used in MacBook Pro A1990. Same IMC family as Skylake/Kaby Lake client.

## MCHBAR location

- PCI 0:0:0 (host bridge / System Agent), offset 0x48 = MCHBAR low dword,
  0x4C = high dword. 64-bit BAR.
- Bit 0 of low dword is enable. Set it if clear.
- Base = `((high << 32) | (low & ~0x7FFF)) & 0x7F_FFFF_8000`.
- Window size 32 KiB.

Source: memtest86plus `system/imc/x86/intel_skl.c:17-47` (identical pattern
applies to Coffee Lake).

## Known MCHBAR offsets (Skylake client family = Coffee Lake)

| Register           | Offset  | Notes                                    |
|--------------------|---------|------------------------------------------|
| TIMINGS            | 0x4000  | tRP/tRCD in [5:0], tRAS in [14:8]        |
| SCHEDULER_CONF     | 0x401C  | [1:0] DDR type (0=DDR4, 1=DDR3)          |
| TIMING_CAS         | 0x4070  | [20:16] tCL                              |
| MAD_DIMM_CH0       | 0x500C  | per-channel DIMM config                  |
| MAD_DIMM_CH1       | 0x5010  | per-channel DIMM config                  |
| DRAM_CLOCK         | 0x5E00  | [3:0] ratio (ratio * 133.34 MHz)         |

Channel offset used by memtest86plus: `0x0400` added when CH0 unpopulated
(ie skip to second channel block for timing reads).

## MAD_DIMM_CHx bit layout (Haswell reference — NEEDS A1990 VALIDATION)

From coreboot `src/northbridge/intel/sandybridge/raminit_common.c`
function `dram_dimm_mapping()` (Sandy/Ivy/Haswell). Layout for Skylake+
shifted but field semantics preserved. Bits below are Haswell; validate
by dumping on A1990.

| Field            | Bits    | Meaning                              |
|------------------|---------|--------------------------------------|
| DIMM_A_SIZE      | [7:0]   | size in 256 MB units                 |
| DIMM_B_SIZE      | [15:8]  | size in 256 MB units                 |
| DIMM_A_SEL       | [16]    | 0 = DIMM A larger, 1 = B larger      |
| DIMM_A_RANKS     | [17]    | 0 = 1 rank, 1 = 2 ranks              |
| DIMM_B_RANKS     | [18]    | 0 = 1 rank, 1 = 2 ranks              |
| DIMM_A_WIDTH     | [19]    | 0 = x8, 1 = x16                      |
| DIMM_B_WIDTH     | [20]    | 0 = x8, 1 = x16                      |
| RANK_INTERLEAVE  | [21]    |                                      |
| ENH_INTERLEAVE   | [22]    |                                      |

For Skylake/Coffee Lake the DDR4 field names become `Dimm_L_Size`,
`Dimm_S_Size`, `DLW`/`DSW` (Larger/Smaller width). Exact bit positions
require Intel doc 337345 vol 2.

## Physical address -> DRAM coordinates

**No public open-source decoder exists for Coffee Lake client.** This is
the main research risk.

What we know:

1. Channel hash: register at MCHBAR+0x5024 (Haswell CHANNEL_HASH,
   offset may differ on SKL+, verify by dump). Contains:
   - HASH_MASK[19:6]: XOR mask over upper PA bits.
   - HASH_LSB_MASK_BIT: which low PA bit is replaced.
   - Enable bit.
   Algorithm: `ch = popcount(PA & HASH_MASK) & 1`.

2. Rank interleave: MAD_DIMM bit 21. When set, a low PA bit (usually
   PA[6] = cache-line) selects between ranks of same channel.

3. Bank/row/col: derived from remaining address bits after channel/rank
   strip. DDR4 x8: 10 col bits, 2 bank-group bits, 2 bank bits, rest row.

4. Enhanced channel mode (ECM): in symmetric mode a low PA bit picks
   channel; in asymmetric each channel gets its own PA range.

Reference files to mine:

- `coreboot/src/northbridge/intel/haswell/registers/mchbar.h`
- `coreboot/src/northbridge/intel/sandybridge/raminit_common.c`
- `memtest86plus/system/imc/x86/intel_skl.c`
- Intel doc 337345 vol 2 (Coffee Lake datasheet, vendor PDF).

EDAC in Linux kernel is NOT useful for client — `sb_edac`/`skx_edac` are
Xeon-SP (server), `pnd2_edac` is Atom. No CFL client EDAC exists upstream.

## Calibration plan for A1990

Boot our USB image with `--dump-imc` mode. Output:

```
MCHBAR base: 0xfe600000
MAD_DIMM_CH0 (0x500C): 0x????????
MAD_DIMM_CH1 (0x5010): 0x????????
MAD_CHNL     (0x5000): 0x????????
CH_HASH?     (0x5024): 0x????????
[ ...0x5000..0x5100 full dump...]
SMBIOS board-id: Mac-937A206F2EE63C01
SPD CH0 DIMM0: [256 bytes or "not present"]
```

Correlate these with A1990's known config (16 GB 2x8 or 32 GB 4x8 per
channel) to lock down exact bit fields.
