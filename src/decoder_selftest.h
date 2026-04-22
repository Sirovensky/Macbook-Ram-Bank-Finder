// SPDX-License-Identifier: GPL-2.0
//
// Decoder self-test harness — validates the DRAMDig-prior bank/row XOR masks
// against real DRAM timing before the row-repair shim trusts them.
//
// Copyright (C) 2024 A1990-memtest contributors.

#ifndef DECODER_SELFTEST_H
#define DECODER_SELFTEST_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    BRR_DECODER_UNKNOWN        = 0,   // not yet tested
    BRR_DECODER_VALIDATED      = 1,   // bank/row decode confirmed by timing
    BRR_DECODER_FAILED         = 2,   // XOR prior wrong for this hardware
    BRR_DECODER_NOT_APPLICABLE = 3,   // family has no row decoder (non-CFL)
} brr_decoder_status_t;

// Run the self-test, return a status.  Writes the result to NVRAM variable
// "BrrDecoderStatus" under the BRR vendor GUID so the shim can read it
// post-ExitBootServices.
//
// `sys_table_arg` is the UEFI system table pointer (must have BootServices
// with allocate_pages, free_pages, stall; and RuntimeServices with
// set_variable available).
//
// Returns BRR_DECODER_UNKNOWN if boot services are unavailable, allocation
// fails, or the test could not be completed within the time budget.
brr_decoder_status_t decoder_selftest_run(void *sys_table_arg);

#endif // DECODER_SELFTEST_H
