#include "geraduo_driver.hpp"

#include "hv/platform/driver_protocol.hpp"

extern "C" {
static NTSTATUS GeraduoCreate(_In_ PDEVICE_OBJECT device_object, _In_ PIRP irp);
static NTSTATUS GeraduoClose(_In_ PDEVICE_OBJECT device_object, _In_ PIRP irp);
static NTSTATUS GeraduoCleanup(_In_ PDEVICE_OBJECT device_object, _In_ PIRP irp);
static NTSTATUS GeraduoDeviceControl(_In_ PDEVICE_OBJECT device_object, _In_ PIRP irp);
VOID GeraduoDriverUnload(_In_ PDRIVER_OBJECT driver_object);
NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT driver_object, _In_ PUNICODE_STRING registry_path);
}

namespace {

const GUID kGeraduoDeviceInterfaceClass = GERADUO_DEVICE_INTERFACE_GUID;

NTSTATUS CompleteIrp(_In_ PIRP irp, _In_ NTSTATUS status, _In_ ULONG_PTR information) {
    irp->IoStatus.Status = status;
    irp->IoStatus.Information = information;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

void CaptureSystemSnapshot(_Inout_ geraduo::SystemSnapshot& snapshot) {
    RTL_OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (NT_SUCCESS(RtlGetVersion(&version))) {
        snapshot.os_major = version.dwMajorVersion;
        snapshot.os_minor = version.dwMinorVersion;
        snapshot.os_build = version.dwBuildNumber;
    }
    snapshot.os_revision = 0;
    snapshot.page_size = PAGE_SIZE;

    // MmGetPhysicalMemoryRanges returns a pool-allocated, zero-terminated array.
    // The loop is capped so that a malformed terminator cannot cause an unbounded walk.
    PHYSICAL_MEMORY_RANGE* ranges = MmGetPhysicalMemoryRanges();
    if (ranges != nullptr) {
        ULONG64 total = 0;
        bool overflow = false;
        for (ULONG index = 0; index < 1024; ++index) {
            const LONGLONG length = ranges[index].NumberOfBytes.QuadPart;
            if (length <= 0) break;
            const ULONG64 amount = static_cast<ULONG64>(length);
            if (total > MAXULONG64 - amount) {
                overflow = true;
                break;
            }
            total += amount;
        }
        ExFreePool(ranges);
        if (!overflow) {
            snapshot.physical_memory_bytes = total;
            snapshot.physical_memory_available = TRUE;
        }
    }
}

constexpr hv::DriverArchitecture DriverArchitectureValue() noexcept {
#if defined(_M_X64)
    return hv::DriverArchitecture::X64;
#elif defined(_M_ARM64)
    return hv::DriverArchitecture::Arm64;
#elif defined(_M_IX86)
    return hv::DriverArchitecture::X86;
#else
    return hv::DriverArchitecture::Unknown;
#endif
}

void FillStaticResponseFields(_In_ const geraduo::DeviceExtension& extension,
                              _In_ ULONG request_id,
                              _Out_ hv::DriverPlatformInfoResponse& output) {
    output.magic = hv::kGeraduoResponseMagic;
    output.protocol_version = hv::kGeraduoProtocolVersion;
    output.structure_size = static_cast<uint32_t>(sizeof(output));
    output.request_id = request_id;
    output.status = static_cast<INT32>(hv::DriverProtocolStatus::Success);
    output.capability_flags = hv::kGeraduoCapabilityDiagnosticQuery |
                              hv::kGeraduoCapabilityOsVersion |
                              hv::kGeraduoCapabilityProcessorInfo |
                              hv::kGeraduoCapabilityCachedSnapshot;
    if (extension.snapshot.physical_memory_available != FALSE)
        output.capability_flags |= hv::kGeraduoCapabilityPhysicalMemory;

    output.driver_version_major = hv::kGeraduoDriverVersionMajor;
    output.driver_version_minor = hv::kGeraduoDriverVersionMinor;
    output.driver_version_patch = hv::kGeraduoDriverVersionPatch;
    output.architecture = static_cast<UINT32>(DriverArchitectureValue());
    output.os_major = extension.snapshot.os_major;
    output.os_minor = extension.snapshot.os_minor;
    output.os_build = extension.snapshot.os_build;
    output.os_revision = extension.snapshot.os_revision;

    const ULONG processors = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    output.active_processor_count = processors != 0 ? processors : 1;
    output.page_size = extension.snapshot.page_size;
    output.physical_memory_bytes = extension.snapshot.physical_memory_bytes;
    output.reserved = 0;
}

} // namespace

namespace geraduo {

NTSTATUS CreateDevice(_In_ PDRIVER_OBJECT driver_object,
                      _Outptr_ PDEVICE_OBJECT* device_object) {
    if (driver_object == nullptr || device_object == nullptr) return STATUS_INVALID_PARAMETER;
    *device_object = nullptr;

    UNICODE_STRING device_name{};
    RtlInitUnicodeString(&device_name, kDeviceName);
    UNICODE_STRING sddl = RTL_CONSTANT_STRING(L"D:P(A;;GA;;;SY)(A;;GR;;;BA)");
    GUID interface_class = kGeraduoDeviceInterfaceClass;

    PDEVICE_OBJECT device = nullptr;
    NTSTATUS status = IoCreateDeviceSecure(
        driver_object, sizeof(DeviceExtension), &device_name, FILE_DEVICE_UNKNOWN,
        FILE_DEVICE_SECURE_OPEN, FALSE, &sddl, &interface_class, &device);
    if (!NT_SUCCESS(status)) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "GeraduoDriver: IoCreateDeviceSecure failed (0x%08lX).\n",
                   static_cast<ULONG>(status));
        return status;
    }

