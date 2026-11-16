#pragma once

#include "hv/platform/kernel_driver_interface.hpp"
#include "hv/platform/platform_compatibility_manager.hpp"
#include "hv/platform/windows_platform_backend.hpp"
#include "hv/virtualization/vmx_capability_manager.hpp"
#include "hv/virtualization/vmx_status.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace hv {

struct VmxDriverStatus {
    CheckStatus status{CheckStatus::NotChecked};
    bool connect_attempted{false};
    bool connected{false};
    bool connected_by_readiness{false};  // this manager opened the connection and will close it
    bool protocol_verified{false};
    std::uint32_t expected_protocol_version{GERADUO_DRIVER_PROTOCOL_VERSION};
    bool platform_info_read{false};
    DriverPlatformInfo platform_info{};
    Status error_code{Status::Success};
    std::string message;
};

struct VmxReadinessOptions {
    // true: a missing/incompatible/failing diagnostic driver blocks readiness.
    // false: the same problems are reported as WARN and kernel-only checks stay NOT_CHECKED.
    bool require_driver{true};
};

struct VmxReadinessReport {
    VmxReadinessVerdict verdict{VmxReadinessVerdict::Failed};
    std::string summary;
    WindowsBackendState backend_state{WindowsBackendState::Stopped};
    bool platform_collected{false};
    WindowsPlatformSnapshot platform;
    bool capability_collected{false};
    VmxCapabilityReport capability;
    bool compatibility_collected{false};
    PlatformCompatibilityReport compatibility;
    VmxDriverStatus driver;
    std::vector<VmxFailureDetail> reasons;
    std::vector<VmxCondition> conditions;
    bool kernel_verification_required{true};  // always true in v0.5.0

    // Conditions that were not verified (status NOT_CHECKED).
    [[nodiscard]] std::vector<VmxCondition> Unverified() const;
};

// Purpose: combines the Windows Platform Backend snapshot, per-processor VMX capability, the
// platform compatibility report and the diagnostic driver into one readiness verdict.
// Responsibility: never claims a "ready" state; unverifiable conditions are listed as NOT_CHECKED.
// Result semantics: Evaluate() returns an error only for verdict == Failed. NotSupported and
// Blocked are valid, successful evaluations.
// Resource handling: when this manager connects the driver it keeps the connection only for a
// ReadyForReview verdict; any other verdict disconnects it before returning. ReleaseDriver()
// closes a retained connection. A connection opened by someone else is never closed.
// Thread-safe: Evaluate() and ReleaseDriver() are serialized.
class VmxReadinessManager final {
public:
    VmxReadinessManager(WindowsPlatformBackend& backend, VmxCapabilityManager& capability,
                        PlatformCompatibilityManager& compatibility, IKernelDriverInterface& driver,
                        VmxReadinessOptions options = VmxReadinessOptions{})
        : backend_(backend),
          capability_(capability),
          compatibility_(compatibility),
          driver_(driver),
          options_(options) {}

    [[nodiscard]] Result Evaluate(VmxReadinessReport& out);
    void ReleaseDriver();

private:
    void CollectCapability(VmxReadinessReport& out);
    void CollectPlatform(VmxReadinessReport& out);
    void CollectDriver(VmxReadinessReport& out);
    void CollectCompatibility(VmxReadinessReport& out);
    void AddKernelConditions(VmxReadinessReport& out);
    [[nodiscard]] Result Finalize(VmxReadinessReport& out);

    WindowsPlatformBackend& backend_;
    VmxCapabilityManager& capability_;
    PlatformCompatibilityManager& compatibility_;
    IKernelDriverInterface& driver_;
    VmxReadinessOptions options_;
    std::mutex mutex_;
    bool owns_connection_{false};
};

} // namespace hv
