# TODO

Living list. PRs welcome. Tick when landed; move to CHANGELOG if kept.

## P0 — correctness blockers

- [x] **Byte-lane → U-designator mapping verified.** Traced from
  `vendor/820-01814-schematic.pdf` via `scripts/trace-dq.py`. All 16
  chips confirmed: 8 per channel, each an x8 chip owning exactly one
  byte lane. `topology/820-01814-a.yaml` flipped to `verified: true`.
- [x] **A1990 is 1R per channel, not 2R.** Earlier assumption was wrong.
  32 GB = 2 channels × 1 rank × 8 × 2 GB (Micron MT40A2G8-NRE). CS#
  routing confirms single chip-select per channel. Consequence: rank
  decode is **always** certain on A1990 (rank=0). MAD_INTRA speculation
  only matters if/when we target a different 2R CFL/KBL board.
- [x] **Rank decode (speculative, for 2R boards)** — `decode_rank()` in
  `cfl_decode.c` parses `MAD_INTRA_CHx` as
  `bit[8]=rank-interleave-enable`, `bits[4:0]=PA-bit-index`. 1R channels
  short-circuit to rank=0 (certain). Speculative path marks output with
  `~`. Not exercised on A1990; kept as the placeholder for future 2R
  boards.
- [ ] **Confirm MAD_INTRA layout on a real 2R board.** Needed before we
  trust speculative rank decode. Options:
    - dump from any CFL/KBL laptop with 2R SODIMM (Dell XPS 15 9570,
      etc.) — different MAD_INTRA value would confirm or falsify the
      bit[8]+[4:0] interpretation.
    - fault injection on a known-bad 2R chip where failing rank is
      physically identified.
- [ ] **Rank-count decode bit location** — on A1990 the IMC-reported
  rank count happens to come out correct (bits[9:8]=0 → 1R) but bit[10]
  is still set (reads as `x16 width` under current decode). Since A1990
  chips are actually x8, bit[10] is **not** a width flag. Needs a
  2R-x8 SODIMM data point to relabel. Tier-1 (no-overlay) output
  currently misreports chip width on A1990.

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

- [x] **External keyboard fallback** — confirmed dead on T2 (both internal
  and USB-C external fail in memtest's xHCI stack). Replaced with
  ConIn-based pre-ExitBootServices menu (`src/efi_menu.c`). See
  `docs/KEYBOARD_T2.md`.

- [x] **Chip-level mask policy** — per-chip masking implemented.
  `# chip: UXXXX` directives in `badmem.txt` cause the shim to walk
  all PAs and reserve every page that decodes to the bad chip's
  (channel, rank). On A1990 (1R per channel) this masks one full
  channel — 16 GiB. Region and chip entries may be mixed freely.
  See `docs/MASKING.md` "Granularity" section and `docs/BADMEM_FORMAT.md`.

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
