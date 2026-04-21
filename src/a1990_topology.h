// SPDX-License-Identifier: GPL-2.0
//
// A1990 (MacBook Pro 15" 2018/2019) memory topology.
//
// Maps (channel, rank, byte lane) -> BGA package designator on the
// A1990 logic board so memory errors can be traced to a specific chip.
//
// Two variants exist; we dispatch by IMC-reported total DRAM size and
// chip width. SMBIOS board-id also checked as a safety gate.

#ifndef A1990_TOPOLOGY_H
#define A1990_TOPOLOGY_H

#include <stdint.h>
#include <stdbool.h>

// Max across variants: 8 packages (32 GB x8 layout)
#define A1990_MAX_PACKAGES 8

typedef enum {
    A1990_VARIANT_UNKNOWN = 0,
    A1990_VARIANT_16GB,    // 4 packages x16
    A1990_VARIANT_32GB,    // 8 packages x8 (or 4 dual-die x16 — TBD)
} a1990_variant_t;

// One BGA package on the logic board.
typedef struct {
    const char *designator;   // e.g. "U1800" — from Apple schematic 820-01041
    uint8_t channel;          // 0 or 1
    uint8_t rank;             // 0 or 1
    uint8_t byte_lane_lo;     // lowest byte lane this chip owns (0..7)
    uint8_t byte_lane_hi;     // highest byte lane (==lo for x8, lo+1 for x16)
} a1990_package_t;

typedef struct {
    a1990_variant_t variant;
    const char *variant_name;
    uint8_t package_count;
    uint8_t chip_width;           // 8 or 16
    a1990_package_t packages[A1990_MAX_PACKAGES];
} a1990_topology_t;

// Apple SMBIOS Type 1 productname strings for A1990.
#define A1990_PRODUCT_2018  "MacBookPro15,1"
#define A1990_PRODUCT_2019  "MacBookPro15,3"

// Detect which A1990 variant we are on (or UNKNOWN).
// Reads SMBIOS, IMC config, and returns a profile pointer or NULL.
const a1990_topology_t *a1990_detect(void);

// Given (channel, rank, byte_lane), return matching package (or NULL).
const a1990_package_t *a1990_lookup(const a1990_topology_t *topo,
                                    uint8_t channel,
                                    uint8_t rank,
                                    uint8_t byte_lane);

#endif // A1990_TOPOLOGY_H
