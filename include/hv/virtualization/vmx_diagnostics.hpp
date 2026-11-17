#pragma once

// Event journal and report serialization for the virtualization foundation.
//
// The journal is an ordinary object owned by the lifecycle controller. It is
// deliberately not a singleton and never stores hardware state. Events are
// bounded and thread-safe; timestamps are wall-clock UTC in JSON output.

#include "hv/diagnostics.hpp"
#include "hv/virtualization/virtualization_backend.hpp"
#include "hv/virtualization/vmx_capability_manager.hpp"
#include "hv/virtualization/vmx_readiness_manager.hpp"

#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

namespace hv {

enum class VmxDiagnosticCode {
    None,
    LifecycleInitialized,
    LifecycleRefreshed,
    LifecycleStopped,
    LifecycleRejected,
    CapabilityEvaluated,
    ReadinessEvaluated,
    DriverUnavailable,
    ProtocolMismatch,
    NotChecked,
    EvaluationFailed,
    ReportGenerated
};

struct VmxDiagnosticEvent {
    EventLevel level{EventLevel::Info};
    VmxDiagnosticCode code{VmxDiagnosticCode::None};
    std::string message;
    std::chrono::system_clock::time_point timestamp{};
};

class VmxDiagnostics final {
public:
    explicit VmxDiagnostics(std::size_t capacity = 256);

    void Record(EventLevel level, VmxDiagnosticCode code, std::string message);
    [[nodiscard]] std::vector<VmxDiagnosticEvent> Snapshot() const;
    void Clear();
    [[nodiscard]] std::size_t Size() const;
    [[nodiscard]] std::size_t Capacity() const;

private:
    void TrimLocked();

    mutable std::mutex mutex_;
    std::vector<VmxDiagnosticEvent> events_;
    std::size_t capacity_{256};
};

// Complete immutable-by-value view of one diagnostic run. The lifecycle
// controller fills it from its last committed reports; a failed refresh keeps
// the previous reports (rollback) and reports the failure separately.
struct VmxDiagnosticsReport {
    VirtualizationBackendStatus backend;
    VmxCapabilityReport capability;
    VmxReadinessReport readiness;
    std::vector<VmxDiagnosticEvent> events;
};

[[nodiscard]] const char* ToString(VmxDiagnosticCode value) noexcept;

[[nodiscard]] std::string FormatVmxDiagnosticsText(const VmxDiagnosticsReport& report);
[[nodiscard]] std::string FormatVmxDiagnosticsJson(const VmxDiagnosticsReport& report);

} // namespace hv
