# SPDX-License-Identifier: GPL-2.0
# Build brr-entry.efi — pre-EBS address-entry UI.  User types bad page
# addresses (hex, comma-separated), we build a BrrBadPages NVRAM blob
# (with +/-1 MiB padding per address), set BrrMaskState=TRIAL_PENDING_PAGE,
# and chainload mask-shim.efi which applies the mask and boots macOS.

CC      ?= gcc
LD      ?= ld
OBJCOPY ?= objcopy

GNUEFI_LIB ?= /usr/lib
GNUEFI_INC ?= /usr/include/efi

CFLAGS = -std=gnu11 -Wall -Wextra \
         -ffreestanding -fno-stack-protector -fno-builtin \
         -mno-red-zone -fshort-wchar \
         -m64 -march=x86-64 \
         -fpic \
         -DEFI_BUILD \
         -I.. \
         -I../mask-common

OBJS_LOCAL = main.o mask_ops.o efi_util.o badmem_parse.o

.PHONY: all clean

all: brr-entry.efi

main.o: main.c ../efi_types.h ../efi_util.h ../badmem_parse.h \
        ../mask-common/mask_ops.h
	$(CC) $(CFLAGS) -c -o $@ $<

mask_ops.o: ../mask-common/mask_ops.c ../mask-common/mask_ops.h \
            ../efi_types.h ../efi_util.h ../badmem_parse.h
	$(CC) $(CFLAGS) -c -o $@ $<

efi_util.o: ../efi_util.c ../efi_util.h ../efi_types.h
	$(CC) $(CFLAGS) -c -o $@ $<

badmem_parse.o: ../badmem_parse.c ../badmem_parse.h
	$(CC) $(CFLAGS) -c -o $@ $<

brr-entry.so: $(OBJS_LOCAL)
	$(LD) -nostdlib -znocombreloc \
	    -T $(GNUEFI_LIB)/elf_x86_64_efi.lds \
	    $(GNUEFI_LIB)/crt0-efi-x86_64.o \
	    $(OBJS_LOCAL) \
	    -o $@ \
	    -shared -Bsymbolic \
	    -L$(GNUEFI_LIB) -lefi -lgnuefi

brr-entry.efi: brr-entry.so
	$(OBJCOPY) \
	    -j .text -j .sdata -j .data -j .rodata \
	    -j .dynamic -j .dynsym -j .rel -j .rela \
	    -j .reloc \
	    --target pei-x86-64 \
	    --subsystem=10 \
	    $< $@

clean:
	rm -f *.o *.so brr-entry.efi
