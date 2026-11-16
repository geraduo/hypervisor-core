#pragma once

#include "hv/platform/cpu_capability_manager.hpp"
#include "hv/platform/kernel_driver_interface.hpp"
#include "hv/platform/platform_provider.hpp"
#include <string>
#include <vector>

namespace hv {

enum class CompatibilityCheckStatus { Pass, Fail, Warning, Info, NotChecked };
enum class CompatibilityVerdict { CompatibleForDiagnostics, ReviewRequired, Unsupported, Inconclusive };

struct CompatibilityCheck {
    std::string id;
    std::string title;
    CompatibilityCheckStatus status{CompatibilityCheckStatus::NotChecked};
    std::string detail;
};

struct PlatformCompatibilityReport {
    CompatibilityVerdict verdict{CompatibilityVerdict::Inconclusive};
    std::string summary;
    WindowsSystemInformation system;
    CpuCapabilityReport cpu;
    std::vector<CompatibilityCheck> checks;
    bool kernel_verification_required{true};
};

class PlatformCompatibilityManager final {
public:
    PlatformCompatibilityManager(IPlatformProvider& platform, CpuCapabilityManager& cpu,
                                 IKernelDriverInterface& driver)
        : platform_(platform), cpu_(cpu), driver_(driver) {}

    [[nodiscard]] Result Evaluate(PlatformCompatibilityReport& out);

private:
    IPlatformProvider& platform_;
    CpuCapabilityManager& cpu_;
    IKernelDriverInterface& driver_;
};

[[nodiscard]] const char* ToString(CompatibilityCheckStatus status) noexcept;
[[nodiscard]] const char* ToString(CompatibilityVerdict verdict) noexcept;
[[nodiscard]] std::string FormatCompatibilityReport(const PlatformCompatibilityReport& report);

} // namespace hv