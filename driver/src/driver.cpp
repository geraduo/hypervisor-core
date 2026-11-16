#include <ntddk.h>
#include <intrin.h>
#include "hv/platform/driver_protocol.h"
#include "hv/platform/driver_protocol_validation.hpp"

namespace {

const GUID kGeraduoInterfaceGuid = GERADUO_DEVICE_INTERFACE_GUID;

struct DEVICE_EXTENSION {
    PDEVICE_OBJECT LowerDeviceObject;
    PDEVICE_OBJECT PhysicalDeviceObject;
    UNICODE_STRING InterfaceName;
    BOOLEAN InterfaceRegistered;
    BOOLEAN InterfaceEnabled;
    BOOLEAN Started;
};

NTSTATUS CompleteIrp(PIRP irp, NTSTATUS status, ULONG_PTR information = 0) {
    irp->IoStatus.Status = status;
    irp->IoStatus.Information = information;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

NTSTATUS CompletionSignal(PDEVICE_OBJECT, PIRP irp, PVOID context) {
    KeSetEvent(static_cast<PKEVENT>(context), IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS ForwardSynchronously(DEVICE_EXTENSION* extension, PIRP irp) {
    if (extension->LowerDeviceObject == nullptr) return STATUS_NO_SUCH_DEVICE;

    KEVENT completion_event;
    KeInitializeEvent(&completion_event, NotificationEvent, FALSE);
    IoCopyCurrentIrpStackLocationToNext(irp);
    IoSetCompletionRoutine(irp, CompletionSignal, &completion_event, TRUE, TRUE, TRUE);
    const NTSTATUS call_status = IoCallDriver(extension->LowerDeviceObject, irp);
    if (call_status == STATUS_PENDING || KeReadStateEvent(&completion_event) == 0) {
        KeWaitForSingleObject(&completion_event, Executive, KernelMode, FALSE, nullptr);
    }
    return irp->IoStatus.Status;
}

NTSTATUS DispatchCreateClose(PDEVICE_OBJECT device_object, PIRP irp) {
    const auto* extension = static_cast<const DEVICE_EXTENSION*>(device_object->DeviceExtension);
    const auto major_function = IoGetCurrentIrpStackLocation(irp)->MajorFunction;
    if (major_function == IRP_MJ_CREATE && extension->Started == FALSE)
        return CompleteIrp(irp, STATUS_DEVICE_NOT_READY);
    return CompleteIrp(irp, STATUS_SUCCESS);
}

NTSTATUS DispatchDeviceControl(PDEVICE_OBJECT device_object, PIRP irp) {
    auto* extension = static_cast<DEVICE_EXTENSION*>(device_object->DeviceExtension);
    const auto* stack = IoGetCurrentIrpStackLocation(irp);
    const ULONG control_code = stack->Parameters.DeviceIoControl.IoControlCode;
    if (control_code != GERADUO_IOCTL_QUERY_PLATFORM_INFO) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
                   "[Geraduo] Unsupported IOCTL 0x%08X\n", control_code);
        return CompleteIrp(irp, STATUS_INVALID_DEVICE_REQUEST);
    }
    if (extension->Started == FALSE) return CompleteIrp(irp, STATUS_DEVICE_NOT_READY);

    const ULONG input_length = stack->Parameters.DeviceIoControl.InputBufferLength;
    const ULONG output_length = stack->Parameters.DeviceIoControl.OutputBufferLength;
    if (output_length < sizeof(GERADUO_QUERY_RESPONSE)) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
                   "[Geraduo] Response buffer too small: %lu\n", output_length);
        return CompleteIrp(irp, STATUS_BUFFER_TOO_SMALL);
    }
    if (input_length < sizeof(GERADUO_QUERY_REQUEST) || irp->AssociatedIrp.SystemBuffer == nullptr) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
                   "[Geraduo] Request buffer too small or unavailable: %lu\n", input_length);
        return CompleteIrp(irp, STATUS_BUFFER_TOO_SMALL);
    }

    GERADUO_QUERY_REQUEST request{};
    RtlCopyMemory(&request, irp->AssociatedIrp.SystemBuffer, sizeof(request));
    auto* response = static_cast<GERADUO_QUERY_RESPONSE*>(irp->AssociatedIrp.SystemBuffer);
    RtlZeroMemory(response, sizeof(*response));
    response->protocol_version = GERADUO_DRIVER_PROTOCOL_VERSION;
    response->structure_size = static_cast<uint32_t>(sizeof(*response));
    response->request_id = request.request_id;

    const auto validation = hv::platform::protocol::ValidateQueryRequest(request, input_length);
    if (validation == hv::platform::protocol::QueryRequestValidation::VersionMismatch) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
                   "[Geraduo] Protocol version mismatch: %lu\n", request.protocol_version);
        response->status = GERADUO_QUERY_STATUS_VERSION_MISMATCH;
        return CompleteIrp(irp, STATUS_SUCCESS, sizeof(*response));
    }
    if (validation != hv::platform::protocol::QueryRequestValidation::Valid) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
                   "[Geraduo] Invalid query structure (size=%lu, request=%I64u)\n",
                   request.structure_size, request.request_id);
        response->status = GERADUO_QUERY_STATUS_INVALID_ARGUMENT;
        return CompleteIrp(irp, STATUS_SUCCESS, sizeof(*response));
    }

    RTL_OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = static_cast<ULONG>(sizeof(version));
    const NTSTATUS version_status = RtlGetVersion(&version);
    if (!NT_SUCCESS(version_status)) {
        response->status = GERADUO_QUERY_STATUS_INTERNAL_ERROR;
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "[Geraduo] RtlGetVersion failed: 0x%08X\n", version_status);
        return CompleteIrp(irp, STATUS_SUCCESS, sizeof(*response));
    }

    response->platform.os_major_version = version.dwMajorVersion;
    response->platform.os_minor_version = version.dwMinorVersion;
    response->platform.os_build_number = version.dwBuildNumber;
    response->platform.architecture = GERADUO_ARCHITECTURE_X64;
    response->platform.processor_count = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    response->platform.page_size = PAGE_SIZE;

