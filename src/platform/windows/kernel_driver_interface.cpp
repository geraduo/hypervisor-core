#include "hv/platform/kernel_driver_interface.hpp"

#include <cstdint>
#include <cstdio>
#include <limits>
#include <sstream>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <setupapi.h>
#include <winioctl.h>
#endif

namespace hv {
namespace {

std::string CombineDetail(std::string primary, const std::string& detail) {
    if (detail.empty()) return primary;
    if (!primary.empty()) primary += " ";
    primary += "[" + detail + "]";
    return primary;
}

DriverDiagnosticCode OpenDiagnosticCode(Status status) {
    switch (status) {
    case Status::NotFound: return DriverDiagnosticCode::DeviceInterfaceMissing;
    case Status::AccessDenied: return DriverDiagnosticCode::DeviceAccessDenied;
    default: return DriverDiagnosticCode::DeviceOpenFailed;
    }
}

Status ProtocolStatusToStatus(DriverProtocolStatus protocol_status) {
    switch (protocol_status) {
    case DriverProtocolStatus::Success: return Status::Success;
    case DriverProtocolStatus::InvalidRequest:
    case DriverProtocolStatus::BufferTooSmall:
        return Status::InvalidArgument;
    case DriverProtocolStatus::UnsupportedProtocolVersion:
    case DriverProtocolStatus::UnsupportedRequest:
        return Status::Unsupported;
    case DriverProtocolStatus::InternalError:
    case DriverProtocolStatus::InformationUnavailable:
        return Status::InternalError;
    }
    return Status::InternalError;
}

std::string JsonEscape(const std::string& text) {
    std::string result;
    for (const char ch : text) {
        const auto value = static_cast<unsigned char>(ch);
        switch (ch) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\n': result += "\\n"; break;
        case '\f': result += "\\f"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (value < 0x20) {
                char buffer[8]{};
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", value);
                result += buffer;
            } else {
                result.push_back(ch);
            }
            break;
        }
    }
    return result;
}

#if defined(_WIN32)
const GUID kGeraduoInterfaceGuid = GERADUO_DEVICE_INTERFACE_GUID;

std::string Win32ErrorText(DWORD error) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::string text = "Win32 error " + std::to_string(error);
    if (length != 0 && buffer != nullptr) {
        const int utf8_length = WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(length),
                                                    nullptr, 0, nullptr, nullptr);
        if (utf8_length > 0) {
            std::string utf8(static_cast<std::size_t>(utf8_length), '\0');
            (void)WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(length), utf8.data(),
                                      utf8_length, nullptr, nullptr);
            while (!utf8.empty() && (utf8.back() == '\r' || utf8.back() == '\n' || utf8.back() == ' '))
                utf8.pop_back();
            if (!utf8.empty()) text += ": " + utf8;
        }
        LocalFree(buffer);
    }
    return text;
}

Status MapOpenError(DWORD error) {
    switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_DEVICE_NOT_CONNECTED:
    case ERROR_NO_SUCH_DEVICE:
    case ERROR_NO_MORE_ITEMS:
        return Status::NotFound;
    case ERROR_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:
        return Status::AccessDenied;
    default:
        return Status::InitializationFailed;
    }
}

Status MapIoctlError(DWORD error) {
    switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_DEVICE_NOT_CONNECTED:
    case ERROR_NO_SUCH_DEVICE:
        return Status::NotFound;
    case ERROR_ACCESS_DENIED:
        return Status::AccessDenied;
    case ERROR_INVALID_FUNCTION:
    case ERROR_NOT_SUPPORTED:
    case ERROR_REVISION_MISMATCH:
        return Status::Unsupported;
    case ERROR_INVALID_PARAMETER:
    case ERROR_INSUFFICIENT_BUFFER:
        return Status::InvalidArgument;
    default:
        return Status::InternalError;
    }
}
#endif

} // namespace

NativeDriverTransport::NativeDriverTransport() = default;

NativeDriverTransport::~NativeDriverTransport() {
    Close();
}

