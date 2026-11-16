#pragma once

#include "hv/platform/driver_diagnostics.hpp"
#include "hv/status.hpp"
#include "hv/virtualization/vmx_readiness_manager.hpp"
#include "hv/virtualization/vmx_status.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace hv {

enum class VmxEventLevel { Info, Warning, Error };

enum class VmxEventCode {
    CheckStarted,
    CheckCompleted,
    ProcessorNotChecked,
    DriverConnected,
    FailureReason,
    StateTransition,
    TransitionRejected,
    StartRejected,
    BackendStopped,
    InternalError
};

struct VmxDiagnosticEvent {
    std::uint64_t sequence{};
    std::int64_t timestamp_ms{};  // milliseconds since the Unix epoch (system clock)
    VmxEventLevel level{VmxEventLevel::Info};
    VmxEventCode code{VmxEventCode::InternalError};
    std::string component;
    std::string message;
    Status error_code{Status::Success};
    bool has_cpu_index{false};
    std::uint32_t cpu_index{};
};

// Purpose: bounded, thread-safe structured event log for the VMX layer. Complements
// DriverDiagnostics (which logs driver traffic) and does not replace it.
class VmxDiagnostics final {
public:
    explicit VmxDiagnostics(std::size_t capacity = 256);

    void Record(VmxEventLevel level, VmxEventCode code, std::string component, std::string message,
                Status error_code = Status::Success);
    void RecordProcessor(VmxEventLevel level, VmxEventCode code, std::string component,
                         std::string message, Status error_code, std::uint32_t cpu_index);
    [[nodiscard]] std::vector<VmxDiagnosticEvent> Snapshot() const;
    void Clear();

private:
    void Push(VmxDiagnosticEvent event);

    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<VmxDiagnosticEvent> events_;
    std::uint64_t next_sequence_{1};
};

struct VmxDiagnosticReport {
    std::string product_version{"0.5.0"};
    VmxLifecycleState state{VmxLifecycleState::Uninitialized};
    bool hypervisor_running{false};              // always false in v0.5.0
    bool hardware_virtualization_started{false}; // always false in v0.5.0
    bool readiness_available{false};
    VmxReadinessReport readiness;
    std::vector<VmxLifecycleTransition> transitions;
    std::vector<VmxDiagnosticEvent> events;
    std::vector<DriverDiagnosticEvent> driver_events;
    std::string last_error;
};

[[nodiscard]] const char* ToString(VmxEventLevel value) noexcept;
[[nodiscard]] const char* ToString(VmxEventCode value) noexcept;

// JSON string escaping per RFC 8259 (quotes, backslash, control characters).
[[nodiscard]] std::string JsonEscape(const std::string& text);
[[nodiscard]] std::string FormatVmxReportText(const VmxDiagnosticReport& report);
[[nodiscard]] std::string FormatVmxReportJson(const VmxDiagnosticReport& report);

} // namespace hv
