# BizarreRamRepair (BRR)

Bootable USB/ISO that identifies which exact soldered BGA DRAM chip has
failed on a MacBook Pro A1990 (15" 2018/2019), and hides the bad pages
— or the entire failing chip's memory region — from macOS without
kernel patches.

Base: fork of [memtest86plus](https://github.com/memtest86plus/memtest86plus)
plus a Coffee Lake-H IMC physical-address decoder, per-board topology
overlay, and an EFI mask-shim that calls
`AllocatePages(AllocateAddress, EfiReservedMemoryType, ...)` before
`ExitBootServices` so macOS sees the bad ranges as firmware-owned and
never allocates from them.

---

## How it works (one screen)

```
USB stick boots -> grub picks entry 1 (Automatic) after 15 s
  -> memtest pre-boot menu (T2 internal keyboard works via UEFI ConIn)
  -> press [P] page-mask  or  [C] chip-mask
  -> memtest runs 1 pass, writes bad PAs/chips to NVRAM, auto-reboots
  -> next USB boot: shim applies trial mask + chains macOS
  -> use macOS normally; verify stability
  -> restart to USB: menu shows "Install permanently? Y/N"; press Y, Y
  -> install.efi backs up BootOrder, copies shim to internal disk, reboots
  -> macOS boots masked forever; revert available via grub entry 4
```

No screen photography.  No second computer.  No `badmem.txt` editing.

---

## Requirements

- MacBook Pro A1990 (15-inch 2018 or 2019).  Other T2 Coffee Lake Macs
  may work with a topology YAML — community contributions welcome.
- T2 Startup Security Utility: **No Security** + **Allow external boot**.
- USB stick ≥ 512 MB.

---

## Use flow

### 1. Detect

Boot the USB stick.  Grub's first entry, **Automatic: memtest + ram-fix**,
auto-selects after 15 s.  Memtest starts loading and shows the pre-boot
menu with a 30-second countdown:

```
  BRR Memtest -- press a key (timeout 30 s -> Run tests)

    [Enter]  Run all tests (no auto-apply)
    [P]      Automatic: page-mask  (one pass, NVRAM save, auto-reboot)
    [C]      Automatic: chip-mask  (one pass, save bad chips, auto-reboot)
    [T]      Fast mode (skip countdowns, run all tests, no auto-apply)
    [R]      Reboot
```

Pick **[P]** or **[C]**:

| Mode | Effect |
|---|---|
| `[P]` page | Reserves only the 4 KiB pages that fail.  Minimal RAM loss. |
| `[C]` chip | Reserves every PA mapping to the failing chip's channel. On A1990 (1-rank per channel) this masks one full channel = 16 GiB.  Much safer against progressive cell decay; half the RAM. |

Memtest runs one full pass with countdowns skipped (30-60 min typical on
32 GB).  Each detected error prints a line identifying channel, byte
lane, and — when the board overlay matches — the exact chip designator
(e.g. `U2620`).  At the end of the pass:

- Bad addresses written to NVRAM as `BrrBadPages`.
- In chip mode, chip designators written as `BrrBadChips`.
- `BrrMaskState = "TRIAL_PENDING_PAGE"` (or `CHIP`).
- Machine reboots automatically in 5 s.

### 2. Trial

On the next boot of the USB stick, grub re-selects entry 1.  The
pre-boot menu detects the `TRIAL_PENDING_*` state and silently
chain-loads `mask-shim.efi`.  The shim:

- Reads `BrrBadPages` (page mode) and/or `BrrBadChips` (chip mode).
- For chip entries: walks physical address space via MCHBAR, reserves
  every 4 KiB page mapping to the bad chip's (channel, rank).
- Calls `AllocatePages(AllocateAddress, EfiReservedMemoryType, ...)`.
- Advances state to `TRIAL_BOOTED`.
- Chain-loads internal macOS `boot.efi`.

macOS boots with the reserved ranges treated as firmware-owned.  Use
macOS normally.  Watch for panics, filesystem corruption, or freezes.
If anything goes wrong, just reboot without the USB — the mask only
applies when the shim runs.

### 3. Permanent install (double confirm)

Once macOS has been stable, boot the USB stick again.  The pre-boot
menu detects `TRIAL_BOOTED`:

```
========================================================
  Trial mask was applied in previous boot (TRIAL_BOOTED)
========================================================
  If macOS booted + ran correctly, you can install the
  mask PERMANENTLY (writes to internal disk + EFI NVRAM).

  Install mask permanently? [Y/N, 30 s timeout = N]:
```

Press **Y**.  A second prompt appears — **Really install permanently?
EFI will be modified**.  Press **Y** again.

The menu chain-loads `install.efi`.  The installer:

- Pre-flight: trial `SetVariable` to confirm NVRAM is writable.
  Aborts cleanly on Medium/Full Security.
- Shows its own detailed confirmation (also 30 s timeout) — press Y.
- Copies `mask-shim.efi` and `badmem.txt` to `\EFI\BRR\` on the
  internal ESP.  Pre-existing `\EFI\BRR\` contents are first moved
  to `\EFI\BRR\backup\` so a later revert can restore them.
- Saves current `BootOrder` as `BrrBackupBootOrder`.
- Enumerates existing `Boot*` EFI variables, saves the list as
  `BrrBackupBootEntries`.
- Creates a new `BootNNNN` variable pointing at the internal shim.
- Prepends it to `BootOrder`.
- Sets `BrrMaskState = "PERMANENT_UNCONFIRMED"`.
- Reboots in 3 s.

On any step failure the installer rolls back everything written so far
and aborts — no half-committed state.

### 4. Post-install confirm

First non-USB boot after install: firmware launches the internal shim.
Shim reads `PERMANENT_UNCONFIRMED` and asks:

```
  BRR mask shim installed (PERMANENT_UNCONFIRMED).
  Did macOS boot correctly with the memory mask?
    Y = confirm installation (permanent)
    N = uninstall and boot without mask
    (timeout -> proceed; prompt repeats next boot)
```

- **Y** — state becomes `PERMANENT_CONFIRMED`.  Silent on every future
  boot.
- **N** — shim uninstalls itself inline (deletes `\EFI\BRR\`, restores
  `BootOrder`, deletes NVRAM state), chain-loads macOS.  USB not
  needed.
- **Timeout** — shim proceeds and asks again next boot.

### 5. Revert

Any time, from the USB stick, pick grub entry **4. Revert all changes
\[REVERT\]**.  `revert.efi`:

- Asks once for confirmation (Y within 30 s).
- Deletes `\EFI\BRR\*` from internal ESP (restoring backup if present).
- Restores original `BootOrder` from `BrrBackupBootOrder` (falls back
  to legacy `A1990MaskOriginalBootOrder` for old installs).
- Deletes our `BootNNNN` variable.
- Deletes all BRR NVRAM vars (and legacy `A1990*` equivalents).
- Reboots.

Idempotent: safe to run from any state, including `NONE`.

---

## Grub menu

```
1. Automatic: memtest + ram-fix     [DEFAULT, 15 s timeout]
2. Run memtest only (diagnostic)
3. Boot macOS normally (no mask)
4. Revert all changes               [REVERT]
```

Entry 2 loads the same memtest — use it to re-test without triggering
the auto-trial flow (press `[Enter]` in the pre-boot menu).  Entry 3
loads `mask-shim --passthrough`, skipping all masking and chain-loading
macOS directly (useful for A/B comparison).

---

## Recovery

| Situation | Fix |
|---|---|
| USB available, want to undo | Boot USB, pick entry 4 |
| USB lost | NVRAM reset (Cmd+Opt+P+R) clears the `BootOrder` override; rebuild USB and pick entry 4 |
| Boot loop after install | Cmd+Opt+P+R.  Or macOS Recovery (⌘R) + `sudo bless --folder /Volumes/Macintosh\ HD --setBoot` |
| macOS upgrade breaks install | Boot USB, entry 4, then reinstall after upgrade completes |

NVRAM vars under vendor GUID `3E3E9DB2-1A2B-4B5C-9D1E-5F6A7B8C9D0E`:

| Var | Meaning |
|---|---|
| `BrrMaskState` | `NONE`, `TRIAL_PENDING_{PAGE,CHIP}`, `TRIAL_BOOTED`, `PERMANENT_UNCONFIRMED`, `PERMANENT_CONFIRMED` |
| `BrrBadPages` | Binary: `[uint32 version=1][uint32 count][uint64 PAs...]` |
| `BrrBadChips` | NUL-separated ASCII chip designators |
| `BrrBackupBootOrder` | Saved BootOrder for revert |
| `BrrBackupBootEntries` | Pre-install list of Boot* slots |
| `BrrBootSlot` | Our BootNNNN slot number |

---

## Granularity: region vs chip

| Mode | RAM cost (A1990 32 GB) | Safety against progressive failure |
|---|---|---|
| Page (`[P]`) | ~4 KiB per detected error | Low — more cells may fail |
| Chip (`[C]`) | 16 GiB (entire channel) | High — all addresses mapping to failing chip are masked |

A failing DRAM cell often spreads — once one bit goes, neighbouring
cells and adjacent rows frequently follow within weeks.  Chip mode
trades RAM for robustness.

On A1990: 2 channels × 1 rank × 8 × 2 GB (Micron MT40A2G8-NRE).  Chips:
`U2300..U2430` = CH-A, `U2500..U2630` = CH-B.  See
`topology/820-01814-a.yaml` for the verified byte-lane mapping.

---

## Build

Docker required; works on macOS and Linux hosts.

```
./scripts/docker-build.sh iso
# produces dist/a1990-memtest.iso
```

Flash:

```
# find device
diskutil list external            # macOS
lsblk                             # Linux

# unmount + dd (replace N)
sudo diskutil unmountDisk /dev/diskN
sudo dd if=dist/a1990-memtest.iso of=/dev/rdiskN bs=4m status=progress
sync && diskutil eject /dev/diskN
```

Or use [balenaEtcher](https://www.balena.io/etcher/) / [Rufus](https://rufus.ie/).

---

## Contributing a new board

Drop boardview + schematic under `boardviews/` and `vendor/` (both
git-ignored).  Run `scripts/trace-dq.py` on a `pdftotext -layout` dump
of the schematic to derive per-chip byte-lane mapping.  Add
`topology/<board-id>.yaml` following `topology/820-01814-a.yaml`.
Rebuild — no code changes needed; the board table auto-links.

The IMC PA decoder in `src/cfl_decode.c` covers Coffee Lake / Kaby
Lake / Skylake client.  Ice Lake / Tiger Lake / Alder Lake / Raptor
Lake each need their own decoder; FSP-blob memory init means each
platform's `MAD_DIMM` / `MAD_INTRA` bit layouts must be
reverse-engineered.

---

## Credits

The EFI memory-map patching approach (`AllocatePages(AllocateAddress,
EfiReservedMemoryType, ...)` before `ExitBootServices()`) is directly
inspired by **0nelight**'s
[macOS-Disable-RAM-Areas](https://github.com/0nelight/macOS-Disable-RAM-Areas).
That project first demonstrated that an EFI driver running
pre-ExitBootServices reliably hides bad memory ranges from macOS
without kernel patches.  BRR automates the full
detect → trial → permanent → revert flow, but the underlying technique
is theirs.  Thank you.

**Derrick Schneider**'s
[writeup on saving a MacBook Pro from bad RAM](https://derrick.blog/2025/02/28/how-i-saved-my-macbook-pro-from-bad-ram/)
documented the end-to-end user workflow on a T2 Mac.  Useful reference
when designing the trial-then-permanent prompt.

Upstream [memtest86plus](https://github.com/memtest86plus/memtest86plus)
— battle-tested RAM test code, GPL v2.

---

## License

GPL v2, inherited from memtest86plus.
