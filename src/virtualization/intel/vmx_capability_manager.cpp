#include "hv/virtualization/vmx_capability_manager.hpp"

#include <string>
#include <utility>

namespace hv {
namespace {

constexpr const char* kComponent = "VmxCapabilityManager";

// Routes the existing ICpuidProvider interface to one specific logical processor so that
// RunPlatformPreflight() can be reused unchanged for every processor.
class ProcessorCpuidAdapter final : public ICpuidProvider {
public:
    ProcessorCpuidAdapter(ILogicalProcessorCpuidProvider& provider, std::uint32_t cpu_index)
        : provider_(provider), cpu_index_(cpu_index) {}

    Result Query(std::uint32_t leaf, std::uint32_t subleaf, CpuidRegisters& out) override {
        return provider_.Query(cpu_index_, leaf, subleaf, out);
    }

private:
    ILogicalProcessorCpuidProvider& provider_;
    std::uint32_t cpu_index_;
};

void AddReason(VmxCapabilityReport& report, VmxFailureReason reason, Status status,
               std::string message) {
    for (const auto& existing : report.reasons) {
        if (existing.reason == reason) return;
    }
    report.reasons.push_back(VmxFailureDetail{reason, status, std::move(message), kComponent});
}

ProcessorDiagnostic ProbeProcessor(ILogicalProcessorCpuidProvider& provider,
                                   std::uint32_t cpu_index) {
    ProcessorDiagnostic diagnostic;
    diagnostic.cpu_index = cpu_index;

    ProcessorCpuidAdapter adapter(provider, cpu_index);
    PreflightReport preflight;
    const Result result = RunPlatformPreflight(adapter, preflight);
    diagnostic.vendor = preflight.cpu_vendor_string;

    if (!result.Succeeded()) {
        // The processor could not be probed. It stays NOT_CHECKED; nothing is assumed.
        diagnostic.vmx_capability = VmxCapabilityState::NotChecked;
        diagnostic.check_status = CheckStatus::NotChecked;
        diagnostic.error_code = result.status;
        diagnostic.diagnostic_result = "NOT_CHECKED: " + result.message;
        return diagnostic;
    }

    if (diagnostic.vendor.empty()) diagnostic.vendor = ToString(preflight.cpu_vendor);

    if (preflight.cpu_vendor != CpuVendor::Intel) {
        diagnostic.vmx_capability = VmxCapabilityState::UnsupportedVendor;
        diagnostic.check_status = CheckStatus::Fail;
        diagnostic.error_code = Status::Unsupported;
        diagnostic.diagnostic_result =
            "CPU vendor is '" + diagnostic.vendor + "'; this release implements only Intel VT-x.";
    } else if (!preflight.leaf1_read) {
        diagnostic.vmx_capability = VmxCapabilityState::NotChecked;
        diagnostic.check_status = CheckStatus::NotChecked;
        diagnostic.error_code = Status::InvalidResponse;
        diagnostic.diagnostic_result = "NOT_CHECKED: CPUID leaf 1 could not be read.";
    } else if (preflight.vmx_cpuid_bit) {
        diagnostic.vmx_capability = VmxCapabilityState::CpuidReported;
        diagnostic.check_status = CheckStatus::Pass;
        diagnostic.error_code = Status::Success;
        diagnostic.diagnostic_result =
            "CPUID.01H:ECX[5] is set. IA32_FEATURE_CONTROL, CR0/CR4 fixed bits and VMXON are not "
            "verified.";
    } else {
        diagnostic.vmx_capability = VmxCapabilityState::NotReported;
        diagnostic.check_status = CheckStatus::Fail;
        diagnostic.error_code = Status::Unsupported;
        diagnostic.diagnostic_result = "CPUID.01H:ECX[5] is clear: VMX is not reported.";
    }
    return diagnostic;
}

} // namespace

Result VmxCapabilityManager::Query(VmxCapabilityReport& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    out = VmxCapabilityReport{};

    // 1. The calling processor, through the existing CpuCapabilityManager / preflight.
    const Result cpu_result = cpu_.Query(out.cpu);
    if (!cpu_result.Succeeded()) {
        AddReason(out, VmxFailureReason::CpuProbeFailed, cpu_result.status,
                  "CPUID probe on the calling processor failed: " + cpu_result.message);
        out.verdict = VmxCapabilityVerdict::Inconclusive;
        out.summary = "CPU capabilities could not be read; no conclusion is possible.";
        return cpu_result;
    }
    out.cpu_collected = true;

    bool definitively_unsupported = false;
    const PreflightReport& current = out.cpu.preflight;
    if (current.verdict == PreflightVerdict::UnsupportedPlatform) {
        const bool other_vendor = current.cpu_vendor == CpuVendor::Amd ||
                                  current.cpu_vendor == CpuVendor::Hygon ||
                                  current.cpu_vendor == CpuVendor::Other;
        AddReason(out,
                  other_vendor ? VmxFailureReason::UnsupportedVendor
                               : VmxFailureReason::UnsupportedPlatform,
                  Status::Unsupported, current.summary);
        definitively_unsupported = true;
    } else if (current.verdict == PreflightVerdict::VmxNotReported) {
        AddReason(out, VmxFailureReason::VmxNotReported, Status::Unsupported, current.summary);
        definitively_unsupported = true;
    }

    // 2. Enumerate logical processors.
    std::uint32_t count = 0;
    const Result count_result = processors_.GetLogicalProcessorCount(count);
    if (!count_result.Succeeded() || count == 0) {
        const Status status = count_result.Succeeded() ? Status::InvalidResponse : count_result.status;
        const std::string detail = count_result.Succeeded()
                                       ? std::string("the provider reported zero logical processors")
                                       : count_result.message;
        if (definitively_unsupported || status == Status::Unsupported) {
            if (!definitively_unsupported) {
                AddReason(out, VmxFailureReason::UnsupportedPlatform, status,
                          "Per-processor CPUID is unavailable on this platform: " + detail);
            }
            out.verdict = VmxCapabilityVerdict::NotSupported;
            out.summary = "The configuration is not supported for the Intel VT-x backend.";
            return Result::Ok();
        }
        AddReason(out, VmxFailureReason::ProcessorEnumerationFailed, status,
                  "Logical processors could not be enumerated: " + detail);
        out.verdict = VmxCapabilityVerdict::Inconclusive;
        out.summary = "Logical processors could not be enumerated; per-processor state is NOT_CHECKED.";
        return Result::Error(status, "Logical processors could not be enumerated: " + detail);
    }

    // 3. Probe every processor independently. No result is propagated to another processor.
    out.processors_expected = count;
    out.processors.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        out.processors.push_back(ProbeProcessor(processors_, index));
    }

