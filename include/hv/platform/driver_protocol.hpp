#pragma once

// Shared user-mode <-> kernel-mode diagnostic protocol for Geraduo Hypervisor
// Core v0.6.0.
//
// The diagnostic driver was not changed in v0.6.0, so the driver version
// constants below intentionally remain 0.5.0. Protocol version 1, the 24-byte
// request, the 80-byte response and the IOCTL code are unchanged.
//
// This header is deliberately free of Windows.h, STL containers, exceptions and
// allocations so that it can be compiled both by the user-mode library and by the
// WDK kernel driver. It defines only a fixed-size, versioned diagnostic exchange.
// There is no physical-memory, process-memory or arbitrary-pointer operation.

// <stdint.h> (not <cstdint>) keeps the same header usable by WDK kernel C++,
// where global fixed-width integer types are available without the C++ runtime.
#include <stdint.h>

namespace hv {

// Protocol version is bumped whenever the binary layout or semantics change.
inline constexpr uint32_t kGeraduoProtocolVersion = 1u;

inline constexpr uint32_t kGeraduoDriverVersionMajor = 0u;
inline constexpr uint32_t kGeraduoDriverVersionMinor = 5u;
inline constexpr uint32_t kGeraduoDriverVersionPatch = 0u;

// Oldest driver version that still speaks protocol version 1. The wire layout
// did not change between driver 0.4.0 and 0.5.0, so a 0.4.x driver remains
// compatible with this user-mode library and is reported as such.
inline constexpr uint32_t kGeraduoMinimumCompatibleDriverMajor = 0u;
inline constexpr uint32_t kGeraduoMinimumCompatibleDriverMinor = 4u;

[[nodiscard]] inline constexpr bool IsDriverVersionCompatible(uint32_t major,
                                                             uint32_t minor) noexcept {
    return major == kGeraduoMinimumCompatibleDriverMajor &&
           minor >= kGeraduoMinimumCompatibleDriverMinor;
}

// "GDRQ" / "GDRP" written as big-endian-looking 32-bit constants.
inline constexpr uint32_t kGeraduoRequestMagic = 0x47524451u;
inline constexpr uint32_t kGeraduoResponseMagic = 0x47524450u;

// Device interface class GUID shared by the driver and the user-mode transport.
// The same bytes are used literally in both projects.
// {f3a8c7e2-5d41-4a9b-8c6e-2b7d1e9f4a30}
#define GERADUO_DEVICE_INTERFACE_GUID \
    { 0xf3a8c7e2u, 0x5d41u, 0x4a9bu, { 0x8cu, 0x6eu, 0x2bu, 0x7du, 0x1eu, 0x9fu, 0x4au, 0x30u } }

// CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_DATA).
// Keeping the numeric value here lets unit tests verify it against the SDK CTL_CODE
// macro without making the shared header depend on <winioctl.h>.
inline constexpr uint32_t kGeraduoFileDeviceUnknown = 0x00000022u;
inline constexpr uint32_t kGeraduoIoctlFunctionQueryPlatformInfo = 0x0800u;
inline constexpr uint32_t kGeraduoIoctlAccessReadData = 0x0001u;
inline constexpr uint32_t kGeraduoIoctlMethodBuffered = 0x0000u;
inline constexpr uint32_t IOCTL_GERADUO_QUERY_PLATFORM_INFO =
    (kGeraduoFileDeviceUnknown << 16) |
    (kGeraduoIoctlAccessReadData << 14) |
    (kGeraduoIoctlFunctionQueryPlatformInfo << 2) |
    kGeraduoIoctlMethodBuffered;

inline constexpr uint32_t kGeraduoIoctlQueryPlatformInfo = IOCTL_GERADUO_QUERY_PLATFORM_INFO;

// Values stored in DriverPlatformInfoResponse::status. Zero is success so that a
// zero-initialized structure can never accidentally claim a successful non-query.
enum class DriverProtocolStatus : int32_t {
    Success = 0,
    InvalidRequest = 1,
    UnsupportedProtocolVersion = 2,
    BufferTooSmall = 3,
    UnsupportedRequest = 4,
    InternalError = 5,
    InformationUnavailable = 6
};

enum class DriverArchitecture : uint32_t {
    Unknown = 0,
    X86 = 1,
    X64 = 2,
    Arm64 = 3
};

// Capability flags describe which response fields were actually populated.
inline constexpr uint32_t kGeraduoCapabilityDiagnosticQuery = 1u << 0;
inline constexpr uint32_t kGeraduoCapabilityOsVersion = 1u << 1;
inline constexpr uint32_t kGeraduoCapabilityProcessorInfo = 1u << 2;
inline constexpr uint32_t kGeraduoCapabilityPhysicalMemory = 1u << 3;
inline constexpr uint32_t kGeraduoCapabilityCachedSnapshot = 1u << 4;

// Fixed layout. The request includes header fields plus a "status" field that must
// be Success in a request. The response repeats the header fields and echoes the
// request id so that stale or mismatched replies are rejected by user mode.
#pragma pack(push, 8)

struct DriverQueryPlatformInfoRequest {
    uint32_t magic;             // kGeraduoRequestMagic
    uint32_t protocol_version;  // kGeraduoProtocolVersion
    uint32_t structure_size;    // sizeof(DriverQueryPlatformInfoRequest)
    uint32_t request_id;        // Nonzero query identifier, echoed by the driver.
    int32_t status;             // DriverProtocolStatus::Success; must be zero.
    uint32_t reserved;          // Must be zero.
};

struct DriverPlatformInfoResponse {
    uint32_t magic;             // kGeraduoResponseMagic
    uint32_t protocol_version;  // kGeraduoProtocolVersion
    uint32_t structure_size;    // sizeof(DriverPlatformInfoResponse)
    uint32_t request_id;        // Echo of the request identifier.
    int32_t status;             // DriverProtocolStatus
    uint32_t capability_flags;  // kGeraduoCapability* bits

    uint32_t driver_version_major;
    uint32_t driver_version_minor;
    uint32_t driver_version_patch;
    uint32_t architecture;      // DriverArchitecture

    uint32_t os_major;
    uint32_t os_minor;
    uint32_t os_build;
    uint32_t os_revision;

    uint32_t active_processor_count;
    uint32_t page_size;
    uint64_t physical_memory_bytes;
    uint64_t reserved;          // Must be zero.
};

#pragma pack(pop)

static_assert(sizeof(DriverQueryPlatformInfoRequest) == 24,
              "The query request layout is part of protocol version 1");
static_assert(sizeof(DriverPlatformInfoResponse) == 80,
              "The platform-info response layout is part of protocol version 1");
static_assert(sizeof(DriverProtocolStatus) == 4, "Protocol status must be 32-bit");
static_assert(sizeof(DriverArchitecture) == 4, "Architecture values must be 32-bit");
static_assert((IOCTL_GERADUO_QUERY_PLATFORM_INFO & 0x3u) == kGeraduoIoctlMethodBuffered,
              "IOCTL_GERADUO_QUERY_PLATFORM_INFO must use METHOD_BUFFERED");

[[nodiscard]] inline constexpr bool IsKnownDriverArchitecture(uint32_t value) noexcept {
    return value == static_cast<uint32_t>(DriverArchitecture::Unknown) ||
           value == static_cast<uint32_t>(DriverArchitecture::X86) ||
           value == static_cast<uint32_t>(DriverArchitecture::X64) ||
           value == static_cast<uint32_t>(DriverArchitecture::Arm64);
}

} // namespace hv
