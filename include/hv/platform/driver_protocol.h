#ifndef GERADUO_HV_DRIVER_PROTOCOL_H
#define GERADUO_HV_DRIVER_PROTOCOL_H

#include <stdint.h>

#define GERADUO_DRIVER_PROTOCOL_VERSION 1u
#define GERADUO_FILE_DEVICE_TYPE 0x8000u
#define GERADUO_IOCTL_FUNCTION_QUERY_PLATFORM_INFO 0x800u

/* CTL_CODE(FILE_DEVICE_GERADUO, 0x800, METHOD_BUFFERED, FILE_READ_DATA). */
#define GERADUO_IOCTL_QUERY_PLATFORM_INFO \
    ((uint32_t)((GERADUO_FILE_DEVICE_TYPE << 16) | (1u << 14) | \
                (GERADUO_IOCTL_FUNCTION_QUERY_PLATFORM_INFO << 2)))

#define GERADUO_DEVICE_INTERFACE_GUID \
    {0x83e51b3c, 0x7a2f, 0x4d11, {0x89, 0x73, 0x3a, 0x2c, 0xdf, 0x14, 0x6b, 0x91}}

#define GERADUO_DEVICE_CLASS_GUID \
    {0xd6bb4f81, 0x12a3, 0x4b85, {0x91, 0xc7, 0x6a, 0x6d, 0x1f, 0x32, 0x54, 0x70}}

typedef enum GERADUO_QUERY_STATUS {
    GERADUO_QUERY_STATUS_SUCCESS = 0,
    GERADUO_QUERY_STATUS_INVALID_ARGUMENT = 1,
    GERADUO_QUERY_STATUS_VERSION_MISMATCH = 2,
    GERADUO_QUERY_STATUS_UNSUPPORTED = 3,
    GERADUO_QUERY_STATUS_INTERNAL_ERROR = 4
} GERADUO_QUERY_STATUS;

typedef enum GERADUO_PLATFORM_ARCHITECTURE {
    GERADUO_ARCHITECTURE_UNKNOWN = 0,
    GERADUO_ARCHITECTURE_X86 = 1,
    GERADUO_ARCHITECTURE_X64 = 2,
    GERADUO_ARCHITECTURE_ARM64 = 3
} GERADUO_PLATFORM_ARCHITECTURE;

#define GERADUO_PLATFORM_FLAG_HYPERVISOR_PRESENT 0x00000001u
#define GERADUO_PLATFORM_FLAG_VMX_REPORTED 0x00000002u

#pragma pack(push, 8)
typedef struct GERADUO_QUERY_REQUEST {
    uint32_t protocol_version;
    uint32_t structure_size;
    uint64_t request_id;
} GERADUO_QUERY_REQUEST;

typedef struct GERADUO_PLATFORM_INFO {
    uint32_t os_major_version;
    uint32_t os_minor_version;
    uint32_t os_build_number;
    uint32_t architecture;
    uint32_t processor_count;
    uint32_t page_size;
    uint32_t flags;
} GERADUO_PLATFORM_INFO;

typedef struct GERADUO_QUERY_RESPONSE {
    uint32_t protocol_version;
    uint32_t structure_size;
    uint64_t request_id;
    int32_t status;
    GERADUO_PLATFORM_INFO platform;
} GERADUO_QUERY_RESPONSE;
#pragma pack(pop)

#ifdef __cplusplus
static_assert(sizeof(GERADUO_QUERY_REQUEST) == 16, "Unexpected query request layout");
static_assert(sizeof(GERADUO_PLATFORM_INFO) == 28, "Unexpected platform info layout");
static_assert(sizeof(GERADUO_QUERY_RESPONSE) == 48, "Unexpected query response layout");
static_assert(GERADUO_IOCTL_QUERY_PLATFORM_INFO == 0x80006000u,
              "Unexpected diagnostic IOCTL value");
#endif

#endif