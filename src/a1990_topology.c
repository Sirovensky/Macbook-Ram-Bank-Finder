// SPDX-License-Identifier: GPL-2.0
//
// A1990 topology tables. BGA U-designators below are PLACEHOLDERS
// ("U18xx" style). Real values must come from Apple schematic 820-01041
// and the matching boardview. Calibration pass on target hardware
// updates this file.

#include "stdint.h"
#include "string.h"

#include "a1990_topology.h"
#include "cfl_decode.h"
#include "smbios.h"

// ---- 16 GB variant: 4 packages x16, 2 channels x 1 rank ------------
// Layout assumption (MUST VERIFY):
//   CH0 rank0 lanes [0..1] -> U1700   (low  16 bits)
//   CH0 rank0 lanes [2..3] -> U1701
//   CH1 rank0 lanes [0..1] -> U1800
//   CH1 rank0 lanes [2..3] -> U1801
static const a1990_topology_t topo_16gb = {
    .variant      = A1990_VARIANT_16GB,
    .variant_name = "A1990 16GB",
    .package_count = 4,
    .chip_width   = 16,
    .packages = {
        { "U1700", 0, 0, 0, 1 },
        { "U1701", 0, 0, 2, 3 },
        { "U1800", 1, 0, 0, 1 },
        { "U1801", 1, 0, 2, 3 },
    },
};

// ---- 32 GB variant: 8 packages x8, 2 channels x 2 ranks ------------
// Layout assumption (MUST VERIFY):
//   CH0 rank0 lanes [0..3] -> U1700..U1703
//   CH0 rank1 lanes [0..3] -> U1710..U1713  (or stacked dies)
//   CH1 rank0 lanes [0..3] -> U1800..U1803
//   CH1 rank1 lanes [0..3] -> U1810..U1813
static const a1990_topology_t topo_32gb = {
    .variant      = A1990_VARIANT_32GB,
    .variant_name = "A1990 32GB",
    .package_count = 8,
    .chip_width   = 8,
    .packages = {
        { "U1700", 0, 0, 0, 0 },
        { "U1701", 0, 0, 1, 1 },
        { "U1702", 0, 0, 2, 2 },
        { "U1703", 0, 0, 3, 3 },
        { "U1800", 1, 0, 0, 0 },
        { "U1801", 1, 0, 1, 1 },
        { "U1802", 1, 0, 2, 2 },
        { "U1803", 1, 0, 3, 3 },
    },
    // NOTE: only rank 0 listed above; if variant is truly 2R then
    // we need to double the table. Left deliberately incomplete
    // pending calibration.
};

static bool is_a1990_board(const char *product)
{
    if (!product) return false;
    return strncmp(product, A1990_PRODUCT_2018, sizeof(A1990_PRODUCT_2018)) == 0
        || strncmp(product, A1990_PRODUCT_2019, sizeof(A1990_PRODUCT_2019)) == 0;
}

const a1990_topology_t *a1990_detect(void)
{
    const char *product = smbios_board_id();
    if (!is_a1990_board(product)) return NULL;

    const struct mc_config *mc = cfl_mc_config();
    if (!mc) return NULL;

    // Variant vote: chip_width is primary signal (DIMM width from MAD_DIMM).
    if (mc->chip_width == 16) return &topo_16gb;
    if (mc->chip_width == 8)  return &topo_32gb;

    return NULL;
}

const a1990_package_t *a1990_lookup(const a1990_topology_t *topo,
                                    uint8_t channel,
                                    uint8_t rank,
                                    uint8_t byte_lane)
{
    if (!topo) return NULL;
    for (unsigned i = 0; i < topo->package_count; i++) {
        const a1990_package_t *p = &topo->packages[i];
        if (p->channel != channel) continue;
        if (p->rank != rank) continue;
        if (byte_lane < p->byte_lane_lo || byte_lane > p->byte_lane_hi) continue;
        return p;
    }
    return NULL;
}
