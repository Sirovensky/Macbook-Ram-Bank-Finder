// SPDX-License-Identifier: GPL-2.0
//
// Minimal text-output and string utilities for standalone EFI applications.
// No libc dependency — freestanding build only.

#ifndef EFI_UTIL_H
#define EFI_UTIL_H

#include "efi_types.h"

// Print a NUL-terminated UTF-16 string via ConOut.
void efi_print(EFI_SYSTEM_TABLE *st, const CHAR16 *s);

// Print a NUL-terminated ASCII string via ConOut (auto-converted to UTF-16).
void efi_printa(EFI_SYSTEM_TABLE *st, const char *s);

// Print a 64-bit hex value with "0x" prefix.
void efi_print_hex(EFI_SYSTEM_TABLE *st, UINT64 val);

// Print a decimal UINTN.
void efi_print_dec(EFI_SYSTEM_TABLE *st, UINTN val);

// Print "\r\n" (CRLF) — required by EFI text protocols.
void efi_newline(EFI_SYSTEM_TABLE *st);

// Length of a UTF-16 NUL-terminated string.
UINTN efi_strlen16(const CHAR16 *s);

// Copy ASCII string to CHAR16 buffer.  dst must be large enough.
void efi_ascii_to_utf16(CHAR16 *dst, const char *src, UINTN max_chars);

// Copy CHAR16 string (NUL-terminated).
void efi_strcpy16(CHAR16 *dst, const CHAR16 *src);

// Concatenate CHAR16 string into dst (which must have room).
void efi_strcat16(CHAR16 *dst, const CHAR16 *src);

// Compare two CHAR16 strings. Returns 0 if equal.
int efi_strcmp16(const CHAR16 *a, const CHAR16 *b);

// Simple busy-wait via BootServices->Stall (microseconds).
void efi_stall_ms(EFI_SYSTEM_TABLE *st, UINTN ms);

// Read one key from ConIn; returns 0 UnicodeChar on no key ready.
CHAR16 efi_readkey(EFI_SYSTEM_TABLE *st);

// Convert UINTN to hex string in buf (at least 18 bytes).
void efi_fmt_hex(CHAR16 *buf, UINT64 val, int min_digits);

// Convert UINTN to decimal string in buf (at least 22 bytes).
void efi_fmt_dec(CHAR16 *buf, UINTN val);

#endif /* EFI_UTIL_H */
