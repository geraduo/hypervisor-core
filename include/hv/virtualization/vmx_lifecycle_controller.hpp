#pragma once

// Transactional lifecycle controller for the Intel VT-x foundation.
//
// The controller is the concrete IVirtualizationBackend since v0.5.0. It owns no
// global state, serializes all transitions with a mutex and rolls back a failed
// Refresh() to the previous committed state. It never executes VMXON.

#include "hv/virtualization/virtualization_backend.hpp"
#include "hv/virtualization/vmx_capability_manager.hpp"
#include "hv/virtualization/vmx_diagnostics.hpp"
#include "hv/virtualization/vmx_readiness_manager.hpp"

#include <cstdint>
#include <mutex>
#include <string>

namespace hv {

class VmxLifecycleController final : public IVirtualizationBackend {
public:
    VmxLifecycleController(VmxCapabilityManager& capability, VmxReadinessManager& readiness)
        : capability_(capability), readiness_(readiness) {}
    ~VmxLifecycleController() override = default;
    VmxLifecycleController(const VmxLifecycleController&) = delete;
    VmxLifecycleController& operator=(const VmxLifecycleController&) = delete;

    // Uninitialized -> CheckingCapabilities -> NotSupported | Blocked |
    // ReadyForReview | Failed. A second Initialize() is rejected.
    [[nodiscard]] Result Initialize();

    // Re-runs the diagnostics from any non-terminal state. A successful run
    // commits the new state; an internal failure rolls back the previous state
    // and reports the failed attempt through GetStatus().
    [[nodiscard]] Result Refresh();

    // Stops the diagnostic backend and releases the driver connection. Stop is
    // allowed from every state except Uninitialized and Stopped.
    [[nodiscard]] Result Stop();

    [[nodiscard]] const char* Name() const noexcept override;
    [[nodiscard]] VirtualizationBackendState GetState() const override;
    [[nodiscard]] VirtualizationBackendStatus GetStatus() const override;
    [[nodiscard]] bool HardwareVmxStarted() const noexcept override;

    [[nodiscard]] VmxDiagnosticsReport CaptureReport() const;
    [[nodiscard]] VmxDiagnostics& Diagnostics() noexcept;
    [[nodiscard]] const VmxDiagnostics& Diagnostics() const noexcept;

private:
    [[nodiscard]] Result RunEvaluationLocked();
    [[nodiscard]] VirtualizationBackendStatus BuildStatusLocked() const;
    void RecordLocked(EventLevel level, VmxDiagnosticCode code, std::string message);

    mutable std::mutex mutex_;
    VmxCapabilityManager& capability_;
    VmxReadinessManager& readiness_;
    VmxDiagnostics diagnostics_;

    VirtualizationBackendState state_{VirtualizationBackendState::Uninitialized};
    VmxBlockReason reason_{VmxBlockReason::None};
    VmxErrorClass error_class_{VmxErrorClass::None};
    std::string detail_;
    std::uint64_t generation_{0};
    std::uint64_t successful_evaluations_{0};
    bool last_transition_failed_{false};
    std::string last_transition_error_;

    bool has_report_{false};
    VmxCapabilityReport capability_report_{};
    VmxReadinessReport readiness_report_{};
};

} // namespace hv
