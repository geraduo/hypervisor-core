#pragma once
#include "hv/platform/cpuid.hpp"
#include "hv/platform/preflight.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace hv {

// What user-mode CPUID can conclude. There is deliberately no "Ready" value:
// kernel-mode checks are always required before VMXON can be considered.
enum class CpuEligibility { Unsupported, ReviewRequired, KernelVerificationRequired };

struct CpuCapabilityReport {
    CpuVendor vendor{CpuVendor::Unknown};
    std::string vendor_string;
    std::string brand_string;
    std::uint32_t family{};
    std::uint32_t model{};
    std::uint32_t stepping{};
    std::uint32_t max_basic_leaf{};
    bool extended_leaf0_read{false};
    std::uint32_t max_extended_leaf{};
    bool brand_string_read{false};
    bool leaf1_read{false};
    bool vmx_cpuid_bit{false};
    bool hypervisor_present_bit{false};
    bool svm_cpuid_bit{false};

    CpuEligibility eligibility{CpuEligibility::Unsupported};
    bool kernel_verification_required{true};
    PreflightVerdict preflight_verdict{PreflightVerdict::ProbeFailed};
    std::string summary;
    std::vector<PreflightCheck> checks;
};

// Reuses the existing CPUID provider and RunPlatformPreflight logic, then adds
// CPU identity, brand-string decoding and explicit unsupported-CPU handling.
class CpuCapabilityManager final {
public:
    explicit CpuCapabilityManager(ICpuidProvider& cpuid) : cpuid_(cpuid) {}
    CpuCapabilityManager(const CpuCapabilityManager&) = delete;
    CpuCapabilityManager& operator=(const CpuCapabilityManager&) = delete;

    [[nodiscard]] Result Probe(CpuCapabilityReport& out);
    [[nodiscard]] std::string GetLastError() const;

private:
    ICpuidProvider& cpuid_;
    mutable std::mutex mutex_;
    std::string last_error_;
};

[[nodiscard]] const char* ToString(CpuEligibility value) noexcept;
[[nodiscard]] std::string FormatCpuCapabilityReportText(const CpuCapabilityReport& report);
[[nodiscard]] std::string FormatCpuCapabilityReportJson(const CpuCapabilityReport& report);

} // namespace hv
