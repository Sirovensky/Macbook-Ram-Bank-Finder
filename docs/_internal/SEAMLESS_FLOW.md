# Seamless ram-fix flow — design

## User-visible

```
Grub menu:
  1. Automatic: memtest + ram-fix  [DEFAULT]
  2. Run memtest only
  3. Boot macOS normally (no mask)
  4. Revert all changes             [REVERT]
```

Entry 1 auto-chooses action based on `A1990MaskState`:

- `NONE` → show EFI menu asking [P]age / [C]hip / [Enter=memtest only]; on P/C, run
  memtest → NVRAM save → set `TRIAL_PENDING_{PAGE,CHIP}` → auto-reboot.
- `TRIAL_PENDING_{PAGE,CHIP}` → auto-launch mask-shim: apply mask, set `TRIAL_BOOTED`,
  chain internal macOS.
- `TRIAL_BOOTED` → prompt **twice** "Make permanent? Y/N". Y,Y → full EFI backup +
  install.efi logic → `PERMANENT_UNCONFIRMED`. Y,N or N → chain macOS w/ mask
  still in trial; prompt repeats on next boot.
- `PERMANENT_UNCONFIRMED` → prompt once "Did macOS boot OK? Y = confirm, N = revert".
  Y → `PERMANENT_CONFIRMED`. N → invoke revert inline.
- `PERMANENT_CONFIRMED` → silent; chain macOS.

Entry 4 (Revert) always works regardless of state. Full teardown.

## State machine

```
NONE
  ↓ [P] in EFI menu
TRIAL_PENDING_PAGE            TRIAL_PENDING_CHIP
  ↓ auto on next USB boot        ↓ auto on next USB boot
TRIAL_BOOTED (shim applied + chained macOS successfully)
  ↓ user boots USB again
  ↓ "Make permanent? [Y/N]"
  ↓ "Really? [Y/N]"
  ├── Y,Y → PERMANENT_UNCONFIRMED  → next boot: confirm
  │           ↓ Y                 ↓ N
  │         PERMANENT_CONFIRMED   revert (→ NONE)
  └── N or Y,N → stay TRIAL_BOOTED (prompt repeats)
```

## NVRAM variables (vendor GUID `3E3E9DB2-1A2B-4B5C-9D1E-5F6A7B8C9D0E`)

| Var | Type | Meaning |
|---|---|---|
| `A1990MaskState` | string | one of `NONE`, `TRIAL_PENDING_PAGE`, `TRIAL_PENDING_CHIP`, `TRIAL_BOOTED`, `PERMANENT_UNCONFIRMED`, `PERMANENT_CONFIRMED` |
| `A1990BadPages` | binary (header + u64 PAs) | accumulator from memtest, region-mode input |
| `A1990BadChips` | string | comma-separated designators (`U2620,U2310`) for chip-mode |
| `A1990BackupBootOrder` | binary (UINT16[]) | pre-install BootOrder |
| `A1990BackupBootEntries` | string | list of our-created BootNNNN slots (for revert) |
| `A1990MaskBootSlot` | UINT16 | our BootNNNN slot (existing) |

## Backup before PERMANENT install

1. Read current `BootOrder` → save as `A1990BackupBootOrder`.
2. Enumerate all `Boot*` vars; note which exist (so if we create `Boot1234`, we know
   it was ours, not pre-existing).
3. Copy pre-existing `\EFI\MASK\` contents on internal ESP (if any) to
   `\EFI\MASK\backup\` before overwriting.
4. Proceed with install.

## Revert (entry 4)

1. Read `A1990BackupBootOrder` → write back as `BootOrder`.
2. Read `A1990MaskBootSlot` → delete that `BootNNNN` variable.
3. Delete `\EFI\MASK\*` on internal ESP.
4. Delete all our NVRAM vars: `A1990MaskState`, `A1990BadPages`, `A1990BadChips`,
   `A1990BackupBootOrder`, `A1990MaskBootSlot`.
5. Reboot.

Idempotent: safe to run from any state (`NONE` state = no-op).

## Keyboard reality

All interactive prompts happen **pre-ExitBootServices** using `ConIn`. No prompts
post-memtest. User decides chip vs page **before** the test runs (acceptable — they
know from prior fault reports which chip is suspect, or pick page mode to be safe).

Permanent / revert prompts happen on **subsequent USB boot** when EFI menu sees
`TRIAL_BOOTED` or `PERMANENT_UNCONFIRMED` states.
