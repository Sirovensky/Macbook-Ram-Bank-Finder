# EFI bad-RAM masking — technical reference

## Context

macOS's memory allocator honours the UEFI memory map. A physical-address
range marked `EfiReservedMemoryType` is treated as firmware-owned and
never allocated by the kernel or userspace. Bad DRAM pages can therefore
be hidden from macOS without kernel patches or Recovery mode.

`mask-shim.efi` calls

```c
AllocatePages(AllocateAddress, EfiReservedMemoryType, pages, &addr)
```

before `ExitBootServices()` for every bad page. The kernel inherits
the updated memory map during the chainload and routes around the
reserved regions permanently for the session.

## Current flow (2026-04)

Three grub entries. User-visible surface:

```
1. Full test  (all patterns, ~30-60 min) [DEFAULT]
2. Fast test  (3 patterns, ~5 min)
3. Setup NVRAM hook + apply mask
```

No state machine. No automatic NVRAM writes. No install/revert.efi in
the boot path. Everything is explicit and user-driven:

1. User boots entry 1 (or 2). Memtest runs, prints the bad-address list,
   halts.
2. User photographs the address list, powers off.
3. User boots entry 3. `brr-entry.efi`:
   - Offers quick-retry if `BrrBadPages` already exists in NVRAM.
   - Otherwise prompts for comma-separated hex addresses.
   - Writes `BrrBadPages` + `BrrMaskState=TRIAL_PENDING_PAGE` via Boot
     Services `SetVariable` (the only persistence path that survives
     reboot on T2).
   - Chainloads `mask-shim.efi`.
4. `mask-shim.efi`:
   - Reads `BrrBadPages` + optional `badmem.txt` from its own loaded-
     image device.
   - Expands each address `+/- 1 MiB`, aligns to 4 KiB pages, calls
     `AllocatePages` per page.
   - Reports coverage as `N/M range(s) fully covered` plus
     per-page-type counts (newly reserved / firmware pre-reserved /
     gap). Any gap is unsafe.
   - Finds internal macOS `boot.efi` via multi-tier SFS scan (root,
     1-level UUID dir, 2-level nested, plus Apple alternates like
     `\com.apple.recovery.boot\boot.efi`).
   - `LoadImage + StartImage` the internal `boot.efi`.

If chainload fails, NVRAM stays intact. User can hard-reboot and retry
entry 3 → **Y** quick-retry without re-typing.

## Persistence semantics on T2

| Mechanism | Persists across reboot? |
|---|---|
| Runtime Services `SetVariable` (post-EBS, any GUID) | **NO** |
| Boot Services `SetVariable` (pre-EBS, any GUID) | **YES** |
| SFS file write post-EBS | **NO** (spec-invalid; writes may succeed but commit is dropped) |
| SFS file write pre-EBS | depends on T2 firmware; ESP on USB works, internal ESP blocked by Full Security |
| `AllocatePages(EfiReservedMemoryType)` | only for the booted macOS session; re-applied by shim on each boot |

All BRR persistence is via pre-EBS `SetVariable`, exactly once per
mask setup. The shim re-applies the reservation on every boot.

## NVRAM schema (vendor GUID `3E3E9DB2-1A2B-4B5C-9D1E-5F6A7B8C9D0E`)

| Var | Set by | Read by | Format |
|---|---|---|---|
| `BrrBadPages` | brr-entry.efi | mask-shim.efi | `[u32 version=1][u32 count][u64 PAs...]` |
| `BrrMaskState` | brr-entry.efi / mask-shim.efi | mask-shim.efi | ASCII string: `TRIAL_PENDING_PAGE` etc. |
| `BrrBadChips` | (unused in current flow) | mask-shim.efi | comma-separated ASCII designators |
| `BrrBadRows` | (unused; row-mode experimental) | mask-shim.efi | header + 8-byte tuples |
| `BrrDecoderStatus` | decoder_selftest.c (not triggered in current flow) | mask-shim.efi | `VALIDATED` / `FAILED` |

Values used in the current flow: `BrrBadPages`, `BrrMaskState`.

## Mask-shim state machine

State values recognised by `mask-shim.efi` (in `BrrMaskState`):

| State | shim action | How to reach it |
|---|---|---|
| absent | proceed with any NVRAM data | first ever run |
| `TRIAL_PENDING_PAGE` | apply page mask, chainload macOS | brr-entry.efi writes this on user Y |
| `TRIAL_PENDING_CHIP` | apply chip mask (force_chip=1), chainload | unused in current flow |
| `TRIAL_BOOTED` | apply existing mask, chainload (fallback) | **not set by current code** — legacy artifact of a removed install flow; may be present from older ISOs |
| `PERMANENT_UNCONFIRMED` | prompt "Did macOS boot OK?" Y/N | install.efi (not in current grub flow) |
| `PERMANENT_CONFIRMED` | silent mask + boot | user pressed Y after install |

State is **not** advanced by mask-shim on success — transitions happen
only through deliberate user actions (brr-entry write, install.efi,
PERMANENT_UNCONFIRMED prompt Y/N). This way a shim crash or boot.efi
miss leaves NVRAM recoverable.

## Known-unused subsystems

The current flow uses page-mode masking exclusively. The following are
compiled but not triggered:

- **Chip-mode masking** (`mask_chips` + `resolve_chip_entries` in
  `efi/mask-shim/main.c`). Only activated by `TRIAL_PENDING_CHIP` or
  `BrrDecoderStatus=FAILED`. Would mask a full 16 GiB channel per bad
  chip on A1990.
- **Row-mode masking** (`mask_rows` + `badmem_row_t`). Only activated
  by `BrrDecoderStatus=VALIDATED` + populated `BrrBadRows`. Requires
  the decoder self-test (`src/decoder_selftest.c`) to run first; not
  wired into the current boot path.
- `install.efi` + `revert.efi` — built and staged in the ISO ESP but
  not invoked by any grub entry. Scaffolding for the future permanent-
  install milestone.

## Historical documents

`docs/_internal/MASKING_RECOVERY.md`, `SEAMLESS_FLOW.md`, and
`PLAN_MASK_AND_KBD.md` describe the older 5-state install/trial/revert
flow. They are retained for design history — the NVRAM state names and
the mask-apply primitives still come from that design — but the
automated state transitions described in those docs are NOT present
in the current codebase.

## See also

- `README.md` — user-facing flow.
- `docs/_internal/KEYBOARD_T2.md` — why ConIn is the only input path.
- `docs/_internal/IMC_NOTES.md` — Coffee Lake IMC register decoding.
- `docs/_internal/BADMEM_FORMAT.md` — `badmem.txt` format (shim fallback).
