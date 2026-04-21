// SPDX-License-Identifier: GPL-2.0
//
// Thin wrapper around memtest86plus's SMBIOS parser. Returns Apple
// SMBIOS Type 1 productname (e.g. "MacBookPro15,1") used to match
// YAML board profiles.

#include "stdint.h"
#include "stddef.h"

#include "smbios.h"        // upstream memtest86plus header
#include "board_topology.h"

// Reimplementation of upstream's static get_tstruct_string(): walks the
// null-separated string table that follows a DMI/SMBIOS structure
// header and returns entry N (1-indexed).
static char *tstruct_string(struct tstruct_header *header,
                            uint16_t maxlen, int n)
{
    if (n < 1) return NULL;
    char *a = (char *)header + header->length;
    n--;
    do {
        if (!*a) n--;
        if (!n && *a) return a;
        a++;
    } while (a < ((char *)header + maxlen) && !(*a == 0 && *(a - 1) == 0));
    return NULL;
}

extern struct system_info *dmi_system_info;

const char *smbios_board_id(void)
{
    if (!dmi_system_info) return NULL;
    return tstruct_string(&dmi_system_info->header,
                          dmi_system_info->header.length,
                          dmi_system_info->productname);
}
