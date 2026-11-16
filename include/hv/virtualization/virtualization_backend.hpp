#pragma once

#include "hv/platform/driver_diagnostics.hpp"
#include "hv/status.hpp"
#include "hv/virtualization/vmx_diagnostics.hpp"
#include "hv/virtualization/vmx_lifecycle_controller.hpp"
#include "hv/virtualization/vmx_readiness_manager.hpp"
#include "hv/virtualization/vmx_status.hpp"

#include <mutex>
#include <string>

namespace hv {

// Purpose: single interface for managing a hardware virtualization backend and its state.
// v0.5.0 contract: Initialize() runs read-only checks. No implementation starts hardware
// virtualization; StartVirtualization() must return an error until a real launch path exists and
// IsHypervisorRunning() must stay false until a launch is actually performed and confirmed.
class IVirtualizationBackend {
public:
    virtual ~IVirtualizationBackend() = default;

    [[nodiscard]] virtual std::string Name() const = 0;
    // Runs the capability/readiness checks. Ok only for ReadyForReview. NotSupported maps to
    // Status::Unsupported, Blocked to InitializationFailed, Failed to the failing status.
    [[nodiscard]] virtual Result Initialize() = 0;
    // Produces a report of the last completed check. Error (InvalidState) if none completed yet.
    [[nodiscard]] virtual Result RunDiagnostics(VmxDiagnosticReport& out) = 0;
    // Not implemented in v0.5.0: always returns Status::Unsupported.
    [[nodiscard]] virtual Result StartVirtualization() = 0;
    // Idempotent. Releases every resource the backend acquired (driver connection).
    [[nodiscard]] virtual Result Shutdown() = 0;
    [[nodiscard]] virtual VirtualizationBackendState GetState() const = 0;
    [[nodiscard]] virtual bool IsHypervisorRunning() const noexcept = 0;
    [[nodiscard]] virtual std::string GetLastError() const = 0;
};

// Purpose: Intel VT-x diagnostic backend built from VmxReadinessManager and
// VmxLifecycleController. It does not execute VMXON and does not start a hypervisor.
// Thread-safe: Initialize() and Shutdown() are serialized; RunDiagnostics() and GetState() may be
// called concurrently with them.
class IntelVirtualizationBackend final : public IVirtualizationBackend {
public:
    IntelVirtualizationBackend(VmxReadinessManager& readiness, VmxDiagnostics& diagnostics,
                               DriverDiagnostics* driver_diagnostics = nullptr);

    [[nodiscard]] std::string Name() const override;
    [[nodiscard]] Result Initialize() override;
    [[nodiscard]] Result RunDiagnostics(VmxDiagnosticReport& out) override;
    [[nodiscard]] Result StartVirtualization() override;
    [[nodiscard]] Result Shutdown() override;
    [[nodiscard]] VirtualizationBackendState GetState() const override;
    [[nodiscard]] bool IsHypervisorRunning() const noexcept override;
    [[nodiscard]] std::string GetLastError() const override;

private:
    void RecordReport(const VmxReadinessReport& report);
    void SetLastError(std::string message);

    VmxReadinessManager& readiness_;
    VmxDiagnostics& diagnostics_;
    DriverDiagnostics* driver_diagnostics_;
    VmxLifecycleController lifecycle_;
    std::mutex operation_mutex_;
    mutable std::mutex report_mutex_;
    VmxReadinessReport last_report_;
    bool has_report_{false};
    std::string last_error_;
};

} // namespace hv
