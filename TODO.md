# TODO

Living list. PRs welcome. Tick when landed; move to CHANGELOG if kept.

## P0 — correctness blockers

- [ ] **Rank decode** — currently every failing address shows **two** suspect
  chips (one per rank). Need to decode rank from PA so output narrows to one.
  - Verify `MAD_INTRA_CHx` bit layout — A1990 reads `0x00000110` on both
    channels, likely encodes rank-interleave PA-bit index at `[8:4]` or
    `[4:0]`. First attempt: treat as PA-bit index, XOR-fold into rank.
  - Fix rank-count decode: `MAD_DIMM` bit[10]=1 on known 2R config currently
    reads as `x16 width` (wrong — should be `2R` flag, width is `x16`
    everywhere on A1990 so bit is ambiguous). Needs second calibration
    data point from an x8 SODIMM laptop to disambiguate.

- [ ] **Byte-lane → U-designator mapping is tentative** (`topology/820-01814-a.yaml`
  has `verified: false`). Per-lane mapping was guessed from designator
  groupings (U23x0 = lanes 0-1, U23x1 = lanes 2-3, etc.). Confirm by
  tracing DQ nets on the 820-01814 schematic or by single-chip fault
  injection. Flip `verified: true` once confirmed.

- [ ] **Skylake/Coffee Lake MAD_DIMM bit-field layout is reverse-engineered,
  not documented.** coreboot has no SKL+ raminit (all FSP-blob). Current
  decoder fits one A1990 data point. Need at least 2 more calibration
  dumps from different CFL/KBL laptop configs to lock down the layout
  (ideal targets: 1R x8 SODIMM, 2R x8 SODIMM, LPDDR3 system).

## P1 — address decode coverage

- [ ] **Bank / row / column PA decode** — `cfl_decode_pa()` returns only
  channel + rank. To narrow faults further (to specific internal DRAM
  bank) requires understanding `MAD_INTRA_CHx` fully. See
  `src/cfl_decode.c` `TODO` comments.

- [ ] **Asymmetric / ECM channel mode** — `decode_channel()` only handles
  symmetric (PA[6] interleave) and hash modes. ECM with per-channel PA
  ranges from `MAD_CHNL` registers is unimplemented.

- [ ] **Channel hash mask** — current code assumes Haswell bit positions
  for `CHANNEL_HASH`. Validate on SKL/CFL with asymmetric dual-rank
  configs before trusting hash-mode decode.

## P2 — UX / build / distribution

- [ ] **Separate FAT32 data partition for `Log.txt`** — user request: so a
  second Mac can read error logs off the USB stick without reflashing.
  ISO currently single partition (ISO9660+ESP). Add a 3rd partition
  mounted RW with newline-appended log. Needs memtest86plus write
  support for USB block devices (currently read-only).

- [ ] **External keyboard fallback** — A1990 T2 blocks internal USB HID,
  but USB-C external keyboards may enumerate. Detect, and if present
  restore `get_key()` interactive menus instead of forcing timed
  countdowns.

- [ ] **Boardview integration** — show failing chip location visually.
  Parse `.bdv`/`.fz` format (stored locally only, gitignored), render
  ASCII heatmap on calibration screen.

- [ ] **README + build guide** — document Docker-on-macOS build path,
  flash instructions (Etcher/Rufus/dd), and contributor flow for adding
  new boards.

## P3 — more boards

- [ ] **MacBook Pro 13" 2018/2019 (820-01521-a, A1989)** — similar
  Coffee Lake IMC, likely smaller U-designator grid. Needs schematic
  access.

- [ ] **Mac mini 2018 (820-01700)** — CFL desktop, LPDDR3 soldered.

- [ ] **iMac 19,x / 20,x** — CFL desktop with SODIMM slots. Topology
  trivial (bank = slot), fault-injection validation easier.

- [ ] **Generic Dell XPS 15 9570 / 9575 (CFL-H)** — open enough
  schematics to validate MAD_DIMM layout on non-Apple DDR4-2400 config.

## P4 — long-term

- [ ] **Newer platforms (Ice Lake, Tiger Lake, Alder Lake, Raptor Lake,
  Meteor Lake, Pantherlake)** — FSP-blob memory init means MAD_DIMM
  decode has to be re-reverse-engineered per platform. Only worth it if
  community wants to extend.

- [ ] **AMD Zen** — already detected by memtest86plus's own code; wire
  up equivalent PA-decode path for AMD SoCs.

## Known non-issues (intentional)

- Timed (10 s) photo pauses instead of `press any key`: A1990 T2 chip
  proxies USB HID through a vendor-specific protocol bare-metal OSes
  can't use. `get_key()` blocks forever. Keep timed pauses.
- `reference/coreboot/` is gitignored — sparse-checkout for local
  docs only. Don't commit.
- `vendor/` and `schematics/` gitignored — Apple schematics are under
  NDA/copyright. Never commit.
