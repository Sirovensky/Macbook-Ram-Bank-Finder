// SPDX-License-Identifier: GPL-2.0
//
// Generic board-topology overlay. Community-contributable: any laptop
// (or desktop) with an open schematic can be added as a YAML file in
// topology/. The generator produces board_table.c at build time.
//
// Two tiers of info:
//   T1 (generic): channel, rank, byte lane, chip width — always available
//                 from IMC alone. No overlay needed.
//   T2 (overlay): BGA U-designator + board-side visual hint.
//                 Requires schematic knowledge per board.

#ifndef BOARD_TOPOLOGY_H
#define BOARD_TOPOLOGY_H

#include <stdint.h>
#include <stdbool.h>

#define BOARD_MAX_PACKAGES  16      // 8 chips/channel × 2 channels
#define BOARD_MAX_PRODUCTS  4       // SMBIOS productnames matching this board

typedef struct {
    const char *designator;     // e.g. "U2300"
    uint8_t channel;            // 0 or 1
    uint8_t rank;               // 0 or 1
    uint8_t byte_lane;          // 0..7 — for x16 chips this is the low lane
    uint8_t chip_width;         // 8 or 16
    const char *location_hint;  // human-readable, e.g. "top-left near CPU"
} board_package_t;

typedef struct {
    const char *board_id;       // e.g. "820-01041"
    const char *friendly_name;  // e.g. "MacBook Pro 15 2018/2019 (A1990)"
    const char *products[BOARD_MAX_PRODUCTS]; // SMBIOS Type 1 productname matches
    uint8_t num_products;
    uint8_t package_count;
    uint8_t chip_width;         // dominant width (8 or 16)
    uint8_t channels;           // 1 or 2
    uint8_t ranks_per_channel;  // 1 or 2
    const board_package_t *packages;
} board_profile_t;

// All compiled-in board profiles. Generated from topology/*.yaml.
extern const board_profile_t *const board_profiles[];
extern const unsigned board_profile_count;

// Detect current board via SMBIOS productname match. Returns NULL if
// no overlay matches — caller falls back to tier-1 generic output.
const board_profile_t *board_detect(void);

// Look up a package by (channel, rank, byte_lane). NULL if no match.
const board_package_t *board_lookup(const board_profile_t *p,
                                    uint8_t channel,
                                    uint8_t rank,
                                    uint8_t byte_lane);

// SMBIOS Type 1 productname accessor (implemented in smbios.c, wraps
// memtest86plus's DMI parser).
const char *smbios_board_id(void);

#endif // BOARD_TOPOLOGY_H
