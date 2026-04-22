# Plan

## Objective

Deliver bootable ISO that, on MacBook Pro A1990, reports which specific
soldered BGA DRAM package is faulty when memory errors are detected, rather
than just "RAM is bad somewhere."

## Approach

Fork memtest86plus. Add a Coffee Lake-H physical-address decoder that converts
`(failing PA, failing bits)` into `(channel, rank, byte_lane, BGA_designator)`.
Overlay with an A1990-specific board topology table keyed on SMBIOS board-id.

## Milestones

### M1 — topology research (no hardware needed)
- Confirm A1990 board-id strings (16 GB and 32 GB variants).
- Confirm BGA package count and width per variant.
  - 16 GB: 4 packages x16 (likely).
  - 32 GB: 8 packages x8 OR 4 dual-die x16 (confirm via boardview).
- Map each package U-designator to (channel, rank, byte_lanes).
- Sources: iFixit A1990 teardown, Apple schematic 820-01041, Louis Rossmann
  boardview (A1990 / Big Sur / whichever release covers the T2 15").
- Output: `src/a1990_topology.h` populated with real U-numbers.

### M2 — stock memtest boots on A1990
- Build memtest86plus v7 unchanged.
- Package as hybrid ISO with GRUB2 UEFI.
- Write USB, configure T2 (Startup Security Utility -> No Security +
  allow external boot), boot-test on A1990.
- Confirm: video output, keyboard input (SPI keyboard via UEFI
  SimpleTextInputProtocol), memory detection shows correct size.
- Blocker risks: Apple EFI quirks, keyboard dead (very unlikely at EFI stage).

### M3 — IMC configuration read
- Extend `intel_skl.c` to fetch and expose per-channel per-DIMM:
  width (x8/x16), ranks, density, bank groups.
- Add `cfl_decode.h` that publishes a `struct mc_config` at boot.
- Validate by dumping raw MCHBAR 0x5000–0x5030 and cross-checking
  against known A1990 spec (32 GB = 2ch x 2 ranks x 8 x8 chips or
  2ch x 1 rank x 8 x8 chips, depending on variant).

### M4 — PA -> DRAM coords decoder
- Implement `decode_pa(uint64_t pa) -> {channel, rank, bank, row, col}`
  using MAD_CHNL_HASH + MAD_INTRA registers.
- Hardest step. No open-source reference for Coffee Lake client.
- Strategy:
  1. Start with Haswell/Sandy Bridge decode from coreboot as template.
  2. Add runtime self-validation: allocate 2 pages at known PAs,
     write distinguishing patterns, dump, correlate with decoder
     output. Abort/warn if mismatch.
  3. Fall back to "channel only" if rank/bank decode not confident.
- Still useful at channel granularity: halves suspect set.

### M5 — bit -> chip mapping
- `xor = good ^ bad` gives failing bit positions in a 64-bit (or wider) word.
- Map bit N to byte lane (N/8).
- Byte lane -> BGA package via topology table.
- Multiple lanes failing -> multiple packages listed.

### M6 — error-reporter hook
- Modify `app/error.c:common_err()` to call our decoder.
- Print line: `CHIP U1800 (ch0 rank0 lane3) U1801 (ch0 rank0 lane4)`
  alongside the existing address/pattern line.

### M7 — hybrid ISO build
- `grub-mkrescue` with custom `grub.cfg` that auto-boots memtest.
- xorriso fallback for environments where grub-mkrescue absent.
- USB-writable via `dd`.
- Output: `a1990-memtest.iso`.

### M8 — real-hardware validation
- Boot on your A1990, capture MCHBAR dump via our calibration tool.
- If any chip actually faulty: confirm correct designator. If all chips
  healthy: cannot validate failure path except via synthetic fault
  injection (see docs/FAULT_INJECTION.md — TBD, optional).

## Out of scope (for now)

- Non-A1990 Macs. Architecture supports adding more boards later via
  topology table.
- Non-Coffee Lake Intel CPUs. Code path is CPUID-gated.
- AMD / Apple Silicon. Completely different MC.
- ECC decode. A1990 is non-ECC.
