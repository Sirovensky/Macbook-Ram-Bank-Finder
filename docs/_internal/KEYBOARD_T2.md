# Keyboard input on A1990 (T2 Mac) — root cause and workaround

## Root cause

MacBook Pro A1990 (820-01814, Coffee Lake-H) uses Apple's T2 security
chip (T8012) for a number of functions including Secure Boot, encrypted
storage, and the embedded controller.  The internal keyboard connects to
the T2, not directly to the Intel PCH.  T2 exposes the keyboard to the
host over a vendor-specific USB HID protocol riding on an internal PCIe
link (XHCI controller on an Apple-proprietary USB fabric).

### Why memtest86plus has no keyboard input on A1990

memtest86plus calls `ExitBootServices()` in `boot/efisetup.c` early in
startup, before running any tests.  After this call:

1. All UEFI boot services, including `SystemTable->ConIn`, are destroyed.
2. memtest's own USB stack (xHCI/EHCI/UHCI drivers in `system/xhci.c`
   etc.) takes over input via `system/keyboard.c::get_key()`.

The problem is twofold:

- The T2's XHCI controller requires a vendor-specific handshake / power
  sequence that the generic xHCI driver in memtest does not implement.
  memtest's driver fails to enumerate any devices on the T2 XHCI
  controller — the device list comes up empty.

- Even if the XHCI controller enumerated correctly, T2 routes the
  internal keyboard through its own USB HID multiplexer.  A plain
  enumeration would not surface the keyboard without the Apple-specific
  protocol glue.

### Why external USB-C keyboards also fail

Confirmed by testing: an external USB-C HID keyboard connected via USB-C
also produces no input.  This is because the USB-C port on A1990 is
wired through the T2's Thunderbolt/USB fabric, not a standard Intel PCH
root hub.  memtest's xHCI driver fails to enumerate the T2 XHCI
controller entirely, so external keyboards on that controller are equally
unreachable.

### Why GRUB works

GRUB reads keyboard input via `SystemTable->ConIn->ReadKeyStroke()`, a
UEFI boot-services call.  The Apple EFI firmware supplies ConIn backed by
its own T2 HID driver (closed-source, part of the firmware).  Because
GRUB runs before `ExitBootServices()`, it has full access to
ConIn — which is why GRUB's menu navigation works perfectly on A1990.

## Workaround (current implementation)

ConIn works before `ExitBootServices()` and is the only reliable input
path on A1990, so **all interactive UI in BRR happens pre-EBS**.

Two places use ConIn:

1. **`efi/brr-entry/main.c`** — the full line-editor for address entry
   (entry 3 in grub).  Uses
   `SystemTable->ConIn->ReadKeyStroke()` for hex digits and
   backspace, `SystemTable->ConOut->OutputString()` for echo and
   prompts.  Pre-EBS, so keyboard works reliably.
2. **`efi/mask-shim/main.c`** — the `PERMANENT_UNCONFIRMED` Y/N
   prompt.  Also pre-EBS.  Polls with `BootServices->Stall(10000)`
   (10 ms per tick) on a 30 s timeout.

The earlier interactive pre-boot menu in `src/efi_menu.c` (with keys
`[Enter]`/`[P]`/`[C]`/`[T]`/`[R]` + 30 s countdown) has been removed.
`efi_menu()` is now a thin cmdline parser that returns
`BRR_FLAG_SKIP_COUNTDOWNS` when grub passes `brr_fast`, otherwise 0.
The flag bit is consumed by `src/calibration.c` to skip the photo-
pause countdowns.

Historical note: the old menu stored its choice in
`boot_params_t::brr_flags` (previously called `a1990_flags`, offset
`0x23c` in the Linux boot protocol's gap 7).  That field still exists
and still carries `SKIP_COUNTDOWNS` / `CALIBRATE_ONLY` flag bits; the
other bits (`TRIAL_PAGE`, `TRIAL_CHIP`, `AUTO_REBOOT_AFTER_PASS`,
`AUTO_TRIAL_CHIP`) are gone because no code writes or reads them
anymore.

## Known limitations

- There is **no keyboard input during test runs**.  After `ExitBootServices()`
  ConIn is gone and memtest's xHCI stack cannot reach the keyboard on A1990.
  To abort a test run, use the power button to force-reboot.

- **Mouse input** is not supported and is out of scope.  GRUB's mouse
  support relies on EFI mouse protocols that are equally destroyed after
  `ExitBootServices()`.

- The workaround is tested only in UEFI boot mode (the normal path for A1990).
  Legacy BIOS mode is not applicable.

- On non-Apple hardware (QEMU, generic x86 PCs), ConIn works normally and
  the pre-boot menu appears and functions identically.  There is no regression
  for non-T2 platforms.
