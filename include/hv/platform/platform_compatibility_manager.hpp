#pragma once
#include "hv/platform/check_status.hpp"
#include "hv/platform/cpuid.hpp"
#include "hv/platform/preflight.hpp"
#include "hv/status.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace hv {

enum class CompatibilityVerdict {
    Unknown,
    NoConflictDetected,
    ReviewRequired,
    KnownConflict,
    UnsupportedPlatform
};

enum class HypervisorEnvironment { Unknown, NotReported, MicrosoftHyperV, ThirdParty, Unrecognized };
enum class VbsState { Unknown, Disabled, EnabledConfigured };
enum class HvciState { Unknown, Disabled, EnabledConfigured };

// User-mode view of Windows virtualization policy. "Configured" is intentionally
// weaker than "running": registry values and WHvGetCapability do not prove that
// every virtualized feature is active or that VMX is owned by a particular host.
struct CompatibilityEnvironmentSnapshot {
    bool vbs_configuration_known{false};
    bool vbs_configured{false};
    bool hvci_configuration_known{false};
    bool hvci_configured{false};
    bool win_hv_platform_api_available{false};
    bool win_hv_platform_query_succeeded{false};
    bool win_hv_platform_hypervisor_present{false};
    std::string vbs_detail;
    std::string hvci_detail;
    std::string whpx_detail;
};

class ICompatibilityEnvironment {
public:
    virtual ~ICompatibilityEnvironment() = default;
    [[nodiscard]] virtual Result Query(CompatibilityEnvironmentSnapshot& out) = 0;
};

// Reads Device Guard / HVCI policy from HKLM and dynamically calls
// WHvGetCapability(WHvCapabilityCodeHypervisorPresent) when WinHvPlatform is present.
class NativeCompatibilityEnvironment final : public ICompatibilityEnvironment {
public:
    [[nodiscard]] Result Query(CompatibilityEnvironmentSnapshot& out) override;
};

struct CompatibilityReport {
    CompatibilityVerdict verdict{CompatibilityVerdict::Unknown};

    CpuVendor cpu_vendor{CpuVendor::Unknown};
    std::string cpu_vendor_string;
    PreflightVerdict preflight_verdict{PreflightVerdict::ProbeFailed};
    bool cpu_vmx_reported{false};
    bool hypervisor_present_bit{false};
    HypervisorVendor cpuid_hypervisor_vendor{HypervisorVendor::None};
    std::string cpuid_hypervisor_vendor_string;

    HypervisorEnvironment environment{HypervisorEnvironment::Unknown};
    VbsState vbs{VbsState::Unknown};
    HvciState hvci{HvciState::Unknown};
    bool win_hv_platform_api_available{false};
    bool win_hv_platform_hypervisor_present{false};
    bool conflict_detected{false};
    bool kernel_verification_required{true};

    std::string summary;
    std::vector<PreflightCheck> checks;
};

// Combines the existing CPUID preflight with Windows Hyper-V/VBS/HVCI checks.
// It never attempts to disable Windows features or take ownership of VMX.
class PlatformCompatibilityManager final {
public:
    PlatformCompatibilityManager(ICpuidProvider& cpuid, ICompatibilityEnvironment& environment)
        : cpuid_(cpuid), environment_(environment) {}
    PlatformCompatibilityManager(const PlatformCompatibilityManager&) = delete;
    PlatformCompatibilityManager& operator=(const PlatformCompatibilityManager&) = delete;

    [[nodiscard]] Result Evaluate(CompatibilityReport& out);
    [[nodiscard]] std::string GetLastError() const;

private:
    ICpuidProvider& cpuid_;
    ICompatibilityEnvironment& environment_;
    mutable std::mutex mutex_;
    std::string last_error_;
};

[[nodiscard]] const char* ToString(CompatibilityVerdict value) noexcept;
[[nodiscard]] const char* ToString(HypervisorEnvironment value) noexcept;
[[nodiscard]] const char* ToString(VbsState value) noexcept;
[[nodiscard]] const char* ToString(HvciState value) noexcept;

[[nodiscard]] std::string FormatCompatibilityReportText(const CompatibilityReport& report);
[[nodiscard]] std::string FormatCompatibilityReportJson(const CompatibilityReport& report);

} // namespace hv
