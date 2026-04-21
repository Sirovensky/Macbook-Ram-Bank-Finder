// SPDX-License-Identifier: GPL-2.0
#ifndef A1990_SMBIOS_H
#define A1990_SMBIOS_H

// Returns SMBIOS "board-id" string (typically SMBIOS type 2 Product).
// On Apple firmware this contains values like "Mac-937A206F2EE63C01".
// Returns NULL if SMBIOS absent.
const char *smbios_board_id(void);

#endif
