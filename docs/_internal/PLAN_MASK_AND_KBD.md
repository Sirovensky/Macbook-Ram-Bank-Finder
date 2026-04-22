# Plan: EFI bad-memory mask shim + keyboard input fix

Two independent tracks. Can be dispatched to parallel sonnet sub-agents —
no shared source files beyond `Makefile` / `scripts/apply.sh` (merge at
end).

---

## Track A — EFI bootshim that masks bad DRAM (Option B), revertable

### Goal

Single USB stick does both:
1. Run memtest86plus, detect bad physical addresses, write list to ESP.
2. On subsequent boots, an EFI application (`mask-shim.efi`) reads the
   list, reserves those pages via UEFI `AllocatePages`, then
   chain-loads macOS's `boot.efi` from the internal APFS Preboot.

macOS sees the reserved ranges in its UEFI memory map and skips them.

**Revertability:** three install states, user-driven transitions:

| State | What persists | Boot source | Revert action |
|---|---|---|---|
| `NONE` | nothing | USB or internal, normal | — |
| `TRIAL` | nothing on disk | USB with `badmem.txt` + shim | pull USB, reboot |
| `PERMANENT` | shim + badmem on internal ESP, EFI `BootOrder` updated | internal ESP → shim → macOS | boot USB, pick "Uninstall" — OR `bless` restore from macOS — OR NVRAM reset |

Only PERMANENT persists across USB removal. User reaches PERMANENT by
explicit confirmation ("did the fix work? yes → install permanently").

### Boot flow (revised grub menu — 5 entries)

```
MacBook A1990 Memtest / Masker
──────────────────────────────
> 1. Run memtest (detect bad chips)
  2. Boot macOS with mask from USB (TRIAL)
  3. Boot macOS normally (no mask)
  4. Install mask to internal disk (PERMANENT)
  5. Uninstall mask from internal disk (REVERT)
```

- Entry 1 runs memtest86plus (current path).
- Entry 2 loads `mask-shim.efi` from USB, reads `badmem.txt` from USB,
  reserves pages, chain-loads internal macOS `boot.efi`. One-shot;
  nothing persisted.
- Entry 3 directly chain-loads internal `boot.efi` with no shim.
  Useful for A/B comparison and as a safe default.
- Entry 4 runs an installer EFI (`install.efi`) that copies
  `mask-shim.efi` + `badmem.txt` to the **internal** ESP, registers
  a new EFI boot entry via `SetVariable` on `BootNNNN` + `BootOrder`.
  Then reboots. Next boot will use the internal shim → no USB
  needed.
- Entry 5 runs `install.efi --uninstall`: removes the internal copies,
  deletes the `BootNNNN`, restores original `BootOrder`. Reboots.

### Permanent install — post-trial confirmation flow

When the internal shim runs in `PERMANENT` state, on each boot it
checks the NVRAM variable `A1990MaskState`:

- `PERMANENT_UNCONFIRMED` — installed but user hasn't confirmed it
  works across a full cold boot. Shim adds a "Did macOS boot OK after
  the fix? [Y to confirm / N to uninstall]" prompt on the *next* boot
  via text-mode ConIn (pre-ExitBootServices, works on T2).
  - Y → set state to `PERMANENT_CONFIRMED`, proceed to macOS.
  - N → automatically run uninstall path, remove everything, reboot.
  - timeout (no key) → proceed to macOS but stay in
    `PERMANENT_UNCONFIRMED` so prompt repeats next boot.
