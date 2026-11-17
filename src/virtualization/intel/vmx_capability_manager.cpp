#include "hv/virtualization/vmx_capability_manager.hpp"

#include "hv/detail/report_json.hpp"

#include <cstdint>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace hv {
namespace {

constexpr std::uint32_t kVmxBit = 1u << 5;
constexpr std::uint32_t kHypervisorBit = 1u << 31;

void AddCheck(VmxCapabilityReport& report, std::string id, std::string title,
              CheckStatus status, std::string detail) {
    report.checks.push_back({std::move(id), std::move(title), status, std::move(detail)});
}

// A CPU capability report is malformed when its internal fields contradict
// each other. This is deliberately separate from a probe failure: a failed
// CPUID read is an error, while an impossible combination means a broken
// provider or a corrupted report.
bool ValidateCpuReport(const CpuCapabilityReport& cpu, std::string& detail) {
    if (!cpu.leaf1_read) {
        detail = "CPUID leaf 1 was not read; VT-x capability cannot be classified.";
        return false;
    }
    if (cpu.max_basic_leaf < 1) {
        detail = "The CPU report claims leaf 1 was read while the maximum basic leaf is below 1.";
        return false;
    }
    if (cpu.vmx_cpuid_bit && cpu.vendor != CpuVendor::Intel) {
        detail = "The CPU report sets the Intel VMX bit for a non-Intel vendor.";
        return false;
    }
    return true;
}

std::string ProcessorSummary(const VmxCapabilityReport& report) {
    std::string text = std::to_string(report.observed_processor_count) + " of " +
                       std::to_string(report.processors.size()) +
                       " reported logical processors were observed; " +
                       std::to_string(report.vmx_enabled_processor_count) +
                       " report VMX.";
    return text;
}

} // namespace

const char* ToString(VmxSupportLevel value) noexcept {
    switch (value) {
    case VmxSupportLevel::NotChecked: return "NotChecked";
    case VmxSupportLevel::NotSupported: return "NotSupported";
    case VmxSupportLevel::ReviewRequired: return "ReviewRequired";
    case VmxSupportLevel::Supported: return "Supported";
    }
    return "NotChecked";
}

const char* ToString(VmxErrorClass value) noexcept {
    switch (value) {
    case VmxErrorClass::None: return "None";
    case VmxErrorClass::CpuProbe: return "CpuProbe";
    case VmxErrorClass::PlatformProbe: return "PlatformProbe";
    case VmxErrorClass::Driver: return "Driver";
    case VmxErrorClass::Protocol: return "Protocol";
    case VmxErrorClass::StateMachine: return "StateMachine";
    case VmxErrorClass::Internal: return "Internal";
    }
    return "Internal";
}

const char* ToString(VmxBlockReason value) noexcept {
    switch (value) {
    case VmxBlockReason::None: return "None";
    case VmxBlockReason::UnsupportedCpuVendor: return "UnsupportedCpuVendor";
    case VmxBlockReason::VmxNotReported: return "VmxNotReported";
    case VmxBlockReason::CpuidProbeFailed: return "CpuidProbeFailed";
    case VmxBlockReason::CpuidReportInvalid: return "CpuidReportInvalid";
    case VmxBlockReason::PerProcessorCheckIncomplete: return "PerProcessorCheckIncomplete";
    case VmxBlockReason::HypervisorPresentReviewRequired: return "HypervisorPresentReviewRequired";
    case VmxBlockReason::PlatformNotChecked: return "PlatformNotChecked";
    case VmxBlockReason::PlatformBackendFailed: return "PlatformBackendFailed";
    case VmxBlockReason::PlatformUnsupported: return "PlatformUnsupported";
    case VmxBlockReason::CompatibilityNotChecked: return "CompatibilityNotChecked";
    case VmxBlockReason::CompatibilityEvaluationFailed: return "CompatibilityEvaluationFailed";
    case VmxBlockReason::CompatibilityUnsupported: return "CompatibilityUnsupported";
    case VmxBlockReason::CompatibilityReviewRequired: return "CompatibilityReviewRequired";
    case VmxBlockReason::HypervisorConflict: return "HypervisorConflict";
    case VmxBlockReason::DriverNotChecked: return "DriverNotChecked";
    case VmxBlockReason::DriverNotConnected: return "DriverNotConnected";
    case VmxBlockReason::DriverAccessDenied: return "DriverAccessDenied";
    case VmxBlockReason::DriverQueryFailed: return "DriverQueryFailed";
    case VmxBlockReason::DriverProtocolMismatch: return "DriverProtocolMismatch";
    case VmxBlockReason::DriverResponseInvalid: return "DriverResponseInvalid";
    case VmxBlockReason::DriverArchitectureMismatch: return "DriverArchitectureMismatch";
    case VmxBlockReason::DriverVersionIncompatible: return "DriverVersionIncompatible";
    case VmxBlockReason::LifecycleNotInitialized: return "LifecycleNotInitialized";
    case VmxBlockReason::LifecycleAlreadyInitialized: return "LifecycleAlreadyInitialized";
    case VmxBlockReason::LifecycleAlreadyStopped: return "LifecycleAlreadyStopped";
    case VmxBlockReason::LifecycleBusy: return "LifecycleBusy";
    case VmxBlockReason::LifecycleInvalidTransition: return "LifecycleInvalidTransition";
    case VmxBlockReason::InternalError: return "InternalError";
    }
    return "InternalError";
}

