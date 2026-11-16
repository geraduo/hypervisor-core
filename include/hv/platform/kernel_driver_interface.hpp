#pragma once

#include "hv/platform/driver_protocol.h"
#include "hv/status.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace hv {

class DriverDiagnostics;
enum class DriverDiagnosticCode;

struct DriverPlatformInfo {
    std::uint32_t os_major_version{};
    std::uint32_t os_minor_version{};
    std::uint32_t os_build_number{};
    std::uint32_t architecture{GERADUO_ARCHITECTURE_UNKNOWN};
    std::uint32_t processor_count{};
    std::uint32_t page_size{};
    bool hypervisor_present{false};
    bool vmx_reported{false};
};

class IKernelDriverInterface {
public:
    virtual ~IKernelDriverInterface() = default;
    [[nodiscard]] virtual Result Connect() = 0;
    virtual void Disconnect() noexcept = 0;
    [[nodiscard]] virtual bool IsConnected() const noexcept = 0;
    [[nodiscard]] virtual Result QueryPlatformInfo(DriverPlatformInfo& out) = 0;
    [[nodiscard]] virtual std::string GetLastError() const = 0;
};

class IDriverDevice {
public:
    virtual ~IDriverDevice() = default;
    [[nodiscard]] virtual Result Open() = 0;
    virtual void Close() noexcept = 0;
    [[nodiscard]] virtual bool IsOpen() const noexcept = 0;
    [[nodiscard]] virtual Result Control(std::uint32_t control_code, const void* input,
                                         std::size_t input_size, void* output,
                                         std::size_t output_size,
                                         std::size_t& bytes_returned) = 0;
};

class KernelDriverInterface final : public IKernelDriverInterface {
public:
    explicit KernelDriverInterface(DriverDiagnostics* diagnostics = nullptr);
    explicit KernelDriverInterface(std::unique_ptr<IDriverDevice> device,
                                   DriverDiagnostics* diagnostics = nullptr);
    ~KernelDriverInterface() override;

    KernelDriverInterface(const KernelDriverInterface&) = delete;
    KernelDriverInterface& operator=(const KernelDriverInterface&) = delete;

    [[nodiscard]] Result Connect() override;
    void Disconnect() noexcept override;
    [[nodiscard]] bool IsConnected() const noexcept override;
    [[nodiscard]] Result QueryPlatformInfo(DriverPlatformInfo& out) override;
    [[nodiscard]] std::string GetLastError() const override;

private:
    void RecordFailure(std::string message, DriverDiagnosticCode code,
                       std::uint64_t request_id = 0);

    mutable std::mutex mutex_;
    std::unique_ptr<IDriverDevice> device_;
    DriverDiagnostics* diagnostics_{};
    bool connected_{false};
    std::uint64_t next_request_id_{1};
    std::string last_error_;
};

[[nodiscard]] std::unique_ptr<IDriverDevice> CreateNativeDriverDevice();

} // namespace hv