# Top-level convenience Makefile for a1990-memtest.
# Real build lives in memtest86plus/build/x86_64.

.PHONY: all apply build iso clean distclean docker docker-iso help

MT := memtest86plus
BUILD_DIR := $(MT)/build/x86_64

all: iso

apply:
	@./scripts/apply.sh

build: apply
	$(MAKE) -C $(BUILD_DIR)

iso: build
	@./scripts/build-iso.sh

# Build inside Docker — use this from macOS/Windows hosts.
docker: docker-iso
docker-iso:
	@./scripts/docker-build.sh iso

clean:
	-$(MAKE) -C $(BUILD_DIR) clean
	-rm -rf dist
	-rm -f src/board_table.c

distclean: clean
	-rm -rf $(MT)/system/board $(MT)/system/a1990
	-cd $(MT) && git checkout -- app/error.c app/main.c build/x86_64/Makefile 2>/dev/null || true

help:
	@echo "targets:"
	@echo "  apply      - generate board_table.c, sync into memtest86plus, patch"
	@echo "  build      - compile memtest.efi + memtest.bin (needs Linux toolchain)"
	@echo "  iso        - produce dist/a1990-memtest.iso (needs grub-mkrescue, xorriso)"
	@echo "  docker     - build ISO inside a Debian container (works on macOS)"
	@echo "  clean      - remove build artifacts"
	@echo "  distclean  - also revert changes to memtest86plus tree"