Result NativeLogicalProcessorProbe::Probe(std::vector<LogicalProcessorObservation>& out) {
    out.clear();
#if defined(_WIN32)
    constexpr std::uint32_t kMaxProcessorsPerGroup = 64;
    const HANDLE thread = GetCurrentThread();
    GROUP_AFFINITY previous{};
    if (!GetThreadGroupAffinity(thread, &previous)) {
        return Result::Error(
            Status::InitializationFailed,
            "GetThreadGroupAffinity failed; per-processor CPUID probing was not attempted.");
    }

    const auto restore = [&]() {
        return SetThreadGroupAffinity(thread, &previous, nullptr) != FALSE;
    };

    const WORD group_count = GetActiveProcessorGroupCount();
    if (group_count == 0) {
        return Result::Error(Status::InitializationFailed,
                             "Windows reported zero active processor groups.");
    }

    std::uint32_t processor_index = 0;
    for (WORD group = 0; group < group_count; ++group) {
        const DWORD group_processors = GetActiveProcessorCount(group);
        if (group_processors == 0 || group_processors > kMaxProcessorsPerGroup) {
            out.clear();
            if (!restore()) {
                return Result::Error(
                    Status::InitializationFailed,
                    "An invalid processor count was reported and thread affinity could not be "
                    "restored.");
            }
            return Result::Error(Status::InitializationFailed,
                                 "Processor group " + std::to_string(group) +
                                     " reported an invalid processor count: " +
                                     std::to_string(group_processors) + ".");
        }

        for (DWORD bit = 0; bit < group_processors; ++bit) {
            LogicalProcessorObservation observation;
            observation.processor_index = processor_index++;

            GROUP_AFFINITY target{};
            target.Group = group;
            target.Mask = static_cast<KAFFINITY>(1) << bit;
            if (!SetThreadGroupAffinity(thread, &target, nullptr)) {
                observation.cpuid_result = Result::Error(
                    Status::AccessDenied,
                    "SetThreadGroupAffinity failed for processor " +
                        std::to_string(observation.processor_index) + ".");
                observation.detail = observation.cpuid_result.message;
                out.push_back(std::move(observation));
                continue;
            }

            CpuidRegisters registers{};
            observation.cpuid_result = cpuid_.Query(1, 0, registers);
            if (observation.cpuid_result.Succeeded()) {
                observation.vmx_reported = (registers.ecx & kVmxBit) != 0;
                observation.hypervisor_present = (registers.ecx & kHypervisorBit) != 0;
                observation.detail =
                    std::string("CPUID.01H:ECX[5]=") +
                    (observation.vmx_reported ? "1" : "0") + ", ECX[31]=" +
                    (observation.hypervisor_present ? "1" : "0") + ".";
            } else {
                observation.detail = observation.cpuid_result.message;
            }
            out.push_back(std::move(observation));
        }
    }

    if (!restore()) {
        out.clear();
        return Result::Error(
            Status::InitializationFailed,
            "The previous thread affinity could not be restored; per-processor results were "
            "discarded.");
    }
    return Result::Ok();
#else
    CpuidRegisters registers{};
    LogicalProcessorObservation observation;
    observation.processor_index = 0;
    observation.cpuid_result = cpuid_.Query(1, 0, registers);
    if (observation.cpuid_result.Succeeded()) {
        observation.vmx_reported = (registers.ecx & kVmxBit) != 0;
        observation.hypervisor_present = (registers.ecx & kHypervisorBit) != 0;
        observation.detail = "CPUID.01H:ECX[5] and ECX[31] were read on the calling processor.";
    } else {
        observation.detail = observation.cpuid_result.message;
    }
    out.push_back(std::move(observation));
    return Result::Ok();
#endif
}

