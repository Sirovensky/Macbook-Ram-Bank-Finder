// SPDX-License-Identifier: GPL-2.0
//
// IMC dispatch layer — selects the correct family driver at runtime via
// CPUID probe, then forwards all calls to it.

#include <stddef.h>  // NULL

#include "imc_dispatch.h"

// ---------------------------------------------------------------------------
// Family driver forward declarations.
// Each family module exposes one struct imc_ops at file scope.
// ---------------------------------------------------------------------------

extern struct imc_ops cfl_skl_kbl_ops;   // Coffee Lake / Kaby Lake / Skylake

// Table of all registered families, probed in order.
// Add new families here (e.g. icl_tgl_ops, adl_ops).
static struct imc_ops * const g_families[] = {
    &cfl_skl_kbl_ops,
};
#define N_FAMILIES (sizeof(g_families) / sizeof(g_families[0]))

// ---------------------------------------------------------------------------
// Active-family cache.
// ---------------------------------------------------------------------------

static const struct imc_ops *g_active;
static bool                  g_detected;

const struct imc_ops *imc_active(void)
{
    if (g_detected) return g_active;
    g_detected = true;
    g_active   = NULL;

    for (unsigned i = 0; i < N_FAMILIES; i++) {
        if (g_families[i]->detect && g_families[i]->detect()) {
            g_active = g_families[i];
            break;
        }
    }
    return g_active;
}

// ---------------------------------------------------------------------------
// Public wrappers.
// ---------------------------------------------------------------------------

const struct mc_config *imc_config(void)
{
    const struct imc_ops *ops = imc_active();
    if (!ops || !ops->config) return NULL;
    return ops->config();
}

struct pa_decoded imc_decode_pa(uint64_t pa)
{
    struct pa_decoded d = { 0 };
    const struct imc_ops *ops = imc_active();
    if (!ops || !ops->decode_pa) return d;
    return ops->decode_pa(pa);
}

void imc_config_refresh(void)
{
    const struct imc_ops *ops = imc_active();
    if (ops && ops->config_refresh) ops->config_refresh();
}

void imc_dump_mchbar(void)
{
    const struct imc_ops *ops = imc_active();
    if (ops && ops->dump_mchbar) ops->dump_mchbar();
}

void imc_dump_mchbar_at(int row_first, int row_last)
{
    const struct imc_ops *ops = imc_active();
    if (ops && ops->dump_mchbar_at) ops->dump_mchbar_at(row_first, row_last);
}