#if defined(_M_AMD64)
    int vendor_registers[4]{};
    __cpuid(vendor_registers, 0);
    if (vendor_registers[0] >= 1) {
        int feature_registers[4]{};
        __cpuid(feature_registers, 1);
        const ULONG features = static_cast<ULONG>(feature_registers[2]);
        if ((features & (1u << 31)) != 0)
            response->platform.flags |= GERADUO_PLATFORM_FLAG_HYPERVISOR_PRESENT;

        CHAR vendor[13]{};
        RtlCopyMemory(vendor, &vendor_registers[1], sizeof(ULONG));
        RtlCopyMemory(vendor + 4, &vendor_registers[3], sizeof(ULONG));
        RtlCopyMemory(vendor + 8, &vendor_registers[2], sizeof(ULONG));
        if (RtlCompareMemory(vendor, "GenuineIntel", 12) == 12 && (features & (1u << 5)) != 0)
            response->platform.flags |= GERADUO_PLATFORM_FLAG_VMX_REPORTED;
    }
#endif

    response->status = GERADUO_QUERY_STATUS_SUCCESS;
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "[Geraduo] Platform diagnostic query completed (request %I64u)\n",
               request.request_id);
    return CompleteIrp(irp, STATUS_SUCCESS, sizeof(*response));
}

NTSTATUS DispatchPnp(PDEVICE_OBJECT device_object, PIRP irp) {
    auto* extension = static_cast<DEVICE_EXTENSION*>(device_object->DeviceExtension);
    const auto* stack = IoGetCurrentIrpStackLocation(irp);

    switch (stack->MinorFunction) {
    case IRP_MN_START_DEVICE: {
        NTSTATUS status = ForwardSynchronously(extension, irp);
        if (NT_SUCCESS(status) && extension->InterfaceRegistered != FALSE) {
            status = IoSetDeviceInterfaceState(&extension->InterfaceName, TRUE);
            if (NT_SUCCESS(status)) {
                extension->InterfaceEnabled = TRUE;
                extension->Started = TRUE;
                DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
                           "[Geraduo] Diagnostic device started\n");
            }
        }
        if (!NT_SUCCESS(status)) {
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                       "[Geraduo] Device start failed: 0x%08X\n", status);
        }
        return CompleteIrp(irp, status);
    }
    case IRP_MN_STOP_DEVICE:
    case IRP_MN_SURPRISE_REMOVAL:
        extension->Started = FALSE;
        if (extension->InterfaceEnabled != FALSE) {
            const NTSTATUS disable_status = IoSetDeviceInterfaceState(&extension->InterfaceName, FALSE);
            if (!NT_SUCCESS(disable_status)) {
                DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
                           "[Geraduo] Interface disable returned 0x%08X\n", disable_status);
            }
            extension->InterfaceEnabled = FALSE;
        }
        IoSkipCurrentIrpStackLocation(irp);
        return IoCallDriver(extension->LowerDeviceObject, irp);

    case IRP_MN_REMOVE_DEVICE: {
        extension->Started = FALSE;
        if (extension->InterfaceEnabled != FALSE) {
            (void)IoSetDeviceInterfaceState(&extension->InterfaceName, FALSE);
            extension->InterfaceEnabled = FALSE;
        }
        const NTSTATUS status = ForwardSynchronously(extension, irp);
        if (extension->LowerDeviceObject != nullptr) {
            IoDetachDevice(extension->LowerDeviceObject);
            extension->LowerDeviceObject = nullptr;
        }
        if (extension->InterfaceName.Buffer != nullptr) RtlFreeUnicodeString(&extension->InterfaceName);
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
                   "[Geraduo] Diagnostic device removed\n");
        IoDeleteDevice(device_object);
        return CompleteIrp(irp, status);
    }
    default:
        IoSkipCurrentIrpStackLocation(irp);
        return IoCallDriver(extension->LowerDeviceObject, irp);
    }
}

