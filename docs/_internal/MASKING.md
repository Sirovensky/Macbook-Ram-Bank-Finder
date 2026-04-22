# EFI bad-RAM masking — user guide

## Quick start (one screen)

1. Boot the USB stick.
2. Wait for grub — entry 1 is selected automatically after 15 s.
3. At the **A1990 Memtest pre-boot menu** (30 s countdown) press **[P]**
   (page-mask) or **[C]** (chip-mask).
4. Memtest runs one pass, saves bad addresses to NVRAM, reboots.
5. Boot USB again — the same grub entry runs a second verification pass
   and reboots.
6. Boot USB a third time — the pre-boot menu detects the saved state and
   offers the **"Install permanently?"** double-confirm prompt.
   Press **Y** twice to install the shim to the internal disk.
7. On the next normal boot the internal shim masks the bad pages and
   chains macOS.  Confirm the prompt that appears.

For recovery paths see [MASKING_RECOVERY.md](MASKING_RECOVERY.md).
For the full state-machine specification see [SEAMLESS_FLOW.md](SEAMLESS_FLOW.md).

---

## Background

macOS's memory allocator honours the UEFI memory map.  A physical-address
range marked `EfiReservedMemoryType` is treated as firmware-owned and never
allocated by the kernel or userspace.  Bad DRAM pages can therefore be
hidden from macOS permanently without kernel patches or Recovery mode.

The mask-shim (`mask-shim.efi`) calls
`AllocatePages(AllocateAddress, EfiReservedMemoryType, ...)` for each bad
range before `ExitBootServices()`, then chain-loads the internal macOS
`boot.efi`.  All interactions are pre-`ExitBootServices` using UEFI ConIn,
which is the only keyboard path that works on A1990 before the T2 chip
hands off the internal keyboard to the OS.

---

## Prerequisites

- MacBook Pro A1990 (15-inch, 2018 or 2019).
- T2 Startup Security Utility: **No Security** and **allow booting from
  external media**.  (Same requirement as running memtest itself.)
- The `a1990-memtest.iso` USB stick.

---

## Grub menu

The grub menu on the USB stick has four entries (default timeout 15 s):

```
1. Automatic: memtest + ram-fix  [DEFAULT]
2. Run memtest only (diagnostic)
3. Boot macOS normally (no mask)
4. Revert all changes             [REVERT]
```

Entry 1 and entry 2 both boot the same memtest binary.  The difference is
intent: entry 2 is a reminder that pressing **[Enter]** in the pre-boot menu
runs a plain diagnostic pass with no auto-apply.

Entry 3 loads `mask-shim.efi --passthrough`, which skips all masking and
chain-loads macOS directly.  Use it for an A/B comparison or to bypass a
bad mask configuration.

Entry 4 loads `revert.efi` and performs a full teardown after one
confirmation prompt.

---

## State machine

The tools communicate through an NVRAM variable `A1990MaskState` (vendor
GUID `3E3E9DB2-1A2B-4B5C-9D1E-5F6A7B8C9D0E`).  The full diagram is in
[SEAMLESS_FLOW.md](SEAMLESS_FLOW.md).  In brief:

| State | What happens on next USB boot (entry 1) |
|---|---|
| absent / `NONE` | Pre-boot menu shown; user picks action |
| `TRIAL_PENDING_PAGE` | Menu bypassed; auto flags set; memtest runs 1 pass; state stays; reboots |
| `TRIAL_PENDING_CHIP` | Same as above but chip-designator path |
| `TRIAL_BOOTED` | Double-confirm "Install permanently?"; Y,Y chains `install.efi` |
| `PERMANENT_UNCONFIRMED` | Handled by the internal shim (not the USB menu) |
| `PERMANENT_CONFIRMED` | Internal shim boots silently |

---

## Detailed walkthrough

### Step 1 — Initial detection pass

1. Boot from the USB stick.
2. Grub selects entry 1 automatically (or press 1).
3. The **A1990 Memtest pre-boot menu** appears:

```
  A1990 Memtest -- press a key (timeout 30 s -> Run tests)

    [Enter]  Run all tests (no auto-apply)
    [P]      Automatic: page-mask (1 pass, NVRAM save, reboot)
    [C]      Automatic: chip-mask (enter chip designators, then same)
    [T]      Fast mode (skip countdowns, run all tests, no auto-apply)
    [R]      Reboot
```

4. Press **[P]** for page-mask (recommended) or **[C]** for chip-mask
   (see "Masking granularity" below for the trade-off).
5. Memtest runs one full pass with countdowns skipped.  Each bad address is
   logged.  At the end of the pass:
   - Bad page addresses are written to NVRAM as `A1990BadPages`.
   - In chip mode, chip designators are written as `A1990BadChips`.
   - `A1990MaskState` is set to `TRIAL_PENDING_PAGE` or `TRIAL_PENDING_CHIP`.
   - The screen shows `[nvram] state -> TRIAL_PENDING_{PAGE,CHIP}`.
   - The machine reboots automatically.