Result NativeDriverTransport::Open(std::string& error_detail) {
    error_detail.clear();
#if defined(_WIN32)
    if (handle_ != nullptr) return Result::Ok();

    HDEVINFO device_set = SetupDiGetClassDevsW(&kGeraduoInterfaceGuid, nullptr, nullptr,
                                               DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (device_set == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        error_detail = Win32ErrorText(error);
        const Status status = error == ERROR_NO_SUCH_DEVICE || error == ERROR_FILE_NOT_FOUND
                                  ? Status::NotFound
                                  : Status::InitializationFailed;
        last_error_ = "Unable to enumerate the Geraduo device interface: " + error_detail;
        return Result::Error(status, last_error_);
    }

    SP_DEVICE_INTERFACE_DATA interface_data{};
    interface_data.cbSize = sizeof(interface_data);
    if (!SetupDiEnumDeviceInterfaces(device_set, nullptr, &kGeraduoInterfaceGuid, 0,
                                     &interface_data)) {
        const DWORD error = GetLastError();
        (void)SetupDiDestroyDeviceInfoList(device_set);
        if (error == ERROR_NO_MORE_ITEMS) {
            error_detail.clear();
            last_error_ =
                "The Geraduo driver device interface is not present. The driver is not installed, "
                "not loaded, or the device failed to start.";
            return Result::Error(Status::NotFound, last_error_);
        }
        error_detail = Win32ErrorText(error);
        last_error_ = "Unable to enumerate the Geraduo device interface: " + error_detail;
        return Result::Error(Status::InitializationFailed, last_error_);
    }

    DWORD required_size = 0;
    (void)SetupDiGetDeviceInterfaceDetailW(device_set, &interface_data, nullptr, 0,
                                           &required_size, nullptr);
    if (required_size == 0) {
        const DWORD error = GetLastError();
        (void)SetupDiDestroyDeviceInfoList(device_set);
        error_detail = Win32ErrorText(error);
        last_error_ = "Unable to query the Geraduo device interface path: " + error_detail;
        return Result::Error(Status::InitializationFailed, last_error_);
    }

    std::vector<unsigned char> detail_buffer(required_size);
    auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detail_buffer.data());
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    if (!SetupDiGetDeviceInterfaceDetailW(device_set, &interface_data, detail, required_size,
                                          nullptr, nullptr)) {
        const DWORD error = GetLastError();
        (void)SetupDiDestroyDeviceInfoList(device_set);
        error_detail = Win32ErrorText(error);
        last_error_ = "Unable to read the Geraduo device interface path: " + error_detail;
        return Result::Error(MapOpenError(error), last_error_);
    }
    const std::wstring device_path(detail->DevicePath);
    (void)SetupDiDestroyDeviceInfoList(device_set);

    HANDLE handle = CreateFileW(device_path.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        error_detail = Win32ErrorText(error);
        last_error_ = "Unable to open the Geraduo diagnostic device: " + error_detail;
        return Result::Error(MapOpenError(error), last_error_);
    }

    handle_ = handle;
    last_error_.clear();
    return Result::Ok();
#else
    (void)error_detail;
    last_error_ = "The Geraduo kernel driver interface is only available on Windows.";
    return Result::Error(Status::Unsupported, last_error_);
#endif
}

Result NativeDriverTransport::Transact(const void* input, std::size_t input_size, void* output,
                                       std::size_t output_size, std::size_t& returned_bytes,
                                       std::string& error_detail) {
    error_detail.clear();
    returned_bytes = 0;
#if defined(_WIN32)
    if (handle_ == nullptr) {
        last_error_ = "The Geraduo diagnostic device is not open.";
        return Result::Error(Status::InvalidState, last_error_);
    }
    if (input == nullptr || output == nullptr || input_size == 0 || output_size == 0) {
        last_error_ = "The driver exchange buffers are invalid.";
        return Result::Error(Status::InvalidArgument, last_error_);
    }
    if (input_size > std::numeric_limits<DWORD>::max() ||
        output_size > std::numeric_limits<DWORD>::max()) {
        last_error_ = "The driver exchange buffers are too large.";
        return Result::Error(Status::InvalidArgument, last_error_);
    }

    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(
        static_cast<HANDLE>(handle_), IOCTL_GERADUO_QUERY_PLATFORM_INFO,
        const_cast<void*>(input), static_cast<DWORD>(input_size), output,
        static_cast<DWORD>(output_size), &returned, nullptr);
    if (!ok) {
        const DWORD error = GetLastError();
        error_detail = Win32ErrorText(error);
        last_error_ = "DeviceIoControl(IOCTL_GERADUO_QUERY_PLATFORM_INFO) failed: " + error_detail;
        return Result::Error(MapIoctlError(error), last_error_);
    }
    returned_bytes = static_cast<std::size_t>(returned);
    last_error_.clear();
    return Result::Ok();
#else
    (void)input;
    (void)input_size;
    (void)output;
    (void)output_size;
    (void)returned_bytes;
    (void)error_detail;
    last_error_ = "The Geraduo kernel driver interface is only available on Windows.";
    return Result::Error(Status::Unsupported, last_error_);
#endif
}

