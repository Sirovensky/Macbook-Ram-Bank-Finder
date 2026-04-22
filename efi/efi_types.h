// SPDX-License-Identifier: GPL-2.0
//
// Minimal UEFI type definitions for the mask-shim and mask-install EFI apps.
// Extends the types already in memtest86plus/boot/efi.h with the additional
// protocols needed by these standalone EFI applications.

#ifndef EFI_TYPES_H
#define EFI_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// ---- UEFI calling convention -----------------------------------------------

#if defined(__x86_64__) || defined(__i386__)
# define EFIAPI __attribute__((ms_abi))
#else
# define EFIAPI
#endif

// ---- Scalar types -----------------------------------------------------------

typedef uint64_t    UINTN;
typedef int64_t     INTN;
typedef uint64_t    EFI_STATUS;
typedef void       *EFI_HANDLE;
typedef uint64_t    EFI_PHYSICAL_ADDRESS;
typedef uint64_t    EFI_VIRTUAL_ADDRESS;
typedef uint16_t    CHAR16;
typedef uint8_t     UINT8;
typedef uint16_t    UINT16;
typedef uint32_t    UINT32;
typedef uint64_t    UINT64;
typedef int32_t     INT32;
typedef void        VOID;
typedef bool        BOOLEAN;

// ---- Status codes -----------------------------------------------------------

#define EFI_SUCCESS             ((EFI_STATUS)0)
#define EFI_ERROR(s)            ((s) & 0x8000000000000000ULL)
#define EFI_INVALID_PARAMETER   (0x8000000000000000ULL | 2)
#define EFI_UNSUPPORTED         (0x8000000000000000ULL | 3)
#define EFI_BUFFER_TOO_SMALL    (0x8000000000000000ULL | 5)
#define EFI_NOT_READY           (0x8000000000000000ULL | 6)
#define EFI_NOT_FOUND           (0x8000000000000000ULL | 14)
#define EFI_VOLUME_FULL         (0x8000000000000000ULL | 13)
#define EFI_ABORTED             (0x8000000000000000ULL | 21)
#define EFI_ACCESS_DENIED       (0x8000000000000000ULL | 15)
#define EFI_OUT_OF_RESOURCES    (0x8000000000000000ULL | 9)

// ---- GUID -------------------------------------------------------------------

typedef struct {
    UINT32  Data1;
    UINT16  Data2;
    UINT16  Data3;
    UINT8   Data4[8];
} EFI_GUID;

// ---- Table header -----------------------------------------------------------

typedef struct {
    UINT64  Signature;
    UINT32  Revision;
    UINT32  HeaderSize;
    UINT32  CRC32;
    UINT32  Reserved;
} EFI_TABLE_HEADER;

// ---- Memory -----------------------------------------------------------------

typedef enum {
    EfiReservedMemoryType      = 0,
    EfiLoaderCode,
    EfiLoaderData,
    EfiBootServicesCode,
    EfiBootServicesData,
    EfiRuntimeServicesCode,
    EfiRuntimeServicesData,
    EfiConventionalMemory,
    EfiUnusableMemory,
    EfiACPIReclaimMemory,
    EfiACPIMemoryNVS,
    EfiMemoryMappedIO,
    EfiMemoryMappedIOPortSpace,
    EfiPalCode,
    EfiPersistentMemory,
    EfiMaxMemoryType,
} EFI_MEMORY_TYPE;

typedef enum {
    AllocateAnyPages,
    AllocateMaxAddress,
    AllocateAddress,
    MaxAllocateType,
} EFI_ALLOCATE_TYPE;

typedef struct {
    UINT32                  Type;
    UINT32                  Pad;
    EFI_PHYSICAL_ADDRESS    PhysicalStart;
    EFI_VIRTUAL_ADDRESS     VirtualStart;
    UINT64                  NumberOfPages;
    UINT64                  Attribute;
} EFI_MEMORY_DESCRIPTOR;

// ---- Input key / ConIn ------------------------------------------------------

typedef struct {
    UINT16  ScanCode;
    CHAR16  UnicodeChar;
} EFI_INPUT_KEY;

typedef struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL_s {
    EFI_STATUS (EFIAPI *Reset)(struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL_s *, BOOLEAN);
    EFI_STATUS (EFIAPI *ReadKeyStroke)(struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL_s *, EFI_INPUT_KEY *);
    void       *WaitForKey;
} EFI_SIMPLE_TEXT_INPUT_PROTOCOL;

// ---- ConOut -----------------------------------------------------------------

typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_s {
    EFI_STATUS (EFIAPI *Reset)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_s *, BOOLEAN);
    EFI_STATUS (EFIAPI *OutputString)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_s *, CHAR16 *);
    EFI_STATUS (EFIAPI *TestString)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_s *, CHAR16 *);
    EFI_STATUS (EFIAPI *QueryMode)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_s *, UINTN, UINTN *, UINTN *);
    EFI_STATUS (EFIAPI *SetMode)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_s *, UINTN);
    EFI_STATUS (EFIAPI *SetAttribute)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_s *, UINTN);
    EFI_STATUS (EFIAPI *ClearScreen)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_s *);
    EFI_STATUS (EFIAPI *SetCursorPosition)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_s *, UINTN, UINTN);
    EFI_STATUS (EFIAPI *EnableCursor)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_s *, BOOLEAN);
    void       *Mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

// ---- Device path ------------------------------------------------------------

typedef struct {
    UINT8   Type;
    UINT8   SubType;
    UINT8   Length[2];
} EFI_DEVICE_PATH_PROTOCOL;

#define EFI_DP_END_TYPE             0x7f
#define EFI_DP_END_SUBTYPE          0xff
#define EFI_DP_MEDIA_TYPE           0x04
#define EFI_DP_MEDIA_FILEPATH       0x04
#define EFI_DP_HW_TYPE              0x02
#define EFI_DP_ACPI_TYPE            0x02
#define EFI_DP_MSG_TYPE             0x03

typedef struct {
    EFI_DEVICE_PATH_PROTOCOL    Header; // Type=4 SubType=4
    CHAR16                      PathName[1]; // variable length
} EFI_FILEPATH_DEVICE_PATH;

// ---- Loaded image -----------------------------------------------------------

typedef struct {
    UINT32                      Revision;
    EFI_HANDLE                  ParentHandle;
    void                       *SystemTable;
    EFI_HANDLE                  DeviceHandle;
    EFI_DEVICE_PATH_PROTOCOL   *FilePath;
    void                       *Reserved;
    UINT32                      LoadOptionsSize;
    void                       *LoadOptions;
    void                       *ImageBase;
    UINT64                      ImageSize;
    EFI_MEMORY_TYPE             ImageCodeType;
    EFI_MEMORY_TYPE             ImageDataType;
    EFI_STATUS (EFIAPI         *Unload)(EFI_HANDLE);
} EFI_LOADED_IMAGE_PROTOCOL;

// ---- Simple file system / EFI_FILE_PROTOCOL ---------------------------------

typedef struct EFI_FILE_PROTOCOL_s {
    UINT64  Revision;
    EFI_STATUS (EFIAPI *Open)(struct EFI_FILE_PROTOCOL_s *, struct EFI_FILE_PROTOCOL_s **, CHAR16 *, UINT64, UINT64);
    EFI_STATUS (EFIAPI *Close)(struct EFI_FILE_PROTOCOL_s *);
    EFI_STATUS (EFIAPI *Delete)(struct EFI_FILE_PROTOCOL_s *);
    EFI_STATUS (EFIAPI *Read)(struct EFI_FILE_PROTOCOL_s *, UINTN *, void *);
    EFI_STATUS (EFIAPI *Write)(struct EFI_FILE_PROTOCOL_s *, UINTN *, void *);
    EFI_STATUS (EFIAPI *GetPosition)(struct EFI_FILE_PROTOCOL_s *, UINT64 *);
    EFI_STATUS (EFIAPI *SetPosition)(struct EFI_FILE_PROTOCOL_s *, UINT64);
    EFI_STATUS (EFIAPI *GetInfo)(struct EFI_FILE_PROTOCOL_s *, EFI_GUID *, UINTN *, void *);
    EFI_STATUS (EFIAPI *SetInfo)(struct EFI_FILE_PROTOCOL_s *, EFI_GUID *, UINTN, void *);
    EFI_STATUS (EFIAPI *Flush)(struct EFI_FILE_PROTOCOL_s *);
} EFI_FILE_PROTOCOL;

#define EFI_FILE_MODE_READ    0x0000000000000001ULL
#define EFI_FILE_MODE_WRITE   0x0000000000000002ULL
#define EFI_FILE_MODE_CREATE  0x8000000000000000ULL

typedef struct {
    UINT64  Size;         // total sizeof(EFI_FILE_INFO) + name
    UINT64  FileSize;
    UINT64  PhysicalSize;
    // EFI_TIME CreateTime, LastAccessTime, ModificationTime (3×16 bytes = 48)
    UINT8   Times[48];
    UINT64  Attribute;
    CHAR16  FileName[1]; // variable
} EFI_FILE_INFO;

typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_s {
    UINT64     Revision;
    EFI_STATUS (EFIAPI *OpenVolume)(struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_s *,
                                    EFI_FILE_PROTOCOL **);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

// ---- Block I/O --------------------------------------------------------------

typedef struct {
    UINT32  MediaId;
    BOOLEAN RemovableMedia;
    BOOLEAN MediaPresent;
    BOOLEAN LogicalPartition;
    BOOLEAN ReadOnly;
    BOOLEAN WriteCaching;
    UINT8   pad[3];
    UINT32  BlockSize;
    UINT32  IoAlign;
    UINT64  LastBlock;
} EFI_BLOCK_IO_MEDIA;

typedef struct EFI_BLOCK_IO_PROTOCOL_s {
    UINT64              Revision;
    EFI_BLOCK_IO_MEDIA *Media;
    EFI_STATUS (EFIAPI *Reset)(struct EFI_BLOCK_IO_PROTOCOL_s *, BOOLEAN);
    EFI_STATUS (EFIAPI *ReadBlocks)(struct EFI_BLOCK_IO_PROTOCOL_s *, UINT32, UINT64, UINTN, void *);
    EFI_STATUS (EFIAPI *WriteBlocks)(struct EFI_BLOCK_IO_PROTOCOL_s *, UINT32, UINT64, UINTN, void *);
    EFI_STATUS (EFIAPI *FlushBlocks)(struct EFI_BLOCK_IO_PROTOCOL_s *);
} EFI_BLOCK_IO_PROTOCOL;

// ---- Boot services ----------------------------------------------------------

typedef EFI_STATUS (EFIAPI *EFI_IMAGE_LOAD)(
    BOOLEAN             BootPolicy,
    EFI_HANDLE          ParentImageHandle,
    EFI_DEVICE_PATH_PROTOCOL *DevicePath,
    void               *SourceBuffer,
    UINTN               SourceSize,
    EFI_HANDLE         *ImageHandle);

typedef EFI_STATUS (EFIAPI *EFI_IMAGE_START)(
    EFI_HANDLE          ImageHandle,
    UINTN              *ExitDataSize,
    CHAR16            **ExitData);

typedef EFI_STATUS (EFIAPI *EFI_HANDLE_PROTOCOL)(
    EFI_HANDLE          Handle,
    EFI_GUID           *Protocol,
    void              **Interface);

typedef EFI_STATUS (EFIAPI *EFI_LOCATE_PROTOCOL)(
    EFI_GUID           *Protocol,
    void               *Registration,
    void              **Interface);

typedef EFI_STATUS (EFIAPI *EFI_LOCATE_HANDLE)(
    int                 SearchType,
    EFI_GUID           *Protocol,
    void               *SearchKey,
    UINTN              *BufferSize,
    EFI_HANDLE         *Buffer);

typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_PAGES)(
    EFI_ALLOCATE_TYPE   Type,
    EFI_MEMORY_TYPE     MemoryType,
    UINTN               Pages,
    EFI_PHYSICAL_ADDRESS *Memory);

typedef EFI_STATUS (EFIAPI *EFI_FREE_PAGES)(
    EFI_PHYSICAL_ADDRESS Memory,
    UINTN                Pages);

typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_POOL)(
    EFI_MEMORY_TYPE     PoolType,
    UINTN               Size,
    void              **Buffer);

typedef EFI_STATUS (EFIAPI *EFI_FREE_POOL)(void *Buffer);

typedef EFI_STATUS (EFIAPI *EFI_SET_VARIABLE)(
    CHAR16             *VariableName,
    EFI_GUID           *VendorGuid,
    UINT32              Attributes,
    UINTN               DataSize,
    void               *Data);

typedef EFI_STATUS (EFIAPI *EFI_GET_VARIABLE)(
    CHAR16             *VariableName,
    EFI_GUID           *VendorGuid,
    UINT32             *Attributes,
    UINTN              *DataSize,
    void               *Data);

typedef EFI_STATUS (EFIAPI *EFI_GET_NEXT_VARIABLE_NAME)(
    UINTN              *VariableNameSize,
    CHAR16             *VariableName,
    EFI_GUID           *VendorGuid);

typedef void (EFIAPI *EFI_RESET_SYSTEM)(
    int   ResetType,
    EFI_STATUS ResetStatus,
    UINTN DataSize,
    void *ResetData);

typedef EFI_STATUS (EFIAPI *EFI_STALL)(UINTN Microseconds);

#define EFI_LOCATE_BY_PROTOCOL  2

#define EFI_VARIABLE_NON_VOLATILE           0x00000001
#define EFI_VARIABLE_BOOTSERVICE_ACCESS     0x00000002
#define EFI_VARIABLE_RUNTIME_ACCESS         0x00000004