If `SetVariable` fails (T2 Startup Security set to "Medium Security"),
the screen shows `[nvram] SetVariable failed` and falls back to the manual
workflow — see "Manual workflow" below.

### Step 2 — Verification pass

1. Boot from the USB stick again.  Enter grub entry 1 (default).
2. The pre-boot menu reads `A1990MaskState` and detects `TRIAL_PENDING_*`.
   It prints `[auto] State = TRIAL_PENDING_PAGE -> skipping menu` and
   returns auto flags without showing the interactive menu.
3. Memtest runs another pass using the saved bad-page list.  At the end it
   writes the updated `A1990BadPages` to NVRAM and reboots automatically.
   State remains `TRIAL_PENDING_*`.

This second pass is optional but confirms the failure is repeatable and
keeps the NVRAM list current.

### Step 3 — Permanent install (double confirm)

The pre-boot menu transitions the state machine to `TRIAL_BOOTED` as part
of the permanent install flow.  When `A1990MaskState = TRIAL_BOOTED` is
detected, the menu shows:

```
========================================================
  Trial mask was applied in previous boot (TRIAL_BOOTED)
========================================================
  If macOS booted + ran correctly, you can install the
  mask PERMANENTLY (writes to internal disk + EFI NVRAM).

  Install mask permanently? [Y/N, 30 s timeout = N]:
```

Press **Y**.  A second prompt follows:

```
  Really install permanently? EFI will be modified [Y/N, 30 s timeout = N]:
```

Press **Y** again.  The pre-boot menu chainloads `install.efi` from the USB
stick.  `install.efi`:

- Shows the count of bad pages found in NVRAM.
- Runs a pre-flight NVRAM write test; aborts cleanly if NVRAM is read-only.
- Shows two more confirmation screens (30 s timeout each; any key other
  than Y cancels with no changes made).
- Copies `mask-shim.efi` and `badmem.txt` to `\EFI\MASK\` on the internal
  ESP.
- Creates a `BootNNNN` EFI variable pointing to the internal shim and
  prepends it to `BootOrder` (saving the original for revert).
- Sets `A1990MaskState = PERMANENT_UNCONFIRMED`.
- Reboots in 3 s.

Note on the `TRIAL_BOOTED` transition: this state is set by `mask-shim.efi`
when it applies a pending trial mask.  The shim is invoked from the internal
disk after permanent install; it reads `TRIAL_PENDING_*`, applies the mask,
advances state to `TRIAL_BOOTED`, and chains macOS.  On the subsequent USB
boot the pre-boot menu then sees `TRIAL_BOOTED` and offers the
double-confirm.

### Step 4 — Post-install confirmation

On the first normal (non-USB) boot after install:

1. Firmware loads the internal shim from `\EFI\MASK\mask-shim.efi`.
2. The shim reads `A1990MaskState = PERMANENT_UNCONFIRMED` and prompts:

```
  A1990 mask shim installed (PERMANENT_UNCONFIRMED).
  Did macOS boot correctly with the memory mask?
    Y = confirm installation (permanent)
    N = uninstall and boot without mask
    (timeout -> proceed; prompt repeats next boot)
```

- **Y**: state advances to `PERMANENT_CONFIRMED`.  Future boots are silent.
- **N**: the shim uninstalls itself inline (cleans NVRAM and ESP files)
  and boots macOS without a mask.  No USB stick needed for cleanup.
- **Timeout (30 s)**: shim proceeds with the mask; prompt repeats on the
  next boot.

---

## NVRAM variables

All state uses vendor GUID `3E3E9DB2-1A2B-4B5C-9D1E-5F6A7B8C9D0E`.

| Variable | Type | Meaning |
|---|---|---|
| `A1990MaskState` | ASCII string | State machine value (see above) |
| `A1990BadPages` | binary blob | Bad PA list from memtest (version=1 header + u64 array) |
| `A1990BadChips` | ASCII string | Comma-separated chip designators for chip mode |
| `A1990BackupBootOrder` | binary (UINT16[]) | Pre-install BootOrder saved for revert |
| `A1990BackupBootEntries` | ASCII string | List of shim-created BootNNNN slots |
| `A1990MaskBootSlot` | UINT16 | The shim's BootNNNN slot number |

Variables are non-volatile and survive power cycles.  An NVRAM reset
(Cmd+Opt+P+R) clears all of them — see [MASKING_RECOVERY.md](MASKING_RECOVERY.md).

---

## Masking granularity: region vs. chip

Two masking policies are available.  They can be combined in a single
`badmem.txt` file.

### Region mode (page granularity)

Default.  Each detected bad address causes the shim to reserve that 4 KiB
page via `AllocatePages`.  Minimises RAM loss but only hides addresses that
were actually observed to fail; the rest of the chip is assumed healthy.

Selected by pressing **[P]** at the pre-boot menu.

### Chip mode (chip-level masking)

Chip directives (`# chip: UXXXX` lines in `badmem.txt`, or designators
auto-detected by the error hook and saved to `A1990BadChips`) trigger the
shim to:

