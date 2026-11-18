#pragma once

// Intel VT-x capability layer.
//
// The manager reuses CpuCapabilityManager for CPU identity and user-mode
// eligibility, then adds a per-logical-processor VMX observation. Nothing here
// executes a privileged instruction: CPUID is unprivileged and may report VMX
// even when IA32_FEATURE_CONTROL has locked VMXON off. That difference is why
// the best user-mode outcome is "supported", never "VMX ready".

#include "hv/platform/check_status.hpp"
#include "hv/platform/cpu_capability_manager.hpp"
#include "hv/virtualization/vmx_status.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace hv {

// One logical processor observation. cpuid_result records whether CPUID leaf 1
// was actually read on that processor. A failure is NOT the same as "VMX not
// reported": the processor status becomes NotChecked, never Fail.
struct LogicalProcessorObservation {
    std::uint32_t processor_index{};
    Result cpuid_result{};
    bool vmx_reported{false};
    bool hypervisor_present{false};
    std::string detail;
};

// Testability seam for per-processor CPUID. The native implementation pins the
// calling thread to each active processor group and restores the previous
// affinity before returning.
class ILogicalProcessorProbe {
public:
    virtual ~ILogicalProcessorProbe() = default;
    [[nodiscard]] virtual Result Probe(std::vector<LogicalProcessorObservation>& out) = 0;
};

class NativeLogicalProcessorProbe final : public ILogicalProcessorProbe {
public:
    explicit NativeLogicalProcessorProbe(ICpuidProvider& cpuid) : cpuid_(cpuid) {}
    NativeLogicalProcessorProbe(const NativeLogicalProcessorProbe&) = delete;
    NativeLogicalProcessorProbe& operator=(const NativeLogicalProcessorProbe&) = delete;

    [[nodiscard]] Result Probe(std::vector<LogicalProcessorObservation>& out) override;

private:
    ICpuidProvider& cpuid_;
};

struct VmxProcessorCapability {
    std::uint32_t processor_index{};
    CheckStatus status{CheckStatus::NotChecked};
    bool vmx_reported{false};
    bool hypervisor_present{false};
    std::string detail;
};

struct VmxCapabilityReport {
    CheckStatus status{CheckStatus::NotChecked};
    VmxSupportLevel support{VmxSupportLevel::NotChecked};
    VmxBlockReason reason{VmxBlockReason::None};
    VmxErrorClass error_class{VmxErrorClass::None};
    std::string summary;

    CpuCapabilityReport cpu;
    bool cpu_report_valid{false};
    bool per_processor_checked{false};
    bool all_processors_checked{false};
    std::uint32_t observed_processor_count{};
    std::uint32_t vmx_enabled_processor_count{};
    std::vector<VmxProcessorCapability> processors;

    bool hypervisor_present{false};
    bool kernel_verification_required{true};

    // Current 0.x boundary: no VMXON, VMCS or guest execution.
    bool hardware_vmx_started{false};
    std::vector<PreflightCheck> checks;
};

class VmxCapabilityManager final {
public:
    // processor_probe may be null. In that case only the calling logical
    // processor is observed and per_processor_checked stays false.
    explicit VmxCapabilityManager(ICpuidProvider& cpuid,
                                  ILogicalProcessorProbe* processor_probe = nullptr)
        : cpuid_(cpuid), cpu_manager_(cpuid), processor_probe_(processor_probe) {}
    VmxCapabilityManager(const VmxCapabilityManager&) = delete;
    VmxCapabilityManager& operator=(const VmxCapabilityManager&) = delete;

    // Probes CPUID through CpuCapabilityManager, then evaluates VT-x support.
    [[nodiscard]] Result Evaluate(VmxCapabilityReport& out);

    // Classifies an existing CPU capability report and performs the per-CPU
    // observation. Exposed so callers that already probed the CPU do not pay
    // for a second CPUID pass, and so malformed reports can be validated.
    [[nodiscard]] Result EvaluateFromCpuReport(const CpuCapabilityReport& cpu,
                                               VmxCapabilityReport& out);

    [[nodiscard]] std::string GetLastError() const;

private:
    [[nodiscard]] Result EvaluateLocked(const CpuCapabilityReport& cpu,
                                        bool cpu_probe_failed,
                                        const std::string& cpu_probe_error,
                                        VmxCapabilityReport& out);

    ICpuidProvider& cpuid_;
    CpuCapabilityManager cpu_manager_;
    ILogicalProcessorProbe* processor_probe_{nullptr};
    mutable std::mutex mutex_;
    std::string last_error_;
};

[[nodiscard]] std::string FormatVmxCapabilityReportText(const VmxCapabilityReport& report);
[[nodiscard]] std::string FormatVmxCapabilityReportJson(const VmxCapabilityReport& report);

} // namespace hv
