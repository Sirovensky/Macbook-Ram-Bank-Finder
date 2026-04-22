# Adding a new board

This memtest names the failing BGA package by mapping `(address, xor bits)`
back through the IMC decoder and onto a board-specific chip layout. To
support a new laptop/board, you contribute one YAML file. No C changes.

## Two contribution tiers

**Tier 1 — product match only** (no schematic needed). The tester will
still print channel + byte lane. Users know "CH A lane 3" instead of a
chip designator, which is usually enough to identify a chip visually.

**Tier 2 — full chip map** (schematic / boardview / iFixit teardown
needed). The tester prints the exact U-designator and a location hint.

## Prerequisites

- IMC family must be supported. Currently only Intel Coffee Lake /
  Kaby Lake / Skylake client (family 6, model 0x8E / 0x9E). Other IMCs
  need a decoder added in `src/*_decode.c`.
- SMBIOS Type 1 `productname` of the target machine. On Linux:
  `cat /sys/class/dmi/id/product_name`. On macOS: `sysctl hw.model`.
- For tier 2: schematic or boardview showing which DRAM chip owns which
  `MEM_x_DQ<n>` net. Louis Rossmann's boardview, OpenBoardView, or
  vendor schematics all work.

## Steps

1. Copy `topology/820-01814-a.yaml` as a template.
2. Replace `board_id` with your Apple board number or vendor PN.
3. Set `products:` to the SMBIOS Type 1 string(s). Multiple laptops can
   share a logic board — list them all.
4. Fill in `chip_width`, `channels`, `ranks_per_channel`.
   - Boot the ISO first and read the calibration dump — it prints the
     decoded IMC state, which tells you what to put here.
5. For tier 2: list every BGA DRAM package. For each one:
   - `designator` — silkscreen text, e.g. `U2300`
   - `channel` — 0 or 1 (see calibration dump)
   - `rank` — 0 or 1
   - `byte_lane` — 0..7. For x16 chips, the low lane of the pair.
   - `location_hint` — optional, a few words a repair tech can use.
6. Leave `verified: false` until you've confirmed on real hardware.

## Verifying your map

Short of a reliable single-chip fault, use these checks:

- Calibration output agrees with physical config (right chip count,
  right channel/rank populated, right DIMM sizes).
- Stress-test a known-bad machine and confirm the flagged designator
  is also the one that shows up hot under IR or visibly damaged.
- If you have a donor board: short one DQ line to ground and confirm
  the right lane + chip is reported.

Once verified, flip `verified: true` and open a PR.

## Schema reference

See [`topology/SCHEMA.md`](../topology/SCHEMA.md).

## Build flow

```
make apply      # regenerates src/board_table.c from topology/*.yaml,
                # syncs into memtest86plus/, applies patches
make build      # compile
make iso        # build bootable ISO
```

You don't need to commit `src/board_table.c` — it's generated and
gitignored. Only the YAML is canonical.
