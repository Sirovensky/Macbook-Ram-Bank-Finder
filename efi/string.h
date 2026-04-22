// SPDX-License-Identifier: GPL-2.0
//
// Minimal string.h for the EFI freestanding build.
//
// Covers common libc string/mem functions as static inlines so the
// linker never sees an unresolved `memcpy` / `memset` etc. emitted by
// GCC for struct assignments or large initializers, even though we
// compile with `-fno-builtin -ffreestanding`.  Defensive; future
// EFI-side code can rely on these being present.

#ifndef EFI_STRING_H
#define EFI_STRING_H

#include <stddef.h>

static inline int strncmp(const char *a, const char *b, size_t n)
{
    while (n-- > 0) {
        unsigned char ca = (unsigned char)*a;
        unsigned char cb = (unsigned char)*b;
        if (ca != cb) return (int)ca - (int)cb;
        if (ca == 0) return 0;
        a++; b++;
    }
    return 0;
}

static inline int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static inline size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static inline char *strcpy(char *d, const char *s)
{
    char *r = d;
    while ((*d++ = *s++)) { }
    return r;
}

static inline void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *dd = (unsigned char *)d;
    const unsigned char *ss = (const unsigned char *)s;
    while (n--) *dd++ = *ss++;
    return d;
}

static inline void *memmove(void *d, const void *s, size_t n)
{
    unsigned char *dd = (unsigned char *)d;
    const unsigned char *ss = (const unsigned char *)s;
    if (dd < ss) {
        while (n--) *dd++ = *ss++;
    } else {
        dd += n; ss += n;
        while (n--) *--dd = *--ss;
    }
    return d;
}

static inline void *memset(void *d, int c, size_t n)
{
    unsigned char *dd = (unsigned char *)d;
    while (n--) *dd++ = (unsigned char)c;
    return d;
}

static inline int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    while (n-- > 0) {
        if (*x != *y) return (int)*x - (int)*y;
        x++; y++;
    }
    return 0;
}

#endif // EFI_STRING_H
