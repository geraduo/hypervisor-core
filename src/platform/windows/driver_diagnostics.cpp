#include "hv/platform/driver_diagnostics.hpp"
#include <algorithm>
#include <utility>

namespace hv {

DriverDiagnostics::DriverDiagnostics(std::size_t capacity) : capacity_(std::max<std::size_t>(capacity, 1)) {}

void DriverDiagnostics::Record(DriverDiagnosticLevel level, DriverDiagnosticCode code,
                               std::string message, std::uint32_t native_error,
                               std::uint64_t request_id, std::string component) {
    std::lock_guard lock(mutex_);
    events_.push_back({next_sequence_++, std::chrono::system_clock::now(), level, code,
                       std::move(component), std::move(message), native_error, request_id});
    while (events_.size() > capacity_) events_.pop_front();
}

std::vector<DriverDiagnosticEvent> DriverDiagnostics::Snapshot() const {
    std::lock_guard lock(mutex_);
    return {events_.begin(), events_.end()};
}

void DriverDiagnostics::Clear() {
    std::lock_guard lock(mutex_);
    events_.clear();
}

const char* ToString(DriverDiagnosticLevel level) noexcept {
    switch (level) {
    case DriverDiagnosticLevel::Info: return "INFO";
    case DriverDiagnosticLevel::Warning: return "WARN";
    case DriverDiagnosticLevel::Error: return "ERROR";
    }
    return "ERROR";
}

const char* ToString(DriverDiagnosticCode code) noexcept {
    switch (code) {
    case DriverDiagnosticCode::Connected: return "connected";
    case DriverDiagnosticCode::Disconnected: return "disconnected";
    case DriverDiagnosticCode::DriverNotFound: return "driver_not_found";
    case DriverDiagnosticCode::AccessDenied: return "access_denied";
    case DriverDiagnosticCode::DriverLoadFailure: return "driver_load_failure";
    case DriverDiagnosticCode::ConnectionFailed: return "connection_failed";
    case DriverDiagnosticCode::QuerySucceeded: return "query_succeeded";
    case DriverDiagnosticCode::QueryFailed: return "query_failed";
    case DriverDiagnosticCode::ProtocolMismatch: return "protocol_mismatch";
    case DriverDiagnosticCode::InvalidResponse: return "invalid_response";
    case DriverDiagnosticCode::DriverInitialization: return "driver_initialization";
    case DriverDiagnosticCode::DriverUnload: return "driver_unload";
    case DriverDiagnosticCode::UnsupportedRequest: return "unsupported_request";
    }
    return "unknown";
}

} // namespace hv