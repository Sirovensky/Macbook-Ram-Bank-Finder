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
extern int      smbios_init_source;
extern uintptr_t smbios_init_ep_addr;
extern uint32_t smbios_init_table_len;
extern uint32_t smbios_table_remaining(const void *p);

const char *smbios_board_id(void)
{
    if (!dmi_system_info) return NULL;
    uint32_t maxlen = smbios_table_remaining(&dmi_system_info->header);
    if (maxlen <= dmi_system_info->header.length) return NULL;
    if (maxlen > 0xFFFF) maxlen = 0xFFFF;
    return tstruct_string(&dmi_system_info->header,
                          (uint16_t)maxlen,
                          dmi_system_info->productname);
}

int smbios_debug_source(void)       { return smbios_init_source; }
uintptr_t smbios_debug_ep_addr(void) { return smbios_init_ep_addr; }
uint32_t smbios_debug_table_len(void){ return smbios_init_table_len; }
