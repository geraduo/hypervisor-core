#pragma once
#include "hv/platform/driver_diagnostics.hpp"
#include "hv/platform/driver_protocol.hpp"
#include "hv/status.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace hv {

// Low-level transport used for testability. The production implementation uses
// SetupAPI + CreateFileW + DeviceIoControl; tests substitute a scripted transport.
class IDriverTransport {
public:
    virtual ~IDriverTransport() = default;

    [[nodiscard]] virtual Result Open(std::string& error_detail) = 0;
    [[nodiscard]] virtual Result Transact(const void* input, std::size_t input_size,
                                          void* output, std::size_t output_size,
                                          std::size_t& returned_bytes,
                                          std::string& error_detail) = 0;
    virtual void Close() noexcept = 0;
    [[nodiscard]] virtual bool IsOpen() const noexcept = 0;
};

// Opens the device interface registered by GeraduoDriver and sends only the
// fixed-size protocol version 1 request. On non-Windows builds Open() reports
// Status::Unsupported. It never opens a raw device path supplied by a caller.
class NativeDriverTransport final : public IDriverTransport {
public:
    NativeDriverTransport();
    ~NativeDriverTransport() override;
    NativeDriverTransport(const NativeDriverTransport&) = delete;
    NativeDriverTransport& operator=(const NativeDriverTransport&) = delete;

    [[nodiscard]] Result Open(std::string& error_detail) override;
    [[nodiscard]] Result Transact(const void* input, std::size_t input_size,
                                  void* output, std::size_t output_size,
                                  std::size_t& returned_bytes,
                                  std::string& error_detail) override;
    void Close() noexcept override;
    [[nodiscard]] bool IsOpen() const noexcept override;

private:
    void* handle_{nullptr};
    std::string last_error_;
};

// Interface used by higher layers and by the diagnostics example. Fakes implement
// this without touching the Windows driver.
class IKernelDriverInterface {
public:
    virtual ~IKernelDriverInterface() = default;

    [[nodiscard]] virtual Result Connect() = 0;
    virtual void Disconnect() = 0;
    [[nodiscard]] virtual bool IsConnected() const = 0;
    [[nodiscard]] virtual Result QueryPlatformInfo(DriverPlatformInfoResponse& out) = 0;
    [[nodiscard]] virtual std::string GetLastError() const = 0;
};

class KernelDriverInterface final : public IKernelDriverInterface {
public:
    KernelDriverInterface();
    explicit KernelDriverInterface(std::unique_ptr<IDriverTransport> transport);
    ~KernelDriverInterface() override;
    KernelDriverInterface(const KernelDriverInterface&) = delete;
    KernelDriverInterface& operator=(const KernelDriverInterface&) = delete;

    [[nodiscard]] Result Connect() override;
    void Disconnect() override;
    [[nodiscard]] bool IsConnected() const override;
    [[nodiscard]] Result QueryPlatformInfo(DriverPlatformInfoResponse& out) override;
    [[nodiscard]] std::string GetLastError() const override;

    [[nodiscard]] DriverDiagnostics& Diagnostics() noexcept;
    [[nodiscard]] const DriverDiagnostics& Diagnostics() const noexcept;

    // Rejects stale, malformed or mismatched replies. Exposed for unit tests.
    [[nodiscard]] static bool ValidateResponse(const DriverPlatformInfoResponse& response,
                                               std::uint32_t expected_request_id,
                                               std::string& error_detail,
                                               Status& error_status);

private:
    std::unique_ptr<IDriverTransport> transport_;
    mutable std::mutex mutex_;
    bool connected_{false};
    std::uint32_t next_request_id_{1};
    std::string last_error_;
    DriverDiagnostics diagnostics_;
};

[[nodiscard]] const char* ToString(DriverProtocolStatus value) noexcept;
[[nodiscard]] const char* ToString(DriverArchitecture value) noexcept;

[[nodiscard]] std::string FormatDriverPlatformInfoText(const DriverPlatformInfoResponse& response);
[[nodiscard]] std::string FormatDriverPlatformInfoJson(const DriverPlatformInfoResponse& response);

} // namespace hv
