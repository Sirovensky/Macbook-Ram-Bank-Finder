# BizarreRamRepair (BRR)

Bootable USB/ISO for the **2018 MacBook Pro 15" (A1990)** that diagnoses
failing soldered DRAM, hides the bad cells from macOS via a pre-boot EFI
shim, and lets the Mac keep running without a motherboard swap.

Base: fork of [memtest86plus](https://github.com/memtest86plus/memtest86plus)
plus a Coffee Lake-H IMC physical-address decoder, per-board BGA
topology overlay, and an EFI `mask-shim.efi` that calls
`AllocatePages(AllocateAddress, EfiReservedMemoryType, ...)` **before**
`ExitBootServices` so macOS sees the bad pages as firmware-owned and
never allocates from them.

---

## TL;DR

1. Flash the ISO to a USB stick. Boot it. **Entry 1** (full test) runs
   memtest for ~30-60 min on 32 GiB and prints the bad-address list.
2. Photograph the bad addresses.
3. Power off. Boot the USB again. Pick **entry 3** (Setup NVRAM hook +
   apply mask). Type the addresses in. Press **Y**.
4. The tool writes NVRAM (persists on T2), chainloads the mask-shim,
   which reserves `+/-1 MiB` around each address, then chainloads
   macOS `boot.efi`. macOS boots with those pages already marked reserved
   and never touches them.

No screen patching. No motherboard swap. No kernel extension.

---

## Requirements

- MacBook Pro A1990 (15-inch 2018 or 2019). Other T2 Coffee Lake Macs
  may work with a new board topology YAML — see `topology/`.
- T2 Startup Security Utility must allow **external boot**.
  "Reduced Security" is enough. "No Security" is safest.
- USB stick >= 256 MB.
- You can hold **Option** at boot to reach the UEFI boot picker. (If the
  Mac won't POST at all, this project can't help — the bad cells must
  be sparse enough that firmware init succeeds.)

---

## The three grub entries

```
1. Full test  (all patterns, ~30-60 min) [DEFAULT]
2. Fast test  (3 patterns, ~5 min)
3. Setup NVRAM hook + apply mask
```

### Entry 1 — Full test (recommended)

Runs all 13 memtest patterns. On a 32 GiB A1990 this is 30-60 minutes.
At pass end you get a block like:

```
BAD ADDRESSES (3 found) -- photograph this line:
0xb21df000, 0xb21e0000, 0xb2200000

Next: power off, boot USB, pick entry 3.
```

Memtest halts — no auto-reboot. Photograph the address line with your
phone. Power off.

Each error line earlier in the pass also identifies the chip
designator, e.g.:

```
    b21df000  ch0 rk0  chip: U2320
```

So you also know which physical BGA on the mainboard is bad. This is
informational (used when sending the board for repair/reflow); the
mask is applied by address, not by chip.

### Entry 2 — Fast test

Same output format, 3 patterns only, ~5 min. Good sanity check; the
full test is the real one.

### Entry 3 — Setup NVRAM hook + apply mask

Loads `brr-entry.efi` pre-ExitBootServices. It:

1. Checks NVRAM for an existing saved mask. If present it offers
   **quick-retry**: press **Y** to re-apply without re-typing.
2. Otherwise prompts for comma-separated hex addresses (0x optional).
3. Writes `BrrBadPages` to NVRAM via Boot Services `SetVariable`
   (persists on T2; Runtime Services `SetVariable` does **not**).
4. Writes `BrrMaskState = "TRIAL_PENDING_PAGE"`.
5. Chainloads `mask-shim.efi`.

The shim reads NVRAM + optional `badmem.txt`, calls
`AllocatePages(EfiReservedMemoryType)` on every page in the
`+/- 1 MiB` window around each bad address (rounded to 4 KiB), then
chainloads the internal macOS `boot.efi`.

**If chainload fails** (e.g. `boot.efi` not located — fixable, see
_Troubleshooting_), NVRAM stays intact. Hard-reboot, re-pick entry 3,
press **Y** at the quick-retry prompt, and retry.

---

## What actually persists

| Thing | Where | Persists across | Who sets it |
|---|---|---|---|
| `BrrBadPages` | NVRAM (vendor GUID `3E3E9DB2-1A2B-4B5C-9D1E-5F6A7B8C9D0E`) | power-off and reboot | `brr-entry.efi` |
| `BrrMaskState` | same GUID | same | `brr-entry.efi` / `mask-shim.efi` |
| Reserved pages | UEFI memory map (EfiReservedMemoryType) | **only until next reboot** | `mask-shim.efi` |

The pages are reserved ONLY for the macOS session that `mask-shim`
chainloaded. Every reboot the shim has to run again. That's why the
current design runs the shim from USB on every boot; a future
`install.efi` will install the shim on the internal ESP + register a
`BootNNNN` override so the shim auto-runs without USB. That's the
next milestone, not shipped yet.

### NVRAM and AllocatePages are independent layers — no conflict

Two separate mechanisms cooperate here:

- **NVRAM** stores the list of bad addresses. Survives reboot.
  If mask-shim's chainload fails for any reason, NVRAM stays
  intact and you can retry (entry 3 → Y). No code path deletes
  `BrrBadPages` on failure.
- **AllocatePages** reserves the actual pages in the UEFI memory
  map handed to macOS. Only effective for the one macOS session
  the shim chainloads. Does NOT persist across reboot — the shim
  re-applies on every boot by re-reading NVRAM.

Because the two layers address different lifetimes (persistent
address list vs per-session reservation), they can't conflict.
Running with both is the intended design: the NVRAM list is the
durable source of truth, the per-session reservation is the
actual protection.

---

## Reading a mask-shim transcript

```
BRR mask-shim v1
=====================================
[shim] state=TRIAL_PENDING_PAGE: applying page mask
[mask] loaded 3 page(s) from NVRAM, 0 range(s) from badmem.txt
[shim] 3 region range(s) to reserve
[shim] mask coverage: 3/3 range(s) fully covered
[shim]   new reserves: 1024 page(s)  (each +/-1 MiB around bad addr)
[shim]   firmware pre-reserved: 512 page(s)  (already OFF-LIMITS to macOS)
[shim] verify: 3/3 bad PA(s) confirmed EfiReservedMemoryType in memory map
[shim]  vol#0: EFI, .fseventsd
[shim]  vol#1: 5A1B3...
[shim]  vol#2: com.apple.r...
...
[shim] find_macos_boot: probed 6 volume(s) (skipped 1 self, 0 removable), FOUND
[shim] found macOS boot.efi
[shim] starting macOS boot.efi...
```

### Coverage line

`mask coverage: N/M range(s) fully covered` — N ranges have **every**
page in the `+/-1 MiB` window either newly-reserved by us or
pre-reserved by firmware. If M != N you'll also see:

```
[shim]   WARNING: 42 page(s) in 1 range(s) NOT protected -- unsafe!
```

That means some pages in the mask window returned an error from
`AllocatePages` that wasn't "already reserved" (e.g. out-of-RAM
region, invalid parameter). Rare — usually the bad address was typed
wrong. Retry entry 3 with correct addresses.

