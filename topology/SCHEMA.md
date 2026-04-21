# Board topology YAML schema

Each `topology/<board-id>.yaml` describes one logic board. The generator
`scripts/gen-topology.py` reads all of them and emits `src/board_table.c`.

## Top-level fields

| field               | type         | required | description |
|---------------------|--------------|----------|-------------|
| `board_id`          | string       | yes      | Apple board number or vendor PN, e.g. `"820-01814-a"` |
| `friendly_name`     | string       | yes      | Human name shown on screen |
| `products`          | list\<string\> | yes    | SMBIOS Type 1 product strings. Match any = this board. Up to 4. |
| `chip_width`        | int (8 or 16)| yes      | Dominant DRAM chip bit width |
| `channels`          | int (1 or 2) | yes      | Populated IMC channels |
| `ranks_per_channel` | int (1 or 2) | yes      | Ranks per channel |
| `packages`          | list         | yes      | One entry per BGA DRAM package (up to 16) |
| `verified`          | bool         | no       | Default false. Set true once validated on real hardware. |
| `notes`             | string       | no       | Free text — source schematic rev, caveats |

## `packages[]` entries

| field           | type   | required | description |
|-----------------|--------|----------|-------------|
| `designator`    | string | yes      | Silkscreen, e.g. `"U2300"` |
| `channel`       | int    | yes      | 0 or 1 (matches IMC channel) |
| `rank`          | int    | yes      | 0 or 1 |
| `byte_lane`     | int    | yes      | 0..7. For x16 chips, the *low* lane of the two it covers |
| `chip_width`    | int    | no       | Override top-level (rare mixed configs). Default inherits. |
| `location_hint` | string | no       | e.g. `"top row near CPU"` — shown next to designator |

## Minimum viable board (tier-1 only)

If you don't have schematic access, you can still contribute a product
match with no `packages:` list — the tester will then print channel +
byte lane only. Set `package_count: 0` effectively by omitting the list.

## Verification checklist

Before flipping `verified: true`:

1. Boot the memtest ISO on the target hardware.
2. Confirm calibration dump shows expected MCHBAR values (channels,
   ranks, chip width, DIMM size).
3. Induce a single-bit error (optional, via known-bad chip or stress
   pattern) and confirm the printed U-designator matches the physically
   failing BGA.