typedef struct {
    EFI_TABLE_HEADER        Hdr;
    void                   *RaiseTpl;
    void                   *RestoreTpl;
    EFI_ALLOCATE_PAGES      AllocatePages;
    EFI_FREE_PAGES          FreePages;
    void                   *GetMemoryMap;
    EFI_ALLOCATE_POOL       AllocatePool;
    EFI_FREE_POOL           FreePool;
    void                   *CreateEvent;
    void                   *SetTimer;
    void                   *WaitForEvent;
    void                   *SignalEvent;
    void                   *CloseEvent;
    void                   *CheckEvent;
    void                   *InstallProtocolInterface;
    void                   *ReinstallProtocolInterface;
    void                   *UninstallProtocolInterface;
    EFI_HANDLE_PROTOCOL     HandleProtocol;
    void                   *Reserved;
    void                   *RegisterProtocolNotify;
    EFI_LOCATE_HANDLE       LocateHandle;
    void                   *LocateDevicePath;
    void                   *InstallConfigurationTable;
    EFI_IMAGE_LOAD          LoadImage;
    EFI_IMAGE_START         StartImage;
    void                   *Exit;
    void                   *UnloadImage;
    void                   *ExitBootServices;
    void                   *GetNextMonotonicCount;
    EFI_STALL               Stall;
    void                   *SetWatchdogTimer;
    void                   *ConnectController;
    void                   *DisconnectController;
    void                   *OpenProtocol;
    void                   *CloseProtocol;
    void                   *OpenProtocolInformation;
    void                   *ProtocolsPerHandle;
    void                   *LocateHandleBuffer;
    EFI_LOCATE_PROTOCOL     LocateProtocol;
    void                   *InstallMultipleProtocolInterfaces;
    void                   *UninstallMultipleProtocolInterfaces;
    void                   *CalculateCrc32;
    void                   *CopyMem;
    void                   *SetMem;
    void                   *CreateEventEx;
} EFI_BOOT_SERVICES;

typedef struct {
    EFI_TABLE_HEADER    Hdr;
    void               *GetTime;
    void               *SetTime;
    void               *GetWakeupTime;
    void               *SetWakeupTime;
    void               *SetVirtualAddressMap;
    void               *ConvertPointer;
    EFI_GET_VARIABLE                 GetVariable;
    EFI_GET_NEXT_VARIABLE_NAME       GetNextVariableName;
    EFI_SET_VARIABLE                 SetVariable;
    void               *GetNextHighMonotonicCount;
    EFI_RESET_SYSTEM    ResetSystem;
    void               *UpdateCapsule;
    void               *QueryCapsuleCaps;
    void               *QueryVariableInfo;
} EFI_RUNTIME_SERVICES;

typedef struct {
    EFI_TABLE_HEADER                Hdr;
    CHAR16                         *FirmwareVendor;
    UINT32                          FirmwareRevision;
    UINT32                          _pad;
    EFI_HANDLE                      ConsoleInHandle;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL *ConIn;
    EFI_HANDLE                      ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE                      StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    EFI_RUNTIME_SERVICES           *RuntimeServices;
    EFI_BOOT_SERVICES              *BootServices;
    UINTN                           NumberOfTableEntries;
    void                           *ConfigurationTable;
} EFI_SYSTEM_TABLE;

// ---- Well-known GUIDs -------------------------------------------------------

// {964E5B22-6459-11D2-8E39-00A0C969723B}
#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    { 0x964e5b22, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

// {CE345171-BA0B-11D2-8E4F-00A0C969723B}
#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    { 0x5b1b31a1, 0x9562, 0x11d2, { 0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

// {964E5B21-6459-11D2-8E39-00A0C969723B}
#define EFI_BLOCK_IO_PROTOCOL_GUID \
    { 0x964e5b21, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

// {0x9576e91}-{0x6d3f}-{0x11d2}-{0x8e}-{39,00,a0,c9,69,72,3b}
#define EFI_DEVICE_PATH_PROTOCOL_GUID \
    { 0x09576e91, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

// EFI_FILE_INFO GUID: {09576E92-6D3F-11D2-8E39-00A0C969723B}
#define EFI_FILE_INFO_GUID \
    { 0x09576e92, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

// ---- Reset types ------------------------------------------------------------

#define EFI_RESET_COLD  0
#define EFI_RESET_WARM  1

// ---- Utility macros ---------------------------------------------------------

// Length of a C array.
#define ARRAY_LEN(a)  (sizeof(a)/sizeof((a)[0]))

// Upper-bound page count for a byte range.
#define PAGE_ROUND_UP(n)   (((n) + 4095ULL) / 4096ULL)
#define PAGE_ROUND_DOWN(a) ((a) & ~4095ULL)

#endif /* EFI_TYPES_H */
