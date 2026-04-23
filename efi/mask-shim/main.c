// SPDX-License-Identifier: GPL-2.0
//
// mask-shim — EFI application that reserves bad DRAM pages then chain-loads
// the macOS boot.efi from the internal APFS Preboot volume.
//
// Usage:
//   Load from grub via `chainloader /EFI/BOOT/mask-shim.efi`
//   badmem.txt location is detected automatically:
//     - running from \EFI\BRR\  -> \EFI\BRR\badmem.txt   (permanent install)
//     - running from \EFI\BOOT\ -> \EFI\BOOT\badmem.txt  (USB trial)
//   The shim tries \EFI\BRR\badmem.txt first, then falls back to
//   \EFI\BOOT\badmem.txt so either install location works.
//
// Responsibilities:
//   1. Read badmem.txt from the loaded-image device (path auto-detected).
//   2. Parse physical address ranges.
//   3. Reserve each range with AllocatePages(AllocateAddress,
//      EfiReservedMemoryType, pages, &addr).
//   4. Check BrrMaskState NVRAM variable.
//      - PERMANENT_UNCONFIRMED: prompt user via ConIn (30s timeout).
//        Y -> set PERMANENT_CONFIRMED.
//        N -> run uninstall inline; skip masking; boot unmasked.
//        timeout -> proceed masked; state stays UNCONFIRMED.
//   5. Find internal macOS boot.efi on Preboot APFS volume.
//      Skip the device that loaded this shim to avoid re-loading from USB.
//   6. LoadImage (with full device path) + StartImage.

#include "../efi_types.h"
#include "../efi_util.h"
#include "../badmem_parse.h"
#include "../mask-common/mask_ops.h"
#include "cfl_decode_shim.h"
// board_shim.c is compiled as a separate object; we only need the header.
#include "../../src/board_topology.h"

// State strings imported from mask_ops.h via #define.
// Convenience aliases:
#define STATE_TRIAL_PENDING_PAGE  BRR_STATE_TRIAL_PENDING_PAGE
#define STATE_TRIAL_PENDING_CHIP  BRR_STATE_TRIAL_PENDING_CHIP
#define STATE_TRIAL_BOOTED        BRR_STATE_TRIAL_BOOTED
#define STATE_PERMANENT_UNCONFIRMED BRR_STATE_PERMANENT_UNCONFIRMED
#define STATE_PERMANENT_CONFIRMED   BRR_STATE_PERMANENT_CONFIRMED

// Shorthand for shared var names.
#define VARNAME_STATE    BRR_VARNAME_STATE
#define VARNAME_BADPAGES BRR_VARNAME_BADPAGES
#define VARNAME_BADCHIPS BRR_VARNAME_BADCHIPS

// Track C: NVRAM variable names for row-level masking (file-local, not in mask_ops.h).
static const CHAR16 VARNAME_BADROWS[]       = L"BrrBadRows";
static const CHAR16 VARNAME_DECODER_STATUS[]= L"BrrDecoderStatus";
static const CHAR16 LEGACY_VARNAME_BADROWS[]= L"A1990BadRows";

// Decoder status values (written as ASCII strings by decoder_selftest; read here).
#define DECODER_STATUS_VALIDATED  "VALIDATED"
#define DECODER_STATUS_FAILED     "FAILED"

// Known macOS boot.efi paths.  Modern macOS (APFS Preboot) puts boot.efi
// at \<SystemVolumeUUID>\System\Library\CoreServices\boot.efi -- the UUID
// directory is per-system-volume so we can't hard-code it.  Older macOS
// (HFS+) puts it at the root-level path.  Recovery volume uses a distinct
// prefix.  The finder below tries root-level first, then scans 1–2 levels
// of subdirectories, covering every known layout.
static const CHAR16 MACOS_BOOT_PATH[] =
    L"\\System\\Library\\CoreServices\\boot.efi";
static const CHAR16 MACOS_BOOT_SUFFIX[] =
    L"\\System\\Library\\CoreServices\\boot.efi";

// Alternate root-level paths to try when the primary path misses.
// Ordered by likelihood on a T2 macOS install.
static const CHAR16 * const MACOS_ALT_PATHS[] = {
    L"\\System\\Library\\CoreServices\\boot.efi",             // HFS+ root
    L"\\com.apple.recovery.boot\\boot.efi",                    // Recovery
    L"\\usr\\standalone\\i386\\boot.efi",                      // firmware staging
    0
};

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static EFI_STATUS read_badmem(EFI_SYSTEM_TABLE *st, EFI_HANDLE image_handle,
                               badmem_range_t *ranges, unsigned *out_count,
                               badmem_chip_t *chips, unsigned *out_chip_count);
static EFI_STATUS mask_pages(EFI_SYSTEM_TABLE *st,
                              const badmem_range_t *ranges, unsigned count);
static EFI_STATUS mask_chips(EFI_SYSTEM_TABLE *st,
                              badmem_chip_t *chips, unsigned chip_count);
static EFI_STATUS mask_rows(EFI_SYSTEM_TABLE *st,
                             badmem_row_t *rows, unsigned count);
static EFI_STATUS resolve_chip_entries(EFI_SYSTEM_TABLE *st,
                                        badmem_chip_t *chips,
                                        unsigned chip_count);
static EFI_STATUS find_macos_boot(EFI_SYSTEM_TABLE *st,
                                   EFI_HANDLE image_handle,
                                   EFI_HANDLE *out_device,
                                   EFI_DEVICE_PATH_PROTOCOL **out_path);
static EFI_STATUS chainload_macos(EFI_SYSTEM_TABLE *st, EFI_HANDLE self,
                                   EFI_HANDLE device,
                                   EFI_DEVICE_PATH_PROTOCOL *path);
static void handle_nvram_state(EFI_SYSTEM_TABLE *st, EFI_HANDLE image,
                                int *skip_mask, int *force_chip);
static CHAR16 prompt_confirm(EFI_SYSTEM_TABLE *st, unsigned timeout_s);
static unsigned read_nvram_badrows(EFI_SYSTEM_TABLE *st,
                                    badmem_row_t *rows, unsigned cap);
static int read_nvram_decoder_status_validated(EFI_SYSTEM_TABLE *st);
static int read_nvram_decoder_status_failed(EFI_SYSTEM_TABLE *st);

// ---------------------------------------------------------------------------
// Helpers: thin wrappers around shared mask_ops functions.
// ---------------------------------------------------------------------------
#define nvram_get_ascii(st, name, buf, sz)   mask_nvram_get_ascii(st, name, buf, sz)
#define nvram_set_ascii(st, name, val)        mask_nvram_set_ascii(st, name, val)
#define nvram_delete(st, name)                mask_nvram_delete(st, name)

// Simple ASCII strcmp.
static int ascii_eq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

// ---------------------------------------------------------------------------
// Device path helper: return total byte length of a device path
// (including the End-of-Entire-Device-Path node).
// ---------------------------------------------------------------------------
static UINTN dp_total_len(const EFI_DEVICE_PATH_PROTOCOL *dp)
{
    UINTN total = 0;
    const UINT8 *p = (const UINT8 *)dp;
    for (;;) {
        UINT16 node_len = (UINT16)p[2] | ((UINT16)p[3] << 8);
        total += node_len;
        if (p[0] == 0x7f && p[1] == 0xff) break; // End-of-Entire
        p += node_len;
    }
    return total;
}

