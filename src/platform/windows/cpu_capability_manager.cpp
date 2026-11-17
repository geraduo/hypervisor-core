#include "hv/platform/cpu_capability_manager.hpp"

#include "report_json.hpp"

#include <array>
#include <sstream>
#include <utility>

namespace hv {
namespace {

constexpr std::uint32_t kExtendedLeaf0 = 0x80000000u;
constexpr std::uint32_t kBrandLeafFirst = 0x80000002u;
constexpr std::uint32_t kBrandLeafLast = 0x80000004u;
constexpr std::uint32_t kMaxExtendedLeaf = 0x8FFFFFFFu;

void AddCheck(CpuCapabilityReport& report, std::string id, std::string title,
              CheckStatus status, std::string detail) {
    report.checks.push_back({std::move(id), std::move(title), status, std::move(detail)});
}

std::string DecodeBrand(const std::array<unsigned char, 48>& bytes) {
    std::string text;
    text.reserve(bytes.size());
    for (const unsigned char value : bytes) {
        if (value == 0) break;
        if (value < 0x20 || value > 0x7E) text.push_back('?');
        else text.push_back(static_cast<char>(value));
    }

    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) ++begin;
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t')) --end;
    return text.substr(begin, end - begin);
}

CpuEligibility MapEligibility(PreflightVerdict verdict) {
    switch (verdict) {
    case PreflightVerdict::PendingKernelVerification:
        return CpuEligibility::KernelVerificationRequired;
    case PreflightVerdict::HypervisorPresentReviewRequired:
        return CpuEligibility::ReviewRequired;
    case PreflightVerdict::ProbeFailed:
    case PreflightVerdict::UnsupportedPlatform:
    case PreflightVerdict::VmxNotReported:
        return CpuEligibility::Unsupported;
    }
    return CpuEligibility::Unsupported;
}

} // namespace

const char* ToString(CpuEligibility value) noexcept {
    switch (value) {
    case CpuEligibility::Unsupported: return "Unsupported";
    case CpuEligibility::ReviewRequired: return "ReviewRequired";
    case CpuEligibility::KernelVerificationRequired: return "KernelVerificationRequired";
    }
    return "Unsupported";
}