1. Look up the designator in the compiled-in board topology table.
2. Read MCHBAR registers (direct MMIO, pre-`ExitBootServices`) to determine
   the PA-to-channel mapping.
3. Walk all PAs from 0 to IMC-reported total (4 KiB step), decode each to
   `(channel, rank)`, and reserve every page that maps to the suspect chip.
4. Print a progress message every 1 GiB and a summary at the end.

On A1990 (one rank per channel, 8 × x8 chips per channel) masking one chip
reserves every PA on that channel — 16 GiB out of 32 GiB total.  Much
safer against progressive failure, but halves usable RAM.

Selected by pressing **[C]** at the pre-boot menu.

### Trade-off table

| Policy | RAM reserved | When to use |
|---|---|---|
| Region (page) | ~pages × 4 KiB | Isolated failure, small number of errors |
| Chip (channel) | 16 GiB on A1990 | Progressive or widespread failure on one chip |

Chip mode is opt-in.  No chip masking occurs unless [C] is chosen or a
`# chip:` directive appears in `badmem.txt`.

---

## Revert (entry 4)

Boot from the USB stick and select **entry 4**.  `revert.efi` prompts once
for confirmation (30 s timeout = cancel), then:

- Deletes `\EFI\MASK\` files from the internal ESP (restoring backup if
  present).
- Restores `BootOrder` from `A1990BackupBootOrder`.
- Deletes the shim's `BootNNNN` EFI variable.
- Deletes all `A1990*` NVRAM variables.
- Reboots in 3 s.

Revert is idempotent: running it from any state, including `NONE`, is safe.

For additional recovery scenarios (no USB stick, boot loops, NVRAM reset)
see [MASKING_RECOVERY.md](MASKING_RECOVERY.md).

---

## Manual workflow (fallback if NVRAM writes are blocked)

Use this if T2 Startup Security is set to "Medium Security" and the
automatic NVRAM save fails.

### Detect bad pages

1. Boot USB, grub entry 1, press **[Enter]** at the pre-boot menu
   (plain diagnostic, no auto-apply).
2. Let at least one full pass run.
3. At the end of each pass the screen shows:

```
--- badmem.txt contents (paste into /EFI/BOOT/badmem.txt) ---
0x200000,4096
0x201000,4096
--- end badmem.txt ---  (2 page(s) recorded)
```

4. Photograph the screen or write down the lines.

### Prepare badmem.txt

On a working Mac (or any machine that can mount FAT32):

1. Mount the USB stick.
2. Open `/EFI/BOOT/badmem.txt` (create it if absent).
3. Paste in the lines from the screen dump.  Keep the `0xADDR,4096` format.
   Comments starting with `#` are allowed.
4. Save and eject.

See [BADMEM_FORMAT.md](BADMEM_FORMAT.md) for the full format specification.

### Install permanently

After placing `badmem.txt` on the USB stick, boot USB and boot grub entry 1.
At the pre-boot menu press **[Enter]** to run a plain test pass, then reboot.
On the next USB boot the pre-boot menu will be in the normal state; you can
then manually proceed by re-running memtest with `badmem.txt` in place, or
invoke `install.efi` directly if the shim logic allows it from your current
state.

Alternatively, if `badmem.txt` is already on the USB and you have no NVRAM
state set, grub entry 3 (passthrough) loads the shim with `--passthrough`
which skips masking.  For a trial boot with masking from USB and a manually
prepared `badmem.txt`, the shim reads `\EFI\BOOT\badmem.txt` automatically
when loaded — use grub entry 3 and remove the `--passthrough` argument
(this requires editing the grub config on the USB).

---

## Credits

The EFI-level memory-map patching approach is inspired by
**0nelight**'s [macOS-Disable-RAM-Areas](https://github.com/0nelight/macOS-Disable-RAM-Areas)
tool.  That project first demonstrated that an EFI driver calling
`AllocatePages(AllocateAddress, EfiReservedMemoryType, ...)` before
`ExitBootServices()` reliably hides bad memory ranges from macOS without
kernel patches.  The mask-shim here automates the full
detect-trial-permanent-revert flow, but the underlying technique is theirs.
Thanks.

Also: **Derrick Schneider**'s [writeup on saving a MacBook Pro from bad RAM](https://derrick.blog/2025/02/28/how-i-saved-my-macbook-pro-from-bad-ram/)
documented the end-to-end user workflow on a T2 Mac.  Useful reference when
designing the trial-then-permanent prompt.