Result VmxCapabilityManager::Evaluate(VmxCapabilityReport& out) {
    std::lock_guard lock(mutex_);
    CpuCapabilityReport cpu;
    const Result cpu_result = cpu_manager_.Probe(cpu);
    if (!cpu_result.Succeeded())
        return EvaluateLocked(cpu, true, cpu_result.message, out);
    return EvaluateLocked(cpu, false, std::string{}, out);
}

Result VmxCapabilityManager::EvaluateFromCpuReport(const CpuCapabilityReport& cpu,
                                                   VmxCapabilityReport& out) {
    std::lock_guard lock(mutex_);
    return EvaluateLocked(cpu, false, std::string{}, out);
}

Result VmxCapabilityManager::EvaluateLocked(const CpuCapabilityReport& cpu,
                                            bool cpu_probe_failed,
                                            const std::string& cpu_probe_error,
                                            VmxCapabilityReport& out) {
    out = VmxCapabilityReport{};
    out.cpu = cpu;
    out.checks = cpu.checks;
    out.hypervisor_present = cpu.hypervisor_present_bit;
    out.kernel_verification_required = true;
    out.hardware_vmx_started = false;

    if (cpu_probe_failed) {
        out.status = CheckStatus::Fail;
        out.support = VmxSupportLevel::NotChecked;
        out.reason = VmxBlockReason::CpuidProbeFailed;
        out.error_class = VmxErrorClass::CpuProbe;
        out.summary = "The CPU capability probe failed; VT-x support was not evaluated.";
        AddCheck(out, "vmx.cpu_probe", "CPU capability probe", CheckStatus::Fail,
                 cpu_probe_error);
        last_error_ = cpu_probe_error;
        return Result::Error(Status::InitializationFailed, last_error_);
    }

    std::string validation_error;
    if (!ValidateCpuReport(cpu, validation_error)) {
        out.status = CheckStatus::Fail;
        out.support = VmxSupportLevel::NotChecked;
        out.reason = VmxBlockReason::CpuidReportInvalid;
        out.error_class = VmxErrorClass::CpuProbe;
        out.summary = "The CPUID capability report is inconsistent and was not classified.";
        AddCheck(out, "vmx.cpu_report", "CPUID capability report", CheckStatus::Fail,
                 validation_error);
        last_error_ = validation_error;
        return Result::Error(Status::InvalidArgument, last_error_);
    }

    out.cpu_report_valid = true;
    AddCheck(out, "vmx.cpu_report", "CPUID capability report", CheckStatus::Pass,
             "CPUID leaf 1 is present; vendor=\"" + cpu.vendor_string + "\", family=" +
                 std::to_string(cpu.family) + ", model=" + std::to_string(cpu.model) + ".");

    if (cpu.vendor != CpuVendor::Intel) {
        out.status = CheckStatus::Fail;
        out.support = VmxSupportLevel::NotSupported;
        out.reason = VmxBlockReason::UnsupportedCpuVendor;
        out.summary = "CPU vendor \"" + cpu.vendor_string +
                      "\" has no implemented virtualization backend. The Intel VT-x "
                      "foundation requires GenuineIntel.";
        AddCheck(out, "vmx.vendor", "Intel CPU vendor", CheckStatus::Fail,
                 "Vendor=\"" + cpu.vendor_string + "\". AMD-V/SVM is not implemented in v0.5.0.");
        last_error_.clear();
        return Result::Ok();
    }

    AddCheck(out, "vmx.vendor", "Intel CPU vendor", CheckStatus::Pass,
             "Vendor=\"" + cpu.vendor_string + "\".");

    if (processor_probe_ == nullptr) {
        VmxProcessorCapability current;
        current.processor_index = 0;
        current.vmx_reported = cpu.vmx_cpuid_bit;
        current.hypervisor_present = cpu.hypervisor_present_bit;
        current.status = cpu.vmx_cpuid_bit ? CheckStatus::Pass : CheckStatus::Fail;
        current.detail =
            "Only the calling logical processor was observed; no per-processor probe is "
            "configured.";
        out.processors.push_back(std::move(current));
        out.observed_processor_count = 1;
        if (cpu.vmx_cpuid_bit) out.vmx_enabled_processor_count = 1;
        out.status = CheckStatus::NotChecked;
        out.support = VmxSupportLevel::NotChecked;
        out.reason = VmxBlockReason::PerProcessorCheckIncomplete;
        out.summary =
            "CPUID was read only on the calling logical processor. Per-processor coverage is "
            "required before readiness can be reviewed.";
        AddCheck(out, "vmx.processors", "Per-logical-processor VMX", CheckStatus::NotChecked,
                 "Per-processor enumeration was not configured; multi-processor coverage is "
                 "unknown.");
        last_error_.clear();
        return Result::Ok();
    }

    std::vector<LogicalProcessorObservation> observations;
    const Result probe_result = processor_probe_->Probe(observations);
    out.per_processor_checked = probe_result.Succeeded();
    if (!probe_result.Succeeded()) {
        out.status = CheckStatus::NotChecked;
        out.support = VmxSupportLevel::NotChecked;
        out.reason = VmxBlockReason::PerProcessorCheckIncomplete;
        out.error_class = VmxErrorClass::CpuProbe;
        out.summary = "The per-logical-processor CPUID probe failed; VMX coverage is unknown.";
        AddCheck(out, "vmx.processors", "Per-logical-processor VMX", CheckStatus::NotChecked,
                 probe_result.message);
        last_error_ = probe_result.message;
        return Result::Error(probe_result.status, last_error_);
    }

    if (observations.empty()) {
        const std::string detail =
            "The per-processor probe succeeded but returned no logical processors.";
        out.status = CheckStatus::Fail;
        out.support = VmxSupportLevel::NotChecked;
        out.reason = VmxBlockReason::CpuidReportInvalid;
        out.error_class = VmxErrorClass::CpuProbe;
        out.summary = "The per-processor CPUID report is malformed.";
        AddCheck(out, "vmx.processors", "Per-logical-processor VMX", CheckStatus::Fail, detail);
        last_error_ = detail;
        return Result::Error(Status::InvalidArgument, last_error_);
    }

    for (std::size_t left = 0; left < observations.size(); ++left) {
        for (std::size_t right = left + 1; right < observations.size(); ++right) {
            if (observations[left].processor_index == observations[right].processor_index) {
                const std::string detail =
                    "The per-processor probe reported logical processor index " +
                    std::to_string(observations[left].processor_index) + " more than once.";
                out.status = CheckStatus::Fail;
                out.support = VmxSupportLevel::NotChecked;
                out.reason = VmxBlockReason::CpuidReportInvalid;
                out.error_class = VmxErrorClass::CpuProbe;
                out.summary = "The per-processor CPUID report is malformed.";
                AddCheck(out, "vmx.processors", "Per-logical-processor VMX", CheckStatus::Fail,
                         detail);
                last_error_ = detail;
                return Result::Error(Status::InvalidArgument, last_error_);
            }
        }
    }

    out.processors.reserve(observations.size());
    for (const auto& observation : observations) {
        VmxProcessorCapability processor;
        processor.processor_index = observation.processor_index;
        processor.hypervisor_present = observation.hypervisor_present;
        if (observation.cpuid_result.Succeeded()) {
            processor.vmx_reported = observation.vmx_reported;
            processor.status = observation.vmx_reported ? CheckStatus::Pass : CheckStatus::Fail;
            processor.detail = observation.detail.empty()
                                   ? (observation.vmx_reported
                                          ? "CPUID.01H:ECX[5] is set."
                                          : "CPUID.01H:ECX[5] is clear.")
                                   : observation.detail;
            ++out.observed_processor_count;
            if (observation.vmx_reported) ++out.vmx_enabled_processor_count;
        } else {
            processor.status = CheckStatus::NotChecked;
            processor.detail = "CPUID leaf 1 could not be read on this processor: " +
                               observation.cpuid_result.message;
        }
        if (observation.hypervisor_present) out.hypervisor_present = true;
        out.processors.push_back(std::move(processor));
    }

    out.all_processors_checked = out.observed_processor_count == out.processors.size();
    AddCheck(out, "vmx.processors", "Per-logical-processor VMX",
             !out.all_processors_checked
                 ? CheckStatus::NotChecked
                 : (out.vmx_enabled_processor_count == out.observed_processor_count
                        ? CheckStatus::Pass
                        : CheckStatus::Fail),
             ProcessorSummary(out));

    if (out.observed_processor_count > 0 &&
        out.vmx_enabled_processor_count == out.observed_processor_count &&
        out.all_processors_checked) {
        if (out.hypervisor_present) {
            out.status = CheckStatus::Warn;
            out.support = VmxSupportLevel::ReviewRequired;
            out.reason = VmxBlockReason::HypervisorPresentReviewRequired;
            out.summary =
                "Intel VMX is reported on every observed logical processor, but a hypervisor "
                "environment is present. VMX ownership requires review; nothing was started.";
        } else {
            out.status = CheckStatus::Pass;
            out.support = VmxSupportLevel::Supported;
            out.reason = VmxBlockReason::None;
            out.summary =
                "Intel VMX is reported on every observed logical processor. This does not prove "
                "that IA32_FEATURE_CONTROL enables VMXON; no privileged instruction was executed.";
        }
        last_error_.clear();
        return Result::Ok();
    }

    if (out.observed_processor_count > 0 &&
        out.vmx_enabled_processor_count < out.observed_processor_count) {
        out.status = CheckStatus::Fail;
        out.support = VmxSupportLevel::NotSupported;
        out.reason = VmxBlockReason::VmxNotReported;
        out.summary =
            "At least one observed logical processor does not report Intel VMX. The implemented "
            "backend cannot use this configuration.";
        last_error_.clear();
        return Result::Ok();
    }

    out.status = CheckStatus::NotChecked;
    out.support = VmxSupportLevel::NotChecked;
    out.reason = VmxBlockReason::PerProcessorCheckIncomplete;
    out.summary =
        "Per-processor VMX coverage is incomplete; readiness cannot be established.";
    last_error_.clear();
    return Result::Ok();
}