    std::uint32_t not_checked = 0;
    std::uint32_t not_reported = 0;
    std::uint32_t wrong_vendor = 0;
    Status first_unchecked_status = Status::Success;
    std::string first_unchecked_message;
    for (const auto& processor : out.processors) {
        if (processor.check_status != CheckStatus::NotChecked) ++out.processors_checked;
        switch (processor.vmx_capability) {
        case VmxCapabilityState::CpuidReported: ++out.processors_vmx_reported; break;
        case VmxCapabilityState::NotReported: ++not_reported; break;
        case VmxCapabilityState::UnsupportedVendor: ++wrong_vendor; break;
        case VmxCapabilityState::NotChecked:
            if (not_checked == 0) {
                first_unchecked_status = processor.error_code;
                first_unchecked_message = "CPU " + std::to_string(processor.cpu_index) + ": " +
                                          processor.diagnostic_result;
            }
            ++not_checked;
            break;
        }
    }

    const std::string total = std::to_string(count);
    if (not_reported > 0) {
        AddReason(out, VmxFailureReason::VmxNotReported, Status::Unsupported,
                  std::to_string(not_reported) + " of " + total +
                      " logical processors do not report CPUID.01H:ECX[5] (VMX).");
    }
    if (wrong_vendor > 0) {
        AddReason(out, VmxFailureReason::UnsupportedVendor, Status::Unsupported,
                  std::to_string(wrong_vendor) + " of " + total +
                      " logical processors are not Intel CPUs.");
    }

    if (definitively_unsupported || not_reported > 0 || wrong_vendor > 0) {
        out.verdict = VmxCapabilityVerdict::NotSupported;
        out.summary = "At least one logical processor does not report Intel VMX.";
    } else if (not_checked > 0) {
        AddReason(out, VmxFailureReason::ProcessorNotChecked, first_unchecked_status,
                  std::to_string(not_checked) + " of " + total +
                      " logical processors are NOT_CHECKED (first: " + first_unchecked_message + ").");
        out.verdict = VmxCapabilityVerdict::Inconclusive;
        out.summary = "VMX was confirmed on " + std::to_string(out.processors_vmx_reported) + " of " +
                      total + " logical processors; the rest are NOT_CHECKED.";
    } else {
        out.verdict = VmxCapabilityVerdict::CpuidReportedOnAllProcessors;
        out.summary = "CPUID reports VMX on all " + total +
                      " logical processors (each checked individually). This is not proof that "
                      "VMXON is permitted.";
    }
    return Result::Ok();
}

} // namespace hv
