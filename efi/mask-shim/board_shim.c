// SPDX-License-Identifier: GPL-2.0
//
// board_shim.c — compile-in board_table.c and board_topology.c for the
// EFI mask-shim.
//
// Provides stubs for smbios_board_id() and diagnostic accessors that the
// src/ files declare but that depend on memtest86plus internals unavailable
// in the EFI freestanding build.
//
// The shim resolves chip designators by scanning board_profiles[] directly;
// it never calls board_detect(), so the SMBIOS stub only needs to link.

#include <stdint.h>
#include <stdbool.h>

// SMBIOS stubs — must be defined before pulling in board_topology.c so the
// linker sees exactly one definition.
const char *smbios_board_id(void)          { return (void *)0; }
int         smbios_debug_source(void)      { return 0; }

// uintptr_t stubs use the largest compatible unsigned type.
static inline uintptr_t _smbios_ep(void)   { return 0; }
static inline uint32_t  _smbios_tl(void)   { return 0; }

// board_topology.h declares these with these exact signatures:
//   uintptr_t smbios_debug_ep_addr(void);
//   uint32_t  smbios_debug_table_len(void);
uintptr_t smbios_debug_ep_addr(void)   { return _smbios_ep(); }
uint32_t  smbios_debug_table_len(void) { return _smbios_tl(); }

// Pull in the topology sources.  board_topology.c uses:
//   #include "string.h"   -> found as efi/string.h via -I..
//   #include "stdint.h"   -> falls through to system stdint.h
//   #include "board_topology.h" -> found via -I../../src
// board_table.c uses:
//   #include "stdint.h"
//   #include "board_topology.h"
#include "../../src/board_topology.c"
#include "../../src/board_table.c"
