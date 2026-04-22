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

## Workaround (Track B)

Since ConIn works before `ExitBootServices()` and is the only reliable
input path on A1990, the fix is to perform all interactive UI while boot
services are still alive.

`src/efi_menu.c` implements a pre-boot menu called from `boot/efisetup.c`
immediately before `ExitBootServices()`.  The menu:

- Uses `SystemTable->ConIn->ReadKeyStroke()` for input.
- Uses `SystemTable->ConOut->OutputString()` for text output.
- Polls with `BootServices->Stall(10000)` (10 ms per tick) so it
  does not busy-spin.
- Waits 30 seconds for input, then defaults to "Run all tests".

The menu offers four choices:

| Key    | Action                                           |
|--------|--------------------------------------------------|
| Enter  | Run all memory tests (default / timeout)         |
| C      | Run calibration dump only, then halt             |
| R      | Reboot                                           |
| T      | Fast mode: skip timed photo countdowns           |

The chosen action is encoded as bit flags in `boot_params_t::a1990_flags`
(offset `0x23c`, the first four bytes of the Linux boot protocol's
unused "gap 7").  The application reads these flags after
`ExitBootServices()` is long past and acts accordingly:

- `A1990_FLAG_SKIP_COUNTDOWNS` (bit 0): `board_calibrate()` omits the
  timed photo pauses so the calibration screens cycle immediately.
- `A1990_FLAG_CALIBRATE_ONLY` (bit 1): `board_calibrate()` halts after
  printing the calibration data (IMC registers + board ID) instead of
  returning to start the memory tests.

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
