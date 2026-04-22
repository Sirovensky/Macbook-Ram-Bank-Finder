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

## How to fill this file

### Region mode

1. Boot the USB.  Pick **entry 1 — Run memtest** from the grub menu.
2. Let memtest run at least one pass.
3. The final screen shows a block labelled `badmem.txt contents:` with
   lines in the `0x...,4096` format.  Photograph the screen.
4. On a working Mac, mount the USB, open `/EFI/BOOT/badmem.txt` in a
   text editor, and paste in the lines you photographed.
5. Save, eject USB.

### Chip mode

Instead of individual page addresses, identify the failing chip
designator from the calibration screen or the error log (the designator
column shows `U2xxx`), then add a `# chip: U2xxx` line.