// Build a full device path for chainloading:
//   <hardware path of device> (all nodes up to but not including the End node)
//   + MediaFilePath node for the target file
//   + End-of-Entire-Device-Path node
// Allocates from pool. Caller frees.
static EFI_STATUS build_file_device_path(EFI_SYSTEM_TABLE *st,
                                          EFI_HANDLE device,
                                          const CHAR16 *path,
                                          EFI_DEVICE_PATH_PROTOCOL **out)
{
    static const EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;

    // Get the hardware path of the target device.
    EFI_DEVICE_PATH_PROTOCOL *hw_dp = NULL;
    EFI_STATUS s = st->BootServices->HandleProtocol(
        device, (EFI_GUID *)&dp_guid, (void **)&hw_dp);
    if (s != EFI_SUCCESS) hw_dp = NULL;

    // Compute prefix length: all nodes of hw_dp *except* the trailing
    // End-of-Entire-Device-Path node (4 bytes).
    UINTN prefix_len = 0;
    if (hw_dp) {
        UINTN total = dp_total_len(hw_dp);
        prefix_len = (total >= 4) ? (total - 4) : 0; // strip End node
    }

    // Compute FilePath node size.
    UINTN name_len  = efi_strlen16(path); // chars (not NUL)
    UINTN fp_sz     = 4 + (name_len + 1) * sizeof(CHAR16);
    UINTN end_sz    = 4;
    UINTN total_sz  = prefix_len + fp_sz + end_sz;

    void *buf = NULL;
    s = st->BootServices->AllocatePool(EfiLoaderData, total_sz, &buf);
    if (s != EFI_SUCCESS) return s;

    UINT8 *p = (UINT8 *)buf;

    // Copy hardware prefix (no End node).
    if (prefix_len > 0) {
        const UINT8 *src = (const UINT8 *)hw_dp;
        for (UINTN i = 0; i < prefix_len; i++) p[i] = src[i];
        p += prefix_len;
    }

    // FilePath node (Type=4, SubType=4).
    p[0] = 0x04; p[1] = 0x04;
    p[2] = (UINT8)(fp_sz & 0xff);
    p[3] = (UINT8)(fp_sz >> 8);
    efi_strcpy16((CHAR16 *)(p + 4), path);
    p += fp_sz;

    // End-of-Entire-Device-Path node.
    p[0] = 0x7f; p[1] = 0xff; p[2] = 4; p[3] = 0;

    *out = (EFI_DEVICE_PATH_PROTOCOL *)buf;
    return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// Binary blob layout written by badmem_log_flush_nvram() in memtest.
// ---------------------------------------------------------------------------
typedef struct {
    UINT32 version;  // = 1
    UINT32 count;    // number of UINT64 PA entries that follow
    // UINT64 pages[count] follows.
} badpages_hdr_t;

// Legacy NVRAM variable names for migration fallback (read-only).
// Forward-declared here so both read_nvram_badpages and read_nvram_badchips
// can reference them.
static const CHAR16 LEGACY_VARNAME_BADCHIPS_FWD[] = {
    'A','1','9','9','0','B','a','d','C','h','i','p','s', 0
};
static const CHAR16 LEGACY_VARNAME_BADPAGES_FWD[] = {
    'A','1','9','9','0','B','a','d','P','a','g','e','s', 0
};
#define LEGACY_VARNAME_BADPAGES LEGACY_VARNAME_BADPAGES_FWD
#define LEGACY_VARNAME_BADCHIPS LEGACY_VARNAME_BADCHIPS_FWD

// ---------------------------------------------------------------------------
// Read BrrBadPages NVRAM variable (with legacy A1990BadPages fallback) and append unique page-aligned entries
// to the existing ranges[] array.  Already-present entries are skipped.
// Returns number of new entries added.
// ---------------------------------------------------------------------------
static unsigned read_nvram_badpages(EFI_SYSTEM_TABLE *st,
                                    badmem_range_t *ranges,
                                    unsigned existing,
                                    unsigned cap)
{
    // Static buffer for the binary blob: header + up to 4096 * 8 bytes.
    static UINT8 blob[8 + 4096 * 8];
    UINTN blob_sz = sizeof(blob);
    UINT32 attrs  = 0;

    EFI_STATUS s = st->RuntimeServices->GetVariable(
        (CHAR16 *)VARNAME_BADPAGES, (EFI_GUID *)&BRR_GUID,
        &attrs, &blob_sz, blob);

    if (s != EFI_SUCCESS) {
        // Legacy A1990BadPages name under BRR_GUID.
        blob_sz = sizeof(blob);
        attrs   = 0;
        s = st->RuntimeServices->GetVariable(
            (CHAR16 *)LEGACY_VARNAME_BADPAGES, (EFI_GUID *)&BRR_GUID,
            &attrs, &blob_sz, blob);
    }

    if (s != EFI_SUCCESS) {
        // Variable absent: no NVRAM data from previous memtest run.
        return 0;
    }

    if (blob_sz < sizeof(badpages_hdr_t)) {
        efi_print(st, L"[mask] BrrBadPages: blob too small, ignoring\r\n");
        return 0;
    }

    badpages_hdr_t *hdr = (badpages_hdr_t *)blob;
    if (hdr->version != 1) {
        efi_print(st, L"[mask] BrrBadPages: unknown version, ignoring\r\n");
        return 0;
    }

    UINTN expected = sizeof(badpages_hdr_t) + (UINTN)hdr->count * sizeof(UINT64);
    if (blob_sz < expected || hdr->count > 4096) {
        efi_print(st, L"[mask] BrrBadPages: inconsistent blob, ignoring\r\n");
        return 0;
    }

    UINT64 *pa_array = (UINT64 *)(blob + sizeof(badpages_hdr_t));
    unsigned added = 0;

    for (UINT32 i = 0; i < hdr->count; i++) {
        UINT64 page_pa = pa_array[i] & ~(UINT64)0xfff;  // page-align

        // Check if this PA is already covered by an existing range.
        int dup = 0;
        for (unsigned j = 0; j < existing + added; j++) {
            if (page_pa >= ranges[j].start &&
                page_pa < ranges[j].start + ranges[j].len) {
                dup = 1;
                break;
            }
        }
        if (dup) continue;

        if (existing + added >= cap) {
            efi_print(st, L"[mask] BrrBadPages: range array full, truncating\r\n");
            break;
        }

        ranges[existing + added].start = page_pa;
        ranges[existing + added].len   = 4096;
        added++;
    }

    return added;
}

// ---------------------------------------------------------------------------
// Read badmem.txt from the shim's own loaded-image device.
// ---------------------------------------------------------------------------

static EFI_STATUS read_badmem(EFI_SYSTEM_TABLE *st, EFI_HANDLE image_handle,
                               badmem_range_t *ranges, unsigned *out_count,
                               badmem_chip_t *chips, unsigned *out_chip_count)
{
    static const EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    static const EFI_GUID lip_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;

    *out_count = 0;
    *out_chip_count = 0;

    // Get the device handle from the loaded-image protocol.
    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    EFI_STATUS s = st->BootServices->HandleProtocol(
        image_handle, (EFI_GUID *)&lip_guid, (void **)&li);
    if (s != EFI_SUCCESS) {
        efi_print(st, L"[shim] no LoadedImage protocol\r\n");
        return s;
    }

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs = NULL;
    s = st->BootServices->HandleProtocol(
        li->DeviceHandle, (EFI_GUID *)&sfs_guid, (void **)&sfs);
    if (s != EFI_SUCCESS) {
        efi_print(st, L"[shim] no SimpleFileSystem on shim device\r\n");
        return s;
    }

    EFI_FILE_PROTOCOL *root = NULL;
    s = sfs->OpenVolume(sfs, &root);
    if (s != EFI_SUCCESS) {
        efi_print(st, L"[shim] cannot open volume\r\n");
        return s;
    }

    // Try \EFI\BRR\badmem.txt first (permanent install location),
    // then fall back to \EFI\BOOT\badmem.txt (USB trial location).
    EFI_FILE_PROTOCOL *f = NULL;
    s = root->Open(root, &f, (CHAR16 *)L"EFI\\BRR\\badmem.txt",
                   EFI_FILE_MODE_READ, 0);
    if (s != EFI_SUCCESS) {
        s = root->Open(root, &f, (CHAR16 *)L"EFI\\BOOT\\badmem.txt",
                       EFI_FILE_MODE_READ, 0);
    }
    if (s != EFI_SUCCESS) {
        // Not an error — just no badmem.txt; proceed without masking.
        root->Close(root);
        return EFI_SUCCESS;
    }

    // Read up to 64 KiB.
    static char file_buf[65536];
    UINTN read_sz = sizeof(file_buf) - 1;
    s = f->Read(f, &read_sz, file_buf);
    f->Close(f);
    root->Close(root);

    if (s != EFI_SUCCESS) {
        efi_print(st, L"[shim] read error on badmem.txt\r\n");
        return s;
    }

    badmem_result_t result;
    badmem_parse_full(file_buf, (uint64_t)read_sz,
                      ranges, BADMEM_MAX_RANGES,
                      chips,  BADMEM_MAX_CHIPS,
                      &result);
    *out_count      = result.n_ranges;
    *out_chip_count = result.n_chips;
    return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// Reserve bad pages.
// ---------------------------------------------------------------------------

// Safety margin added around every detected bad range.  Bad DRAM cells
// often indicate adjacent-cell degradation about to manifest — match the
// approach documented by Derrick Schneider for his A1990 repair:
// expand each reported range by 1 MiB on each side, aligned to 4 KiB.
// Yields ~2 MiB reserved per detected range minimum.  If multiple ranges
// are close together, AllocatePages handles overlap by skipping already-
// reserved pages (counted separately).
#define MASK_EXPAND_BYTES   (1ULL * 1024 * 1024)
#define MASK_PAGE_SIZE      4096ULL

static EFI_STATUS mask_pages(EFI_SYSTEM_TABLE *st,
                              const badmem_range_t *ranges, unsigned count)
{
    // Reporting semantics:
    //   pages_new       — pages we reserved via AllocatePages this call.
    //   pages_pre       — pages firmware already owned (AllocateAddress failed
    //                     with EFI_NOT_FOUND / EFI_ACCESS_DENIED).  These are
    //                     still safely OFF-LIMITS to macOS, just not reserved
    //                     by us.  Coverage is the UNION of both sets.
    //   ranges_covered  — ranges where EVERY page in the +/-1 MiB window is
    //                     either newly-reserved OR pre-reserved.  This is
    //                     true "coverage" of the range.
    //   ranges_gaps     — ranges with AT LEAST ONE page still returned to the
    //                     OS as usable RAM.  These are the dangerous ones.
    unsigned ranges_covered = 0;
    unsigned ranges_gaps    = 0;
    UINT64   pages_new = 0;
    UINT64   pages_pre = 0;
    UINT64   pages_gap = 0;

    for (unsigned i = 0; i < count; i++) {
        UINT64 orig_start = ranges[i].start;
        UINT64 orig_end   = orig_start + ranges[i].len;

        // Expand +/- 1 MiB, align to 4 KiB page.
        UINT64 exp_start = (orig_start > MASK_EXPAND_BYTES)
                            ? (orig_start - MASK_EXPAND_BYTES) : 0;
        UINT64 exp_end   = orig_end + MASK_EXPAND_BYTES;
        exp_start &= ~(MASK_PAGE_SIZE - 1);
        exp_end    = (exp_end + MASK_PAGE_SIZE - 1) & ~(MASK_PAGE_SIZE - 1);

        unsigned this_new = 0, this_pre = 0, this_gap = 0;
        for (UINT64 pa = exp_start; pa < exp_end; pa += MASK_PAGE_SIZE) {
            EFI_PHYSICAL_ADDRESS addr = (EFI_PHYSICAL_ADDRESS)pa;
            EFI_STATUS s = st->BootServices->AllocatePages(
                AllocateAddress, EfiReservedMemoryType, 1, &addr);
            if (s == EFI_SUCCESS) {
                this_new++;
            } else if (s == EFI_NOT_FOUND || s == EFI_ACCESS_DENIED) {
                // Firmware already owns this page (reserved memory type,
                // MMIO, or outside the RAM map).  Safe — macOS won't touch.
                this_pre++;
            } else {
                // Anything else (OUT_OF_RESOURCES, INVALID_PARAMETER, etc.)
                // means we failed to mask AND firmware didn't reserve it.
                // Page might still land in macOS's usable pool — DANGEROUS.
                this_gap++;
            }
        }
        pages_new += this_new;
        pages_pre += this_pre;
        pages_gap += this_gap;
        if (this_gap == 0) ranges_covered++;
        else               ranges_gaps++;
    }

    efi_print(st, L"[shim] mask coverage: ");
    efi_print_dec(st, (UINTN)ranges_covered);
    efi_print(st, L"/");
    efi_print_dec(st, (UINTN)count);
    efi_print(st, L" range(s) fully covered\r\n");
    efi_print(st, L"[shim]   new reserves: ");
    efi_print_dec(st, (UINTN)pages_new);
    efi_print(st, L" page(s)  (each +/-1 MiB around bad addr)\r\n");
    efi_print(st, L"[shim]   firmware pre-reserved: ");
    efi_print_dec(st, (UINTN)pages_pre);
    efi_print(st, L" page(s)  (already OFF-LIMITS to macOS)\r\n");
    if (pages_gap > 0) {
        efi_print(st, L"[shim]   WARNING: ");
        efi_print_dec(st, (UINTN)pages_gap);
        efi_print(st, L" page(s) in ");
        efi_print_dec(st, (UINTN)ranges_gaps);
        efi_print(st, L" range(s) NOT protected -- unsafe!\r\n");
    }
    return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// Chip-level masking support.
// ---------------------------------------------------------------------------

// Simple ASCII strcmp helper (file-local; mask-shim already has ascii_eq).
static int desig_eq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (*a == '\0' && *b == '\0') ? 1 : 0;
}

// Resolve each chip designator against board_profiles[].
// First match across all profiles wins.
// Unresolved entries (typos / unknown board) are logged and left
// with resolved=0; the PA walk will skip them.
static EFI_STATUS resolve_chip_entries(EFI_SYSTEM_TABLE *st,
                                        badmem_chip_t *chips,
                                        unsigned chip_count)
{
    for (unsigned i = 0; i < chip_count; i++) {
        badmem_chip_t *ce = &chips[i];
        int found = 0;
        for (unsigned pi = 0; pi < board_profile_count && !found; pi++) {
            const board_profile_t *prof = board_profiles[pi];
            for (unsigned pk = 0; pk < prof->package_count && !found; pk++) {
                const board_package_t *pkg = &prof->packages[pk];
                if (desig_eq(pkg->designator, ce->designator)) {
                    ce->channel   = pkg->channel;
                    ce->rank      = pkg->rank;
                    ce->byte_lane = pkg->byte_lane;
                    ce->resolved  = 1;
                    found = 1;
                }
            }
        }
        if (!found) {
            efi_print(st, L"[mask] WARNING: chip designator not found in board table: ");
            efi_printa(st, ce->designator);
            efi_print(st, L" (skipped)\r\n");
        } else {
            efi_print(st, L"[mask] chip ");
            efi_printa(st, ce->designator);
            efi_print(st, L" -> ch=");
            efi_print_dec(st, ce->channel);
            efi_print(st, L" rank=");
            efi_print_dec(st, ce->rank);
            efi_print(st, L"\r\n");
        }
    }
    return EFI_SUCCESS;
}

// Walk all PAs from 0 to total_memory in 4 KiB steps.
// For each PA that decodes to a (channel, rank) matching any resolved chip
// entry, reserve it via AllocatePages.
static EFI_STATUS mask_chips(EFI_SYSTEM_TABLE *st,
                              badmem_chip_t *chips, unsigned chip_count)
{
    if (chip_count == 0) return EFI_SUCCESS;

    // Count resolved entries.
    unsigned resolved = 0;
    for (unsigned i = 0; i < chip_count; i++)
        if (chips[i].resolved) resolved++;

    if (resolved == 0) {
        efi_print(st, L"[mask] chip mode: no resolved chips, nothing to scan\r\n");
        return EFI_SUCCESS;
    }

    if (!shim_cfl_init()) {
        efi_print(st, L"[mask] chip mode: IMC not accessible (not Coffee Lake?)\r\n");
        return EFI_UNSUPPORTED;
    }

    uint64_t total = shim_cfl_total_memory();
    if (total == 0) {
        efi_print(st, L"[mask] chip mode: zero memory reported by IMC\r\n");
        return EFI_UNSUPPORTED;
    }

    efi_print(st, L"[mask] chip mode: scanning ");
    efi_print_dec(st, (UINTN)(total >> 30));
    efi_print(st, L" GiB address space for ");
    efi_print_dec(st, (UINTN)resolved);
    efi_print(st, L" chip(s)...\r\n");

    unsigned reserved = 0, skipped = 0;
    uint64_t next_progress = 0x40000000ULL; // first progress at 1 GiB

    for (uint64_t pa = 0; pa < total; pa += 4096) {
        // Progress indicator every 1 GiB.
        if (pa >= next_progress) {
            efi_print(st, L"[chip] scanning PA 0x");
            CHAR16 hex[20];
            efi_fmt_hex(hex, pa >> 30, 1);
            efi_print(st, hex);
            efi_print(st, L" GiB...\r\n");
            next_progress += 0x40000000ULL;
        }

        struct shim_pa_decoded d = shim_cfl_decode_pa(pa);
        if (!d.valid) continue;

        // Match against resolved chip entries.
        int match = 0;
        for (unsigned i = 0; i < chip_count && !match; i++) {
            if (!chips[i].resolved) continue;
            if (chips[i].channel == d.channel && chips[i].rank == d.rank)
                match = 1;
        }
        if (!match) continue;

        EFI_PHYSICAL_ADDRESS addr = (EFI_PHYSICAL_ADDRESS)pa;
        EFI_STATUS s = st->BootServices->AllocatePages(
            AllocateAddress, EfiReservedMemoryType, 1, &addr);
        if (s == EFI_SUCCESS) {
            reserved++;
        } else {
            skipped++;
        }
    }

    efi_print(st, L"[mask] chip pages reserved: ");
    efi_print_dec(st, (UINTN)reserved);
    efi_print(st, L", skipped: ");
    efi_print_dec(st, (UINTN)skipped);
    efi_print(st, L"\r\n");

    return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// Row-level masking (Track C).
// ---------------------------------------------------------------------------

// Per-row PA enumeration cap: ~2 pages per row for a 16 KB row (4 chips * 4096).
// Use 2048 to bound scan time — at 4 KiB steps over 32 GiB that's 8M iterations.
// In practice a single DRAM row produces ~2 PAs in the physical map.
#define ROW_ENUM_CAP  2048

static EFI_STATUS mask_rows(EFI_SYSTEM_TABLE *st,
                             badmem_row_t *rows, unsigned count)
{
    if (count == 0) return EFI_SUCCESS;

    if (!shim_cfl_init()) {
        efi_print(st, L"[mask] row mode: IMC not accessible (not Coffee Lake?)\r\n");
        return EFI_UNSUPPORTED;
    }

    uint64_t total = shim_cfl_total_memory();
    if (total == 0) {
        efi_print(st, L"[mask] row mode: zero memory reported by IMC\r\n");
        return EFI_UNSUPPORTED;
    }

    efi_print(st, L"[mask] row mode: enumerating PAs for ");
    efi_print_dec(st, (UINTN)count);
    efi_print(st, L" bad row(s)...\r\n");

    // Static PA buffer — shared across all rows.
    static uint64_t pa_buf[ROW_ENUM_CAP];

    unsigned total_reserved = 0;
    unsigned total_skipped  = 0;

    for (unsigned i = 0; i < count; i++) {
        unsigned n_pas = shim_cfl_enumerate_row(
            rows[i].channel, rows[i].rank,
            rows[i].bank_group, rows[i].bank, rows[i].row,
            pa_buf, ROW_ENUM_CAP);

        for (unsigned j = 0; j < n_pas; j++) {
            EFI_PHYSICAL_ADDRESS addr = (EFI_PHYSICAL_ADDRESS)pa_buf[j];
            EFI_STATUS s = st->BootServices->AllocatePages(
                AllocateAddress, EfiReservedMemoryType, 1, &addr);
            if (s == EFI_SUCCESS)
                total_reserved++;
            else
                total_skipped++;
        }
    }

    efi_print(st, L"[shim] Row-mode mask active (");
    efi_print_dec(st, (UINTN)count);
    efi_print(st, L" rows, ~");
    efi_print_dec(st, (UINTN)total_reserved);
    efi_print(st, L" pages reserved, ");
    efi_print_dec(st, (UINTN)total_skipped);
    efi_print(st, L" pre-reserved)\r\n");

    return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// Read BrrBadRows NVRAM variable (with legacy A1990BadRows fallback).
// Returns number of row tuples parsed.
// ---------------------------------------------------------------------------
static unsigned read_nvram_badrows(EFI_SYSTEM_TABLE *st,
                                    badmem_row_t *rows, unsigned cap)
{
    // Header (8 bytes) + 256 tuples * 8 bytes = 2056 bytes max.
    static UINT8 blob[8 + BADMEM_MAX_ROWS * 8];
    UINTN blob_sz = sizeof(blob);
    UINT32 attrs  = 0;

    static const EFI_GUID APPLE_GUID_READ2 = {
        0x7c436110, 0xab2a, 0x4bbb,
        { 0xa8, 0x80, 0xfe, 0x41, 0x99, 0x5c, 0x9f, 0x82 }
    };

    EFI_STATUS s = st->RuntimeServices->GetVariable(
        (CHAR16 *)VARNAME_BADROWS, (EFI_GUID *)&BRR_GUID,
        &attrs, &blob_sz, blob);

    if (s != EFI_SUCCESS) {
        // Apple-GUID fallback.
        blob_sz = sizeof(blob);
        attrs   = 0;
        s = st->RuntimeServices->GetVariable(
            (CHAR16 *)VARNAME_BADROWS, (EFI_GUID *)&APPLE_GUID_READ2,
            &attrs, &blob_sz, blob);
    }

    if (s != EFI_SUCCESS) {
        // Legacy A1990BadRows under BRR_GUID.
        blob_sz = sizeof(blob);
        attrs   = 0;
        s = st->RuntimeServices->GetVariable(
            (CHAR16 *)LEGACY_VARNAME_BADROWS, (EFI_GUID *)&BRR_GUID,
            &attrs, &blob_sz, blob);
    }

    if (s != EFI_SUCCESS) return 0;

    return badmem_parse_rows_blob(blob, (unsigned)blob_sz, rows, cap);
}

// ---------------------------------------------------------------------------
// Read BrrDecoderStatus NVRAM variable.
// Returns 1 if status == "VALIDATED", 0 otherwise.
// ---------------------------------------------------------------------------
static int read_nvram_decoder_status_validated(EFI_SYSTEM_TABLE *st)
{
    char buf[32] = {0};
    EFI_STATUS s = mask_nvram_get_ascii(st, VARNAME_DECODER_STATUS,
                                         buf, sizeof(buf));
    if (s != EFI_SUCCESS) return 0;
    return ascii_eq(buf, DECODER_STATUS_VALIDATED);
}

// Returns 1 if status == "FAILED", 0 otherwise.
static int read_nvram_decoder_status_failed(EFI_SYSTEM_TABLE *st)
{
    char buf[32] = {0};
    EFI_STATUS s = mask_nvram_get_ascii(st, VARNAME_DECODER_STATUS,
                                         buf, sizeof(buf));
    if (s != EFI_SUCCESS) return 0;
    return ascii_eq(buf, DECODER_STATUS_FAILED);
}

// ---------------------------------------------------------------------------
// Find internal macOS boot.efi.
// ---------------------------------------------------------------------------

// We enumerate all SimpleFileSystem handles and probe for the boot.efi path.
// We skip the current loaded-image device (that's our own device — USB or
// internal ESP — so we never accidentally re-load from it).
// We also skip removable media.

// EFI_FILE_INFO layout (per UEFI spec §12.5.2.2):
//   offset  size  field
//      0     8    Size           (total struct bytes incl. FileName)
//      8     8    FileSize
//     16     8    PhysicalSize
//     24    16    CreateTime       (EFI_TIME — 16 bytes fixed)
//     40    16    LastAccessTime
//     56    16    ModificationTime
//     72     8    Attribute        <-- check 0x10 for EFI_FILE_DIRECTORY
//     80     -    FileName[]       <-- NUL-terminated CHAR16 string
//
// Earlier revisions of this function used `8*5 + 16*3 = 88` for Attribute
// and `8*6 + 16*3 = 96` for FileName — a miscount that added 2 spurious u64s
// before Attribute.  The result: attribute reads were garbage (two of the
// EFI_TIME timestamp bytes), causing the directory check to randomly pass or
// fail, and filename reads started 16 bytes into the filename, corrupting
// the constructed path.  On T2 Macs with APFS Preboot, this meant
// `find_macos_boot` never succeeded despite probing all SFS volumes.
#define FILEINFO_ATTR_OFFS    72
#define FILEINFO_FNAME_OFFS   80

// Maximum directory entries we snapshot at one level.  Preboot usually has
// 1–5 UUID dirs plus "System" / ".fseventsd"; 32 is generous.
#define MAX_LEVEL_DIRS 32
// Max CHAR16 per UUID-ish name (a UUID is 36 chars; pad for safety).
#define MAX_DIRNAME_CHARS 64

// Collect child directory names under `dir` into out_names[] (flat buffer of
// MAX_LEVEL_DIRS * MAX_DIRNAME_CHARS CHAR16 slots).  Returns count.  Does NOT
// call any nested Open during iteration — so it's safe to re-Open after this
// returns, avoiding any firmware re-entrancy issues on the same file handle.
static unsigned list_child_dirs(EFI_FILE_PROTOCOL *dir,
                                 CHAR16 *out_names /* [MAX*MAX] */)
{
    if (!dir) return 0;
    EFI_STATUS s = dir->SetPosition(dir, 0);
    if (s != EFI_SUCCESS) return 0;

    unsigned n = 0;
    // 4 KiB buffer — comfortably handles CHAR16 filenames up to ~2000 chars
    // even though UEFI caps at 255.  Heap-alloc via static storage is fine
    // because mask-shim is single-threaded.
    static UINT8 dirent_buf[4096];

    for (;;) {
        UINTN dirent_sz = sizeof(dirent_buf);
        s = dir->Read(dir, &dirent_sz, dirent_buf);
        if (s == EFI_BUFFER_TOO_SMALL) {
            // Skip oversized entry — UEFI spec allows resubmission with
            // larger buffer, but in practice 4K is enough and dropping one
            // malformed entry beats aborting the whole scan.
            continue;
        }
        if (s != EFI_SUCCESS || dirent_sz == 0) break;

        UINT64 attrs = *(UINT64 *)(dirent_buf + FILEINFO_ATTR_OFFS);
        CHAR16 *fname = (CHAR16 *)(dirent_buf + FILEINFO_FNAME_OFFS);

        // Skip "." / ".." always.
        if (fname[0] == L'.' && fname[1] == 0) continue;
        if (fname[0] == L'.' && fname[1] == L'.' && fname[2] == 0) continue;
        // Skip empty names (defensive).
        if (fname[0] == 0) continue;

        // Prefer directories, but if the firmware doesn't set the DIR
        // bit correctly (observed on some T2 APFS drivers) fall back to
        // accepting every entry — we'll validate by trying to Open the
        // full path, which will just fail for non-dirs.
        if (!(attrs & 0x10 /* EFI_FILE_DIRECTORY */)) {
            // Accept anyway.  Worst case: extra Open() attempts on
            // regular files, each returns EFI_NOT_FOUND cheaply.
        }

        // Copy into slot[n].
        CHAR16 *slot = out_names + (UINTN)n * MAX_DIRNAME_CHARS;
        UINTN k;
        for (k = 0; fname[k] && k + 1 < MAX_DIRNAME_CHARS; k++)
            slot[k] = fname[k];
        slot[k] = 0;
        n++;
        if (n >= MAX_LEVEL_DIRS) break;
    }
    return n;
}

// Join prefix + "\" + name + suffix into out[].  Returns total chars (excl NUL).
static UINTN path_join(CHAR16 *out, UINTN cap,
                        const CHAR16 *prefix, const CHAR16 *name,
                        const CHAR16 *suffix)
{
    UINTN pos = 0;
    if (prefix) {
        for (UINTN i = 0; prefix[i] && pos + 1 < cap; i++)
            out[pos++] = prefix[i];
    }
    if (name) {
        if (pos + 1 < cap) out[pos++] = L'\\';
        for (UINTN i = 0; name[i] && pos + 1 < cap; i++)
            out[pos++] = name[i];
    }
    if (suffix) {
        for (UINTN i = 0; suffix[i] && pos + 1 < cap; i++)
            out[pos++] = suffix[i];
    }
    out[pos] = 0;
    return pos;
}

static EFI_STATUS try_path(EFI_SYSTEM_TABLE *st,
                            EFI_HANDLE device,
                            EFI_FILE_PROTOCOL *root,
                            const CHAR16 *path,
                            EFI_DEVICE_PATH_PROTOCOL **out_path)
{
    EFI_FILE_PROTOCOL *f = NULL;
    EFI_STATUS s = root->Open(root, &f, (CHAR16 *)path, EFI_FILE_MODE_READ, 0);
    if (s != EFI_SUCCESS) return s;
    f->Close(f);
    return build_file_device_path(st, device, path, out_path);
}

// Try each candidate macOS boot.efi path under `root` on the given handle.
// Returns EFI_SUCCESS on first hit, writing *out_path via build_file_device_path.
//
// Candidate paths (in priority order):
//   1. \System\Library\CoreServices\boot.efi             (HFS+ root)
//   2. \com.apple.recovery.boot\boot.efi                 (Recovery)
//   3. \usr\standalone\i386\boot.efi                     (firmware staging)
//   4. \<dir>\System\Library\CoreServices\boot.efi       (APFS Preboot, 1 level)
//   5. \<dir>\<subdir>\System\Library\CoreServices\boot.efi  (some nested layouts)
//
// Scanning is two-phase to avoid re-entrant Open() calls during Read()
// iteration — first we snapshot directory names with list_child_dirs(),
// then we iterate the snapshot and Open each candidate path.  This avoids
// any firmware quirk where nested Open on the same directory handle could
// corrupt Read iteration state.
static EFI_STATUS try_open_candidates(EFI_SYSTEM_TABLE *st,
                                       EFI_HANDLE device,
                                       EFI_FILE_PROTOCOL *root,
                                       EFI_DEVICE_PATH_PROTOCOL **out_path)
{
    static CHAR16 full_path[512];
    EFI_STATUS s;

    // Tier 1: fixed root-level candidates.
    for (unsigned i = 0; MACOS_ALT_PATHS[i]; i++) {
        s = try_path(st, device, root, MACOS_ALT_PATHS[i], out_path);
        if (s == EFI_SUCCESS) return s;
    }

    // Tier 2: snapshot 1-level dirs, then probe each with every alt suffix.
    static CHAR16 level1_names[MAX_LEVEL_DIRS * MAX_DIRNAME_CHARS];
    unsigned n1 = list_child_dirs(root, level1_names);

    for (unsigned i = 0; i < n1; i++) {
        CHAR16 *name = level1_names + (UINTN)i * MAX_DIRNAME_CHARS;

        // Try <dir>\<each-alt-path>.  The first alt is the standard
        // Preboot suffix \System\Library\CoreServices\boot.efi, which
        // catches APFS UUID-prefix layout.  The others cover recovery
        // and firmware-staging layouts inside UUID subdirs.
        for (unsigned ai = 0; MACOS_ALT_PATHS[ai]; ai++) {
            path_join(full_path, sizeof(full_path) / sizeof(full_path[0]),
                      L"\\", name, MACOS_ALT_PATHS[ai]);
            s = try_path(st, device, root, full_path, out_path);
            if (s == EFI_SUCCESS) return s;
        }

        // Tier 3: open <name> and snapshot its children, then probe.
        CHAR16 dir_only[128];
        path_join(dir_only, sizeof(dir_only) / sizeof(dir_only[0]),
                  L"\\", name, L"");

        EFI_FILE_PROTOCOL *sub = NULL;
        s = root->Open(root, &sub, dir_only, EFI_FILE_MODE_READ, 0);
        if (s != EFI_SUCCESS) continue;

        static CHAR16 level2_names[MAX_LEVEL_DIRS * MAX_DIRNAME_CHARS];
        unsigned n2 = list_child_dirs(sub, level2_names);
        sub->Close(sub);

        for (unsigned j = 0; j < n2; j++) {
            CHAR16 *name2 = level2_names + (UINTN)j * MAX_DIRNAME_CHARS;

            UINTN pos = 0;
            UINTN cap = sizeof(full_path) / sizeof(full_path[0]);
            full_path[pos++] = L'\\';
            for (UINTN k = 0; name[k] && pos + 1 < cap; k++)
                full_path[pos++] = name[k];
            full_path[pos++] = L'\\';
            for (UINTN k = 0; name2[k] && pos + 1 < cap; k++)
                full_path[pos++] = name2[k];
            for (UINTN k = 0; MACOS_BOOT_SUFFIX[k] && pos + 1 < cap; k++)
                full_path[pos++] = MACOS_BOOT_SUFFIX[k];
            full_path[pos] = 0;

            s = try_path(st, device, root, full_path, out_path);
            if (s == EFI_SUCCESS) return s;
        }
    }

    return EFI_NOT_FOUND;
}

static EFI_STATUS find_macos_boot(EFI_SYSTEM_TABLE *st,
                                   EFI_HANDLE image_handle,
                                   EFI_HANDLE *out_device,
                                   EFI_DEVICE_PATH_PROTOCOL **out_path)
{
    static const EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    static const EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    static const EFI_GUID lip_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;

    // Get our own device handle so we can skip it.
    EFI_HANDLE own_dev = NULL;
    {
        EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
        EFI_STATUS s2 = st->BootServices->HandleProtocol(
            image_handle, (EFI_GUID *)&lip_guid, (void **)&li);
        if (s2 == EFI_SUCCESS && li) own_dev = li->DeviceHandle;
    }

    EFI_HANDLE *handles = NULL;
    UINTN buf_sz = 0;
    EFI_STATUS s = st->BootServices->LocateHandle(
        EFI_LOCATE_BY_PROTOCOL, (EFI_GUID *)&sfs_guid, NULL, &buf_sz, NULL);
    if (s != EFI_BUFFER_TOO_SMALL && s != EFI_SUCCESS) return s;

    s = st->BootServices->AllocatePool(EfiLoaderData, buf_sz, (void **)&handles);
    if (s != EFI_SUCCESS) return s;

    s = st->BootServices->LocateHandle(
        EFI_LOCATE_BY_PROTOCOL, (EFI_GUID *)&sfs_guid, NULL, &buf_sz, handles);
    if (s != EFI_SUCCESS) { st->BootServices->FreePool(handles); return s; }

    UINTN n_handles = buf_sz / sizeof(EFI_HANDLE);
    EFI_STATUS found = EFI_NOT_FOUND;
    unsigned probed = 0, skipped_self = 0, skipped_removable = 0;

    // Two-pass: first pass skips our own device (preferred path — we want
    // the internal SSD, not re-booting from USB).  Second pass ONLY runs if
    // the first found nothing, and includes own_dev as a last-resort check
    // (covers firmware quirks where own_dev is misreported).
    for (int pass = 0; pass < 2 && found != EFI_SUCCESS; pass++) {
        probed = 0;
        skipped_self = 0;
        skipped_removable = 0;

        for (UINTN i = 0; i < n_handles; i++) {
            if (pass == 0 && handles[i] == own_dev) { skipped_self++; continue; }

            // Skip removable media on first pass only.
            if (pass == 0) {
                EFI_BLOCK_IO_PROTOCOL *bio = NULL;
                s = st->BootServices->HandleProtocol(
                    handles[i], (EFI_GUID *)&bio_guid, (void **)&bio);
                if (s == EFI_SUCCESS && bio->Media && bio->Media->RemovableMedia) {
                    skipped_removable++;
                    continue;
                }
            }

            EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs = NULL;
            s = st->BootServices->HandleProtocol(
                handles[i], (EFI_GUID *)&sfs_guid, (void **)&sfs);
            if (s != EFI_SUCCESS) continue;

            EFI_FILE_PROTOCOL *root = NULL;
            s = sfs->OpenVolume(sfs, &root);
            if (s != EFI_SUCCESS) continue;

            probed++;

            // Diagnostic: on the FIRST pass, dump up to 3 root child-dir
            // names so a user photo reveals what the volume contains.
            // Helps debug the "NOT FOUND" case remotely.
            if (pass == 0) {
                static CHAR16 peek_names[3 * MAX_DIRNAME_CHARS];
                unsigned peek_n = 0;
                EFI_STATUS ps = root->SetPosition(root, 0);
                if (ps == EFI_SUCCESS) {
                    static UINT8 peek_buf[4096];
                    while (peek_n < 3) {
                        UINTN psz = sizeof(peek_buf);
                        ps = root->Read(root, &psz, peek_buf);
                        if (ps == EFI_BUFFER_TOO_SMALL) continue;
                        if (ps != EFI_SUCCESS || psz == 0) break;
                        UINT64 a = *(UINT64 *)(peek_buf + FILEINFO_ATTR_OFFS);
                        CHAR16 *fn = (CHAR16 *)(peek_buf + FILEINFO_FNAME_OFFS);
                        if (fn[0] == L'.' && fn[1] == 0) continue;
                        if (fn[0] == L'.' && fn[1] == L'.' && fn[2] == 0) continue;
                        (void)a;
                        CHAR16 *slot = peek_names + (UINTN)peek_n * MAX_DIRNAME_CHARS;
                        // Truncate to 12 chars for display.
                        UINTN k;
                        for (k = 0; fn[k] && k < 12; k++) slot[k] = fn[k];
                        slot[k] = 0;
                        peek_n++;
                    }
                }
                efi_print(st, L"[shim]  vol#");
                efi_print_dec(st, (UINTN)i);
                efi_print(st, L": ");
                for (unsigned pi = 0; pi < peek_n; pi++) {
                    if (pi > 0) efi_print(st, L", ");
                    efi_print(st, peek_names + (UINTN)pi * MAX_DIRNAME_CHARS);
                }
                if (peek_n == 0) efi_print(st, L"(empty)");
                efi_print(st, L"\r\n");
            }

            found = try_open_candidates(st, handles[i], root, out_path);
            root->Close(root);

            if (found == EFI_SUCCESS) {
                *out_device = handles[i];
                break;
            }
        }
    }

    efi_print(st, L"[shim] find_macos_boot: probed ");
    efi_print_dec(st, (UINTN)probed);
    efi_print(st, L" volume(s) (skipped ");
    efi_print_dec(st, (UINTN)skipped_self);
    efi_print(st, L" self, ");
    efi_print_dec(st, (UINTN)skipped_removable);
    efi_print(st, L" removable), ");
    efi_print(st, (found == EFI_SUCCESS) ? L"FOUND\r\n" : L"NOT FOUND\r\n");

    st->BootServices->FreePool(handles);
    return found;
}

// ---------------------------------------------------------------------------
// BootOrder fallback: iterate firmware-registered boot options and try the
// first that LoadImages successfully (skipping our own USB entry).
//
// EFI_LOAD_OPTION layout (per UEFI spec §3.1.3):
//   UINT32                     Attributes
//   UINT16                     FilePathListLength
//   CHAR16                     Description[] (null-terminated)
//   EFI_DEVICE_PATH_PROTOCOL   FilePathList[]  (may be multi-instance)
//   UINT8                      OptionalData[]
//
// We skip options whose Description contains L"USB" or the substring L"BRR"
// (avoid recursing into ourselves if we got registered).
// ---------------------------------------------------------------------------

static const EFI_GUID BS_GLOBAL_GUID_LOCAL = {
    0x8BE4DF61, 0x93CA, 0x11D2,
    { 0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C }
};

static int u16_contains(const CHAR16 *hay, const CHAR16 *needle)
{
    if (!hay || !needle) return 0;
    for (UINTN i = 0; hay[i]; i++) {
        UINTN j = 0;
        while (hay[i + j] && needle[j] && hay[i + j] == needle[j]) j++;
        if (!needle[j]) return 1;
    }
    return 0;
}

static EFI_STATUS try_bootorder_chainload(EFI_SYSTEM_TABLE *st, EFI_HANDLE self)
{
    // Read BootOrder.
    static UINT8 order_buf[512];
    UINTN order_sz = sizeof(order_buf);
    UINT32 attrs = 0;
    EFI_STATUS s = st->RuntimeServices->GetVariable(
        (CHAR16 *)L"BootOrder", (EFI_GUID *)&BS_GLOBAL_GUID_LOCAL,
        &attrs, &order_sz, order_buf);
    if (s != EFI_SUCCESS || order_sz < 2) return EFI_NOT_FOUND;

    UINT16 *order = (UINT16 *)order_buf;
    UINTN n_entries = order_sz / 2;

    efi_print(st, L"[shim] BootOrder fallback: ");
    efi_print_dec(st, n_entries);
    efi_print(st, L" entries\r\n");

    for (UINTN i = 0; i < n_entries; i++) {
        UINT16 slot = order[i];

        // Build L"Boot%04X" name.
        static const CHAR16 hex_digits[] = L"0123456789ABCDEF";
        CHAR16 name[9];
        name[0] = L'B'; name[1] = L'o'; name[2] = L'o'; name[3] = L't';
        name[4] = hex_digits[(slot >> 12) & 0xF];
        name[5] = hex_digits[(slot >>  8) & 0xF];
        name[6] = hex_digits[(slot >>  4) & 0xF];
        name[7] = hex_digits[(slot >>  0) & 0xF];
        name[8] = 0;

        static UINT8 opt_buf[4096];
        UINTN opt_sz = sizeof(opt_buf);
        UINT32 oattrs = 0;
        s = st->RuntimeServices->GetVariable(
            name, (EFI_GUID *)&BS_GLOBAL_GUID_LOCAL,
            &oattrs, &opt_sz, opt_buf);
        if (s != EFI_SUCCESS || opt_sz < 8) continue;

        // Parse EFI_LOAD_OPTION header.
        UINT16 fpl_len = (UINT16)opt_buf[4] | ((UINT16)opt_buf[5] << 8);
        CHAR16 *desc = (CHAR16 *)(opt_buf + 6);
        // Walk to end of desc (null-terminated CHAR16).
        UINTN di = 0;
        while ((UINTN)(6 + (di + 1) * 2) < opt_sz && desc[di] != 0) di++;
        UINTN desc_bytes = (di + 1) * 2;

        // Device path follows description.
        UINT8 *dp = opt_buf + 6 + desc_bytes;
        if ((UINTN)(dp - opt_buf) + fpl_len > opt_sz) continue;

        // Skip obvious non-candidates.
        if (u16_contains(desc, L"USB")) {
            efi_print(st, L"[shim]   ");
            efi_print(st, name);
            efi_print(st, L": skip (USB)\r\n");
            continue;
        }
        if (u16_contains(desc, L"BRR") || u16_contains(desc, L"mask")) {
            efi_print(st, L"[shim]   ");
            efi_print(st, name);
            efi_print(st, L": skip (self)\r\n");
            continue;
        }

        efi_print(st, L"[shim]   ");
        efi_print(st, name);
        efi_print(st, L": ");
        efi_print(st, desc);
        efi_print(st, L"\r\n");

        EFI_HANDLE new_image = NULL;
        EFI_STATUS ls = st->BootServices->LoadImage(
            0, self, (EFI_DEVICE_PATH_PROTOCOL *)dp, NULL, 0, &new_image);
        if (ls != EFI_SUCCESS) {
            efi_print(st, L"[shim]     LoadImage failed: ");
            efi_print_hex(st, (UINT64)ls);
            efi_print(st, L"\r\n");
            continue;
        }

        efi_print(st, L"[shim]     chainloading...\r\n");
        UINTN exit_sz = 0;
        CHAR16 *exit_data = NULL;
        ls = st->BootServices->StartImage(new_image, &exit_sz, &exit_data);
        efi_print(st, L"[shim]     returned: ");
        efi_print_hex(st, (UINT64)ls);
        efi_print(st, L"\r\n");
        // If StartImage returned, macOS panicked or user selected
        // something; no point trying other entries.
        return ls;
    }

    return EFI_NOT_FOUND;
}

// ---------------------------------------------------------------------------
// Chain-load macOS.
// ---------------------------------------------------------------------------

static EFI_STATUS chainload_macos(EFI_SYSTEM_TABLE *st, EFI_HANDLE self,
                                   EFI_HANDLE device,
                                   EFI_DEVICE_PATH_PROTOCOL *path)
{
    (void)device;
    EFI_HANDLE new_image = NULL;
    EFI_STATUS s = st->BootServices->LoadImage(
        0 /*BootPolicy=false*/, self, path, NULL, 0, &new_image);
    if (s != EFI_SUCCESS) {
        efi_print(st, L"[shim] LoadImage failed: ");
        efi_print_hex(st, (UINT64)s);
        efi_print(st, L"\r\n");
        return s;
    }

    efi_print(st, L"[shim] starting macOS boot.efi...\r\n");

    UINTN exit_sz = 0;
    CHAR16 *exit_data = NULL;
    s = st->BootServices->StartImage(new_image, &exit_sz, &exit_data);
    // If we reach here, StartImage returned — something went wrong in macOS.
    efi_print(st, L"[shim] macOS boot.efi returned: ");
    efi_print_hex(st, (UINT64)s);
    efi_print(st, L"\r\n");
    return s;
}

// ---------------------------------------------------------------------------
// Confirmation prompt — generic Y/N with timeout.
// Returns L'Y', L'N', or 0 on timeout.
// ---------------------------------------------------------------------------

static CHAR16 prompt_confirm(EFI_SYSTEM_TABLE *st, unsigned timeout_s)
{
    unsigned ticks = timeout_s * 10;
    for (unsigned t = 0; t < ticks; t++) {
        if (t % 10 == 0) {
            efi_print(st, L"  Timeout: ");
            efi_print_dec(st, (UINTN)(timeout_s - t / 10));
            efi_print(st, L" s  \r");
        }
        CHAR16 k = efi_readkey(st);
        if (k == L'Y' || k == L'y') { efi_newline(st); return L'Y'; }
        if (k == L'N' || k == L'n') { efi_newline(st); return L'N'; }
        efi_stall_ms(st, 100);
    }
    efi_newline(st);
    return 0; // timeout
}

// (legacy var names declared earlier above read_nvram_badpages)

// ---------------------------------------------------------------------------
// Read BrrBadChips NVRAM variable (with legacy A1990BadChips fallback) and
// populate chip entries from it.  Chips are comma-separated ASCII designators.
// Returns number of chips parsed.
// ---------------------------------------------------------------------------
static unsigned read_nvram_badchips(EFI_SYSTEM_TABLE *st,
                                    badmem_chip_t *chips, unsigned cap)
{
    static char chip_buf[256];
    EFI_STATUS s = mask_nvram_get_ascii(st, BRR_VARNAME_BADCHIPS,
                                         chip_buf, sizeof(chip_buf));
    if (s != EFI_SUCCESS) {
        // Fallback: try legacy A1990BadChips name.
        s = mask_nvram_get_ascii(st, LEGACY_VARNAME_BADCHIPS,
                                  chip_buf, sizeof(chip_buf));
    }
    if (s != EFI_SUCCESS) return 0;

    unsigned n = 0;
    char *p = chip_buf;
    while (*p && n < cap) {
        // Skip leading commas / spaces.
        while (*p == ',' || *p == ' ') p++;
        if (!*p) break;

        // Copy designator up to comma or end.
        unsigned di = 0;
        while (*p && *p != ',' && di + 1 < sizeof(chips[n].designator)) {
            chips[n].designator[di++] = *p++;
        }
        chips[n].designator[di] = '\0';
        if (di > 0) n++;
    }
    return n;
}

// ---------------------------------------------------------------------------
// Full NVRAM state machine.
//
// On entry: skip_mask=0, force_chip=0.
// On exit:
//   skip_mask=1  -> do not apply any mask (revert path taken).
//   force_chip=1 -> use chip masking from BrrBadChips (not badmem.txt).
// ---------------------------------------------------------------------------

static void handle_nvram_state(EFI_SYSTEM_TABLE *st, EFI_HANDLE image,
                                int *skip_mask, int *force_chip)
{
    char state[64] = {0};
    EFI_STATUS s = mask_nvram_get_ascii(st, BRR_VARNAME_STATE,
                                         state, sizeof(state));

    if (s != EFI_SUCCESS) {
        // No state variable — USB trial mode without memtest auto-run.
        // Proceed with whatever mask sources are available.
        return;
    }

    // -----------------------------------------------------------------------
    // TRIAL_PENDING_PAGE / TRIAL_PENDING_CHIP
    // Apply the mask and proceed to chainload macOS.  Do NOT advance state
    // here: state stays TRIAL_PENDING_* so that if the shim crashes or
    // boot.efi cannot be found, the user can try again (re-running this
    // shim won't lose its work -- the NVRAM is still pristine from
    // brr-entry.efi).  State only advances to TRIAL_BOOTED on a later
    // deliberate step (install.efi --permanent, not part of trial flow).
    // -----------------------------------------------------------------------
    if (ascii_eq(state, STATE_TRIAL_PENDING_PAGE)) {
        efi_print(st, L"[shim] state=TRIAL_PENDING_PAGE: applying page mask\r\n");
        return;
    }

    if (ascii_eq(state, STATE_TRIAL_PENDING_CHIP)) {
        efi_print(st, L"[shim] state=TRIAL_PENDING_CHIP: applying chip mask\r\n");
        *force_chip = 1;
        return;
    }

    // -----------------------------------------------------------------------
    // TRIAL_BOOTED
    // The efi_menu chainloads the shim when state=TRIAL_PENDING_*.
    // If shim IS invoked directly while TRIAL_BOOTED (e.g. permanent install
    // already done), fall back to: apply whatever mask is in NVRAM, chain macOS.
    // force_chip is left 0 so efi_main merges BrrBadPages + badmem.txt,
    // which is the full mask from the previous trial run.
    // -----------------------------------------------------------------------
    if (ascii_eq(state, STATE_TRIAL_BOOTED)) {
        efi_print(st, L"[shim] state=TRIAL_BOOTED: applying existing mask (fallback)\r\n");
        // No state transition — TRIAL_BOOTED persists so the EFI menu can
        // pick it up on the next USB boot and offer the "make permanent?" flow.
        return;
    }

    // -----------------------------------------------------------------------
    // PERMANENT_UNCONFIRMED
    // Single prompt "Did macOS boot OK?"
    //   Y -> PERMANENT_CONFIRMED (silent on future boots)
    //   N -> uninstall inline, skip masking, boot unmasked.
    // -----------------------------------------------------------------------
    if (ascii_eq(state, STATE_PERMANENT_UNCONFIRMED)) {
        efi_print(st, L"\r\n");
        efi_print(st, L"  BRR mask shim installed (PERMANENT_UNCONFIRMED).\r\n");
        efi_print(st, L"  Did macOS boot correctly with the memory mask?\r\n");
        efi_print(st, L"    Y = confirm installation (permanent)\r\n");
        efi_print(st, L"    N = uninstall and boot without mask\r\n");
        efi_print(st, L"    (timeout -> proceed; prompt repeats next boot)\r\n");
        efi_print(st, L"\r\n");

        CHAR16 answer = prompt_confirm(st, 30);
        if (answer == L'Y') {
            EFI_STATUS ss = mask_nvram_set_ascii(st, BRR_VARNAME_STATE,
                                                  STATE_PERMANENT_CONFIRMED);
            if (ss == EFI_SUCCESS)
                efi_print(st, L"[shim] state -> PERMANENT_CONFIRMED\r\n");
            else
                efi_print(st, L"[shim] WARNING: could not confirm state\r\n");
        } else if (answer == L'N') {
            efi_print(st, L"[shim] user declined — uninstalling mask...\r\n");
            const char *err = NULL;
            uninstall_mask_full(image, st, &err);
            *skip_mask = 1;
            efi_print(st, L"[shim] booting macOS without mask.\r\n");
            efi_stall_ms(st, 2000);
        }
        // timeout: stay PERMANENT_UNCONFIRMED; proceed with mask.
        return;
    }

    // -----------------------------------------------------------------------
    // PERMANENT_CONFIRMED: silent boot with mask.
    // -----------------------------------------------------------------------
    if (ascii_eq(state, STATE_PERMANENT_CONFIRMED)) {
        // Nothing to do; mask will be applied normally.
        return;
    }

    // Unknown state — log it and proceed.
    efi_print(st, L"[shim] unknown state '");
    efi_printa(st, state);
    efi_print(st, L"'; proceeding with mask\r\n");
}

// ---------------------------------------------------------------------------
// EFI entry point.
// ---------------------------------------------------------------------------

// Note: gnu-efi's crt0 calls efi_main in System V ABI (rdi=image, rsi=st),
// not Microsoft ABI.  Do NOT use EFIAPI here.
EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    st->ConOut->ClearScreen(st->ConOut);
    efi_print(st, L"BRR mask-shim v1\r\n");
    efi_print(st, L"=====================================\r\n");

    // Check load options for --no-mask / --passthrough flag.
    // grub passes the chainloader argument as LoadOptions (UTF-16).
    int force_no_mask = 0;
    {
        static const EFI_GUID lip_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
        EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
        EFI_STATUS s2 = st->BootServices->HandleProtocol(
            image, (EFI_GUID *)&lip_guid, (void **)&li);
        if (s2 == EFI_SUCCESS && li && li->LoadOptions && li->LoadOptionsSize > 0) {
            CHAR16 *opts = (CHAR16 *)li->LoadOptions;
            UINTN n16 = li->LoadOptionsSize / sizeof(CHAR16);
            static const CHAR16 nomask[]     = L"--no-mask";
            static const CHAR16 passthrough[] = L"--passthrough";
            UINTN flen1 = efi_strlen16(nomask);
            UINTN flen2 = efi_strlen16(passthrough);
            for (UINTN i = 0; i < n16; i++) {
                if (i + flen1 <= n16) {
                    UINTN m = 1;
                    for (UINTN j = 0; j < flen1; j++)
                        if (opts[i+j] != nomask[j]) { m = 0; break; }
                    if (m) { force_no_mask = 1; break; }
                }
                if (i + flen2 <= n16) {
                    UINTN m = 1;
                    for (UINTN j = 0; j < flen2; j++)
                        if (opts[i+j] != passthrough[j]) { m = 0; break; }
                    if (m) { force_no_mask = 1; break; }
                }
            }
        }
    }

    if (force_no_mask) {
        efi_print(st, L"[shim] passthrough mode: no masking\r\n");
    }

    // 1. NVRAM state machine (only in masked mode).
    int skip_mask  = force_no_mask;
    int force_chip = 0; // set to 1 by state machine for TRIAL_PENDING_CHIP
    if (!force_no_mask)
        handle_nvram_state(st, image, &skip_mask, &force_chip);

    // 2. Read bad-memory data and apply mask (unless user pressed N).
    static badmem_range_t ranges[BADMEM_MAX_RANGES];
    static badmem_chip_t  chips[BADMEM_MAX_CHIPS];
    static badmem_row_t   rows[BADMEM_MAX_ROWS];
    unsigned n_ranges = 0, n_chips = 0, n_rows = 0;

    if (!skip_mask) {
        // -----------------------------------------------------------------------
        // Track C decision tree:
        //
        //   decoder_status == VALIDATED && BrrBadRows present
        //       -> row-mode: enumerate and reserve per-row PAs (~8 KiB per row)
        //
        //   decoder_status == FAILED
        //       -> warn user; fall back to chip-mode from BrrBadChips
        //
        //   chip mode (force_chip from TRIAL_PENDING_CHIP, or fallback):
        //       -> full-channel mask from BrrBadChips
        //
        //   else:
        //       -> page-mode from BrrBadPages + badmem.txt
        // -----------------------------------------------------------------------

        int decoder_validated = read_nvram_decoder_status_validated(st);
        int decoder_failed    = read_nvram_decoder_status_failed(st);

        n_rows = read_nvram_badrows(st, rows, BADMEM_MAX_ROWS);

        if (decoder_validated && n_rows > 0) {
            // ------------------------------------------------------------------
            // ROW MODE: fine-grained masking — ~2 pages per bad row.
            // ------------------------------------------------------------------
            efi_print(st, L"[shim] decoder=VALIDATED, ");
            efi_print_dec(st, (UINTN)n_rows);
            efi_print(st, L" bad row(s) in NVRAM -> row-mode mask\r\n");
            mask_rows(st, rows, n_rows);

        } else if (decoder_failed || (force_chip && !decoder_validated)) {
            // ------------------------------------------------------------------
            // CHIP MODE: decoder failed (or chip trial) — coarse-grained mask.
            // Print warning banner when decoder explicitly FAILED.
            // ------------------------------------------------------------------
            if (decoder_failed) {
                efi_print(st, L"\r\n");
                efi_print(st, L"  *** WARNING: Decoder validation FAILED ***\r\n");
                efi_print(st, L"  Using full-channel chip mask (coarse-grained).\r\n");
                efi_print(st, L"  This masks ~16 GiB per bad chip instead of ~8 KiB.\r\n");
                efi_print(st, L"  To recover row-mode: run memtest again with an\r\n");
                efi_print(st, L"  updated decoder (DRAMA-style learning TBD).\r\n");
                efi_print(st, L"\r\n");
            }

            if (force_chip) {
                n_chips = read_nvram_badchips(st, chips, BADMEM_MAX_CHIPS);
                efi_print(st, L"[shim] chip mode: loaded ");
                efi_print_dec(st, (UINTN)n_chips);
                efi_print(st, L" chip designator(s) from NVRAM\r\n");
            } else {
                // force_chip == 0 but decoder_failed: load chips from NVRAM anyway.
                n_chips = read_nvram_badchips(st, chips, BADMEM_MAX_CHIPS);
                if (n_chips == 0) {
                    // No BrrBadChips: fall through to page-mode below.
                    goto page_mode;
                }
                efi_print(st, L"[shim] chip fallback: loaded ");
                efi_print_dec(st, (UINTN)n_chips);
                efi_print(st, L" chip designator(s) from NVRAM\r\n");
            }

            resolve_chip_entries(st, chips, n_chips);
            mask_chips(st, chips, n_chips);

        } else {
page_mode:
            // ------------------------------------------------------------------
            // PAGE MODE: existing behaviour — BrrBadPages + badmem.txt.
            // Also used as fallback when decoder status is UNKNOWN/absent.
            // ------------------------------------------------------------------
            if (!force_chip) {
                EFI_STATUS rs = read_badmem(st, image, ranges, &n_ranges,
                                            chips, &n_chips);
                if (rs != EFI_SUCCESS) {
                    efi_print(st, L"[shim] warning: could not read badmem.txt\r\n");
                }

                // Merge NVRAM bad pages.
                unsigned nvram_added = read_nvram_badpages(st, ranges, n_ranges,
                                                           BADMEM_MAX_RANGES);
                if (nvram_added > 0 || n_ranges > 0) {
                    efi_print(st, L"[mask] loaded ");
                    efi_print_dec(st, (UINTN)nvram_added);
                    efi_print(st, L" page(s) from NVRAM, ");
                    efi_print_dec(st, (UINTN)n_ranges);
                    efi_print(st, L" range(s) from badmem.txt\r\n");
                }
                n_ranges += nvram_added;
            }

            if (n_ranges > 0) {
                efi_print(st, L"[shim] ");
                efi_print_dec(st, (UINTN)n_ranges);
                efi_print(st, L" region range(s) to reserve\r\n");
                mask_pages(st, ranges, n_ranges);
            }

            if (n_chips > 0) {
                efi_print(st, L"[shim] ");
                efi_print_dec(st, (UINTN)n_chips);
                efi_print(st, L" chip directive(s) found\r\n");
                resolve_chip_entries(st, chips, n_chips);
                mask_chips(st, chips, n_chips);
            }

            if (n_ranges == 0 && n_chips == 0) {
                efi_print(st, L"[shim] No bad-memory data found.  Chain-loading macOS unmasked.\r\n");
            }
        }
    }

    // 3a. Primary: enumerate SFS volumes for boot.efi at known paths.
    EFI_HANDLE boot_device = NULL;
    EFI_DEVICE_PATH_PROTOCOL *boot_path = NULL;
    EFI_STATUS s = find_macos_boot(st, image, &boot_device, &boot_path);
    if (s == EFI_SUCCESS) {
        efi_print(st, L"[shim] found macOS boot.efi\r\n");
        efi_stall_ms(st, 1000);
        return chainload_macos(st, image, boot_device, boot_path);
    }

    // 3b. Fallback: try UEFI BootOrder.  Apple firmware registers
    // internal macOS under a Boot#### entry; if SFS scan missed it
    // (unusual APFS layout, firmware quirk), the registered device
    // path is authoritative.
    efi_print(st, L"[shim] primary scan failed, trying BootOrder...\r\n");
    s = try_bootorder_chainload(st, image);
    if (s == EFI_SUCCESS) return s;

    efi_print(st, L"[shim] ERROR: macOS boot.efi not found\r\n");
    efi_print(st, L"  Is internal SSD present? Is T2 security set correctly?\r\n");
    efi_stall_ms(st, 10000);
    return EFI_NOT_FOUND;
}
