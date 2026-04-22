# Top-level convenience Makefile for a1990-memtest.
# Real build lives in memtest86plus/build/x86_64.

.PHONY: all apply build iso efi test-shim clean distclean docker docker-iso help

MT := memtest86plus
BUILD_DIR := $(MT)/build/x86_64

all: iso

apply:
	@./scripts/apply.sh

build: apply
	$(MAKE) -C $(BUILD_DIR)

iso: build
	@./scripts/build-iso.sh

# Build EFI helper applications (mask-shim.efi, install.efi).
# These are built separately from memtest86plus inside the same Docker image.
efi:
	$(MAKE) -C efi

# Hosted unit test for the badmem.txt parser.  Does not require Docker or
# any EFI toolchain — uses the host C compiler.
test-shim:
	$(MAKE) -C efi test

# Build inside Docker — use this from macOS/Windows hosts.
docker: docker-iso
docker-iso:
	@./scripts/docker-build.sh iso

clean:
	-$(MAKE) -C $(BUILD_DIR) clean
	-$(MAKE) -C efi clean
	-rm -rf dist
	-rm -f src/board_table.c

distclean: clean
	-rm -rf $(MT)/system/board $(MT)/system/a1990
	-cd $(MT) && git checkout -- app/error.c app/main.c system/screen.c system/smbios.c build/x86_64/Makefile 2>/dev/null || true
	-cd $(MT) && rm -f grub/a1990-*.cfg

help:
	@echo "targets:"
	@echo "  apply      - generate board_table.c, sync into memtest86plus, patch"
	@echo "  build      - compile memtest.efi + memtest.bin (needs Linux toolchain)"
	@echo "  efi        - build mask-shim.efi + install.efi (EFI helper apps)"
	@echo "  test-shim  - run hosted badmem.txt parser unit tests"
	@echo "  iso        - produce dist/a1990-memtest.iso (needs grub-mkrescue, xorriso)"
	@echo "  docker     - build ISO inside a Debian container (works on macOS)"
	@echo "  clean      - remove build artifacts"
	@echo "  distclean  - also revert changes to memtest86plus tree"