std::string VmxCapabilityManager::GetLastError() const {
    std::lock_guard lock(mutex_);
    return last_error_;
}

std::string FormatVmxCapabilityReportText(const VmxCapabilityReport& report) {
    std::ostringstream out;
    out << "Geraduo Intel VT-x capability report\n";
    out << "Status: " << ToString(report.status) << '\n';
    out << "Support: " << ToString(report.support) << '\n';
    out << "Reason: " << ToString(report.reason) << '\n';
    out << "Error class: " << ToString(report.error_class) << '\n';
    out << "Summary: " << report.summary << '\n';
    out << "CPU vendor: "
        << (report.cpu.vendor_string.empty() ? "(unread)" : report.cpu.vendor_string) << " ["
        << ToString(report.cpu.vendor) << "]\n";
    out << "CPU brand: " << (report.cpu.brand_string.empty() ? "(not read)"
                                                            : report.cpu.brand_string)
        << '\n';
    out << "CPU signature: family " << report.cpu.family << ", model " << report.cpu.model
        << ", stepping " << report.cpu.stepping << '\n';
    out << "CPU eligibility: " << ToString(report.cpu.eligibility)
        << ", preflight verdict: " << ToString(report.cpu.preflight_verdict) << '\n';
    out << "CPUID VMX bit: " << (report.cpu.vmx_cpuid_bit ? "yes" : "no")
        << ", hypervisor present: " << (report.hypervisor_present ? "yes" : "no") << '\n';
    out << "Per-processor checked: " << (report.per_processor_checked ? "yes" : "no")
        << ", all processors observed: " << (report.all_processors_checked ? "yes" : "no")
        << " (" << report.observed_processor_count << " observed, "
        << report.vmx_enabled_processor_count << " with VMX)\n";
    out << "Logical processors:\n";
    for (const auto& processor : report.processors) {
        out << "  [" << ToString(processor.status) << "] CPU " << processor.processor_index
            << " - VMX=" << (processor.vmx_reported ? "yes" : "no")
            << ", hypervisor=" << (processor.hypervisor_present ? "yes" : "no") << " - "
            << processor.detail << '\n';
    }
    out << "Checks:\n";
    for (const auto& check : report.checks)
        out << "  [" << ToString(check.status) << "] " << check.title << " - " << check.detail
            << '\n';
    out << "Kernel-mode verification required: "
        << (report.kernel_verification_required ? "yes" : "no") << '\n';
    out << "Hardware VMX started: " << (report.hardware_vmx_started ? "yes" : "no") << '\n';
    return out.str();
}