### Verify line

`[shim] verify: N/M bad PA(s) confirmed EfiReservedMemoryType in memory map`
— after mask application, shim re-reads the UEFI memory map and
checks each bad PA landed in a descriptor marked `EfiReservedMemoryType`
(Type==0). N == M is the success case. If N < M, a per-PA warning
prints showing which PAs still read as a non-reserved type — treat
this as the mask having failed silently.

### Volume peek

`vol#N: A, B, C` shows the first three non-dot child names found at
the root of SFS volume N. On an APFS Preboot volume you'll see
UUID-looking strings (e.g. `5A1B3...`). On the ISO FAT-ESP you'll see
`EFI`. This helps diagnose `NOT FOUND` remotely — photograph this
line and the path the finder used.

---

## Build

Requires Docker. Works on macOS and Linux hosts.

```
make docker-iso
# -> dist/a1990-memtest.iso
```

Flash:

```sh
# macOS
diskutil list external
sudo diskutil unmountDisk /dev/diskN
sudo dd if=dist/a1990-memtest.iso of=/dev/rdiskN bs=4m status=progress
sync && diskutil eject /dev/diskN

# Linux
lsblk
sudo dd if=dist/a1990-memtest.iso of=/dev/sdN bs=4M status=progress conv=fsync
```

Or balenaEtcher / Rufus.

Hosted unit test (no Docker needed):

```
make test-shim
```

Runs the badmem.txt parser against fixtures — useful when hacking
`efi/badmem_parse.c`.

---

## Troubleshooting

### "Could not chainload mask-shim"

brr-entry failed to `LoadImage` or `StartImage` the shim. Usually
the USB stick was reformatted / mask-shim.efi missing. Check the ISO
was flashed complete (compare SHA-256 to the one printed at build
end).

### "[shim] ERROR: macOS boot.efi not found"

The shim couldn't locate `boot.efi` on any APFS volume. Causes:

- **Internal SSD disconnected / missing** — common after logic-board
  work. Check that macOS still shows the disk in Disk Utility when
  booted normally.
- **T2 Full Security** — prevents external boot from seeing internal
  APFS volumes. Change to "Reduced" or "No Security" in Recovery
  (Cmd-R at boot).
