/* SPDX-License-Identifier: GPL-2.0 */
/* EFI application linker script for x86_64.
 * Produces a shared library that objcopy --target=efi-app-x86_64 converts
 * to a PE32+ EFI binary.
 */

OUTPUT_FORMAT("elf64-x86-64");
OUTPUT_ARCH(i386:x86-64);

ENTRY(efi_main);

SECTIONS {
    . = 0;

    .text . : ALIGN(4096) {
        *(.text)
        *(.text.*)
    }

    .rodata . : ALIGN(4096) {
        *(.rodata)
        *(.rodata.*)
    }

    .data . : ALIGN(4096) {
        *(.sdata)
        *(.data)
        *(.data.*)
        *(.got)
        *(.got.plt)
    }

    .dynamic . : ALIGN(4096) { *(.dynamic) }
    .dynsym  . : ALIGN(4096) { *(.dynsym) }
    .dynstr  . : ALIGN(4096) { *(.dynstr) }
    .rel     . : ALIGN(4096) { *(.rel.*) }
    .rela    . : ALIGN(4096) { *(.rela.*) }
    .reloc   . : ALIGN(4096) { *(.reloc) }

    .bss . : ALIGN(4096) {
        *(.sbss)
        *(.bss)
        *(.bss.*)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.note.*)
        *(.comment*)
        *(.eh_frame*)
    }
}