std::string FormatVmxCapabilityReportJson(const VmxCapabilityReport& report) {
    std::ostringstream out;
    out << "{\n";
    out << "  \"status\": \"" << ToString(report.status) << "\",\n";
    out << "  \"support\": \"" << ToString(report.support) << "\",\n";
    out << "  \"reason\": \"" << ToString(report.reason) << "\",\n";
    out << "  \"error_class\": \"" << ToString(report.error_class) << "\",\n";
    out << "  \"summary\": \"" << detail::JsonEscape(report.summary) << "\",\n";
    out << "  \"cpu\": {\n";
    out << "    \"vendor\": \"" << ToString(report.cpu.vendor) << "\",\n";
    out << "    \"vendor_string\": \"" << detail::JsonEscape(report.cpu.vendor_string)
        << "\",\n";
    out << "    \"brand\": \"" << detail::JsonEscape(report.cpu.brand_string) << "\",\n";
    out << "    \"family\": " << report.cpu.family << ", \"model\": " << report.cpu.model
        << ", \"stepping\": " << report.cpu.stepping << ",\n";
    out << "    \"vmx_cpuid_bit\": " << detail::JsonBool(report.cpu.vmx_cpuid_bit) << ",\n";
    out << "    \"hypervisor_present_bit\": "
        << detail::JsonBool(report.cpu.hypervisor_present_bit) << ",\n";
    out << "    \"eligibility\": \"" << ToString(report.cpu.eligibility) << "\",\n";
    out << "    \"preflight_verdict\": \"" << ToString(report.cpu.preflight_verdict)
        << "\"\n";
    out << "  },\n";
    out << "  \"cpu_report_valid\": " << detail::JsonBool(report.cpu_report_valid) << ",\n";
    out << "  \"per_processor_checked\": " << detail::JsonBool(report.per_processor_checked)
        << ",\n";
    out << "  \"all_processors_checked\": "
        << detail::JsonBool(report.all_processors_checked) << ",\n";
    out << "  \"observed_processor_count\": " << report.observed_processor_count << ",\n";
    out << "  \"vmx_enabled_processor_count\": " << report.vmx_enabled_processor_count << ",\n";
    out << "  \"hypervisor_present\": " << detail::JsonBool(report.hypervisor_present) << ",\n";
    out << "  \"kernel_verification_required\": "
        << detail::JsonBool(report.kernel_verification_required) << ",\n";
    out << "  \"hardware_vmx_started\": " << detail::JsonBool(report.hardware_vmx_started)
        << ",\n";
    out << "  \"processors\": [\n";
    for (std::size_t index = 0; index < report.processors.size(); ++index) {
        const auto& processor = report.processors[index];
        out << "    {\"processor_index\": " << processor.processor_index << ", \"status\": \""
            << ToString(processor.status) << "\", \"vmx_reported\": "
            << detail::JsonBool(processor.vmx_reported) << ", \"hypervisor_present\": "
            << detail::JsonBool(processor.hypervisor_present) << ", \"detail\": \""
            << detail::JsonEscape(processor.detail) << "\"}"
            << (index + 1 < report.processors.size() ? "," : "") << '\n';
    }
    out << "  ],\n";
    out << "  \"checks\": [\n";
    for (std::size_t index = 0; index < report.checks.size(); ++index) {
        const auto& check = report.checks[index];
        out << "    {\"id\": \"" << detail::JsonEscape(check.id) << "\", \"title\": \""
            << detail::JsonEscape(check.title) << "\", \"status\": \"" << ToString(check.status)
            << "\", \"detail\": \"" << detail::JsonEscape(check.detail) << "\"}"
            << (index + 1 < report.checks.size() ? "," : "") << '\n';
    }
    out << "  ]\n}\n";
    return out.str();
}

} // namespace hv