NTSTATUS DispatchPower(PDEVICE_OBJECT device_object, PIRP irp) {
    auto* extension = static_cast<DEVICE_EXTENSION*>(device_object->DeviceExtension);
    if (extension->LowerDeviceObject == nullptr) return CompleteIrp(irp, STATUS_NO_SUCH_DEVICE);
    PoStartNextPowerIrp(irp);
    IoSkipCurrentIrpStackLocation(irp);
    return PoCallDriver(extension->LowerDeviceObject, irp);
}

NTSTATUS DispatchPassThrough(PDEVICE_OBJECT device_object, PIRP irp) {
    auto* extension = static_cast<DEVICE_EXTENSION*>(device_object->DeviceExtension);
    if (extension->LowerDeviceObject == nullptr) return CompleteIrp(irp, STATUS_NO_SUCH_DEVICE);
    IoSkipCurrentIrpStackLocation(irp);
    return IoCallDriver(extension->LowerDeviceObject, irp);
}

NTSTATUS AddDevice(PDRIVER_OBJECT driver_object, PDEVICE_OBJECT physical_device_object) {
    PDEVICE_OBJECT device_object = nullptr;
    NTSTATUS status = IoCreateDevice(driver_object, static_cast<ULONG>(sizeof(DEVICE_EXTENSION)), nullptr,
                                     GERADUO_FILE_DEVICE_TYPE, FILE_DEVICE_SECURE_OPEN,
                                     FALSE, &device_object);
    if (!NT_SUCCESS(status)) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "[Geraduo] IoCreateDevice failed: 0x%08X\n", status);
        return status;
    }

    auto* extension = static_cast<DEVICE_EXTENSION*>(device_object->DeviceExtension);
    RtlZeroMemory(extension, sizeof(*extension));
    extension->PhysicalDeviceObject = physical_device_object;
    extension->LowerDeviceObject = IoAttachDeviceToDeviceStack(device_object, physical_device_object);
    if (extension->LowerDeviceObject == nullptr) {
        IoDeleteDevice(device_object);
        return STATUS_NO_SUCH_DEVICE;
    }

    status = IoRegisterDeviceInterface(physical_device_object, &kGeraduoInterfaceGuid,
                                       nullptr, &extension->InterfaceName);
    if (!NT_SUCCESS(status)) {
        IoDetachDevice(extension->LowerDeviceObject);
        IoDeleteDevice(device_object);
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "[Geraduo] IoRegisterDeviceInterface failed: 0x%08X\n", status);
        return status;
    }
    extension->InterfaceRegistered = TRUE;
    device_object->Flags &= ~DO_DEVICE_INITIALIZING;
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "[Geraduo] Diagnostic device object created\n");
    return STATUS_SUCCESS;
}

VOID DriverUnload(PDRIVER_OBJECT driver_object) {
    UNREFERENCED_PARAMETER(driver_object);
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "[Geraduo] Driver unloaded\n");
}

} // namespace

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driver_object, PUNICODE_STRING registry_path) {
    UNREFERENCED_PARAMETER(registry_path);
    for (ULONG index = 0; index <= IRP_MJ_MAXIMUM_FUNCTION; ++index)
        driver_object->MajorFunction[index] = DispatchPassThrough;

    driver_object->MajorFunction[IRP_MJ_CREATE] = DispatchCreateClose;
    driver_object->MajorFunction[IRP_MJ_CLEANUP] = DispatchCreateClose;
    driver_object->MajorFunction[IRP_MJ_CLOSE] = DispatchCreateClose;
    driver_object->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DispatchDeviceControl;
    driver_object->MajorFunction[IRP_MJ_PNP] = DispatchPnp;
    driver_object->MajorFunction[IRP_MJ_POWER] = DispatchPower;
    driver_object->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = DispatchPassThrough;
    driver_object->DriverExtension->AddDevice = AddDevice;
    driver_object->DriverUnload = DriverUnload;

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "[Geraduo] Diagnostic driver entry completed\n");
    return STATUS_SUCCESS;
}