// SPDX-License-Identifier: GPL-2.0
//
// Minimal text-output and string utilities for standalone EFI applications.

#include "efi_util.h"

// ---------------------------------------------------------------------------
// String utilities
// ---------------------------------------------------------------------------

UINTN efi_strlen16(const CHAR16 *s)
{
    UINTN n = 0;
    while (s[n]) n++;
    return n;
}

void efi_ascii_to_utf16(CHAR16 *dst, const char *src, UINTN max_chars)
{
    UINTN i = 0;
    while (src[i] && i + 1 < max_chars) {
        dst[i] = (CHAR16)(unsigned char)src[i];
        i++;
    }
    dst[i] = 0;
}

void efi_strcpy16(CHAR16 *dst, const CHAR16 *src)
{
    while ((*dst++ = *src++));
}

void efi_strcat16(CHAR16 *dst, const CHAR16 *src)
{
    while (*dst) dst++;
    efi_strcpy16(dst, src);
}

int efi_strcmp16(const CHAR16 *a, const CHAR16 *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)*a - (int)*b;
}

// ---------------------------------------------------------------------------
// Number formatting
// ---------------------------------------------------------------------------

void efi_fmt_hex(CHAR16 *buf, UINT64 val, int min_digits)
{
    static const CHAR16 hex[] = L"0123456789abcdef";
    CHAR16 tmp[18];
    int n = 0;
    if (val == 0) {
        tmp[n++] = '0';
    } else {
        UINT64 v = val;
        while (v) {
            tmp[n++] = hex[v & 0xf];
            v >>= 4;
        }
    }
    while (n < min_digits) tmp[n++] = '0';
    // reverse
    for (int i = 0; i < n / 2; i++) {
        CHAR16 t = tmp[i]; tmp[i] = tmp[n-1-i]; tmp[n-1-i] = t;
    }
    tmp[n] = 0;
    efi_strcpy16(buf, tmp);
}

void efi_fmt_dec(CHAR16 *buf, UINTN val)
{
    CHAR16 tmp[22];
    int n = 0;
    if (val == 0) {
        tmp[n++] = '0';
    } else {
        UINTN v = val;
        while (v) {
            tmp[n++] = (CHAR16)('0' + v % 10);
            v /= 10;
        }
    }
    for (int i = 0; i < n / 2; i++) {
        CHAR16 t = tmp[i]; tmp[i] = tmp[n-1-i]; tmp[n-1-i] = t;
    }
    tmp[n] = 0;
    efi_strcpy16(buf, tmp);
}

// ---------------------------------------------------------------------------
// Output helpers
// ---------------------------------------------------------------------------

void efi_print(EFI_SYSTEM_TABLE *st, const CHAR16 *s)
{
    if (st && st->ConOut)
        st->ConOut->OutputString(st->ConOut, (CHAR16 *)s);
}

void efi_printa(EFI_SYSTEM_TABLE *st, const char *s)
{
    CHAR16 buf[256];
    efi_ascii_to_utf16(buf, s, 256);
    efi_print(st, buf);
}

void efi_print_hex(EFI_SYSTEM_TABLE *st, UINT64 val)
{
    CHAR16 buf[20];
    efi_print(st, L"0x");
    efi_fmt_hex(buf, val, 1);
    efi_print(st, buf);
}

void efi_print_dec(EFI_SYSTEM_TABLE *st, UINTN val)
{
    CHAR16 buf[22];
    efi_fmt_dec(buf, val);
    efi_print(st, buf);
}

void efi_newline(EFI_SYSTEM_TABLE *st)
{
    efi_print(st, L"\r\n");
}

// ---------------------------------------------------------------------------
// Input / timing
// ---------------------------------------------------------------------------

void efi_stall_ms(EFI_SYSTEM_TABLE *st, UINTN ms)
{
    if (st && st->BootServices && st->BootServices->Stall)
        st->BootServices->Stall(ms * 1000); // Stall is in microseconds
}

CHAR16 efi_readkey(EFI_SYSTEM_TABLE *st)
{
    if (!st || !st->ConIn) return 0;
    EFI_INPUT_KEY key = {0, 0};
    EFI_STATUS s = st->ConIn->ReadKeyStroke(st->ConIn, &key);
    if (s == EFI_SUCCESS) return key.UnicodeChar;
    return 0;
}
