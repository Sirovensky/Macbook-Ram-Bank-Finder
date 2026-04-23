# badmem.txt format

`badmem.txt` is a plain-text file listing bad DRAM pages on the A1990.
It lives at `/EFI/BOOT/badmem.txt` on the USB/ISO ESP partition (and at
`\EFI\MASK\badmem.txt` on the internal ESP after a permanent install).

Two kinds of entries may appear in any order in the same file:

## Region entries (page-level masking)

```
# comment — ignored
0xSTART,LEN_BYTES
```

- One range per non-comment line.
- `0xSTART` — physical base address in hexadecimal (0x prefix required).
- `LEN_BYTES` — length in bytes, decimal or hex.
- Both START and LEN are aligned to 4096-byte page boundaries by the shim
  (START rounded **down**, LEN rounded **up**) before calling
  `AllocatePages`.
- Maximum 4096 region entries.

### Region example

```
# MacBook Pro A1990 bad pages — detected 2024-01-15
0x200000,4096
0x201000,4096
0x1ff800000,8192
```

## Chip entries (chip-level masking)

Lines of the form:

```
# chip: UXXXX
```

are **chip directives** — not plain comments.  The parser recognises the
keyword `chip:` after `#` and any whitespace (case-insensitive).  The
rest of the line is the chip designator (e.g. `U2410`).

When the mask-shim encounters chip directives:

1. It looks up the designator in the compiled-in board topology table
   (generated from `topology/*.yaml`).
2. It reads the IMC's MCHBAR registers to determine the PA→channel
   mapping.
3. It walks every 4 KiB page from PA 0 to the IMC-reported total memory.
4. Pages that decode to the bad chip's (channel, rank) are reserved via
   `AllocatePages(AllocateAddress, EfiReservedMemoryType, 1, &pa)`.

On the A1990 (1R per channel, 8 chips per channel), masking one chip
effectively reserves every PA on that chip's channel — 16 GiB of the
32 GiB total.

- Maximum 32 chip entries.
- An unknown designator (typo, or board not in topology) is logged and
  skipped; masking continues for all other entries.
- Progress messages are printed every 1 GiB during the PA walk.

### Chip example

```
# chip: U2410       -- reserve all pages mapping to this chip
# chip: U2300
```

## Mixed example

Region and chip entries may be mixed freely:

```
# MacBook Pro A1990 — bad chip U2410 + isolated extra page
# chip: U2410
0x1ff800000,4096    # stray error on a different chip
```

## Notes

- Any other `#`-prefixed line (not matching `# chip:`) is a plain comment
  and is silently skipped.
- Blank lines are silently skipped.
- An empty or missing `badmem.txt` causes the shim to proceed directly to
  `boot.efi` with no reservations (safe default).
- The shim does **not** write to this file.

## How `badmem.txt` is used in the current flow

The 3-entry grub flow (see `README.md`) does **not** require editing
`badmem.txt` by hand.  Users type bad addresses directly into
`brr-entry.efi` (grub entry 3), which writes them to NVRAM via pre-EBS
`SetVariable`.  `mask-shim.efi` reads NVRAM first, then optionally
merges in any `badmem.txt` it finds on the same device it was loaded
from (tries `\EFI\BRR\badmem.txt` then `\EFI\BOOT\badmem.txt`).

So `badmem.txt` is a **secondary** path, useful for:

- **Pinned addresses.** If you ship a stick to another A1990 user, you
  can pre-populate `badmem.txt` on the ESP so the shim masks those
  addresses even if the user skips the brr-entry step.
- **Chip-mode directives.** NVRAM currently stores only pages; chip
  designators (`U2xxx`) must be listed in `badmem.txt` as
  `# chip: U2xxx` lines to trigger chip-mode masking.  Chip-mode is
  coarse (masks one full channel per bad chip = 16 GiB on A1990); use
  only when a chip is failing rapidly across many pages.

### Editing `badmem.txt` after a flash

1. Boot any computer (not the A1990).
2. Mount the USB stick's ESP (the FAT partition).
3. Create or open `\EFI\BOOT\badmem.txt`.  Each line is either:
   - `0x<address>,4096` — reserve the 4 KiB page at that PA.  The
     shim also expands the range by `+/-1 MiB` at mask-apply time.
   - `# chip: U2320` — reserve every page decoding to chip U2320.
   - `# <text>` — comment, ignored.
4. Save, unmount, eject.

No `badmem.txt` == "NVRAM-only mode", which is the default.