Result CpuCapabilityManager::Probe(CpuCapabilityReport& out) {
    std::lock_guard lock(mutex_);
    out = CpuCapabilityReport{};

    PreflightReport preflight;
    const Result preflight_result = RunPlatformPreflight(cpuid_, preflight);

    out.vendor = preflight.cpu_vendor;
    out.vendor_string = preflight.cpu_vendor_string;
    out.family = preflight.family;
    out.model = preflight.model;
    out.stepping = preflight.stepping;
    out.max_basic_leaf = preflight.max_basic_leaf;
    out.extended_leaf0_read = preflight.extended_leaf0_read;
    out.max_extended_leaf = preflight.max_extended_leaf;
    out.leaf1_read = preflight.leaf1_read;
    out.vmx_cpuid_bit = preflight.vmx_cpuid_bit;
    out.hypervisor_present_bit = preflight.hypervisor_present_bit;
    out.svm_cpuid_bit = preflight.svm_cpuid_bit;
    out.preflight_verdict = preflight.verdict;
    out.checks = preflight.checks;

    if (!preflight_result.Succeeded()) {
        out.eligibility = CpuEligibility::Unsupported;
        out.kernel_verification_required = true;
        out.summary = "CPUID probing failed; CPU capability cannot be determined. Nothing was enabled.";
        AddCheck(out, "cpu.probe", "CPUID capability probe", CheckStatus::Fail,
                 preflight_result.message);
        last_error_ = preflight_result.message;
        return Result::Error(preflight_result.status, last_error_);
    }

    std::uint32_t max_extended = preflight.max_extended_leaf;
    bool extended_known = preflight.extended_leaf0_read;
    if (!extended_known) {
        CpuidRegisters registers{};
        const Result extended = cpuid_.Query(kExtendedLeaf0, 0, registers);
        if (extended.Succeeded()) {
            max_extended = registers.eax;
            extended_known = true;
            out.extended_leaf0_read = true;
            out.max_extended_leaf = max_extended;
            if (max_extended < kExtendedLeaf0 || max_extended > kMaxExtendedLeaf) {
                AddCheck(out, "cpu.brand", "CPU brand string", CheckStatus::Warn,
                         "The maximum extended CPUID leaf is malformed; the brand string was not read.");
                extended_known = false;
            }
        } else if (extended.status == Status::Unsupported || extended.status == Status::NotFound) {
            AddCheck(out, "cpu.brand", "CPU brand string", CheckStatus::NotChecked,
                     "Extended CPUID leaves are not available; the brand string was not read.");
        } else {
            AddCheck(out, "cpu.brand", "CPU brand string", CheckStatus::Warn,
                     "The extended CPUID maximum leaf could not be read: " + extended.message);
        }
    }

    if (extended_known && (max_extended < kExtendedLeaf0 || max_extended > kMaxExtendedLeaf)) {
        AddCheck(out, "cpu.brand", "CPU brand string", CheckStatus::Warn,
                 "The maximum extended CPUID leaf is malformed; the brand string was not read.");
        extended_known = false;
    }

    if (extended_known && max_extended >= kBrandLeafLast) {
        std::array<unsigned char, 48> bytes{};
        bool read_ok = true;
        for (std::uint32_t index = 0; index < 3; ++index) {
            CpuidRegisters registers{};
            const Result leaf = cpuid_.Query(kBrandLeafFirst + index, 0, registers);
            if (!leaf.Succeeded()) {
                AddCheck(out, "cpu.brand", "CPU brand string", CheckStatus::Warn,
                         "CPUID brand leaf could not be read: " + leaf.message);
                read_ok = false;
                break;
            }
            const std::array<std::uint32_t, 4> values = {registers.eax, registers.ebx,
                                                         registers.ecx, registers.edx};
            for (std::size_t value_index = 0; value_index < values.size(); ++value_index) {
                for (std::uint32_t byte_index = 0; byte_index < 4; ++byte_index) {
                    const std::size_t offset = index * 16 + value_index * 4 + byte_index;
                    bytes[offset] = static_cast<unsigned char>(
                        (values[value_index] >> (8u * byte_index)) & 0xFFu);
                }
            }
        }
        if (read_ok) {
            out.brand_string_read = true;
            out.brand_string = DecodeBrand(bytes);
            AddCheck(out, "cpu.brand", "CPU brand string",
                     out.brand_string.empty() ? CheckStatus::Warn : CheckStatus::Pass,
                     out.brand_string.empty() ? "CPUID brand leaves returned an empty string."
                                              : "Brand=\"" + out.brand_string + "\".");
        }
    } else if (extended_known) {
        AddCheck(out, "cpu.brand", "CPU brand string", CheckStatus::NotChecked,
                 "The maximum extended leaf does not advertise CPUID.80000002H-80000004H.");
    }

    out.eligibility = MapEligibility(preflight.verdict);
    out.kernel_verification_required = true;

    switch (out.eligibility) {
    case CpuEligibility::KernelVerificationRequired:
        AddCheck(out, "cpu.eligibility", "CPU eligibility", CheckStatus::Info,
                 "CPUID is consistent with Intel VT-x. Kernel-mode IA32_FEATURE_CONTROL, "
                 "CR0/CR4 fixed-bit and VMX capability checks are still required.");
        out.summary =
            "Intel VT-x is reported by CPUID. The CPU is eligible only for kernel-mode "
            "verification; VMXON has not been attempted and no hardware virtualization was enabled.";
        break;
    case CpuEligibility::ReviewRequired:
        AddCheck(out, "cpu.eligibility", "CPU eligibility", CheckStatus::Warn,
                 "A hypervisor environment is reported. VMX ownership cannot be assumed and "
                 "kernel-mode checks are required.");
        out.summary =
            "A hypervisor environment is reported by CPUID. This is not proof of a conflict, but "
            "ownership of VMX cannot be assumed; kernel-mode verification is required.";
        break;
    case CpuEligibility::Unsupported:
        AddCheck(out, "cpu.eligibility", "CPU eligibility", CheckStatus::Fail,
                 preflight.summary.empty() ? "This CPU is not supported by the implemented backend."
                                           : preflight.summary);
        out.summary = preflight.summary.empty()
                          ? "The CPU is not supported by the implemented virtualization backend."
                          : preflight.summary;
        break;
    }

    if (out.vendor != CpuVendor::Intel && !out.brand_string.empty()) {
        AddCheck(out, "cpu.vendor_backend", "Vendor-specific virtualization backend",
                 CheckStatus::Info,
                 "Brand \"" + out.brand_string + "\" is recorded for diagnostics only; no AMD-V/SVM "
                 "backend is implemented in v0.5.0.");
    }

    last_error_.clear();
    return Result::Ok();
}

