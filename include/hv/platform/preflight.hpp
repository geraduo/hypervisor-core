#pragma once
#include "hv/platform/check_status.hpp"
#include "hv/platform/cpuid.hpp"
#include "hv/status.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace hv {

enum class CpuVendor { Unknown, Intel, Amd, Hygon, Other };
enum class HypervisorVendor { None, MicrosoftHyperV, Kvm, VMware, Xen, VirtualBox, QemuTcg, Other };

// There is intentionally NO "Ready" verdict. A user-mode CPUID preflight cannot prove that VMXON
// will succeed (IA32_FEATURE_CONTROL, CR0/CR4 fixed bits, firmware and other hypervisors are
// outside its view). The best possible outcome is PendingKernelVerification.
enum class PreflightVerdict {
    ProbeFailed,                // CPUID could not be read; nothing can be concluded.
    UnsupportedPlatform,        // Not x86, unsupported vendor (e.g. AMD: SVM backend not implemented),
                                // or required CPUID leaves are missing.
    VmxNotReported,             // Intel CPU, but CPUID.01H:ECX[5] (VMX) is clear.
    HypervisorPresentReviewRequired, // CPUID.01H:ECX[31] set; this is not proof of a conflict.
    HypervisorConflict = HypervisorPresentReviewRequired, // Compatibility alias; use the name above.
    PendingKernelVerification   // CPUID is consistent with VMX; kernel-mode checks still required.
};

struct PreflightCheck {
    std::string id;
    std::string title;
    CheckStatus status{CheckStatus::NotChecked};
    std::string detail;
};

// Structured result of the user-mode platform preflight.
struct PreflightReport {
    CpuVendor cpu_vendor{CpuVendor::Unknown};
    std::string cpu_vendor_string;
    std::uint32_t max_basic_leaf{};
    bool extended_leaf0_read{false};
    std::uint32_t max_extended_leaf{};

    bool leaf1_read{false};
    std::uint32_t family{};      // display family (extended family folded in)
    std::uint32_t model{};       // display model (extended model folded in)
    std::uint32_t stepping{};

    bool vmx_cpuid_bit{false};            // CPUID.01H:ECX[5]
    bool hypervisor_present_bit{false};   // CPUID.01H:ECX[31]

    HypervisorVendor hypervisor_vendor{HypervisorVendor::None};
    std::string hypervisor_vendor_string;
    std::uint32_t max_hypervisor_leaf{};

    bool svm_read{false};                 // informational only; AMD-V is not implemented
    bool svm_cpuid_bit{false};            // CPUID.80000001H:ECX[2]

    PreflightVerdict verdict{PreflightVerdict::ProbeFailed};
    std::string summary;
    bool kernel_verification_required{true};  // always true: see PreflightVerdict
    std::vector<PreflightCheck> checks;
};

// Purpose: evaluate what CPUID alone can say about VT-x suitability.
// Responsibility: read leaves 0 and 1 when advertised, 0x40000000 only when ECX[31] is set, and
// first read 0x80000000 before 0x80000001 on AMD/Hygon. Never executes privileged instructions.
// Returns an error Result only when CPUID probing itself failed (verdict == ProbeFailed).
// A non-ready determination (e.g. VmxNotReported) is a successful Result with a verdict.
[[nodiscard]] Result RunPlatformPreflight(ICpuidProvider& cpuid, PreflightReport& out);

[[nodiscard]] const char* ToString(CpuVendor value) noexcept;
[[nodiscard]] const char* ToString(HypervisorVendor value) noexcept;
[[nodiscard]] const char* ToString(CheckStatus value) noexcept;
[[nodiscard]] const char* ToString(PreflightVerdict value) noexcept;

[[nodiscard]] std::string FormatReportText(const PreflightReport& report);
[[nodiscard]] std::string FormatReportJson(const PreflightReport& report);

} // namespace hv
