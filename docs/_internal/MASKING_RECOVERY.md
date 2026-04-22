# Masking recovery guide

This document covers every recovery scenario for the A1990 bad-RAM mask,
from "I just want to go back to normal" to "my Mac won't boot at all."

For the full state-machine specification see [SEAMLESS_FLOW.md](SEAMLESS_FLOW.md).
For the normal install workflow see [MASKING.md](MASKING.md).

---

## State overview

| State | Where shim lives | How to reach it | How to revert |
|---|---|---|---|
| `NONE` (or absent) | nowhere | default / after uninstall | already clean |
| `TRIAL_PENDING_PAGE` | nowhere (NVRAM only) | user pressed [P] at pre-boot menu | grub entry 4 or NVRAM reset |
| `TRIAL_PENDING_CHIP` | nowhere (NVRAM only) | user pressed [C] at pre-boot menu | grub entry 4 or NVRAM reset |
| `TRIAL_BOOTED` | internal ESP | after shim applied pending mask on first internal boot | grub entry 4 or NVRAM reset |
| `PERMANENT_UNCONFIRMED` | internal ESP | after `install.efi` ran successfully | grub entry 4 or press N at shim prompt or NVRAM reset |
| `PERMANENT_CONFIRMED` | internal ESP | after pressing Y at the shim's confirmation prompt | grub entry 4 or NVRAM reset |

---

## Scenario A — TRIAL_PENDING state, want to abort

You pressed [P] or [C] at the pre-boot menu and the machine rebooted.
The state is `TRIAL_PENDING_PAGE` or `TRIAL_PENDING_CHIP`.  Nothing has
been written to the internal disk yet.

**Recovery — USB available (recommended):**
1. Insert the USB stick and boot from it.
2. At the grub menu choose **entry 4 — Revert all changes**.
3. Press **Y** at the confirmation prompt.
4. All `A1990*` NVRAM variables are deleted.  Internal disk is untouched.
   Machine reboots normally.

**Recovery — no USB:**
Perform an NVRAM reset (see "NVRAM reset" below).  Since nothing is on the
internal ESP, a normal boot follows.

---

## Scenario B — PERMANENT installed, USB available

You installed permanently and want to remove the mask.

**Recovery:**
1. Insert the USB stick and boot from it.
2. At the grub menu choose **entry 4 — Revert all changes**.
3. Press **Y** at the confirmation prompt (30 s timeout cancels with no
   changes).
4. `revert.efi` will:
   - Delete `\EFI\MASK\mask-shim.efi` and `\EFI\MASK\badmem.txt` from the
     internal ESP (restoring any backup if present).
   - Restore `BootOrder` from the saved `A1990BackupBootOrder` variable.
   - Delete the shim's `BootNNNN` EFI variable.
   - Delete all `A1990*` NVRAM variables.
   - Reboot.
5. Next boot follows the original boot path — no shim.

---

## Scenario C — PERMANENT_UNCONFIRMED, macOS did not boot correctly

The shim was installed but macOS showed problems (panic, hang, filesystem
errors).  The shim's `PERMANENT_UNCONFIRMED` prompt is specifically designed
for this case.

**Recovery — press N at the shim prompt:**

On the next boot the internal shim shows:

```
  A1990 mask shim installed (PERMANENT_UNCONFIRMED).
  Did macOS boot correctly with the memory mask?
    Y = confirm installation (permanent)
    N = uninstall and boot without mask
    (timeout -> proceed; prompt repeats next boot)
```

Press **N**.  The shim uninstalls itself inline:
- Deletes `\EFI\MASK\` files from the internal ESP.
- Restores `BootOrder`.
- Deletes all `A1990*` NVRAM variables.
- Boots macOS without any mask.

No USB stick is needed.

**Recovery — USB available:**
Use grub entry 4 as in Scenario B.

---

## Scenario D — PERMANENT installed, USB lost/unavailable

### Option 1: NVRAM reset

An NVRAM reset clears all non-volatile EFI variables, which removes our
`BootOrder` modification.  The firmware falls back to its built-in boot
path, which bypasses the shim entry.

1. Shut down the Mac fully (not sleep).
2. Press and hold **Cmd + Option + P + R** while pressing the power button.
3. Keep holding for about 20 seconds.  On T2 Macs, wait until the Apple
   logo appears and disappears twice.
4. Release.  The Mac boots using the firmware's fallback boot path.

Note: NVRAM reset does **not** delete files from the internal ESP.  The
`\EFI\MASK\` directory and its files remain but are no longer in the boot
path.  They can be cleaned up from macOS after it boots (see "Clean-up from
macOS Terminal" below).

### Option 2: macOS Recovery

On T2 Macs, holding **Cmd + R** at startup enters macOS Recovery.  Recovery
boots its own `boot.efi` directly from the Preboot volume, bypassing NVRAM's
`BootOrder` entirely.

From Recovery Terminal:

```bash
# Find the internal ESP partition
diskutil list | grep EFI

# Mount it (example: disk0s1)
diskutil mount /dev/disk0s1

# Remove mask files
rm -rf /Volumes/EFI/EFI/MASK

# Restore boot order via bless
bless --mount "/Volumes/Macintosh HD" --setBoot

# Unmount
diskutil unmount /dev/disk0s1
```

### Option 3: Internet Recovery

Hold **Cmd + Option + R** at startup to boot Internet Recovery.  Proceed as
in Option 2 once you have Terminal access.

---

## Scenario E — Shim loops or causes immediate reboot

If the internal shim crashes or triggers a reboot loop before macOS can
start:

1. Insert the USB stick and boot from it.
2. Choose grub **entry 4** — Revert.

If the USB stick is not available, use the NVRAM reset (Scenario D,
Option 1).

The `PERMANENT_UNCONFIRMED` prompt is specifically timed to catch this:
if macOS did not boot successfully the first time after permanent install,
the user can press **N** at the shim's next invocation and the shim
uninstalls itself without requiring the USB stick.

---

## Scenario F — Shim working, later macOS upgrade breaks it

macOS major upgrades may relocate `boot.efi` or change the APFS Preboot
volume layout in ways the shim does not expect.

**Symptoms:** macOS no longer boots; the shim prints `macOS boot.efi not
found` and hangs.

**Recovery:**
1. Boot from the USB stick, grub entry 4 — Revert.
2. After revert, upgrade the USB ISO to a newer version if available.
3. Re-run the full detect-install flow from MASKING.md.

---

## Clean-up from macOS Terminal (post-revert)

After an NVRAM reset the `\EFI\MASK\` directory may remain on the internal
ESP.  Remove it once macOS is running:

```bash
# Find the internal ESP
diskutil list | grep EFI

# Mount it (example: disk0s1)
sudo diskutil mount /dev/disk0s1

# Remove mask files
sudo rm -rf /Volumes/EFI/EFI/MASK

# Unmount
sudo diskutil unmount /dev/disk0s1
```

---

## Summary

| Situation | Fastest revert |
|---|---|
| TRIAL_PENDING (no internal changes yet) | Grub entry 4, or NVRAM reset |
| PERMANENT + USB available | Grub entry 4 |
| PERMANENT_UNCONFIRMED, macOS failed | Press N at shim prompt |
| PERMANENT + no USB, Mac boots | NVRAM reset, then clean up ESP from Terminal |
| Shim causes boot loop | Grub entry 4 (USB), or NVRAM reset |
| macOS upgrade broke shim | Grub entry 4, then reinstall |
