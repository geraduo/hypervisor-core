#include "hv/platform/preflight.hpp"
#include <cstdio>
#include <iomanip>
#include <sstream>

namespace hv {

const char* ToString(CpuVendor v) noexcept {
    switch (v) {
    case CpuVendor::Unknown: return "Unknown";
    case CpuVendor::Intel: return "Intel";
    case CpuVendor::Amd: return "AMD";
    case CpuVendor::Hygon: return "Hygon";
    case CpuVendor::Other: return "Other";
    }
    return "Unknown";
}

const char* ToString(HypervisorVendor v) noexcept {
    switch (v) {
    case HypervisorVendor::None: return "None";
    case HypervisorVendor::MicrosoftHyperV: return "Microsoft Hyper-V";
    case HypervisorVendor::Kvm: return "KVM";
    case HypervisorVendor::VMware: return "VMware";
    case HypervisorVendor::Xen: return "Xen";
    case HypervisorVendor::VirtualBox: return "VirtualBox";
    case HypervisorVendor::QemuTcg: return "QEMU TCG";
    case HypervisorVendor::Other: return "Other";
    }
    return "Other";
}

const char* ToString(CheckStatus v) noexcept {
    switch (v) {
    case CheckStatus::Pass: return "PASS";
    case CheckStatus::Fail: return "FAIL";
    case CheckStatus::Warn: return "WARN";
    case CheckStatus::Info: return "INFO";
    case CheckStatus::NotChecked: return "NOT CHECKED";
    }
    return "NOT CHECKED";
}

const char* ToString(PreflightVerdict v) noexcept {
    switch (v) {
    case PreflightVerdict::ProbeFailed: return "ProbeFailed";
    case PreflightVerdict::UnsupportedPlatform: return "UnsupportedPlatform";
    case PreflightVerdict::VmxNotReported: return "VmxNotReported";
    case PreflightVerdict::HypervisorPresentReviewRequired: return "HypervisorPresentReviewRequired";
    case PreflightVerdict::PendingKernelVerification: return "PendingKernelVerification";
    }
    return "ProbeFailed";
}

std::string FormatReportText(const PreflightReport& r) {
    std::ostringstream out;
    out << "Geraduo Platform Preflight (user-mode, CPUID only)\n";
    out << "Verdict: " << ToString(r.verdict) << '\n';
    out << "Summary: " << r.summary << '\n';
    out << "CPU vendor: " << (r.cpu_vendor_string.empty() ? "(unread)" : r.cpu_vendor_string)
        << " [" << ToString(r.cpu_vendor) << "], max basic leaf " << r.max_basic_leaf << '\n';
    if (r.extended_leaf0_read)
        out << "Max extended CPUID leaf: 0x" << std::hex << r.max_extended_leaf << std::dec << '\n';
    else
        out << "Max extended CPUID leaf: not checked\n";
    if (r.leaf1_read)
        out << "CPU signature: family " << r.family << ", model " << r.model
            << ", stepping " << r.stepping << '\n';
    out << "Hypervisor: " << (r.hypervisor_present_bit ? ToString(r.hypervisor_vendor) : "not reported");
    if (!r.hypervisor_vendor_string.empty()) out << " (\"" << r.hypervisor_vendor_string << "\")";
    out << '\n';
    out << "Checks:\n";
    for (const auto& c : r.checks)
        out << "  [" << ToString(c.status) << "] " << c.title << " - " << c.detail << '\n';
    out << "Kernel-mode verification required: " << (r.kernel_verification_required ? "yes" : "no") << '\n';
    return out.str();
}

namespace {
std::string JsonEscape(const std::string& text) {
    std::string result;
    for (const char ch : text) {
        const auto value = static_cast<unsigned char>(ch);
        switch (ch) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\n': result += "\\n"; break;
        case '\f': result += "\\f"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (value < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", value);
                result += buffer;
            } else {
                result.push_back(ch);
            }
        }
    }
    return result;
}
const char* B(bool v) { return v ? "true" : "false"; }
} // namespace

std::string FormatReportJson(const PreflightReport& r) {
    std::ostringstream o;
    o << "{\n";
    o << "  \"verdict\": \"" << ToString(r.verdict) << "\",\n";
    o << "  \"summary\": \"" << JsonEscape(r.summary) << "\",\n";
    o << "  \"kernel_verification_required\": " << B(r.kernel_verification_required) << ",\n";
    o << "  \"cpu\": {\"vendor\": \"" << JsonEscape(r.cpu_vendor_string) << "\", \"kind\": \""
      << ToString(r.cpu_vendor) << "\", \"max_basic_leaf\": " << r.max_basic_leaf
      << ", \"family\": " << r.family << ", \"model\": " << r.model
      << ", \"stepping\": " << r.stepping << "},\n";
    o << "  \"extended_cpuid\": {\"leaf0_read\": " << B(r.extended_leaf0_read)
      << ", \"max_leaf\": " << r.max_extended_leaf << "},\n";
    o << "  \"vmx_cpuid_bit\": " << B(r.vmx_cpuid_bit) << ",\n";
    o << "  \"hypervisor\": {\"present\": " << B(r.hypervisor_present_bit) << ", \"vendor\": \""
      << JsonEscape(r.hypervisor_vendor_string) << "\", \"kind\": \"" << ToString(r.hypervisor_vendor)
      << "\", \"max_leaf\": " << r.max_hypervisor_leaf << "},\n";
    o << "  \"svm\": {\"read\": " << B(r.svm_read) << ", \"cpuid_bit\": " << B(r.svm_cpuid_bit) << "},\n";
    o << "  \"checks\": [\n";
    for (std::size_t i = 0; i < r.checks.size(); ++i) {
        const auto& c = r.checks[i];
        o << "    {\"id\": \"" << JsonEscape(c.id) << "\", \"status\": \"" << ToString(c.status)
          << "\", \"title\": \"" << JsonEscape(c.title) << "\", \"detail\": \"" << JsonEscape(c.detail) << "\"}"
          << (i + 1 < r.checks.size() ? "," : "") << '\n';
    }
    o << "  ]\n}\n";
    return o.str();
}

} // namespace hv