- `PERMANENT_CONFIRMED` — silent boot to macOS.
- Missing / `NONE` — shim not active; direct macOS boot (shouldn't
  happen if shim is running — it's a sanity fallback).

Rationale: user explicitly commits only after seeing macOS run stable.
If permanent install breaks boot, user can revert from USB entry 5
even when stuck in a reboot loop.

### Actionable items

**A1. Define the badmem list format.**
- Location: FAT partition inside the ISO/USB, path
  `/EFI/BOOT/badmem.txt`.
- Format: one range per line — `0xSTART,LEN_BYTES`. Optional `#` comments.
- Rationale: trivial to parse, human-editable (add/remove entries by
  hand to tune).
- Deliverable: `docs/BADMEM_FORMAT.md`.

**A2. Give memtest write access to the ESP.**
- Currently ISO is ISO9660 + ESP (read-only). Need a writable FAT32
  data partition *or* write support for the existing ESP via
  memtest's USB stack.
- Simplest path: add a 3rd GPT partition (type `EBD0A0A2`, MSFT Basic
  Data) with a small FAT32 volume labeled `MT86P_LOG`. Already a
  pre-existing P2 TODO ("separate FAT32 data partition for Log.txt").
- Deliverable: update `scripts/build-iso.sh` to append the partition;
  tweak `scripts/docker-build.sh` if required.
- Blocker: memtest86plus USB block-device writes are unimplemented.
  Workaround v1: user transfers `badmem.txt` manually off the machine
  after seeing the report, then mounts USB on a working Mac and adds
  the file. Defer write-back to later.

**A3. Wire error_hook to emit badmem entries.**
- On every `board_report_error()`, append `0xPA,4096` (one page) to an
  in-memory buffer.
- After tests finish (or on reboot trigger), print the buffer to the
  scroll area in badmem format. User photographs and types into
  `badmem.txt` on a working Mac.
- File: `src/error_hook.c` + new `src/badmem_log.c`.
- Deliverable: at end of run, a full `badmem.txt`-ready dump on screen.

**A4. Scaffold `efi/mask-shim/` EFI application.**
- New directory: `efi/mask-shim/` with `main.c`, `Makefile`,
  `README.md`.
- Use the same gnu-efi or EDK2-lite headers memtest86plus already
  bundles (`memtest86plus/boot/efi.h`). Stand-alone PE32+ output.
- Entry point:
  ```c
  EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st);
  ```
- Deliverable: builds `mask-shim.efi` inside the same Docker build.

**A5. Implement the masking logic.**
- Open volume containing `/EFI/BOOT/badmem.txt` via SimpleFileSystem
  protocol on the shim's own loaded-image device.
- Parse line-by-line. For each `(addr, len)`:
  - Round addr down to 4 KiB, len up to 4 KiB multiple.
  - `gBS->AllocatePages(AllocateAddress, EfiReservedMemoryType,
    pages, &addr)`. On failure log+continue (range may be already
    reserved by firmware).
- Count successful reservations, print summary on screen.

**A6. Chain-load macOS `boot.efi`.**
- Path: internal APFS Preboot volume's
  `\System\Library\CoreServices\boot.efi`.
- T2 Startup Security Utility must be at "Medium"+AllowExternal OR
  "None"; same constraint as memtest (already documented).
- Discovery: enumerate block-io handles, probe each for a Preboot
  volume, look for the above path. Prefer internal disk (HDD=not
  USB).
- Use `LoadImage` with the discovered device path, then `StartImage`.
- Deliverable: shim successfully hands off to macOS.

**A7. Trial-mode revert (USB-based).**
- Grub menu entry 3 skips the shim and directly chain-loads internal
  `boot.efi`. No code change in shim — just a new `grub.cfg` entry.
- Document: "pull USB, reboot" also works (nothing on disk
  persists in trial mode).
- Deliverable: `grub/a1990-efi.cfg` update.

**A8. Integrate into ISO build.**
- `mask-shim.efi` + `install.efi` copied into ESP at
  `/EFI/BOOT/`.
- `build-iso.sh` updated.
- `grub.cfg` entries launch each binary with appropriate args.

**A9. Unit-test parser (hosted).**
- Extract `badmem.txt` parser into a pure-C module.
- Hosted test: `efi/mask-shim/test_parse.c` that reads sample input,
  prints ranges, asserts count.
- `make test-shim` target.

**A10. Document.**
- `docs/MASKING.md`: user-facing guide — when to use, how to fill
  `badmem.txt`, trial → permanent path, how to revert at each stage.

**A11. Permanent install path (`install.efi`).**
- New EFI app `efi/mask-install/main.c`.
- Must be invokable both interactively (from grub entry 4) and
  non-interactively (future: from within shim after Y-confirmation).
- Steps:
  1. Locate internal disk's ESP via SimpleFileSystem handle
     enumeration + partition type check (`\\EFI\\` directory present).
  2. Copy `\\EFI\\BOOT\\mask-shim.efi` and `\\EFI\\BOOT\\badmem.txt`
     from USB (current loaded-image device) to the internal ESP
     under `\\EFI\\MASK\\mask-shim.efi` and `\\EFI\\MASK\\badmem.txt`.
  3. Create a `BootNNNN` EFI variable using `SetVariable` with
     `EFI_VARIABLE_NON_VOLATILE | BOOTSERVICE_ACCESS | RUNTIME_ACCESS`:
     - Load option description: `"A1990 bad-RAM mask"`.
     - File path: internal ESP `\\EFI\\MASK\\mask-shim.efi`.
  4. Read current `BootOrder`, prepend our new `NNNN`, write back.
  5. Save original `BootOrder` in our own NVRAM var
     `A1990MaskOriginalBootOrder` for revert.
  6. Set `A1990MaskState = PERMANENT_UNCONFIRMED`.
  7. Reboot.
- Failure modes: ESP write denied (SIP?) → report, abort, no
  partial state. ESP full → report, abort.

**A12. Uninstall path (`install.efi --uninstall`).**
- Reverse of A11:
  1. Delete files from internal ESP.
  2. Read `A1990MaskOriginalBootOrder`, restore as `BootOrder`.
  3. Delete our `BootNNNN` variable.
  4. Delete state vars (`A1990MaskState`, `A1990MaskOriginalBootOrder`).
  5. Reboot.
- Idempotent: if not installed, no-op + informational message.

**A13. Post-install confirmation in shim.**
- Internal shim (running from permanent install) checks
  `A1990MaskState` at start.
- If `PERMANENT_UNCONFIRMED`: display confirm prompt via
  ConIn (pre-ExitBootServices; works on T2 keyboard) for
  up to 30 s.
  - Y → set state `PERMANENT_CONFIRMED`; proceed.
  - N → invoke uninstall inline (skip the double-reboot); proceed
        to macOS without mask; next boot is clean.
  - timeout → proceed to macOS, state stays `PERMANENT_UNCONFIRMED`;
        prompt again next boot.
- If `PERMANENT_CONFIRMED`: silent; proceed.

**A14. NVRAM state machine.**
- Variables under vendor GUID (pick one random, doc it):
  - `A1990MaskState` (string): one of `NONE`, `PERMANENT_UNCONFIRMED`,
    `PERMANENT_CONFIRMED`.
  - `A1990MaskOriginalBootOrder` (byte array): for revert.
- Document in `docs/MASKING.md`.

**A15. Stuck-state recovery.**
- Scenario: permanent install boots OK, user confirms, but later a
  macOS upgrade breaks or hardware fault worsens; system won't boot.
- Recovery paths, documented:
  - Boot USB, pick entry 5 (Uninstall).
  - Boot without mask: NVRAM reset (Cmd+Opt+P+R) — clears our
    `BootOrder` tweak too.
  - From macOS recovery (⌘R): `bless` restore.
- Deliverable: `docs/MASKING_RECOVERY.md`.

### Acceptance criteria (Track A)

- `make iso` produces a 5-entry grub menu.
- Entry 1 runs memtest + prints badmem list.
- Entry 2 trial-masks macOS from USB. macOS boots with reduced RAM.
- Entry 3 normal macOS boot (no mask). Confirms non-destructive.
- Entry 4 installs permanently. Reboots; internal-disk boot now goes
  through shim. Next boot prompts "did it work?" — Y persists.
- Entry 5 uninstalls cleanly. Internal ESP cleaned, EFI
  `BootOrder` restored.
- Verified: USB removed → internal-masked boot still works after
  permanent install.
- Verified: NVRAM reset wipes our install state (recovery path).

---

## Track B — Keyboard input in memtest on A1990

### Root cause

GRUB works because it uses UEFI `SystemTable->ConIn->ReadKeyStroke`
which is backed by firmware's own HID driver (T2 passes internal
keyboard through to firmware).