    auto* extension = static_cast<DeviceExtension*>(device->DeviceExtension);
    RtlZeroMemory(extension, sizeof(*extension));
    device->Flags |= DO_BUFFERED_IO;
    device->Flags &= ~DO_DIRECT_IO;

    UNICODE_STRING symbolic_link{};
    RtlInitUnicodeString(&symbolic_link, kSymbolicLinkName);
    status = IoCreateSymbolicLink(&symbolic_link, &device_name);
    if (!NT_SUCCESS(status)) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "GeraduoDriver: IoCreateSymbolicLink failed (0x%08lX).\n",
                   static_cast<ULONG>(status));
        IoDeleteDevice(device);
        return status;
    }
    extension->symbolic_link_created = TRUE;

    status = IoRegisterDeviceInterface(device, &interface_class, nullptr,
                                       &extension->interface_name);
    if (!NT_SUCCESS(status)) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "GeraduoDriver: IoRegisterDeviceInterface failed (0x%08lX).\n",
                   static_cast<ULONG>(status));
        DestroyDevice(device);
        return status;
    }
    extension->interface_registered = TRUE;

    status = IoSetDeviceInterfaceState(&extension->interface_name, TRUE);
    if (!NT_SUCCESS(status)) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "GeraduoDriver: IoSetDeviceInterfaceState failed (0x%08lX).\n",
                   static_cast<ULONG>(status));
        DestroyDevice(device);
        return status;
    }
    extension->interface_enabled = TRUE;

    CaptureSystemSnapshot(extension->snapshot);
    device->Flags &= ~DO_DEVICE_INITIALIZING;

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "GeraduoDriver: device created; interface %wZ.\n", &extension->interface_name);
    *device_object = device;
    return STATUS_SUCCESS;
}

void DestroyDevice(_In_ PDEVICE_OBJECT device_object) {
    if (device_object == nullptr) return;
    auto* extension = static_cast<DeviceExtension*>(device_object->DeviceExtension);
    if (extension != nullptr) {
        if (extension->interface_enabled != FALSE) {
            (void)IoSetDeviceInterfaceState(&extension->interface_name, FALSE);
            extension->interface_enabled = FALSE;
        }
        if (extension->interface_registered != FALSE) {
            RtlFreeUnicodeString(&extension->interface_name);
            extension->interface_registered = FALSE;
        }
        if (extension->symbolic_link_created != FALSE) {
            UNICODE_STRING symbolic_link{};
            RtlInitUnicodeString(&symbolic_link, kSymbolicLinkName);
            (void)IoDeleteSymbolicLink(&symbolic_link);
            extension->symbolic_link_created = FALSE;
        }
    }
    IoDeleteDevice(device_object);
}

