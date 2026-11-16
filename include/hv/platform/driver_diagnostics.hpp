#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace hv {

enum class DriverDiagnosticLevel { Info, Warning, Error };
enum class DriverDiagnosticCode {
    Connected,
    Disconnected,
    DriverNotFound,
    AccessDenied,
    DriverLoadFailure,
    ConnectionFailed,
    QuerySucceeded,
    QueryFailed,
    ProtocolMismatch,
    InvalidResponse,
    DriverInitialization,
    DriverUnload,
    UnsupportedRequest
};

struct DriverDiagnosticEvent {
    std::uint64_t sequence{};
    std::chrono::system_clock::time_point timestamp{};
    DriverDiagnosticLevel level{DriverDiagnosticLevel::Info};
    DriverDiagnosticCode code{DriverDiagnosticCode::ConnectionFailed};
    std::string component{"KernelDriverInterface"};
    std::string message;
    std::uint32_t native_error{};
    std::uint64_t request_id{};
};

class DriverDiagnostics final {
public:
    explicit DriverDiagnostics(std::size_t capacity = 128);

    void Record(DriverDiagnosticLevel level, DriverDiagnosticCode code, std::string message,
                std::uint32_t native_error = 0, std::uint64_t request_id = 0,
                std::string component = "KernelDriverInterface");
    [[nodiscard]] std::vector<DriverDiagnosticEvent> Snapshot() const;
    void Clear();

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<DriverDiagnosticEvent> events_;
    std::uint64_t next_sequence_{1};
};

[[nodiscard]] const char* ToString(DriverDiagnosticLevel level) noexcept;
[[nodiscard]] const char* ToString(DriverDiagnosticCode code) noexcept;

} // namespace hv