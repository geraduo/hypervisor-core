#pragma once

// Readiness layer: combines the VT-x capability report with the Windows
// platform backend, the Windows compatibility policy and the diagnostic driver
// protocol. The output is a verdict for kernel-stage review, not a hardware
// launch authorization.

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

struct VmxReadinessOptions {
    bool query_platform{true};
    bool query_driver{true};
    bool query_compatibility{true};
};

// Structured result of the user-mode <-> driver compatibility check. Fields
// marked *_checked are true only when the corresponding comparison actually
// ran. Everything else stays explicitly unverified.
struct VmxDriverCompatibilityReport {
    CheckStatus status{CheckStatus::NotChecked};
    bool query_attempted{false};
    bool connected{false};
    bool query_succeeded{false};

    bool protocol_version_checked{false};
    bool protocol_version_compatible{false};
    std::uint32_t expected_protocol_version{kGeraduoProtocolVersion};
    std::uint32_t reported_protocol_version{0};

    bool structure_layout_checked{false};
    bool structure_layout_compatible{false};
    std::uint32_t expected_response_size{static_cast<std::uint32_t>(
        sizeof(DriverPlatformInfoResponse))};
    std::uint32_t reported_structure_size{0};

    bool driver_version_checked{false};
    bool driver_version_compatible{false};
    std::uint32_t driver_version_major{0};
    std::uint32_t driver_version_minor{0};
    std::uint32_t driver_version_patch{0};

    bool architecture_checked{false};
    bool architecture_compatible{false};
    DriverArchitecture driver_architecture{DriverArchitecture::Unknown};

    bool processor_count_checked{false};
    bool processor_count_matches{false};
    std::uint32_t driver_processor_count{0};
    std::uint32_t platform_processor_count{0};

    std::string detail;
};

struct VmxReadinessReport {
    VmxReadinessState state{VmxReadinessState::NotEvaluated};
    CheckStatus status{CheckStatus::NotChecked};
    VmxBlockReason reason{VmxBlockReason::None};
    VmxErrorClass error_class{VmxErrorClass::None};
    std::string summary;

    VmxSupportLevel vmx_support{VmxSupportLevel::NotChecked};
    PlatformState platform_state{PlatformState::Uninitialized};
    PlatformSupport platform_support{PlatformSupport::Unsupported};
    CompatibilityVerdict compatibility_verdict{CompatibilityVerdict::Unknown};
    VmxDriverCompatibilityReport driver;

    std::vector<PreflightCheck> checks;

    bool kernel_verification_required{true};
    bool hardware_vmx_started{false};
    bool hardware_start_supported{false};
};

class VmxReadinessManager final {
public:
    VmxReadinessManager(WindowsPlatformBackend& platform, IKernelDriverInterface& driver,
                        PlatformCompatibilityManager& compatibility)
        : platform_(platform), driver_(driver), compatibility_(compatibility) {}
    VmxReadinessManager(const VmxReadinessManager&) = delete;
    VmxReadinessManager& operator=(const VmxReadinessManager&) = delete;

    // Uses the options stored by SetOptions().
    [[nodiscard]] Result Evaluate(const VmxCapabilityReport& capability,
                                  VmxReadinessReport& out);

    // Explicit options, used by tests and one-shot callers.
    [[nodiscard]] Result Evaluate(const VmxCapabilityReport& capability,
                                  VmxReadinessReport& out,
                                  const VmxReadinessOptions& options);

    void SetOptions(const VmxReadinessOptions& options);
    [[nodiscard]] VmxReadinessOptions GetOptions() const;

    // Releases the diagnostic driver connection. Called by the lifecycle
    // controller on Stop() so a stopped backend leaves no handle open.
    void ReleaseConnections();

    [[nodiscard]] std::string GetLastError() const;

private:
    [[nodiscard]] Result EvaluateLocked(const VmxCapabilityReport& capability,
                                        VmxReadinessReport& out,
                                        const VmxReadinessOptions& options);

    mutable std::mutex mutex_;
    WindowsPlatformBackend& platform_;
    IKernelDriverInterface& driver_;
    PlatformCompatibilityManager& compatibility_;
    VmxReadinessOptions options_{};
    std::string last_error_;
};

[[nodiscard]] std::string FormatVmxReadinessReportText(const VmxReadinessReport& report);
[[nodiscard]] std::string FormatVmxReadinessReportJson(const VmxReadinessReport& report);

} // namespace hv