NTSTATUS QueryPlatformInfo(_In_ PDEVICE_OBJECT device_object, _In_ PIRP irp,
                           _In_ PIO_STACK_LOCATION stack) {
    if (device_object == nullptr || irp == nullptr || stack == nullptr)
        return STATUS_INVALID_PARAMETER;

    const ULONG input_length = stack->Parameters.DeviceIoControl.InputBufferLength;
    const ULONG output_length = stack->Parameters.DeviceIoControl.OutputBufferLength;
    if (input_length < sizeof(hv::DriverQueryPlatformInfoRequest)) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "GeraduoDriver: query rejected: input buffer is too small (%lu).\n",
                   input_length);
        irp->IoStatus.Information = 0;
        return STATUS_INVALID_PARAMETER;
    }
    if (output_length < sizeof(hv::DriverPlatformInfoResponse)) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "GeraduoDriver: query rejected: output buffer is too small (%lu).\n",
                   output_length);
        irp->IoStatus.Information = 0;
        return STATUS_BUFFER_TOO_SMALL;
    }

    auto* system_buffer = irp->AssociatedIrp.SystemBuffer;
    if (system_buffer == nullptr) {
        irp->IoStatus.Information = 0;
        return STATUS_INVALID_PARAMETER;
    }

    // METHOD_BUFFERED shares one system buffer for input and output. Copy the
    // request header before the response overwrites the buffer.
    hv::DriverQueryPlatformInfoRequest request{};
    RtlCopyMemory(&request, system_buffer, sizeof(request));

    auto* output = static_cast<hv::DriverPlatformInfoResponse*>(system_buffer);
    RtlZeroMemory(output, sizeof(*output));

    const auto* extension = static_cast<const geraduo::DeviceExtension*>(
        device_object->DeviceExtension);
    FillStaticResponseFields(*extension, request.request_id, *output);

    bool accepted = true;
    if (request.magic != hv::kGeraduoRequestMagic) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
                   "GeraduoDriver: query rejected: bad request magic 0x%08lX.\n",
                   static_cast<ULONG>(request.magic));
        output->status = static_cast<INT32>(hv::DriverProtocolStatus::InvalidRequest);
        accepted = false;
    } else if (request.protocol_version != hv::kGeraduoProtocolVersion) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
                   "GeraduoDriver: query rejected: protocol version %lu, expected %lu.\n",
                   static_cast<unsigned long>(request.protocol_version),
                   static_cast<unsigned long>(hv::kGeraduoProtocolVersion));
        output->status =
            static_cast<INT32>(hv::DriverProtocolStatus::UnsupportedProtocolVersion);
        accepted = false;
    } else if (request.structure_size !=
                   static_cast<uint32_t>(sizeof(hv::DriverQueryPlatformInfoRequest)) ||
               request.status != static_cast<INT32>(hv::DriverProtocolStatus::Success) ||
               request.reserved != 0 || request.request_id == 0) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
                   "GeraduoDriver: query rejected: malformed request header "
                   "(size=%lu, status=%ld, reserved=%lu, id=%lu).\n",
                   static_cast<unsigned long>(request.structure_size),
                   static_cast<LONG>(request.status),
                   static_cast<unsigned long>(request.reserved),
                   static_cast<unsigned long>(request.request_id));
        output->status = static_cast<INT32>(hv::DriverProtocolStatus::InvalidRequest);
        accepted = false;
    }

    if (!accepted) {
        irp->IoStatus.Information = sizeof(*output);
        return STATUS_SUCCESS;
    }

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "GeraduoDriver: platform query id=%lu completed.\n",
               static_cast<unsigned long>(request.request_id));
    irp->IoStatus.Information = sizeof(*output);
    return STATUS_SUCCESS;
}

} // namespace geraduo