std::string CpuCapabilityManager::GetLastError() const {
    std::lock_guard lock(mutex_);
    return last_error_;
}

std::string FormatCpuCapabilityReportText(const CpuCapabilityReport& report) {
    std::ostringstream out;
    out << "Geraduo CPU capability report\n";
    out << "Vendor: " << (report.vendor_string.empty() ? "(unread)" : report.vendor_string) << " ["
        << ToString(report.vendor) << "]\n";
    out << "Brand: " << (report.brand_string.empty() ? "(not read)" : report.brand_string) << '\n';
    out << "Signature: family " << report.family << ", model " << report.model
        << ", stepping " << report.stepping << '\n';
    out << "CPUID: max basic leaf " << report.max_basic_leaf;
    if (report.extended_leaf0_read)
        out << ", max extended leaf 0x" << std::hex << report.max_extended_leaf << std::dec;
    else
        out << ", extended leaves not read";
    out << '\n';
    out << "Features: VMX reported=" << (report.vmx_cpuid_bit ? "yes" : "no")
        << ", SVM reported=" << (report.svm_cpuid_bit ? "yes" : "no")
        << ", hypervisor present=" << (report.hypervisor_present_bit ? "yes" : "no") << '\n';
    out << "Eligibility: " << ToString(report.eligibility) << '\n';
    out << "Preflight verdict: " << ToString(report.preflight_verdict) << '\n';
    out << "Summary: " << report.summary << '\n';
    out << "Checks:\n";
    for (const auto& check : report.checks)
        out << "  [" << ToString(check.status) << "] " << check.title << " - " << check.detail
            << '\n';
    out << "Kernel-mode verification required: "
        << (report.kernel_verification_required ? "yes" : "no") << '\n';
    return out.str();
}

std::string FormatCpuCapabilityReportJson(const CpuCapabilityReport& report) {
    std::ostringstream out;
    out << "{\n";
    out << "  \"vendor\": \"" << ToString(report.vendor) << "\",\n";
    out << "  \"vendor_string\": \"" << detail::JsonEscape(report.vendor_string) << "\",\n";
    out << "  \"brand\": \"" << detail::JsonEscape(report.brand_string) << "\",\n";
    out << "  \"brand_string_read\": " << detail::JsonBool(report.brand_string_read) << ",\n";
    out << "  \"family\": " << report.family << ", \"model\": " << report.model
        << ", \"stepping\": " << report.stepping << ",\n";
    out << "  \"max_basic_leaf\": " << report.max_basic_leaf << ",\n";
    out << "  \"extended_leaf0_read\": " << detail::JsonBool(report.extended_leaf0_read)
        << ", \"max_extended_leaf\": " << report.max_extended_leaf << ",\n";
    out << "  \"vmx_cpuid_bit\": " << detail::JsonBool(report.vmx_cpuid_bit) << ",\n";
    out << "  \"svm_cpuid_bit\": " << detail::JsonBool(report.svm_cpuid_bit) << ",\n";
    out << "  \"hypervisor_present_bit\": " << detail::JsonBool(report.hypervisor_present_bit)
        << ",\n";
    out << "  \"eligibility\": \"" << ToString(report.eligibility) << "\",\n";
    out << "  \"preflight_verdict\": \"" << ToString(report.preflight_verdict) << "\",\n";
    out << "  \"kernel_verification_required\": "
        << detail::JsonBool(report.kernel_verification_required) << ",\n";
    out << "  \"summary\": \"" << detail::JsonEscape(report.summary) << "\",\n";
    out << "  \"checks\": [\n";
    for (std::size_t i = 0; i < report.checks.size(); ++i) {
        const auto& check = report.checks[i];
        out << "    {\"id\": \"" << detail::JsonEscape(check.id) << "\", \"title\": \""
            << detail::JsonEscape(check.title) << "\", \"status\": \"" << ToString(check.status)
            << "\", \"detail\": \"" << detail::JsonEscape(check.detail) << "\"}"
            << (i + 1 < report.checks.size() ? "," : "") << '\n';
    }
    out << "  ]\n}\n";
    return out.str();
}

} // namespace hv
