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

// Known APFS Preboot path to macOS boot.efi.
static const CHAR16 MACOS_BOOT_PATH[] =
    L"\\System\\Library\\CoreServices\\boot.efi";

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
        // Fallback: try legacy A1990BadPages name.
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

static EFI_STATUS mask_pages(EFI_SYSTEM_TABLE *st,
                              const badmem_range_t *ranges, unsigned count)
{
    unsigned ok = 0, skip = 0;
    for (unsigned i = 0; i < count; i++) {
        EFI_PHYSICAL_ADDRESS addr = (EFI_PHYSICAL_ADDRESS)ranges[i].start;
        UINTN pages = (UINTN)(ranges[i].len / 4096);
        EFI_STATUS s = st->BootServices->AllocatePages(
            AllocateAddress, EfiReservedMemoryType, pages, &addr);
        if (s == EFI_SUCCESS) {
            ok++;
        } else {
            // Already reserved or firmware owns it — harmless.
            skip++;
        }
    }
    efi_print(st, L"[shim] masked ");
    efi_print_dec(st, (UINTN)ok);
    efi_print(st, L"/");
    efi_print_dec(st, (UINTN)count);
    efi_print(st, L" range(s) (");
    efi_print_dec(st, (UINTN)skip);
    efi_print(st, L" already reserved)\r\n");
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
// Find internal macOS boot.efi.
// ---------------------------------------------------------------------------

// We enumerate all SimpleFileSystem handles and probe for the boot.efi path.
// We skip the current loaded-image device (that's our own device — USB or
// internal ESP — so we never accidentally re-load from it).
// We also skip removable media.

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

    for (UINTN i = 0; i < n_handles; i++) {
        // Skip our own device (USB or the ESP we were loaded from).
        if (handles[i] == own_dev) continue;

        // Skip removable media.
        EFI_BLOCK_IO_PROTOCOL *bio = NULL;
        s = st->BootServices->HandleProtocol(
            handles[i], (EFI_GUID *)&bio_guid, (void **)&bio);
        if (s == EFI_SUCCESS && bio->Media && bio->Media->RemovableMedia)
            continue;

        // Try to open the volume and find boot.efi.
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs = NULL;
        s = st->BootServices->HandleProtocol(
            handles[i], (EFI_GUID *)&sfs_guid, (void **)&sfs);
        if (s != EFI_SUCCESS) continue;

        EFI_FILE_PROTOCOL *root = NULL;
        s = sfs->OpenVolume(sfs, &root);
        if (s != EFI_SUCCESS) continue;

        EFI_FILE_PROTOCOL *f = NULL;
        s = root->Open(root, &f, (CHAR16 *)MACOS_BOOT_PATH,
                       EFI_FILE_MODE_READ, 0);
        root->Close(root);
        if (s == EFI_SUCCESS) {
            f->Close(f);
            *out_device = handles[i];
            // Build a device path for LoadImage.
            found = build_file_device_path(st, handles[i],
                                           MACOS_BOOT_PATH, out_path);
            break;
        }
    }

    st->BootServices->FreePool(handles);
    return found;
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
// populate chip entries from it.  Chips are NUL-separated ASCII designators.
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
    // Auto-apply mask, set state to TRIAL_BOOTED, chain macOS.
    // (The EFI menu should have branched us here automatically from grub
    //  entry 1 when it detected these states.)
    // -----------------------------------------------------------------------
    if (ascii_eq(state, STATE_TRIAL_PENDING_PAGE)) {
        efi_print(st, L"[shim] state=TRIAL_PENDING_PAGE: applying page mask\r\n");
        // Mask sources: NVRAM BrrBadPages + badmem.txt (handled in main).
        EFI_STATUS ss = mask_nvram_set_ascii(st, BRR_VARNAME_STATE,
                                              STATE_TRIAL_BOOTED);
        if (ss == EFI_SUCCESS)
            efi_print(st, L"[shim] state -> TRIAL_BOOTED\r\n");
        else
            efi_print(st, L"[shim] WARNING: could not advance state to TRIAL_BOOTED\r\n");
        return;
    }

    if (ascii_eq(state, STATE_TRIAL_PENDING_CHIP)) {
        efi_print(st, L"[shim] state=TRIAL_PENDING_CHIP: applying chip mask\r\n");
        *force_chip = 1;
        EFI_STATUS ss = mask_nvram_set_ascii(st, BRR_VARNAME_STATE,
                                              STATE_TRIAL_BOOTED);
        if (ss == EFI_SUCCESS)
            efi_print(st, L"[shim] state -> TRIAL_BOOTED\r\n");
        else
            efi_print(st, L"[shim] WARNING: could not advance state to TRIAL_BOOTED\r\n");
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

    // 2. Read badmem.txt and mask pages/chips (unless user pressed N).
    static badmem_range_t ranges[BADMEM_MAX_RANGES];
    static badmem_chip_t  chips[BADMEM_MAX_CHIPS];
    unsigned n_ranges = 0, n_chips = 0;

    if (!skip_mask) {
        // In force_chip mode (TRIAL_PENDING_CHIP): load chip designators from
        // BrrBadChips NVRAM and do NOT use page ranges from badmem.txt.
        if (force_chip) {
            n_chips = read_nvram_badchips(st, chips, BADMEM_MAX_CHIPS);
            efi_print(st, L"[shim] chip mode: loaded ");
            efi_print_dec(st, (UINTN)n_chips);
            efi_print(st, L" chip designator(s) from NVRAM\r\n");
        } else {
            EFI_STATUS s = read_badmem(st, image, ranges, &n_ranges,
                                       chips, &n_chips);
            if (s != EFI_SUCCESS) {
                efi_print(st, L"[shim] warning: could not read badmem.txt\r\n");
            }

            // Merge NVRAM bad pages (written by memtest's badmem_log_flush_nvram).
            // NVRAM entries that duplicate file entries are silently skipped.
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

        unsigned region_ok = 0, chip_ok = 0;

        if (n_ranges > 0) {
            efi_print(st, L"[shim] ");
            efi_print_dec(st, (UINTN)n_ranges);
            efi_print(st, L" region range(s) to reserve\r\n");
            mask_pages(st, ranges, n_ranges);
            region_ok = n_ranges;
        }

        if (n_chips > 0) {
            efi_print(st, L"[shim] ");
            efi_print_dec(st, (UINTN)n_chips);
            efi_print(st, L" chip directive(s) found\r\n");
            resolve_chip_entries(st, chips, n_chips);
            mask_chips(st, chips, n_chips);
            chip_ok = n_chips;
        }

        if (region_ok == 0 && chip_ok == 0) {
            efi_print(st, L"[shim] no bad pages listed; proceeding without mask\r\n");
        }
    }

    // 3. Find macOS boot.efi.
    EFI_HANDLE boot_device = NULL;
    EFI_DEVICE_PATH_PROTOCOL *boot_path = NULL;
    EFI_STATUS s = find_macos_boot(st, image, &boot_device, &boot_path);
    if (s != EFI_SUCCESS) {
        efi_print(st, L"[shim] ERROR: macOS boot.efi not found\r\n");
        efi_print(st, L"  Is internal SSD present? Is T2 security set correctly?\r\n");
        efi_stall_ms(st, 10000);
        return s;
    }

    efi_print(st, L"[shim] found macOS boot.efi\r\n");
    efi_stall_ms(st, 1000);

    // 4. Chain-load macOS.
    return chainload_macos(st, image, boot_device, boot_path);
}
