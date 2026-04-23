# SPDX-License-Identifier: GPL-2.0
# Build all EFI helper applications for the a1990-memtest project.
# Targets:
#   all       — build mask-shim.efi, install.efi, revert.efi
#   test      — run hosted parser tests (uses host CC, no EFI env needed)
#   clean     — remove build artifacts

CC ?= gcc

.PHONY: all test clean

all: mask-shim/mask-shim.efi mask-install/install.efi revert/revert.efi \
     brr-entry/brr-entry.efi

# Shared dependency for any target that includes mask-common.
MASK_COMMON_SRCS = mask-common/mask_ops.c mask-common/mask_ops.h \
                   efi_util.c efi_util.h efi_types.h badmem_parse.c badmem_parse.h

mask-shim/mask-shim.efi: mask-shim/main.c mask-shim/cfl_decode_shim.c \
                          mask-shim/cfl_decode_shim.h mask-shim/board_shim.c \
                          efi_util.c badmem_parse.c \
                          efi_types.h efi_util.h badmem_parse.h string.h \
                          mask-common/mask_ops.c mask-common/mask_ops.h \
                          ../src/board_topology.h ../src/board_topology.c \
                          ../src/board_table.c
	$(MAKE) -C mask-shim

mask-install/install.efi: mask-install/main.c $(MASK_COMMON_SRCS)
	$(MAKE) -C mask-install

revert/revert.efi: revert/main.c $(MASK_COMMON_SRCS)
	$(MAKE) -C revert

brr-entry/brr-entry.efi: brr-entry/main.c $(MASK_COMMON_SRCS)
	$(MAKE) -C brr-entry

test: mask-shim/test_parse.c badmem_parse.c badmem_parse.h
	$(CC) -std=gnu11 -Wall -Wextra -o /tmp/test_parse mask-shim/test_parse.c
	/tmp/test_parse

clean:
	$(MAKE) -C mask-shim clean
	$(MAKE) -C mask-install clean
	$(MAKE) -C revert clean
	$(MAKE) -C brr-entry clean
	rm -f /tmp/test_parse
