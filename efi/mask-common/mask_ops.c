// SPDX-License-Identifier: GPL-2.0
//
// Shared install / uninstall logic for mask-install and mask-shim.
//
// Both install.efi and mask-shim.efi link this object to share the
// install_mask_full() / uninstall_mask_full() entry points.
//
// Copyright (C) 2024 A1990-memtest contributors.

#include "mask_ops.h"
#include "../efi_util.h"
#include "../badmem_parse.h"

// ---------------------------------------------------------------------------
// Exported GUID and variable names.
// ---------------------------------------------------------------------------

const EFI_GUID BRR_GUID = {
    0x3e3e9db2, 0x1a2b, 0x4b5c,
    { 0x9d, 0x1e, 0x5f, 0x6a, 0x7b, 0x8c, 0x9d, 0x0e }
};

static const EFI_GUID EFI_GLOBAL_GUID = {
    0x8be4df61, 0x93ca, 0x11d2,
    { 0xaa, 0x0d, 0x00, 0xe0, 0x98, 0x03, 0x2b, 0x8c }
};

// Exported (declared extern in mask_ops.h).
const CHAR16 BRR_VARNAME_STATE[]          = L"BrrMaskState";
const CHAR16 BRR_VARNAME_BADPAGES[]       = L"BrrBadPages";
const CHAR16 BRR_VARNAME_BADCHIPS[]       = L"BrrBadChips";
const CHAR16 BRR_VARNAME_ORIGBOOT[]       = L"BrrBackupBootOrder";
const CHAR16 BRR_VARNAME_ORIGBOOT_LEGACY[]= L"A1990MaskOriginalBootOrder"; // legacy fallback — do NOT rename
const CHAR16 BRR_VARNAME_BOOTSLOT[]       = L"BrrBootSlot";
const CHAR16 BRR_VARNAME_BOOTENTRIES[]    = L"BrrBackupBootEntries";

// Internal paths.
#define INTERNAL_MASK_DIR     L"EFI\\BRR"
#define INTERNAL_SHIM_PATH    L"EFI\\BRR\\mask-shim.efi"
#define INTERNAL_BADMEM       L"EFI\\BRR\\badmem.txt"
#define INTERNAL_BACKUP_DIR   L"EFI\\BRR\\backup"
#define INTERNAL_SHIM_BACKUP  L"EFI\\BRR\\backup\\mask-shim.efi"
#define INTERNAL_BADMEM_BKUP  L"EFI\\BRR\\backup\\badmem.txt"

#define USB_SHIM_PATH   L"EFI\\BOOT\\mask-shim.efi"
#define USB_BADMEM      L"EFI\\BOOT\\badmem.txt"

#define COPY_MAX_BYTES (4u * 1024u * 1024u)

// ---------------------------------------------------------------------------
// GUID aliases
// ---------------------------------------------------------------------------
static const EFI_GUID sfs_guid_ops = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static const EFI_GUID bio_guid_ops = EFI_BLOCK_IO_PROTOCOL_GUID;
static const EFI_GUID lip_guid_ops = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static const EFI_GUID fi_guid_ops  = EFI_FILE_INFO_GUID;
static const EFI_GUID dp_guid_ops  = EFI_DEVICE_PATH_PROTOCOL_GUID;

// ---------------------------------------------------------------------------
// NVRAM helpers (exported)
// ---------------------------------------------------------------------------

EFI_STATUS mask_nvram_set_ascii(EFI_SYSTEM_TABLE *st, const CHAR16 *name,
                                 const char *val)
{
    UINTN sz = 0;
    while (val[sz]) sz++;
    return st->RuntimeServices->SetVariable(
        (CHAR16 *)name, (EFI_GUID *)&BRR_GUID,
        EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS |
        EFI_VARIABLE_RUNTIME_ACCESS,
        sz, (void *)val);
}

EFI_STATUS mask_nvram_get_ascii(EFI_SYSTEM_TABLE *st, const CHAR16 *name,
                                 char *buf, UINTN bufsz)
{
    UINTN sz = bufsz - 1;
    UINT32 attrs = 0;
    EFI_STATUS s = st->RuntimeServices->GetVariable(
        (CHAR16 *)name, (EFI_GUID *)&BRR_GUID, &attrs, &sz, buf);
    if (s == EFI_SUCCESS) buf[sz] = '\0';
    return s;
}

void mask_nvram_delete(EFI_SYSTEM_TABLE *st, const CHAR16 *name)
{
    st->RuntimeServices->SetVariable(
        (CHAR16 *)name, (EFI_GUID *)&BRR_GUID, 0, 0, NULL);
}

static EFI_STATUS nvram_set_raw(EFI_SYSTEM_TABLE *st, const CHAR16 *name,
                                  const EFI_GUID *guid,
                                  const void *data, UINTN sz)
{
    return st->RuntimeServices->SetVariable(
        (CHAR16 *)name, (EFI_GUID *)guid,
        EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS |
        EFI_VARIABLE_RUNTIME_ACCESS,
        sz, (void *)data);
}

