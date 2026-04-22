// SPDX-License-Identifier: GPL-2.0
//
// Minimal Coffee Lake IMC PA-decode for the EFI mask-shim.
//
// This is a self-contained re-implementation that replaces src/cfl_decode.c
// for the shim build.  It has no dependency on map_region(), cpuid_info,
// pci_config_read32(), display.h, or any memtest86plus internals.
//
// All MMIO accesses use the raw physical address — valid before
// ExitBootServices() because EFI firmware identity-maps MMIO.
// PCI config space is read via CF8/CFC port I/O (x86 only).

#ifndef CFL_DECODE_SHIM_H
#define CFL_DECODE_SHIM_H

#include <stdint.h>
#include <stdbool.h>

// Result of decoding a physical address.  Only channel and rank are used
// by the shim; the rest are stubs for interface compatibility.
struct shim_pa_decoded {
    bool    valid;
    uint8_t channel;
    uint8_t rank;
    bool    rank_valid;
};

// Read MCHBAR registers and return a decoded (channel, rank) for pa.
// Returns .valid=false if not on Coffee Lake or MCHBAR inaccessible.
struct shim_pa_decoded shim_cfl_decode_pa(uint64_t pa);

// Return the total populated DRAM size in bytes (sum of both channels).
// Returns 0 if not on Coffee Lake.
uint64_t shim_cfl_total_memory(void);

// Re-read MCHBAR state (call once before any decode loop).
// Returns true if Coffee Lake detected and MCHBAR accessible.
bool shim_cfl_init(void);

#endif // CFL_DECODE_SHIM_H
