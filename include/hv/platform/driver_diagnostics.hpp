#pragma once
#include "hv/diagnostics.hpp"
#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

namespace hv {

enum class DriverDiagnosticCode {
    None,
    DeviceInterfaceMissing,
    DeviceOpenFailed,
    DeviceAccessDenied,
    IoctlFailed,
    ProtocolVersionMismatch,
    ResponseMalformed,
    NotConnected,
    Connected,
    Disconnected,
    QuerySucceeded,
    QueryFailed,
    InitializationFailed
};

struct DriverDiagnosticEvent {
    EventLevel level{EventLevel::Info};
    DriverDiagnosticCode code{DriverDiagnosticCode::None};
    std::string message;
    std::chrono::system_clock::time_point timestamp{};
};

// Bounded, thread-safe journal for user-mode driver interaction. Keeping the
// journal in user mode means a missing or misbehaving driver can still be
// diagnosed; the driver itself only emits DbgPrint messages.
class DriverDiagnostics final {
public:
    explicit DriverDiagnostics(std::size_t capacity = 256);

    void Record(EventLevel level, DriverDiagnosticCode code, std::string message);
    [[nodiscard]] std::vector<DriverDiagnosticEvent> Snapshot() const;
    void Clear();
    [[nodiscard]] std::size_t Size() const;
    [[nodiscard]] std::size_t Capacity() const;

private:
    void TrimLocked();

    mutable std::mutex mutex_;
    std::vector<DriverDiagnosticEvent> events_;
    std::size_t capacity_{256};
};

[[nodiscard]] const char* ToString(EventLevel value) noexcept;
[[nodiscard]] const char* ToString(DriverDiagnosticCode value) noexcept;

[[nodiscard]] std::string FormatDriverDiagnosticsText(
    const std::vector<DriverDiagnosticEvent>& events);
[[nodiscard]] std::string FormatDriverDiagnosticsJson(
    const std::vector<DriverDiagnosticEvent>& events);

} // namespace hv