void NativeDriverTransport::Close() noexcept {
#if defined(_WIN32)
    if (handle_ != nullptr) {
        (void)CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
#endif
}

bool NativeDriverTransport::IsOpen() const noexcept {
    return handle_ != nullptr;
}

KernelDriverInterface::KernelDriverInterface()
    : transport_(std::make_unique<NativeDriverTransport>()) {}

KernelDriverInterface::KernelDriverInterface(std::unique_ptr<IDriverTransport> transport)
    : transport_(std::move(transport)) {}

KernelDriverInterface::~KernelDriverInterface() {
    Disconnect();
}

Result KernelDriverInterface::Connect() {
    std::lock_guard lock(mutex_);
    if (transport_ == nullptr) {
        last_error_ = "No driver transport is configured.";
        diagnostics_.Record(EventLevel::Error, DriverDiagnosticCode::InitializationFailed,
                            last_error_);
        return Result::Error(Status::InvalidState, last_error_);
    }
    if (connected_ && transport_->IsOpen()) return Result::Ok();

    std::string detail;
    const Result opened = transport_->Open(detail);
    if (!opened.Succeeded()) {
        connected_ = false;
        last_error_ = CombineDetail(opened.message, detail);
        diagnostics_.Record(EventLevel::Error, OpenDiagnosticCode(opened.status), last_error_);
        return Result::Error(opened.status, last_error_);
    }

    connected_ = true;
    last_error_.clear();
    diagnostics_.Record(EventLevel::Info, DriverDiagnosticCode::Connected,
                        "Connected to the Geraduo diagnostic device interface.");
    return Result::Ok();
}

void KernelDriverInterface::Disconnect() {
    std::lock_guard lock(mutex_);
    const bool was_connected = connected_ || (transport_ != nullptr && transport_->IsOpen());
    if (transport_ != nullptr) transport_->Close();
    connected_ = false;
    if (was_connected) {
        diagnostics_.Record(EventLevel::Info, DriverDiagnosticCode::Disconnected,
                            "Disconnected from the Geraduo diagnostic device interface.");
    }
}

bool KernelDriverInterface::IsConnected() const {
    std::lock_guard lock(mutex_);
    return connected_ && transport_ != nullptr && transport_->IsOpen();
}

Result KernelDriverInterface::QueryPlatformInfo(DriverPlatformInfoResponse& out) {
    std::lock_guard lock(mutex_);
    out = DriverPlatformInfoResponse{};

    if (transport_ == nullptr || !connected_ || !transport_->IsOpen()) {
        connected_ = false;
        last_error_ = "Connect() must succeed before QueryPlatformInfo() is called.";
        diagnostics_.Record(EventLevel::Error, DriverDiagnosticCode::NotConnected, last_error_);
        return Result::Error(Status::InvalidState, last_error_);
    }

    DriverQueryPlatformInfoRequest request{};
    request.magic = kGeraduoRequestMagic;
    request.protocol_version = kGeraduoProtocolVersion;
    request.structure_size = sizeof(DriverQueryPlatformInfoRequest);
    request.request_id = next_request_id_++;
    if (next_request_id_ == 0) next_request_id_ = 1;
    request.status = static_cast<std::int32_t>(DriverProtocolStatus::Success);
    request.reserved = 0;

    DriverPlatformInfoResponse response{};
    std::size_t returned_bytes = 0;
    std::string detail;
    const Result exchange = transport_->Transact(&request, sizeof(request), &response,
                                                 sizeof(response), returned_bytes, detail);
    if (!exchange.Succeeded()) {
        last_error_ = CombineDetail(exchange.message, detail);
        diagnostics_.Record(EventLevel::Error, DriverDiagnosticCode::IoctlFailed, last_error_);
        return Result::Error(exchange.status, last_error_);
    }

    if (returned_bytes != sizeof(response)) {
        last_error_ = "The driver returned " + std::to_string(returned_bytes) +
                      " bytes; protocol version 1 requires exactly " +
                      std::to_string(sizeof(response)) + ".";
        diagnostics_.Record(EventLevel::Error, DriverDiagnosticCode::ResponseMalformed,
                            last_error_);
        return Result::Error(Status::InternalError, last_error_);
    }

    std::string validation_error;
    Status validation_status = Status::InternalError;
    if (!ValidateResponse(response, request.request_id, validation_error, validation_status)) {
        last_error_ = validation_error;
        const auto protocol_status = static_cast<DriverProtocolStatus>(response.status);
        DriverDiagnosticCode code = DriverDiagnosticCode::ResponseMalformed;
        if (response.protocol_version != kGeraduoProtocolVersion ||
            protocol_status == DriverProtocolStatus::UnsupportedProtocolVersion) {
            code = DriverDiagnosticCode::ProtocolVersionMismatch;
        } else if (protocol_status != DriverProtocolStatus::Success) {
            code = DriverDiagnosticCode::QueryFailed;
        }
        diagnostics_.Record(EventLevel::Error, code, last_error_);
        return Result::Error(validation_status, last_error_);
    }

    out = response;
    last_error_.clear();
    diagnostics_.Record(EventLevel::Info, DriverDiagnosticCode::QuerySucceeded,
                        "IOCTL_GERADUO_QUERY_PLATFORM_INFO completed successfully.");
    return Result::Ok();
}

std::string KernelDriverInterface::GetLastError() const {
    std::lock_guard lock(mutex_);
    return last_error_;
}

DriverDiagnostics& KernelDriverInterface::Diagnostics() noexcept {
    return diagnostics_;
}

const DriverDiagnostics& KernelDriverInterface::Diagnostics() const noexcept {
    return diagnostics_;
}

bool KernelDriverInterface::ValidateResponse(const DriverPlatformInfoResponse& response,
                                              std::uint32_t expected_request_id,
                                              std::string& error_detail,
                                              Status& error_status) {
    error_detail.clear();
    error_status = Status::InternalError;

    if (response.magic != kGeraduoResponseMagic) {
        error_detail = "Driver response magic does not match the Geraduo protocol.";
        return false;
    }
    if (response.protocol_version != kGeraduoProtocolVersion) {
        error_detail = "Driver protocol version " + std::to_string(response.protocol_version) +
                       " does not match the expected version " +
                       std::to_string(kGeraduoProtocolVersion) + ".";
        error_status = Status::Unsupported;
        return false;
    }
    if (response.structure_size != sizeof(DriverPlatformInfoResponse)) {
        error_detail = "Driver response structure size " + std::to_string(response.structure_size) +
                       " does not match " + std::to_string(sizeof(DriverPlatformInfoResponse)) + ".";
        return false;
    }
    if (response.request_id != expected_request_id) {
        error_detail = "Driver response request id " + std::to_string(response.request_id) +
                       " does not match the query id " + std::to_string(expected_request_id) + ".";
        return false;
    }

    const auto protocol_status = static_cast<DriverProtocolStatus>(response.status);
    if (protocol_status != DriverProtocolStatus::Success) {
        error_detail = std::string("Driver reported protocol status ") + ToString(protocol_status) +
                       ".";
        error_status = ProtocolStatusToStatus(protocol_status);
        return false;
    }
    if (response.architecture == static_cast<std::uint32_t>(DriverArchitecture::Unknown) ||
        !IsKnownDriverArchitecture(response.architecture)) {
        error_detail = "Driver reported an unknown processor architecture.";
        return false;
    }
    if (response.active_processor_count == 0) {
        error_detail = "Driver reported zero active processors.";
        return false;
    }
    if (response.page_size == 0 || (response.page_size & (response.page_size - 1u)) != 0) {
        error_detail = "Driver reported an invalid page size.";
        return false;
    }
    if (response.reserved != 0) {
        error_detail = "Driver response reserved field is not zero.";
        return false;
    }

    error_status = Status::Success;
    return true;
}

const char* ToString(DriverProtocolStatus value) noexcept {
    switch (value) {
    case DriverProtocolStatus::Success: return "Success";
    case DriverProtocolStatus::InvalidRequest: return "InvalidRequest";
    case DriverProtocolStatus::UnsupportedProtocolVersion: return "UnsupportedProtocolVersion";
    case DriverProtocolStatus::BufferTooSmall: return "BufferTooSmall";
    case DriverProtocolStatus::UnsupportedRequest: return "UnsupportedRequest";
    case DriverProtocolStatus::InternalError: return "InternalError";
    case DriverProtocolStatus::InformationUnavailable: return "InformationUnavailable";
    }
    return "InternalError";
}

const char* ToString(DriverArchitecture value) noexcept {
    switch (value) {
    case DriverArchitecture::Unknown: return "Unknown";
    case DriverArchitecture::X86: return "x86";
    case DriverArchitecture::X64: return "x64";
    case DriverArchitecture::Arm64: return "ARM64";
    }
    return "Unknown";
}

std::string FormatDriverPlatformInfoText(const DriverPlatformInfoResponse& response) {
    std::ostringstream out;
    out << "Geraduo kernel driver platform information\n";
    out << "Protocol: " << response.protocol_version
        << " (response " << response.structure_size << " bytes), status "
        << ToString(static_cast<DriverProtocolStatus>(response.status)) << '\n';
    out << "Driver version: " << response.driver_version_major << '.'
        << response.driver_version_minor << '.' << response.driver_version_patch << '\n';
    out << "Windows: " << response.os_major << '.' << response.os_minor << '.'
        << response.os_build << '.' << response.os_revision << '\n';
    out << "Architecture: " << ToString(static_cast<DriverArchitecture>(response.architecture))
        << '\n';
    out << "Active processors: " << response.active_processor_count << '\n';
    out << "Page size: " << response.page_size << '\n';
    out << "Physical memory: " << response.physical_memory_bytes << " bytes\n";
    out << "Capability flags: 0x" << std::hex << response.capability_flags << std::dec << '\n';
    return out.str();
}

std::string FormatDriverPlatformInfoJson(const DriverPlatformInfoResponse& response) {
    std::ostringstream out;
    out << "{\n";
    out << "  \"protocol_version\": " << response.protocol_version << ",\n";
    out << "  \"structure_size\": " << response.structure_size << ",\n";
    out << "  \"request_id\": " << response.request_id << ",\n";
    out << "  \"status\": \"" << ToString(static_cast<DriverProtocolStatus>(response.status))
        << "\",\n";
    out << "  \"driver_version\": \"" << response.driver_version_major << '.'
        << response.driver_version_minor << '.' << response.driver_version_patch << "\",\n";
    out << "  \"os_version\": \"" << response.os_major << '.' << response.os_minor << '.'
        << response.os_build << '.' << response.os_revision << "\",\n";
    out << "  \"architecture\": \""
        << ToString(static_cast<DriverArchitecture>(response.architecture)) << "\",\n";
    out << "  \"active_processor_count\": " << response.active_processor_count << ",\n";
    out << "  \"page_size\": " << response.page_size << ",\n";
    out << "  \"physical_memory_bytes\": " << response.physical_memory_bytes << ",\n";
    out << "  \"capability_flags\": " << response.capability_flags << ",\n";
    out << "  \"note\": \"" << JsonEscape("read-only diagnostic response") << "\"\n";
    out << "}\n";
    return out.str();
}

} // namespace hv