static EFI_STATUS nvram_get_raw(EFI_SYSTEM_TABLE *st, const CHAR16 *name,
                                  const EFI_GUID *guid, void *buf, UINTN *sz)
{
    UINT32 attrs = 0;
    return st->RuntimeServices->GetVariable(
        (CHAR16 *)name, (EFI_GUID *)guid, &attrs, sz, buf);
}

static void nvram_del_global(EFI_SYSTEM_TABLE *st, const CHAR16 *name)
{
    st->RuntimeServices->SetVariable(
        (CHAR16 *)name, (EFI_GUID *)&EFI_GLOBAL_GUID, 0, 0, NULL);
}

// ---------------------------------------------------------------------------
// Device path helpers
// ---------------------------------------------------------------------------

static UINTN dp_total_len_ops(const EFI_DEVICE_PATH_PROTOCOL *dp)
{
    UINTN total = 0;
    const UINT8 *p = (const UINT8 *)dp;
    for (;;) {
        UINT16 node_len = (UINT16)p[2] | ((UINT16)p[3] << 8);
        total += node_len;
        if (p[0] == 0x7f && p[1] == 0xff) break;
        p += node_len;
    }
    return total;
}

static EFI_STATUS build_full_device_path_ops(EFI_SYSTEM_TABLE *st,
                                              EFI_HANDLE device,
                                              const CHAR16 *path,
                                              EFI_DEVICE_PATH_PROTOCOL **out_dp,
                                              UINTN *out_sz)
{
    EFI_DEVICE_PATH_PROTOCOL *hw_dp = NULL;
    EFI_STATUS s = st->BootServices->HandleProtocol(
        device, (EFI_GUID *)&dp_guid_ops, (void **)&hw_dp);
    if (s != EFI_SUCCESS) hw_dp = NULL;

    UINTN prefix_len = 0;
    if (hw_dp) {
        UINTN total = dp_total_len_ops(hw_dp);
        prefix_len = (total >= 4) ? (total - 4) : 0;
    }

    UINTN name_len = efi_strlen16(path);
    UINTN fp_sz    = 4 + (name_len + 1) * sizeof(CHAR16);
    UINTN end_sz   = 4;
    UINTN total_sz = prefix_len + fp_sz + end_sz;

    void *buf = NULL;
    s = st->BootServices->AllocatePool(EfiLoaderData, total_sz, &buf);
    if (s != EFI_SUCCESS) return s;

    UINT8 *p = (UINT8 *)buf;
    if (prefix_len > 0) {
        const UINT8 *src = (const UINT8 *)hw_dp;
        for (UINTN i = 0; i < prefix_len; i++) p[i] = src[i];
        p += prefix_len;
    }
    p[0] = 0x04; p[1] = 0x04;
    p[2] = (UINT8)(fp_sz & 0xff);
    p[3] = (UINT8)(fp_sz >> 8);
    efi_strcpy16((CHAR16 *)(p + 4), path);
    p += fp_sz;
    p[0] = 0x7f; p[1] = 0xff; p[2] = 4; p[3] = 0;

    *out_dp = (EFI_DEVICE_PATH_PROTOCOL *)buf;
    *out_sz = total_sz;
    return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// File helpers
// ---------------------------------------------------------------------------

static EFI_STATUS copy_file_ops(EFI_SYSTEM_TABLE *st,
                                  EFI_FILE_PROTOCOL *src_root,
                                  const CHAR16 *src_name,
                                  EFI_FILE_PROTOCOL *dst_root,
                                  const CHAR16 *dst_name)
{
    EFI_FILE_PROTOCOL *src = NULL;
    EFI_STATUS s = src_root->Open(src_root, &src, (CHAR16 *)src_name,
                                   EFI_FILE_MODE_READ, 0);
    if (s != EFI_SUCCESS) return s;

    UINT8 info_buf[sizeof(EFI_FILE_INFO) + 256];
    UINTN info_sz = sizeof(info_buf);
    s = src->GetInfo(src, (EFI_GUID *)&fi_guid_ops, &info_sz, info_buf);
    if (s != EFI_SUCCESS) { src->Close(src); return s; }

    EFI_FILE_INFO *fi = (EFI_FILE_INFO *)info_buf;
    UINTN file_sz = (UINTN)fi->FileSize;
    if (file_sz > COPY_MAX_BYTES) { src->Close(src); return EFI_VOLUME_FULL; }
    if (file_sz == 0) {
        src->Close(src);
        EFI_FILE_PROTOCOL *dst = NULL;
        s = dst_root->Open(dst_root, &dst, (CHAR16 *)dst_name,
                            EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE |
                            EFI_FILE_MODE_CREATE, 0);
        if (s == EFI_SUCCESS) { dst->Flush(dst); dst->Close(dst); }
        return s;
    }

    void *file_buf = NULL;
    s = st->BootServices->AllocatePool(EfiLoaderData, file_sz, &file_buf);
    if (s != EFI_SUCCESS) { src->Close(src); return s; }

    UINTN read_sz = file_sz;
    s = src->Read(src, &read_sz, file_buf);
    src->Close(src);
    if (s != EFI_SUCCESS) { st->BootServices->FreePool(file_buf); return s; }

    EFI_FILE_PROTOCOL *dst = NULL;
    s = dst_root->Open(dst_root, &dst, (CHAR16 *)dst_name,
                        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE |
                        EFI_FILE_MODE_CREATE, 0);
    if (s != EFI_SUCCESS) { st->BootServices->FreePool(file_buf); return s; }

    UINTN write_sz = read_sz;
    s = dst->Write(dst, &write_sz, file_buf);
    dst->Flush(dst);
    dst->Close(dst);
    st->BootServices->FreePool(file_buf);
    return s;
}

static void mkdir_p_ops(EFI_FILE_PROTOCOL *root, const CHAR16 *path)
{
    EFI_FILE_PROTOCOL *d = NULL;
    root->Open(root, &d, (CHAR16 *)path,
               EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
               0x0000000000000010ULL);
    if (d) d->Close(d);
}

static void delete_file_ops(EFI_FILE_PROTOCOL *root, const CHAR16 *path)
{
    EFI_FILE_PROTOCOL *f = NULL;
    EFI_STATUS s = root->Open(root, &f, (CHAR16 *)path,
                               EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    if (s == EFI_SUCCESS) f->Delete(f);
}

// ---------------------------------------------------------------------------
// Boot* slot enumeration — for backup of pre-existing slots.
// ---------------------------------------------------------------------------

// Save a comma-separated list of currently-existing Boot* slot numbers (hex,
// e.g. "0100,0200,0041") into BrrBackupBootEntries.
// This lets revert distinguish our newly-created slot from pre-existing ones.
static void save_existing_boot_entries(EFI_SYSTEM_TABLE *st)
{
    // We build up to 512 bytes of "NNNN,NNNN,...\0".
    static char csv[512];
    UINTN csv_len = 0;
    unsigned count = 0;

    // GetNextVariableName iteration: start with empty name + zeroed GUID.
    static CHAR16 var_name[128];
    EFI_GUID var_guid;

    // Zero out starting state.
    var_name[0] = 0;
    {
        UINT8 *g = (UINT8 *)&var_guid;
        for (UINTN i = 0; i < sizeof(var_guid); i++) g[i] = 0;
    }

    for (;;) {
        UINTN name_sz = sizeof(var_name);
        EFI_STATUS s = st->RuntimeServices->GetNextVariableName(
            &name_sz, var_name, &var_guid);
        if (s == EFI_NOT_FOUND) break;
        if (s != EFI_SUCCESS)   break;

        // Match BootNNNN in the Global GUID namespace.
        // EFI_GLOBAL_GUID = {8be4df61-93ca-11d2-aa0d-00e098032b8c}
        const UINT8 global_bytes[16] = {
            0x61,0xdf,0xe4,0x8b, 0xca,0x93, 0xd2,0x11,
            0xaa,0x0d, 0x00,0xe0,0x98,0x03,0x2b,0x8c
        };
        const UINT8 *gb = (const UINT8 *)&var_guid;
        int is_global = 1;
        for (int i = 0; i < 16; i++) {
            if (gb[i] != global_bytes[i]) { is_global = 0; break; }
        }
        if (!is_global) continue;

        // Check name = "Boot" + 4 hex digits.
        if (var_name[0] != L'B' || var_name[1] != L'o' ||
            var_name[2] != L'o' || var_name[3] != L't') continue;

        // Must be exactly 8 chars total (Boot + 4 hex).
        UINTN nm_len = 0;
        while (var_name[nm_len]) nm_len++;
        if (nm_len != 8) continue;

        // All remaining 4 chars must be hex.
        int all_hex = 1;
        for (UINTN i = 4; i < 8; i++) {
            CHAR16 c = var_name[i];
            if (!((c >= L'0' && c <= L'9') ||
                  (c >= L'A' && c <= L'F') ||
                  (c >= L'a' && c <= L'f'))) { all_hex = 0; break; }
        }
        if (!all_hex) continue;

        // Append "NNNN" to csv (4 hex chars, uppercase).
        if (csv_len + 6 < sizeof(csv)) {
            if (count > 0) csv[csv_len++] = ',';
            for (UINTN i = 4; i < 8; i++) {
                CHAR16 c = var_name[i];
                if (c >= L'a' && c <= L'f') c -= (L'a' - L'A');
                csv[csv_len++] = (char)c;
            }
            csv[csv_len] = '\0';
            count++;
        }
    }

    if (count > 0) {
        st->RuntimeServices->SetVariable(
            (CHAR16 *)BRR_VARNAME_BOOTENTRIES, (EFI_GUID *)&BRR_GUID,
            EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS |
            EFI_VARIABLE_RUNTIME_ACCESS,
            csv_len, csv);
        efi_print(st, L"[mask-ops] saved ");
        efi_print_dec(st, (UINTN)count);
        efi_print(st, L" pre-existing Boot* slot(s) to BrrBackupBootEntries\r\n");
    } else {
        efi_print(st, L"[mask-ops] no pre-existing Boot* slots found\r\n");
    }
}

// ---------------------------------------------------------------------------
// Locate USB and internal ESP
// ---------------------------------------------------------------------------

static EFI_STATUS open_usb_root_ops(EFI_SYSTEM_TABLE *st, EFI_HANDLE img,
                                     EFI_FILE_PROTOCOL **root_out)
{
    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    EFI_STATUS s = st->BootServices->HandleProtocol(
        img, (EFI_GUID *)&lip_guid_ops, (void **)&li);
    if (s != EFI_SUCCESS) return s;

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs = NULL;
    s = st->BootServices->HandleProtocol(
        li->DeviceHandle, (EFI_GUID *)&sfs_guid_ops, (void **)&sfs);
    if (s != EFI_SUCCESS) return s;

    return sfs->OpenVolume(sfs, root_out);
}

static EFI_STATUS open_internal_esp_ops(EFI_SYSTEM_TABLE *st, EFI_HANDLE usb_dev,
                                         EFI_FILE_PROTOCOL **root_out,
                                         EFI_HANDLE *dev_out)
{
    EFI_HANDLE *handles = NULL;
    UINTN buf_sz = 0;
    EFI_STATUS s = st->BootServices->LocateHandle(
        EFI_LOCATE_BY_PROTOCOL, (EFI_GUID *)&sfs_guid_ops, NULL, &buf_sz, NULL);
    if (s != EFI_BUFFER_TOO_SMALL && s != EFI_SUCCESS) return s;

    s = st->BootServices->AllocatePool(EfiLoaderData, buf_sz, (void **)&handles);
    if (s != EFI_SUCCESS) return s;

    s = st->BootServices->LocateHandle(
        EFI_LOCATE_BY_PROTOCOL, (EFI_GUID *)&sfs_guid_ops, NULL, &buf_sz, handles);
    if (s != EFI_SUCCESS) { st->BootServices->FreePool(handles); return s; }

    UINTN n = buf_sz / sizeof(EFI_HANDLE);

    EFI_HANDLE   cand_dev[8];
    EFI_FILE_PROTOCOL *cand_root[8];
    UINTN n_cand = 0;

    for (UINTN i = 0; i < n; i++) {
        if (handles[i] == usb_dev) continue;

        EFI_BLOCK_IO_PROTOCOL *bio = NULL;
        EFI_STATUS s2 = st->BootServices->HandleProtocol(
            handles[i], (EFI_GUID *)&bio_guid_ops, (void **)&bio);
        if (s2 == EFI_SUCCESS && bio->Media && bio->Media->RemovableMedia)
            continue;

        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs = NULL;
        s2 = st->BootServices->HandleProtocol(
            handles[i], (EFI_GUID *)&sfs_guid_ops, (void **)&sfs);
        if (s2 != EFI_SUCCESS) continue;

        EFI_FILE_PROTOCOL *root = NULL;
        s2 = sfs->OpenVolume(sfs, &root);
        if (s2 != EFI_SUCCESS) continue;

        EFI_FILE_PROTOCOL *d = NULL;
        s2 = root->Open(root, &d, (CHAR16 *)L"EFI", EFI_FILE_MODE_READ, 0);
        if (s2 == EFI_SUCCESS) {
            d->Close(d);
            if (n_cand < 8) {
                cand_dev[n_cand]  = handles[i];
                cand_root[n_cand] = root;
                n_cand++;
            } else {
                root->Close(root);
            }
        } else {
            root->Close(root);
        }
    }

    st->BootServices->FreePool(handles);

    if (n_cand == 0) return EFI_NOT_FOUND;
    if (n_cand > 1) {
        for (UINTN i = 0; i < n_cand; i++) cand_root[i]->Close(cand_root[i]);
        efi_print(st, L"[mask-ops] ERROR: multiple internal ESP candidates.\r\n");
        efi_print(st, L"  Disconnect external drives and retry.\r\n");
        return EFI_ABORTED;
    }

    *root_out = cand_root[0];
    *dev_out  = cand_dev[0];
    return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// BootNNNN entry management
// ---------------------------------------------------------------------------

static EFI_STATUS install_boot_entry_ops(EFI_SYSTEM_TABLE *st,
                                          EFI_HANDLE esp_dev,
                                          const CHAR16 *shim_path,
                                          UINT16 *slot_out)
{
    static const CHAR16 desc[] = L"BizarreRamRepair bad-RAM mask";
    UINTN desc_bytes = (efi_strlen16(desc) + 1) * sizeof(CHAR16);

    EFI_DEVICE_PATH_PROTOCOL *full_dp = NULL;
    UINTN dp_bytes = 0;
    EFI_STATUS sdp = build_full_device_path_ops(st, esp_dev, shim_path,
                                                 &full_dp, &dp_bytes);
    if (sdp != EFI_SUCCESS) {
        UINTN path_chars = efi_strlen16(shim_path) + 1;
        UINTN fp_sz = 4 + path_chars * sizeof(CHAR16);
        dp_bytes = fp_sz + 4;
        void *fb = NULL;
        sdp = st->BootServices->AllocatePool(EfiLoaderData, dp_bytes, &fb);
        if (sdp != EFI_SUCCESS) return sdp;
        UINT8 *q = (UINT8 *)fb;
        q[0] = 0x04; q[1] = 0x04;
        q[2] = (UINT8)(fp_sz & 0xff); q[3] = (UINT8)(fp_sz >> 8);
        efi_strcpy16((CHAR16 *)(q + 4), shim_path);
        q += fp_sz;
        q[0] = 0x7f; q[1] = 0xff; q[2] = 4; q[3] = 0;
        full_dp = (EFI_DEVICE_PATH_PROTOCOL *)fb;
    }

    UINTN total = 4 + 2 + desc_bytes + dp_bytes;
    static UINT8 load_opt[4096];
    if (total > sizeof(load_opt)) { st->BootServices->FreePool(full_dp); return EFI_BUFFER_TOO_SMALL; }

    UINT8 *p = load_opt;
    *(UINT32 *)p = 0x00000001; p += 4;
    *(UINT16 *)p = (UINT16)dp_bytes; p += 2;
    for (UINTN i = 0; i < desc_bytes; i++) ((UINT8 *)p)[i] = ((UINT8 *)desc)[i];
    p += desc_bytes;
    { const UINT8 *src = (const UINT8 *)full_dp; for (UINTN i = 0; i < dp_bytes; i++) p[i] = src[i]; }
    p += dp_bytes; (void)p;

    st->BootServices->FreePool(full_dp);

    // Find free Boot slot 0x0100–0x01FE.
    CHAR16 varname[16];
    UINT16 slot = 0x0100;
    for (; slot < 0x01FF; slot++) {
        varname[0]='B'; varname[1]='o'; varname[2]='o'; varname[3]='t';
        CHAR16 hex[5]; efi_fmt_hex(hex, slot, 4);
        varname[4]=hex[0]; varname[5]=hex[1]; varname[6]=hex[2]; varname[7]=hex[3];
        varname[8]=0;
        UINTN sz = 0;
        EFI_STATUS s = nvram_get_raw(st, varname, &EFI_GLOBAL_GUID, NULL, &sz);
        if (s == EFI_NOT_FOUND) break;
    }
    if (slot >= 0x01FF) return EFI_OUT_OF_RESOURCES;

    varname[0]='B'; varname[1]='o'; varname[2]='o'; varname[3]='t';
    { CHAR16 hex[5]; efi_fmt_hex(hex, slot, 4); varname[4]=hex[0]; varname[5]=hex[1]; varname[6]=hex[2]; varname[7]=hex[3]; }
    varname[8]=0;

    EFI_STATUS s = nvram_set_raw(st, varname, &EFI_GLOBAL_GUID, load_opt, total);
    if (s != EFI_SUCCESS) return s;

    *slot_out = slot;
    return EFI_SUCCESS;
}

static EFI_STATUS prepend_boot_order_ops(EFI_SYSTEM_TABLE *st, UINT16 slot,
                                          UINT16 *orig_buf, UINTN *orig_count)
{
    static UINT16 current[256];
    UINTN sz = sizeof(current);
    EFI_STATUS s = nvram_get_raw(st, L"BootOrder", &EFI_GLOBAL_GUID, current, &sz);
    UINTN n = 0;
    if (s == EFI_SUCCESS) n = sz / sizeof(UINT16);

    for (UINTN i = 0; i < n; i++) orig_buf[i] = current[i];
    *orig_count = n;

    static UINT16 new_order[257];
    new_order[0] = slot;
    for (UINTN i = 0; i < n; i++) new_order[i+1] = current[i];
    return nvram_set_raw(st, L"BootOrder", &EFI_GLOBAL_GUID,
                         new_order, (n+1)*sizeof(UINT16));
}

// ---------------------------------------------------------------------------
// install_mask_full()
// ---------------------------------------------------------------------------

EFI_STATUS install_mask_full(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st,
                              const char **err_msg)
{
    if (err_msg) *err_msg = NULL;

    // Locate USB root.
    EFI_FILE_PROTOCOL *usb_root = NULL;
    EFI_STATUS s = open_usb_root_ops(st, image_handle, &usb_root);
    if (s != EFI_SUCCESS) {
        if (err_msg) *err_msg = "cannot open USB volume";
        return s;
    }

    // Locate internal ESP.
    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    st->BootServices->HandleProtocol(image_handle, (EFI_GUID *)&lip_guid_ops, (void **)&li);
    EFI_HANDLE usb_dev = li ? li->DeviceHandle : NULL;

    EFI_FILE_PROTOCOL *esp_root = NULL;
    EFI_HANDLE esp_dev = NULL;
    s = open_internal_esp_ops(st, usb_dev, &esp_root, &esp_dev);
    if (s != EFI_SUCCESS) {
        usb_root->Close(usb_root);
        if (err_msg) *err_msg = "cannot find unique internal ESP";
        return s;
    }

    // Phase 4: enumerate pre-existing Boot* slots -> BrrBackupBootEntries.
    save_existing_boot_entries(st);

    // Phase E: backup existing \EFI\BRR\ files before overwriting.
    {
        EFI_FILE_PROTOCOL *check = NULL;
        EFI_STATUS sc = esp_root->Open(esp_root, &check,
                                        (CHAR16 *)INTERNAL_SHIM_PATH,
                                        EFI_FILE_MODE_READ, 0);
        if (sc == EFI_SUCCESS) {
            check->Close(check);
            efi_print(st, L"[mask-ops] backing up existing EFI\\BRR files...\r\n");
            mkdir_p_ops(esp_root, INTERNAL_MASK_DIR);
            mkdir_p_ops(esp_root, INTERNAL_BACKUP_DIR);
            // Best-effort: ignore errors from backup copies.
            copy_file_ops(st, esp_root, INTERNAL_SHIM_PATH,
                          esp_root, INTERNAL_SHIM_BACKUP);
            copy_file_ops(st, esp_root, INTERNAL_BADMEM,
                          esp_root, INTERNAL_BADMEM_BKUP);
        }
    }

    // Phase E: save current BootOrder as BrrBackupBootOrder.
    {
        static UINT16 cur_order[256];
        UINTN sz_o = sizeof(cur_order);
        EFI_STATUS so = nvram_get_raw(st, L"BootOrder", &EFI_GLOBAL_GUID,
                                       cur_order, &sz_o);
        if (so == EFI_SUCCESS && sz_o > 0) {
            nvram_set_raw(st, BRR_VARNAME_ORIGBOOT, &BRR_GUID,
                          cur_order, sz_o);
        }
    }

    // Create EFI\BRR directory.
    mkdir_p_ops(esp_root, L"EFI");
    mkdir_p_ops(esp_root, INTERNAL_MASK_DIR);

    // Copy mask-shim.efi from USB.
    efi_print(st, L"[mask-ops] copying mask-shim.efi...\r\n");
    s = copy_file_ops(st, usb_root, USB_SHIM_PATH, esp_root, INTERNAL_SHIM_PATH);
    if (s != EFI_SUCCESS) {
        delete_file_ops(esp_root, INTERNAL_SHIM_PATH);
        esp_root->Close(esp_root);
        usb_root->Close(usb_root);
        if (err_msg) *err_msg = "failed to copy mask-shim.efi";
        return s;
    }

    // Copy badmem.txt (allowed to be absent).
    efi_print(st, L"[mask-ops] copying badmem.txt...\r\n");
    s = copy_file_ops(st, usb_root, USB_BADMEM, esp_root, INTERNAL_BADMEM);
    if (s != EFI_SUCCESS && s != EFI_NOT_FOUND) {
        efi_print(st, L"[mask-ops] warning: badmem.txt copy failed (continuing)\r\n");
    }

    usb_root->Close(usb_root);

    // Register BootNNNN.
    efi_print(st, L"[mask-ops] registering EFI boot entry...\r\n");
    UINT16 slot = 0;
    s = install_boot_entry_ops(st, esp_dev, L"\\EFI\\BRR\\mask-shim.efi", &slot);
    if (s != EFI_SUCCESS) {
        delete_file_ops(esp_root, INTERNAL_SHIM_PATH);
        delete_file_ops(esp_root, INTERNAL_BADMEM);
        esp_root->Close(esp_root);
        if (err_msg) *err_msg = "could not create BootNNNN variable";
        return s;
    }

    efi_print(st, L"[mask-ops] boot slot: 0x");
    efi_print_hex(st, slot);
    efi_print(st, L"\r\n");

    nvram_set_raw(st, BRR_VARNAME_BOOTSLOT, &BRR_GUID, &slot, sizeof(slot));

    // Prepend slot to BootOrder (orig already saved above).
    static UINT16 orig_order[256];
    UINTN orig_n = 0;
    s = prepend_boot_order_ops(st, slot, orig_order, &orig_n);
    if (s != EFI_SUCCESS) {
        // Rollback.
        {
            CHAR16 bvn[9];
            bvn[0]='B'; bvn[1]='o'; bvn[2]='o'; bvn[3]='t';
            CHAR16 hx[5]; efi_fmt_hex(hx, slot, 4);
            bvn[4]=hx[0]; bvn[5]=hx[1]; bvn[6]=hx[2]; bvn[7]=hx[3]; bvn[8]=0;
            nvram_del_global(st, bvn);
        }
        mask_nvram_delete(st, BRR_VARNAME_BOOTSLOT);
        delete_file_ops(esp_root, INTERNAL_SHIM_PATH);
        delete_file_ops(esp_root, INTERNAL_BADMEM);
        esp_root->Close(esp_root);
        if (err_msg) *err_msg = "BootOrder update failed";
        return s;
    }

    esp_root->Close(esp_root);

    // Set state PERMANENT_UNCONFIRMED.
    mask_nvram_set_ascii(st, BRR_VARNAME_STATE, BRR_STATE_PERMANENT_UNCONFIRMED);

    efi_print(st, L"[mask-ops] install complete; state=PERMANENT_UNCONFIRMED\r\n");
    return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// uninstall_mask_full() — Phase 5 full teardown
// ---------------------------------------------------------------------------

// Delete a single file and report what happened.
// Returns 1 if deleted, 0 if not found / error.
static int delete_file_log(EFI_SYSTEM_TABLE *st, EFI_FILE_PROTOCOL *root,
                            const CHAR16 *path, const char *label)
{
    EFI_FILE_PROTOCOL *f = NULL;
    EFI_STATUS s = root->Open(root, &f, (CHAR16 *)path,
                              EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    if (s != EFI_SUCCESS) return 0;
    f->Delete(f);
    efi_print(st, L"  deleted: ");
    efi_printa(st, label);
    efi_print(st, L"\r\n");
    return 1;
}

EFI_STATUS uninstall_mask_full(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st,
                                const char **err_msg)
{
    if (err_msg) *err_msg = NULL;

    efi_print(st, L"[mask-ops] ---- revert teardown ----\r\n");

    unsigned n_boot_order_restored = 0;
    unsigned n_boot_entry_deleted  = 0;
    unsigned n_files_deleted       = 0;
    unsigned n_nvram_deleted       = 0;

    // -----------------------------------------------------------------------
    // Step 1: Restore BootOrder.
    // Read from canonical name first; fall back to legacy name for older installs.
    // -----------------------------------------------------------------------
    {
        static UINT16 orig_order[256];
        UINTN sz_o = sizeof(orig_order);
        EFI_STATUS s = nvram_get_raw(st, BRR_VARNAME_ORIGBOOT,
                                     &BRR_GUID, orig_order, &sz_o);
        if (s != EFI_SUCCESS || sz_o == 0) {
            // Fallback: legacy variable name from older installs.
            sz_o = sizeof(orig_order);
            s = nvram_get_raw(st, BRR_VARNAME_ORIGBOOT_LEGACY,
                              &BRR_GUID, orig_order, &sz_o);
        }
        if (s == EFI_SUCCESS && sz_o > 0) {
            EFI_STATUS sw = nvram_set_raw(st, L"BootOrder", &EFI_GLOBAL_GUID,
                                          orig_order, sz_o);
            if (sw == EFI_SUCCESS) {
                efi_print(st, L"  restored: BootOrder (");
                efi_print_dec(st, sz_o / sizeof(UINT16));
                efi_print(st, L" entries)\r\n");
                n_boot_order_restored = 1;
            } else {
                efi_print(st, L"  WARNING: BootOrder restore failed\r\n");
            }
        } else {
            efi_print(st, L"  no saved BootOrder — skipping (clean state)\r\n");
        }
    }

    // -----------------------------------------------------------------------
    // Step 2: Delete our BootNNNN variable (SetVariable size=0 to erase).
    // -----------------------------------------------------------------------
    {
        UINT16 slot = 0;
        UINTN slot_sz = sizeof(slot);
        EFI_STATUS s = nvram_get_raw(st, BRR_VARNAME_BOOTSLOT,
                                     &BRR_GUID, &slot, &slot_sz);
        if (s == EFI_SUCCESS) {
            CHAR16 varname[9];
            varname[0]='B'; varname[1]='o'; varname[2]='o'; varname[3]='t';
            CHAR16 hex[5]; efi_fmt_hex(hex, slot, 4);
            varname[4]=hex[0]; varname[5]=hex[1]; varname[6]=hex[2];
            varname[7]=hex[3]; varname[8]=0;

            // Delete by SetVariable with DataSize=0.
            st->RuntimeServices->SetVariable(
                varname, (EFI_GUID *)&EFI_GLOBAL_GUID, 0, 0, NULL);

            efi_print(st, L"  deleted: Boot");
            efi_print(st, hex);
            efi_print(st, L" (our BootNNNN entry)\r\n");
            n_boot_entry_deleted = 1;
        } else {
            efi_print(st, L"  no BrrBootSlot — boot entry already gone\r\n");
        }
    }

    // -----------------------------------------------------------------------
    // Step 3: Delete all files under \EFI\BRR\ on internal ESP.
    //         Covers: mask-shim.efi, badmem.txt, backup/mask-shim.efi,
    //                 backup/badmem.txt.
    //         Also deletes the backup\ subdirectory itself.
    // -----------------------------------------------------------------------
    {
        EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
        st->BootServices->HandleProtocol(image_handle, (EFI_GUID *)&lip_guid_ops,
                                         (void **)&li);
        EFI_HANDLE usb_dev = li ? li->DeviceHandle : NULL;

        EFI_FILE_PROTOCOL *esp_root = NULL;
        EFI_HANDLE esp_dev = NULL;
        EFI_STATUS s = open_internal_esp_ops(st, usb_dev, &esp_root, &esp_dev);
        (void)esp_dev;

        if (s != EFI_SUCCESS) {
            efi_print(st, L"  no internal ESP found — skipping file cleanup\r\n");
        } else {
            efi_print(st, L"  cleaning \\EFI\\BRR\\ on internal ESP...\r\n");

            // Delete known files in backup/ first, then backup/ dir itself,
            // then the main BRR/ files and dir.
            n_files_deleted += (unsigned)delete_file_log(st, esp_root,
                INTERNAL_SHIM_BACKUP, "\\EFI\\BRR\\backup\\mask-shim.efi");
            n_files_deleted += (unsigned)delete_file_log(st, esp_root,
                INTERNAL_BADMEM_BKUP, "\\EFI\\BRR\\backup\\badmem.txt");

            // Delete backup\ directory (only succeeds if empty).
            {
                EFI_FILE_PROTOCOL *d = NULL;
                EFI_STATUS sd = esp_root->Open(esp_root, &d,
                    (CHAR16 *)INTERNAL_BACKUP_DIR,
                    EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
                if (sd == EFI_SUCCESS) {
                    d->Delete(d);
                    efi_print(st, L"  deleted: \\EFI\\BRR\\backup\\\r\n");
                    n_files_deleted++;
                }
            }

            n_files_deleted += (unsigned)delete_file_log(st, esp_root,
                INTERNAL_SHIM_PATH, "\\EFI\\BRR\\mask-shim.efi");
            n_files_deleted += (unsigned)delete_file_log(st, esp_root,
                INTERNAL_BADMEM, "\\EFI\\BRR\\badmem.txt");

            // Delete BRR\ directory itself (only succeeds if now empty).
            {
                EFI_FILE_PROTOCOL *d = NULL;
                EFI_STATUS sd = esp_root->Open(esp_root, &d,
                    (CHAR16 *)INTERNAL_MASK_DIR,
                    EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
                if (sd == EFI_SUCCESS) {
                    d->Delete(d);
                    efi_print(st, L"  deleted: \\EFI\\BRR\\\r\n");
                    n_files_deleted++;
                }
            }

            esp_root->Close(esp_root);
        }
    }

    // -----------------------------------------------------------------------
    // Step 4: Delete ALL BRR NVRAM variables (vendor GUID namespace).
    //         Enumerate via GetNextVariableName and delete any that match.
    //         Also explicitly delete known names for reliability.
    // -----------------------------------------------------------------------
    efi_print(st, L"  cleaning BRR NVRAM variables...\r\n");

    // Explicit delete of every known variable (handles both canonical and legacy).
    static const CHAR16 * const known_vars[] = {
        BRR_VARNAME_STATE,
        BRR_VARNAME_BADPAGES,
        BRR_VARNAME_BADCHIPS,
        BRR_VARNAME_ORIGBOOT,
        BRR_VARNAME_ORIGBOOT_LEGACY,
        BRR_VARNAME_BOOTENTRIES,
        BRR_VARNAME_BOOTSLOT,
        // Also purge the probe var in case it was left behind.
        L"BrrMaskProbe",
    };
    static const unsigned n_known = sizeof(known_vars)/sizeof(known_vars[0]);

    for (unsigned i = 0; i < n_known; i++) {
        EFI_STATUS sd = st->RuntimeServices->SetVariable(
            (CHAR16 *)known_vars[i], (EFI_GUID *)&BRR_GUID, 0, 0, NULL);
        if (sd == EFI_SUCCESS) {
            efi_print(st, L"  deleted NVRAM: ");
            efi_print(st, known_vars[i]);
            efi_print(st, L"\r\n");
            n_nvram_deleted++;
        }
        // EFI_NOT_FOUND is expected for absent vars — idempotent.
    }

    // Sweep via GetNextVariableName to catch any other BRR vars
    // (e.g. from future additions or partial installs).
    {
        static CHAR16 sweep_name[128];
        EFI_GUID sweep_guid;
        sweep_name[0] = 0;
        {
            UINT8 *g = (UINT8 *)&sweep_guid;
            for (UINTN i = 0; i < sizeof(sweep_guid); i++) g[i] = 0;
        }

        for (;;) {
            UINTN name_sz = sizeof(sweep_name);
            EFI_STATUS s = st->RuntimeServices->GetNextVariableName(
                &name_sz, sweep_name, &sweep_guid);
            if (s == EFI_NOT_FOUND) break;
            if (s != EFI_SUCCESS)   break;

            // Check if this is our vendor GUID.
            const UINT8 *ga = (const UINT8 *)&sweep_guid;
            const UINT8 *gb = (const UINT8 *)&BRR_GUID;
            int is_ours = 1;
            for (int i = 0; i < 16; i++) {
                if (ga[i] != gb[i]) { is_ours = 0; break; }
            }
            if (!is_ours) continue;

            // Delete this variable.
            EFI_STATUS sd = st->RuntimeServices->SetVariable(
                sweep_name, &sweep_guid, 0, 0, NULL);
            if (sd == EFI_SUCCESS) {
                efi_print(st, L"  deleted NVRAM (sweep): ");
                efi_print(st, sweep_name);
                efi_print(st, L"\r\n");
                n_nvram_deleted++;
                // After deletion the iterator position is undefined;
                // restart from the beginning.
                sweep_name[0] = 0;
                {
                    UINT8 *g = (UINT8 *)&sweep_guid;
                    for (UINTN i = 0; i < sizeof(sweep_guid); i++) g[i] = 0;
                }
            }
        }
    }

    // -----------------------------------------------------------------------
    // Summary
    // -----------------------------------------------------------------------
    efi_print(st, L"\r\n[mask-ops] revert summary:\r\n");
    efi_print(st, L"  BootOrder restored: ");
    efi_print(st, n_boot_order_restored ? L"YES\r\n" : L"no (was clean)\r\n");
    efi_print(st, L"  BootNNNN deleted:   ");
    efi_print(st, n_boot_entry_deleted  ? L"YES\r\n" : L"no (not found)\r\n");
    efi_print(st, L"  Files deleted:      ");
    efi_print_dec(st, (UINTN)n_files_deleted);
    efi_print(st, L"\r\n");
    efi_print(st, L"  NVRAM vars deleted: ");
    efi_print_dec(st, (UINTN)n_nvram_deleted);
    efi_print(st, L"\r\n");
    efi_print(st, L"[mask-ops] revert complete.\r\n");

    return EFI_SUCCESS;
}
