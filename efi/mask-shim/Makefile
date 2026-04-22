# SPDX-License-Identifier: GPL-2.0
# Build mask-shim.efi — a standalone EFI application (PE32+, x86_64).
# Requires gnu-efi (available in the Docker build image as `gnu-efi` package).
# Output: mask-shim.efi in this directory.

CC      ?= gcc
LD      ?= ld
OBJCOPY ?= objcopy

# gnu-efi install paths (Debian bookworm).
GNUEFI_LIB ?= /usr/lib
GNUEFI_INC ?= /usr/include/efi

# Compilation flags for EFI freestanding code.
CFLAGS = -std=gnu11 -Wall -Wextra \
         -ffreestanding -fno-stack-protector -fno-builtin \
         -mno-red-zone -fshort-wchar \
         -m64 -march=x86-64 \
         -fpic \
         -DEFI_BUILD \
         -I.. \
         -I../mask-common \
         -I../../src

# Object files to link.
OBJS_LOCAL = main.o mask_ops.o efi_util.o badmem_parse.o cfl_decode_shim.o board_shim.o

.PHONY: all clean

all: mask-shim.efi

main.o: main.c ../efi_types.h ../efi_util.h ../badmem_parse.h \
        ../mask-common/mask_ops.h \
        cfl_decode_shim.h ../../src/board_topology.h
	$(CC) $(CFLAGS) -c -o $@ $<

mask_ops.o: ../mask-common/mask_ops.c ../mask-common/mask_ops.h \
            ../efi_types.h ../efi_util.h ../badmem_parse.h
	$(CC) $(CFLAGS) -c -o $@ $<

efi_util.o: ../efi_util.c ../efi_util.h ../efi_types.h
	$(CC) $(CFLAGS) -c -o $@ $<

badmem_parse.o: ../badmem_parse.c ../badmem_parse.h
	$(CC) $(CFLAGS) -c -o $@ $<

cfl_decode_shim.o: cfl_decode_shim.c cfl_decode_shim.h
	$(CC) $(CFLAGS) -c -o $@ $<

board_shim.o: board_shim.c ../../src/board_topology.h \
              ../../src/board_topology.c ../../src/board_table.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Link into an ELF shared object using gnu-efi's crt0 and linker script.
mask-shim.so: $(OBJS_LOCAL)
	$(LD) -nostdlib -znocombreloc \
	    -T $(GNUEFI_LIB)/elf_x86_64_efi.lds \
	    $(GNUEFI_LIB)/crt0-efi-x86_64.o \
	    $(OBJS_LOCAL) \
	    -o $@ \
	    -shared -Bsymbolic \
	    -L$(GNUEFI_LIB) -lefi -lgnuefi

# Convert ELF shared object to PE32+ EFI application.
mask-shim.efi: mask-shim.so
	$(OBJCOPY) \
	    -j .text -j .sdata -j .data -j .rodata \
	    -j .dynamic -j .dynsym -j .rel -j .rela \
	    -j .reloc \
	    --target pei-x86-64 \
	    --subsystem=10 \
	    $< $@

clean:
	rm -f *.o *.so mask-shim.efi test_parse