memtest86plus calls `ExitBootServices()` early in its EFI loader
(`memtest86plus/boot/efisetup.c`), then uses its own xHCI/EHCI/UHCI
USB stack + PS/2 to poll input (`system/keyboard.c::get_key()`).

- Post-ExitBootServices, UEFI ConIn is gone.
- T2 hides internal keyboard behind a vendor-specific USB handoff
  that memtest's xHCI driver doesn't navigate. → dead input.

### Confirmed: external USB-C keyboard does NOT work

User tested a USB-C HID keyboard — dead. Means memtest's xHCI stack
itself fails to enumerate devices on the T2 XHCI controller, not just
the internal keyboard. Rules out the "plug a dongle" workaround.

Only remaining fix: **run interactive input through UEFI ConIn while
boot services are still alive**. ConIn is driven by firmware's own
USB/HID driver, which is what GRUB uses (confirmed working).

Constraint: ConIn dies the moment memtest calls `ExitBootServices()`
(currently in `memtest86plus/boot/efisetup.c`). Solution is to make
all interactive UI happen *before* that call.

### Scope

- A pre-test ConIn menu for the single interactive decision point
  ("run tests / run calibration-only / reboot").
- Post-ExitBootServices, tests run headless with fixed-timed
  countdowns (already our fork's behavior — no regression).
- Mouse: out of scope. GRUB handles mouse via firmware drivers;
  memtest has never used mouse input on any platform.

### Actionable items

**B1. Implement ConIn-backed pre-boot menu.**
- New files: `src/efi_menu.c`, `src/efi_menu.h`.
- Hook point: `memtest86plus/boot/efisetup.c` — call the menu
  *before* the `ExitBootServices()` code path (near line 337; see
  context already captured). Inject via a patch under `patches/`.
- Uses `SystemTable->ConIn->ReadKeyStroke(ConIn, &key)` polling
  with `gBS->Stall(10000)` (10 ms) to pace. Plain text output via
  `ConOut->OutputString()`.
- Menu:
  ```
  A1990 Memtest — press a key (timeout 30 s → Run tests)
    [Enter] Run all tests
    [C]     Run calibration dump only, then halt
    [R]     Reboot
    [T]     Skip on-screen countdowns during tests (fast mode)
  ```
- Timeout default = Run tests (so current "just boot it" behavior
  is preserved; menu is opt-in).
- Chosen action stored in a magic memory page or `boot_params_t`
  extension, consumed by app/main.c.

**B2. Pipe menu flags into the app.**
- Extend `boot_params_t` with a reserved `uint32_t a1990_flags`
  field (patch `memtest86plus/boot/bootparams.h` and loader).
- Flags:
  - `A1990_FLAG_SKIP_COUNTDOWNS` — calibration skips timed photo
    pauses.
  - `A1990_FLAG_CALIBRATE_ONLY` — run calibration, halt.
- app/main.c honors both.

**B3. Grub cfg sanity check.**
- Existing entries already point at memtest's loader (`mt86plus`).
  No change expected; verify grub cfg compiles with the new 5-entry
  list from Track A.

**B4. Test.**
- QEMU UEFI run: boots, ConIn menu appears, timeout-runs tests.
- A1990 hardware: boot ISO, confirm internal keyboard navigates
  menu. This is the critical acceptance test.

**B5. Document.**
- `docs/KEYBOARD_T2.md`: one-page explanation — root cause (memtest's
  USB stack vs T2), why external USB-C keyboards also fail, why
  ConIn works pre-ExitBootServices, and known limitations (no input
  during test runs — if you need to abort, reboot via power button).

### Acceptance criteria (Track B)

- Memtest ISO boots on A1990.
- At the pre-test menu, pressing Enter (internal keyboard) starts
  tests.
- Pressing C runs calibration only.
- Pressing R reboots.
- No regression on non-Apple hardware — generic ConIn works
  everywhere UEFI is present.

---

## Subagent dispatch plan

Two sonnet subagents in parallel (independent file touch-sets):

- **Agent-A** → Track A items A1–A15.
- **Agent-B** → Track B items B1–B5.

Merge surface is minimal: both touch `scripts/build-iso.sh`,
`grub/a1990-efi.cfg`, and `Makefile`. Reconcile those after both
finish.

After subagents return, humans (you + me):
1. Review both sets of diffs.
2. Resolve merge conflicts in shared files.
3. Run `make iso` → boot on A1990 → accept.

### Out of scope (this plan)

- Writable FAT on USB from within memtest (punted to separate TODO).
- Persistent NVRAM-based masking.
- Mouse input (GRUB menu mouse support is a GRUB feature, not memtest;
  memtest never used mouse).
- BGA rework tooling.