- **Unusual APFS layout** — the shim tries 3 tiers of paths (root,
  1-level, 2-level) plus alternates (`\com.apple.recovery.boot\...`,
  `\usr\standalone\i386\...`). If all miss, photograph the `vol#N`
  diagnostic lines — they reveal what's actually at each volume root.

NVRAM stays intact on shim failure. You can hard-reboot and either:

- Re-pick entry 3 → **Y** quick-retry.
- Unplug USB, power on, macOS boots **unmasked** (so a pre-existing
  bad cell will likely panic). Only safe if you don't boot at all.
- Hold Option at boot, pick macOS manually — same caveat.

### macOS kernel panic

Happens when macOS tries to use a bad page that wasn't masked. Means
the mask window was too narrow OR you missed an address on the
memtest list. Boot USB entry 1 again, full test; every additional
hit widens the mask.

### T2 NVRAM got wiped

Possible causes:
- Cmd-Option-P-R at boot (user-initiated NVRAM reset).
- SMC reset combined with firmware update.
- Battery disconnect on some repairs.

Re-type your addresses in entry 3 — no big deal.

---

## How it works under the hood

### 1. Address capture

`memtest86plus` runs the RAM tests; on each miscompare it calls our
hook `board_report_error(pa, xor_bits)` which appends the PA to a
static array. At pass end on CPU 0 we decode each PA through the
Coffee Lake IMC register map to a `(channel, rank, bank, row)` tuple
and cross-reference `board_topology.c` to identify the specific BGA
designator. We then print the bad-address list in `%x, %x, %x` format
for the user to photograph.

No NVRAM write during the memtest pass — `common_err()` holds
`error_mutex` across the hook, and any display-side work can block
on `scroll()`. Heavy decoding happens once, post-pass, single-
threaded on the BSP.

### 2. Pre-EBS NVRAM write (Apple T2 quirk)

On Apple T2 hardware:

- `SetVariable` called via **Runtime Services** (after
  `ExitBootServices`) does **NOT** persist across reboot, under
  any GUID — verified empirically.
- `SetVariable` called via **Boot Services** (before
  `ExitBootServices`) DOES persist — the T2 firmware commits these
  to internal flash at the EBS transition.

So the "write NVRAM after memtest runs" path is fundamentally
unavailable on this hardware. Instead we have the user transcribe
the addresses into a pre-EBS EFI app (`brr-entry.efi`), which writes
NVRAM while still in Boot Services.

### 3. Mask application

`mask-shim.efi` reads `BrrBadPages` from NVRAM, expands each address
by `+/- 1 MiB`, aligns to 4 KiB pages, and calls
`AllocatePages(AllocateAddress, EfiReservedMemoryType, 1, &pa)` for
each page. Pages that were already firmware-reserved return
`EFI_NOT_FOUND` / `EFI_ACCESS_DENIED` — that counts as "already safe",
not as failure.

Then the shim uses `LocateHandle(ByProtocol, SimpleFileSystem)` to
enumerate every filesystem, scans each for `boot.efi` at several
known path tiers, and `LoadImage + StartImage`s the first hit. macOS
inherits the current UEFI memory map via `ExitBootServices`, sees
the reserved pages, and routes around them for the lifetime of the
kernel session.

### 4. What's NOT implemented yet

- **Permanent install.** Today the shim runs once per boot, from USB.
  An `install.efi` helper to copy the shim to the internal ESP and
  register a `BootNNNN` override (so shim runs without USB) is the
  next milestone. `efi/mask-install/` has partial scaffolding.
- **Chip-mode masking at scale.** Chip-level masking is wired
  (`BrrBadChips` + `shim_cfl_decode_pa`), but the page-mode single-
  address flow is the default and recommended. Chip mode reserves
  16 GiB per bad chip on A1990 — useful only for chips with
  rapidly-spreading failures.
- **Row-mode masking.** `BrrDecoderStatus=VALIDATED` + `BrrBadRows`
  path exists for DRAM-row granularity (~8 KiB per row) but the
  validator is disabled by default. Experimental.

---

## Credits

The EFI memory-map patching approach
(`AllocatePages(AllocateAddress, EfiReservedMemoryType, ...)` before
`ExitBootServices`) is directly inspired by **0nelight**'s
[macOS-Disable-RAM-Areas](https://github.com/0nelight/macOS-Disable-RAM-Areas).

**Derrick Schneider**'s
[writeup on saving a MacBook Pro from bad RAM](https://derrick.blog/2025/02/28/how-i-saved-my-macbook-pro-from-bad-ram/)
provided the +/- 1 MiB mask-window heuristic and the end-to-end user
workflow.

Upstream [memtest86plus](https://github.com/memtest86plus/memtest86plus)
— GPL v2.

---

## License

GPL v2, inherited from memtest86plus.
