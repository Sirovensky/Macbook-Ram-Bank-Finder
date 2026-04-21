// SPDX-License-Identifier: GPL-2.0
//
// Thin wrapper around memtest86plus's SMBIOS parser. Returns Apple
// SMBIOS Type 1 productname (e.g. "MacBookPro15,1") used to gate
// A1990 detection.

#include "stdint.h"
#include "stddef.h"

#include "smbios.h"

// Opaque forward-decl to match memtest86plus's internal parser.
struct tstruct_header;

// Pull memtest86plus's string helper + system_info global.
extern char *get_tstruct_string(struct tstruct_header *header,
                                uint16_t maxlen, int n);

struct mt86_tstruct_header {
    uint8_t  type;
    uint8_t  length;
    uint16_t handle;
};

struct mt86_system_info {
    struct mt86_tstruct_header header;
    uint8_t manufacturer;
    uint8_t productname;
    // remainder unused here
};

extern struct mt86_system_info *dmi_system_info;

const char *smbios_board_id(void)
{
    if (!dmi_system_info) return NULL;
    return get_tstruct_string((struct tstruct_header *)&dmi_system_info->header,
                              dmi_system_info->header.length,
                              dmi_system_info->productname);
}
