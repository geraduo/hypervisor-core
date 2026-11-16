#include "hv/platform/kernel_driver_interface.hpp"
#include "hv/platform/driver_diagnostics.hpp"
#include <limits>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <SetupAPI.h>
#include <cstddef>
#include <vector>

namespace {

const GUID kGeraduoInterfaceGuid = GERADUO_DEVICE_INTERFACE_GUID;

hv::Result FromWin32Error(DWORD error, const char* operation) {
    const std::string message = std::string(operation) + " failed with Win32 error " +
                                std::to_string(error);
    if (error == ERROR_ACCESS_DENIED) return hv::Result::Error(hv::Status::AccessDenied, message);
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ||
        error == ERROR_NO_MORE_ITEMS || error == ERROR_DEV_NOT_EXIST) {
        return hv::Result::Error(hv::Status::DriverNotFound, message);
    }
    return hv::Result::Error(hv::Status::TransportError, message);
}

class Win32DriverDevice final : public hv::IDriverDevice {
public:
    ~Win32DriverDevice() override { Close(); }

    hv::Result Open() override {
        if (handle_ != INVALID_HANDLE_VALUE) return hv::Result::Ok();

        const HDEVINFO device_list = SetupDiGetClassDevsW(&kGeraduoInterfaceGuid, nullptr, nullptr,
                                                          DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (device_list == INVALID_HANDLE_VALUE)
            return FromWin32Error(GetLastError(), "SetupDiGetClassDevsW");

        DWORD last_error = ERROR_FILE_NOT_FOUND;
        for (DWORD index = 0;; ++index) {
            SP_DEVICE_INTERFACE_DATA interface_data{};
            interface_data.cbSize = sizeof(interface_data);
            if (!SetupDiEnumDeviceInterfaces(device_list, nullptr, &kGeraduoInterfaceGuid,
                                             index, &interface_data)) {
                last_error = GetLastError();
                if (last_error == ERROR_NO_MORE_ITEMS) last_error = ERROR_FILE_NOT_FOUND;
                break;
            }

            DWORD required_size = 0;
            (void)SetupDiGetDeviceInterfaceDetailW(device_list, &interface_data, nullptr, 0,
                                                    &required_size, nullptr);
            if (required_size < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {
                last_error = GetLastError();
                continue;
            }

            std::vector<std::byte> detail_storage(required_size);
            auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detail_storage.data());
            detail->cbSize = static_cast<DWORD>(sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W));
            if (!SetupDiGetDeviceInterfaceDetailW(device_list, &interface_data, detail,
                                                  required_size, nullptr, nullptr)) {
                last_error = GetLastError();
                continue;
            }

            HANDLE candidate = CreateFileW(detail->DevicePath, GENERIC_READ,
                                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (candidate != INVALID_HANDLE_VALUE) {
                handle_ = candidate;
                last_error = ERROR_SUCCESS;
                break;
            }
            last_error = GetLastError();
        }

        SetupDiDestroyDeviceInfoList(device_list);
        if (handle_ == INVALID_HANDLE_VALUE) return FromWin32Error(last_error, "Open Geraduo device interface");
        return hv::Result::Ok();
    }

    void Close() noexcept override {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

    bool IsOpen() const noexcept override { return handle_ != INVALID_HANDLE_VALUE; }

    hv::Result Control(std::uint32_t control_code, const void* input, std::size_t input_size,
                       void* output, std::size_t output_size,
                       std::size_t& bytes_returned) override {
        bytes_returned = 0;
        if (!IsOpen()) return hv::Result::Error(hv::Status::DeviceNotConnected, "Device handle is closed");
        if (input_size > std::numeric_limits<DWORD>::max() ||
            output_size > std::numeric_limits<DWORD>::max()) {
            return hv::Result::Error(hv::Status::InvalidArgument, "IOCTL buffer size exceeds DWORD limits");
        }

        DWORD transferred = 0;
        const BOOL completed = DeviceIoControl(handle_, control_code,
                                               const_cast<void*>(input), static_cast<DWORD>(input_size),
                                               output, static_cast<DWORD>(output_size),
                                               &transferred, nullptr);
        if (!completed) return FromWin32Error(GetLastError(), "DeviceIoControl");
        bytes_returned = transferred;
        return hv::Result::Ok();
    }

private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

} // namespace
#else
namespace {

class UnsupportedDriverDevice final : public hv::IDriverDevice {
public:
    hv::Result Open() override {
        return hv::Result::Error(hv::Status::Unsupported,
                                 "Kernel driver transport is available only on Windows");
    }
    void Close() noexcept override {}
    bool IsOpen() const noexcept override { return false; }
    hv::Result Control(std::uint32_t, const void*, std::size_t, void*, std::size_t,
                       std::size_t& bytes_returned) override {
        bytes_returned = 0;
        return hv::Result::Error(hv::Status::Unsupported,
                                 "Kernel driver transport is available only on Windows");
    }
};

} // namespace
#endif

namespace hv {

std::unique_ptr<IDriverDevice> CreateNativeDriverDevice() {
#ifdef _WIN32
    return std::make_unique<Win32DriverDevice>();
#else
    return std::make_unique<UnsupportedDriverDevice>();
#endif
}

KernelDriverInterface::KernelDriverInterface(DriverDiagnostics* diagnostics)
    : KernelDriverInterface(CreateNativeDriverDevice(), diagnostics) {}

KernelDriverInterface::KernelDriverInterface(std::unique_ptr<IDriverDevice> device,
                                             DriverDiagnostics* diagnostics)
    : device_(std::move(device)), diagnostics_(diagnostics) {
    if (!device_) device_ = CreateNativeDriverDevice();
}

KernelDriverInterface::~KernelDriverInterface() { Disconnect(); }

void KernelDriverInterface::RecordFailure(std::string message, DriverDiagnosticCode code,
                                          std::uint64_t request_id) {
    last_error_ = std::move(message);
    if (diagnostics_ != nullptr) {
        const auto level = code == DriverDiagnosticCode::DriverNotFound ||
                                   code == DriverDiagnosticCode::AccessDenied
                               ? DriverDiagnosticLevel::Warning
                               : DriverDiagnosticLevel::Error;
        diagnostics_->Record(level, code, last_error_, 0, request_id);
    }
}

Result KernelDriverInterface::Connect() {
    std::lock_guard lock(mutex_);
    if (connected_ && device_->IsOpen()) {
        last_error_.clear();
        return Result::Ok();
    }

    connected_ = false;
    const auto result = device_->Open();
    if (!result.Succeeded()) {
        const auto code = result.status == Status::DriverNotFound ? DriverDiagnosticCode::DriverNotFound
                          : result.status == Status::AccessDenied ? DriverDiagnosticCode::AccessDenied
                                                                  : DriverDiagnosticCode::ConnectionFailed;
        RecordFailure(result.message, code);
        return result;
    }
    if (!device_->IsOpen()) {
        const std::string message = "Driver transport reported success without an open device handle";
        RecordFailure(message, DriverDiagnosticCode::ConnectionFailed);
        return Result::Error(Status::TransportError, message);
    }

    connected_ = true;
    last_error_.clear();
    if (diagnostics_ != nullptr)
        diagnostics_->Record(DriverDiagnosticLevel::Info, DriverDiagnosticCode::Connected,
                             "Connected to the Geraduo diagnostic device");
    return Result::Ok();
}

void KernelDriverInterface::Disconnect() noexcept {
    std::lock_guard lock(mutex_);
    const bool was_connected = connected_ || (device_ && device_->IsOpen());
    if (device_ && was_connected) device_->Close();
    connected_ = false;
    if (was_connected && diagnostics_ != nullptr) {
        diagnostics_->Record(DriverDiagnosticLevel::Info, DriverDiagnosticCode::Disconnected,
                             "Closed the Geraduo diagnostic device");
    }
    last_error_.clear();
}

bool KernelDriverInterface::IsConnected() const noexcept {
    std::lock_guard lock(mutex_);
    return connected_ && device_ != nullptr && device_->IsOpen();
}

Result KernelDriverInterface::QueryPlatformInfo(DriverPlatformInfo& out) {
    std::lock_guard lock(mutex_);
    if (!connected_ || !device_ || !device_->IsOpen()) {
        const std::string message = "The Geraduo diagnostic device is not connected";
        RecordFailure(message, DriverDiagnosticCode::QueryFailed);
        return Result::Error(Status::DeviceNotConnected, message);
    }

    std::uint64_t request_id = next_request_id_++;
    if (request_id == 0) request_id = next_request_id_++;
    const GERADUO_QUERY_REQUEST request{GERADUO_DRIVER_PROTOCOL_VERSION,
                                        static_cast<std::uint32_t>(sizeof(GERADUO_QUERY_REQUEST)),
                                        request_id};
    GERADUO_QUERY_RESPONSE response{};
    std::size_t bytes_returned = 0;
    const auto transport_result = device_->Control(GERADUO_IOCTL_QUERY_PLATFORM_INFO,
                                                   &request, sizeof(request), &response,
                                                   sizeof(response), bytes_returned);
    if (!transport_result.Succeeded()) {
        RecordFailure(transport_result.message, DriverDiagnosticCode::QueryFailed, request_id);
        return transport_result;
    }
    if (bytes_returned < sizeof(response)) {
        const std::string message = "Driver response is smaller than the protocol structure";
        RecordFailure(message, DriverDiagnosticCode::InvalidResponse, request_id);
        return Result::Error(Status::InvalidResponse, message);
    }
    if (response.protocol_version != GERADUO_DRIVER_PROTOCOL_VERSION) {
        const std::string message = "Driver protocol version does not match the user-mode client";
        RecordFailure(message, DriverDiagnosticCode::ProtocolMismatch, request_id);
        return Result::Error(Status::ProtocolMismatch, message);
    }
    if (response.structure_size != sizeof(GERADUO_QUERY_RESPONSE) || response.request_id != request_id) {
        const std::string message = "Driver response size or request identifier is invalid";
        RecordFailure(message, DriverDiagnosticCode::InvalidResponse, request_id);
        return Result::Error(Status::InvalidResponse, message);
    }

    if (response.status != GERADUO_QUERY_STATUS_SUCCESS) {
        Status status = Status::InternalError;
        DriverDiagnosticCode diagnostic_code = DriverDiagnosticCode::QueryFailed;
        if (response.status == GERADUO_QUERY_STATUS_VERSION_MISMATCH) {
            status = Status::ProtocolMismatch;
            diagnostic_code = DriverDiagnosticCode::ProtocolMismatch;
        } else if (response.status == GERADUO_QUERY_STATUS_INVALID_ARGUMENT) {
            status = Status::InvalidArgument;
        } else if (response.status == GERADUO_QUERY_STATUS_UNSUPPORTED) {
            status = Status::Unsupported;
        }
        const std::string message = "Driver rejected the platform query with protocol status " +
                                    std::to_string(response.status);
        RecordFailure(message, diagnostic_code, request_id);
        return Result::Error(status, message);
    }

    DriverPlatformInfo result;
    result.os_major_version = response.platform.os_major_version;
    result.os_minor_version = response.platform.os_minor_version;
    result.os_build_number = response.platform.os_build_number;
    result.architecture = response.platform.architecture;
    result.processor_count = response.platform.processor_count;
    result.page_size = response.platform.page_size;
    result.hypervisor_present = (response.platform.flags & GERADUO_PLATFORM_FLAG_HYPERVISOR_PRESENT) != 0;
    result.vmx_reported = (response.platform.flags & GERADUO_PLATFORM_FLAG_VMX_REPORTED) != 0;
    out = result;
    last_error_.clear();
    if (diagnostics_ != nullptr)
        diagnostics_->Record(DriverDiagnosticLevel::Info, DriverDiagnosticCode::QuerySucceeded,
                             "Platform information response validated", 0, request_id);
    return Result::Ok();
}

std::string KernelDriverInterface::GetLastError() const {
    std::lock_guard lock(mutex_);
    return last_error_;
}

} // namespace hv