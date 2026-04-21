// SPDX-License-Identifier: GPL-2.0
//
// Generic board-topology lookup. No board data here — all profiles
// are compiled into `board_table.c`, which the YAML generator emits.

#include "stdint.h"
#include "string.h"

#include "board_topology.h"

static bool product_matches(const board_profile_t *p, const char *product)
{
    if (!product) return false;
    for (unsigned i = 0; i < p->num_products; i++) {
        const char *needle = p->products[i];
        if (!needle) continue;
        unsigned n = 0;
        while (needle[n]) n++;
        // Require exact equality (strncmp n+1 covers trailing NUL).
        if (strncmp(product, needle, n + 1) == 0) return true;
    }
    return false;
}

const board_profile_t *board_detect(void)
{
    const char *product = smbios_board_id();
    if (!product) return NULL;
    for (unsigned i = 0; i < board_profile_count; i++) {
        const board_profile_t *p = board_profiles[i];
        if (!p) continue;
        if (product_matches(p, product)) return p;
    }
    return NULL;
}

const board_package_t *board_lookup(const board_profile_t *p,
                                    uint8_t channel,
                                    uint8_t rank,
                                    uint8_t byte_lane)
{
    if (!p) return NULL;
    for (unsigned i = 0; i < p->package_count; i++) {
        const board_package_t *pk = &p->packages[i];
        if (pk->channel != channel) continue;
        if (pk->rank != rank) continue;
        uint8_t lane_hi = pk->byte_lane + (pk->chip_width == 16 ? 1 : 0);
        if (byte_lane < pk->byte_lane || byte_lane > lane_hi) continue;
        return pk;
    }
    return NULL;
}
