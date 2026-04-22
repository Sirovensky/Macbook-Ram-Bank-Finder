// SPDX-License-Identifier: GPL-2.0
//
// Thin compatibility shim — re-exports imc_dispatch.h and maps the old
// cfl_* names to the new imc_* API.
//
// New code should include "imc_dispatch.h" directly.  This header is
// preserved so that any out-of-tree callers and the EFI shim build
// (which has its own cfl_decode_shim.c) continue to compile unchanged.

#ifndef CFL_DECODE_H
#define CFL_DECODE_H

#include "imc_dispatch.h"

// Old names mapped to new API (inline wrappers or macro aliases).
// struct mc_dimm, mc_channel, mc_config, pa_decoded are defined in
// imc_dispatch.h and are identical to the old layouts, extended with
// bank_row_valid + bank_row_speculative fields.

static inline const struct mc_config *cfl_mc_config(void)
{
    return imc_config();
}

static inline void cfl_mc_config_refresh(void)
{
    imc_config_refresh();
}

static inline struct pa_decoded cfl_decode_pa(uint64_t pa)
{
    return imc_decode_pa(pa);
}

static inline void cfl_dump_mchbar(void)
{
    imc_dump_mchbar();
}

static inline void cfl_dump_mchbar_at(int row_first, int row_last)
{
    imc_dump_mchbar_at(row_first, row_last);
}

#endif // CFL_DECODE_H
