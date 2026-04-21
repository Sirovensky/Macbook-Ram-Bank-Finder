# Top-level convenience Makefile for a1990-memtest.
# Real build lives in memtest86plus/build/x86_64.

.PHONY: all apply build iso clean distclean

MT := memtest86plus
BUILD_DIR := $(MT)/build/x86_64

all: iso

apply:
	@./scripts/apply.sh

build: apply
	$(MAKE) -C $(BUILD_DIR)

iso: build
	@./scripts/build-iso.sh

clean:
	-$(MAKE) -C $(BUILD_DIR) clean
	-rm -rf dist

distclean: clean
	-rm -rf $(MT)/system/a1990
	-cd $(MT) && git checkout -- app/error.c build/x86_64/Makefile 2>/dev/null || true

help:
	@echo "targets:"
	@echo "  apply     - copy src/ into memtest86plus tree + patch"
	@echo "  build     - compile memtest.efi + memtest.bin"
	@echo "  iso       - produce dist/a1990-memtest.iso"
	@echo "  clean     - remove build artifacts"
	@echo "  distclean - also revert changes to memtest86plus tree"
