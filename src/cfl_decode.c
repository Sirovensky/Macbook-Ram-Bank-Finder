// SPDX-License-Identifier: GPL-2.0
//
// Compatibility stub — the real implementation now lives in:
//   src/imc/cfl_skl_kbl.c  (family driver)
//   src/imc_dispatch.c      (dispatch layer)
//
// This file is kept so that apply.sh can include board/cfl_decode.o in the
// Makefile without patching the object-file list.  It compiles to an empty
// translation unit (no symbols conflict because cfl_decode.h now maps the
// old names to inline wrappers around imc_*).

// Nothing to compile — all symbols come from imc_dispatch.o and
// imc/cfl_skl_kbl.o, exposed via inline wrappers in cfl_decode.h.
