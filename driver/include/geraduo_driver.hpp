#pragma once

#include "hv/platform/driver_protocol.hpp"

#include <ntddk.h>
#include <wdmsec.h>

namespace geraduo {

inline constexpr wchar_t kDeviceName[] = L"\\Device\\GeraduoDriver";
inline constexpr wchar_t kSymbolicLinkName[] = L"\\DosDevices\\GeraduoDriver";

struct SystemSnapshot {
    ULONG os_major{};
    ULONG os_minor{};
    ULONG os_build{};
    ULONG os_revision{};
    ULONG page_size{};
    ULONG64 physical_memory_bytes{};
    BOOLEAN physical_memory_available{FALSE};
};

struct DeviceExtension {
    UNICODE_STRING interface_name{};
    BOOLEAN interface_registered{FALSE};
    BOOLEAN interface_enabled{FALSE};
    BOOLEAN symbolic_link_created{FALSE};
    LONG open_handles{0};
    SystemSnapshot snapshot{};
};

[[nodiscard]] NTSTATUS CreateDevice(_In_ PDRIVER_OBJECT driver_object,
                                    _Outptr_ PDEVICE_OBJECT* device_object);
void DestroyDevice(_In_ PDEVICE_OBJECT device_object);
[[nodiscard]] NTSTATUS QueryPlatformInfo(_In_ PDEVICE_OBJECT device_object, _In_ PIRP irp,
                                         _In_ PIO_STACK_LOCATION stack);

} // namespace geraduo