extern "C" {

static NTSTATUS GeraduoCreate(_In_ PDEVICE_OBJECT device_object, _In_ PIRP irp) {
    if (device_object != nullptr && device_object->DeviceExtension != nullptr) {
        auto* extension = static_cast<geraduo::DeviceExtension*>(device_object->DeviceExtension);
        const LONG handles = InterlockedIncrement(&extension->open_handles);
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
                   "GeraduoDriver: handle opened (%ld).\n", handles);
    }
    return CompleteIrp(irp, STATUS_SUCCESS, 0);
}

static NTSTATUS GeraduoClose(_In_ PDEVICE_OBJECT device_object, _In_ PIRP irp) {
    UNREFERENCED_PARAMETER(device_object);
    return CompleteIrp(irp, STATUS_SUCCESS, 0);
}

static NTSTATUS GeraduoCleanup(_In_ PDEVICE_OBJECT device_object, _In_ PIRP irp) {
    if (device_object != nullptr && device_object->DeviceExtension != nullptr) {
        auto* extension = static_cast<geraduo::DeviceExtension*>(device_object->DeviceExtension);
        const LONG handles = InterlockedDecrement(&extension->open_handles);
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
                   "GeraduoDriver: handle cleaned up (%ld).\n", handles);
    }
    return CompleteIrp(irp, STATUS_SUCCESS, 0);
}

static NTSTATUS GeraduoDeviceControl(_In_ PDEVICE_OBJECT device_object, _In_ PIRP irp) {
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    if (stack->Parameters.DeviceIoControl.IoControlCode ==
        hv::IOCTL_GERADUO_QUERY_PLATFORM_INFO) {
        status = geraduo::QueryPlatformInfo(device_object, irp, stack);
    } else {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
                   "GeraduoDriver: unsupported IOCTL 0x%08lX.\n",
                   static_cast<ULONG>(stack->Parameters.DeviceIoControl.IoControlCode));
        irp->IoStatus.Information = 0;
    }
    return CompleteIrp(irp, status, irp->IoStatus.Information);
}

VOID GeraduoDriverUnload(_In_ PDRIVER_OBJECT driver_object) {
    if (driver_object == nullptr) return;
    PDEVICE_OBJECT device = driver_object->DeviceObject;
    if (device != nullptr) {
        auto* extension = static_cast<geraduo::DeviceExtension*>(device->DeviceExtension);
        if (extension != nullptr && extension->open_handles > 0) {
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
                       "GeraduoDriver: unloading with %ld open handle(s).\n",
                       extension->open_handles);
        }
        geraduo::DestroyDevice(device);
    }
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "GeraduoDriver: unloaded.\n");
}

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT driver_object, _In_ PUNICODE_STRING registry_path) {
    UNREFERENCED_PARAMETER(registry_path);
    if (driver_object == nullptr) return STATUS_INVALID_PARAMETER;

    driver_object->MajorFunction[IRP_MJ_CREATE] = GeraduoCreate;
    driver_object->MajorFunction[IRP_MJ_CLOSE] = GeraduoClose;
    driver_object->MajorFunction[IRP_MJ_CLEANUP] = GeraduoCleanup;
    driver_object->MajorFunction[IRP_MJ_DEVICE_CONTROL] = GeraduoDeviceControl;
    driver_object->DriverUnload = GeraduoDriverUnload;

    PDEVICE_OBJECT device = nullptr;
    const NTSTATUS status = geraduo::CreateDevice(driver_object, &device);
    if (!NT_SUCCESS(status)) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "GeraduoDriver: DriverEntry failed (0x%08lX).\n",
                   static_cast<ULONG>(status));
        return status;
    }

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "GeraduoDriver: DriverEntry completed; protocol version %lu.\n",
               static_cast<unsigned long>(hv::kGeraduoProtocolVersion));
    return STATUS_SUCCESS;
}

} // extern "C"
