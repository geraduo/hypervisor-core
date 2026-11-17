#include "hv/platform/platform_compatibility_manager.hpp"

#include "report_json.hpp"

#include <sstream>
#include <utility>

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

void AddCheck(CompatibilityReport& report, std::string id, std::string title,
              CheckStatus status, std::string detail) {
    report.checks.push_back({std::move(id), std::move(title), status, std::move(detail)});
}

HypervisorEnvironment ClassifyEnvironment(const PreflightReport& preflight,
                                          const CompatibilityEnvironmentSnapshot& environment) {
    if (preflight.hypervisor_present_bit) {
        switch (preflight.hypervisor_vendor) {
        case HypervisorVendor::MicrosoftHyperV: return HypervisorEnvironment::MicrosoftHyperV;
        case HypervisorVendor::Kvm:
        case HypervisorVendor::VMware:
        case HypervisorVendor::Xen:
        case HypervisorVendor::VirtualBox:
        case HypervisorVendor::QemuTcg:
            return HypervisorEnvironment::ThirdParty;
        case HypervisorVendor::None:
        case HypervisorVendor::Other:
            return HypervisorEnvironment::Unrecognized;
        }
    }
    if (environment.win_hv_platform_query_succeeded &&
        environment.win_hv_platform_hypervisor_present) {
        return HypervisorEnvironment::MicrosoftHyperV;
    }
    return HypervisorEnvironment::NotReported;
}

#if defined(_WIN32)
struct RegistryFlagResult {
    bool known{false};
    bool enabled{false};
    std::string detail;
};

RegistryFlagResult ReadRegistryFlag(const wchar_t* subkey, const wchar_t* value_name,
                                    bool absent_means_disabled) {
    RegistryFlagResult result;
    DWORD data = 0;
    DWORD size = sizeof(data);
    const LSTATUS status = RegGetValueW(HKEY_LOCAL_MACHINE, subkey, value_name,
                                        RRF_RT_REG_DWORD, nullptr, &data, &size);
    if (status == ERROR_SUCCESS) {
        result.known = true;
        result.enabled = data != 0;
        result.detail = "Registry value is present and set to " + std::to_string(data) + ".";
        return result;
    }
    if (status == ERROR_FILE_NOT_FOUND) {
        result.known = absent_means_disabled;
        result.enabled = false;
        result.detail = absent_means_disabled
                            ? "Registry value is absent; treated as disabled by default."
                            : "Registry value is absent; the configured state is unknown.";
        return result;
    }
    result.detail = "Registry read failed with Win32 error " + std::to_string(status) + ".";
    return result;
}

std::string Win32Code(DWORD code) {
    return "Win32 error " + std::to_string(code);
}
#endif

} // namespace

const char* ToString(CompatibilityVerdict value) noexcept {
    switch (value) {
    case CompatibilityVerdict::Unknown: return "Unknown";
    case CompatibilityVerdict::NoConflictDetected: return "NoConflictDetected";
    case CompatibilityVerdict::ReviewRequired: return "ReviewRequired";
    case CompatibilityVerdict::KnownConflict: return "KnownConflict";
    case CompatibilityVerdict::UnsupportedPlatform: return "UnsupportedPlatform";
    }
    return "Unknown";
}

const char* ToString(HypervisorEnvironment value) noexcept {
    switch (value) {
    case HypervisorEnvironment::Unknown: return "Unknown";
    case HypervisorEnvironment::NotReported: return "NotReported";
    case HypervisorEnvironment::MicrosoftHyperV: return "MicrosoftHyperV";
    case HypervisorEnvironment::ThirdParty: return "ThirdParty";
    case HypervisorEnvironment::Unrecognized: return "Unrecognized";
    }
    return "Unknown";
}

const char* ToString(VbsState value) noexcept {
    switch (value) {
    case VbsState::Unknown: return "Unknown";
    case VbsState::Disabled: return "Disabled";
    case VbsState::EnabledConfigured: return "EnabledConfigured";
    }
    return "Unknown";
}

const char* ToString(HvciState value) noexcept {
    switch (value) {
    case HvciState::Unknown: return "Unknown";
    case HvciState::Disabled: return "Disabled";
    case HvciState::EnabledConfigured: return "EnabledConfigured";
    }
    return "Unknown";
}

Result NativeCompatibilityEnvironment::Query(CompatibilityEnvironmentSnapshot& out) {
    out = CompatibilityEnvironmentSnapshot{};
#if defined(_WIN32)
    const RegistryFlagResult vbs = ReadRegistryFlag(
        L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard",
        L"EnableVirtualizationBasedSecurity", true);
    out.vbs_configuration_known = vbs.known;
    out.vbs_configured = vbs.enabled;
    out.vbs_detail = vbs.detail;

    const RegistryFlagResult hvci = ReadRegistryFlag(
        L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\HypervisorEnforcedCodeIntegrity",
        L"Enabled", false);
    out.hvci_configuration_known = hvci.known;
    out.hvci_configured = hvci.enabled;
    out.hvci_detail = hvci.detail;

    HMODULE library = LoadLibraryW(L"WinHvPlatform.dll");
    if (library == nullptr) {
        out.win_hv_platform_api_available = false;
        out.whpx_detail = "WinHvPlatform.dll is not available; " + Win32Code(GetLastError()) + ".";
        return Result::Ok();
    }

    using WHvGetCapabilityFn = HRESULT(WINAPI*)(UINT32, void*, UINT32, UINT32*);
    WHvGetCapabilityFn get_capability = nullptr;
#pragma warning(push)
#pragma warning(disable : 4191) // GetProcAddress returns FARPROC; explicit conversion is intentional.
    get_capability = reinterpret_cast<WHvGetCapabilityFn>(
        GetProcAddress(library, "WHvGetCapability"));
#pragma warning(pop)
    if (get_capability == nullptr) {
        out.win_hv_platform_api_available = false;
        out.whpx_detail =
            "WinHvPlatform.dll is present but WHvGetCapability is not exported; " +
            Win32Code(GetLastError()) + ".";
        (void)FreeLibrary(library);
        return Result::Ok();
    }

    out.win_hv_platform_api_available = true;
    // WHvCapabilityCodeHypervisorPresent = 0x00000000, buffer is a BOOL.
    BOOL present = FALSE;
    UINT32 written = 0;
    const HRESULT hr = get_capability(0u, &present, sizeof(present), &written);
    if (SUCCEEDED(hr) && written >= sizeof(present)) {
        out.win_hv_platform_query_succeeded = true;
        out.win_hv_platform_hypervisor_present = present != FALSE;
        out.whpx_detail = out.win_hv_platform_hypervisor_present
                              ? "WHvGetCapability reports that a Windows hypervisor is present."
                              : "WHvGetCapability reports that no Windows hypervisor is present.";
    } else {
        out.whpx_detail = "WHvGetCapability(WHvCapabilityCodeHypervisorPresent) failed with HRESULT " +
                          std::to_string(static_cast<long long>(hr)) + ".";
    }
    (void)FreeLibrary(library);
    return Result::Ok();
#else
    (void)out;
    return Result::Error(Status::Unsupported,
                         "Windows compatibility probes are only available on Windows.");
#endif
}

Result PlatformCompatibilityManager::Evaluate(CompatibilityReport& out) {
    std::lock_guard lock(mutex_);
    out = CompatibilityReport{};

    PreflightReport preflight;
    const Result preflight_result = RunPlatformPreflight(cpuid_, preflight);
    out.cpu_vendor = preflight.cpu_vendor;
    out.cpu_vendor_string = preflight.cpu_vendor_string;
    out.preflight_verdict = preflight.verdict;
    out.cpu_vmx_reported = preflight.vmx_cpuid_bit;
    out.hypervisor_present_bit = preflight.hypervisor_present_bit;
    out.cpuid_hypervisor_vendor = preflight.hypervisor_vendor;
    out.cpuid_hypervisor_vendor_string = preflight.hypervisor_vendor_string;
    out.kernel_verification_required = true;
    out.checks = preflight.checks;

    if (!preflight_result.Succeeded()) {
        out.verdict = CompatibilityVerdict::Unknown;
        out.summary =
            "The CPUID preflight failed, so platform compatibility was not evaluated. "
            "Nothing was enabled.";
        AddCheck(out, "compatibility.preflight", "CPUID preflight", CheckStatus::Fail,
                 preflight_result.message);
        last_error_ = preflight_result.message;
        return Result::Error(preflight_result.status, last_error_);
    }

    CompatibilityEnvironmentSnapshot environment;
    const Result environment_result = environment_.Query(environment);
    const bool environment_ok = environment_result.Succeeded();
    if (environment_ok) {
        AddCheck(out, "windows.compatibility_environment", "Windows compatibility environment",
                 CheckStatus::Pass,
                 "Device Guard, HVCI and Windows Hypervisor Platform probes completed.");
    } else {
        AddCheck(out, "windows.compatibility_environment", "Windows compatibility environment",
                 environment_result.status == Status::Unsupported ? CheckStatus::Info
                                                                  : CheckStatus::Warn,
                 "Windows compatibility probes are unavailable: " + environment_result.message);
    }

    out.environment = ClassifyEnvironment(preflight, environment);
    out.win_hv_platform_api_available = environment.win_hv_platform_api_available;
    out.win_hv_platform_hypervisor_present =
        environment.win_hv_platform_query_succeeded &&
        environment.win_hv_platform_hypervisor_present;

    if (environment.vbs_configuration_known) {
        out.vbs = environment.vbs_configured ? VbsState::EnabledConfigured : VbsState::Disabled;
    } else {
        out.vbs = VbsState::Unknown;
    }

    if (out.vbs == VbsState::Disabled) {
        out.hvci = HvciState::Disabled;
    } else if (environment.hvci_configuration_known) {
        out.hvci = environment.hvci_configured ? HvciState::EnabledConfigured : HvciState::Disabled;
    } else {
        out.hvci = HvciState::Unknown;
    }

    switch (out.environment) {
    case HypervisorEnvironment::NotReported:
        AddCheck(out, "compatibility.hypervisor", "Virtualization conflict detection",
                 CheckStatus::Pass,
                 "No hypervisor environment is reported by CPUID or WHvGetCapability.");
        break;
    case HypervisorEnvironment::MicrosoftHyperV:
        AddCheck(out, "compatibility.hypervisor", "Virtualization conflict detection",
                 CheckStatus::Warn,
                 "A Windows hypervisor / Hyper-V environment is reported. Direct VMX ownership is "
                 "expected to conflict unless nested virtualization or a different design is used.");
        break;
    case HypervisorEnvironment::ThirdParty:
        AddCheck(out, "compatibility.hypervisor", "Virtualization conflict detection",
                 CheckStatus::Warn,
                 "A third-party hypervisor is reported. Running as a nested guest is not validated "
                 "by v0.5.0.");
        break;
    case HypervisorEnvironment::Unrecognized:
        AddCheck(out, "compatibility.hypervisor", "Virtualization conflict detection",
                 CheckStatus::Warn,
                 "A hypervisor environment is reported, but its vendor was not recognized. "
                 "Compatibility requires review.");
        break;
    case HypervisorEnvironment::Unknown:
        AddCheck(out, "compatibility.hypervisor", "Virtualization conflict detection",
                 CheckStatus::NotChecked, "The hypervisor environment is unknown.");
        break;
    }

    AddCheck(out, "windows.vbs", "Virtualization-Based Security (VBS)",
             out.vbs == VbsState::Disabled ? CheckStatus::Pass
             : out.vbs == VbsState::EnabledConfigured ? CheckStatus::Warn
                                                      : CheckStatus::NotChecked,
             environment.vbs_detail.empty()
                 ? std::string("VBS configuration state is ") + ToString(out.vbs) + "."
                 : environment.vbs_detail);

    AddCheck(out, "windows.hvci", "Hypervisor-Enforced Code Integrity (HVCI)",
             out.hvci == HvciState::Disabled ? CheckStatus::Pass
             : out.hvci == HvciState::EnabledConfigured ? CheckStatus::Warn
                                                         : CheckStatus::NotChecked,
             out.vbs == VbsState::Disabled
                 ? "HVCI is inactive because VBS is disabled; VBS is a prerequisite for HVCI."
                 : (environment.hvci_detail.empty()
                        ? std::string("HVCI configuration state is ") + ToString(out.hvci) + "."
                        : environment.hvci_detail));

    if (!environment_ok) {
        AddCheck(out, "windows.whpx_api", "Windows Hypervisor Platform API",
                 CheckStatus::NotChecked,
                 "WinHvPlatform availability was not determined.");
    } else if (!out.win_hv_platform_api_available) {
        AddCheck(out, "windows.whpx_api", "Windows Hypervisor Platform API", CheckStatus::Info,
                 environment.whpx_detail.empty()
                     ? "WinHvPlatform is not available; this API is optional."
                     : environment.whpx_detail);
    } else if (environment.win_hv_platform_query_succeeded) {
        AddCheck(out, "windows.whpx_api", "Windows Hypervisor Platform API",
                 out.win_hv_platform_hypervisor_present ? CheckStatus::Warn : CheckStatus::Pass,
                 environment.whpx_detail);
    } else {
        AddCheck(out, "windows.whpx_api", "Windows Hypervisor Platform API", CheckStatus::Info,
                 environment.whpx_detail.empty()
                     ? "WinHvPlatform is present but the hypervisor-present query did not complete."
                     : environment.whpx_detail);
    }

    AddCheck(out, "system.kernel_restrictions", "System restrictions and kernel state",
             CheckStatus::NotChecked,
             "Code-integrity/driver-signing policy, IOMMU/VT-d, firmware setup and per-CPU "
             "IA32_FEATURE_CONTROL/VMX state require elevated or kernel-mode tools.");
    AddCheck(out, "system.boundary", "Hardware virtualization boundary", CheckStatus::Info,
             "No VMXON, VMCS, hardware EPT/NPT, physical-memory access, guest launch or "
             "hypervisor concealment was attempted.");

    const bool whpx_conflict =
        environment.win_hv_platform_query_succeeded &&
        environment.win_hv_platform_hypervisor_present;
    out.conflict_detected = out.environment == HypervisorEnvironment::MicrosoftHyperV ||
                            out.environment == HypervisorEnvironment::ThirdParty ||
                            whpx_conflict;

    if (preflight.verdict == PreflightVerdict::UnsupportedPlatform ||
        preflight.verdict == PreflightVerdict::VmxNotReported) {
        out.verdict = CompatibilityVerdict::UnsupportedPlatform;
        out.summary =
            "The CPU does not have an implemented, usable user-mode virtualization path. "
            "Kernel-mode VMXON was not attempted.";
    } else if (out.conflict_detected) {
        out.verdict = CompatibilityVerdict::KnownConflict;
        out.summary =
            "A hypervisor environment that owns or is expected to own VMX is reported. "
            "Direct VMX ownership is expected to conflict unless nested virtualization is "
            "available; kernel-mode verification is required.";
    } else if (out.environment == HypervisorEnvironment::Unrecognized) {
        out.verdict = CompatibilityVerdict::ReviewRequired;
        out.summary =
            "A hypervisor environment is reported but was not recognized. Compatibility requires "
            "review; no conflict is claimed and no VMXON was attempted.";
    } else if (out.vbs == VbsState::EnabledConfigured || out.hvci == HvciState::EnabledConfigured) {
        out.verdict = CompatibilityVerdict::ReviewRequired;
        out.summary =
            "VBS/HVCI is configured. It may reserve the hypervisor for Windows; review is required "
            "and no VMXON was attempted.";
    } else if (out.vbs == VbsState::Unknown || out.hvci == HvciState::Unknown) {
        out.verdict = CompatibilityVerdict::ReviewRequired;
        out.summary =
            "The VBS/HVCI state could not be fully determined. Review is required; no conflict is "
            "claimed and no VMXON was attempted.";
    } else if (!environment_ok) {
        out.verdict = CompatibilityVerdict::ReviewRequired;
        out.summary =
            "Windows compatibility probes did not complete. Review is required; no conflict is "
            "claimed and no VMXON was attempted.";
    } else if (preflight.verdict == PreflightVerdict::HypervisorPresentReviewRequired) {
        out.verdict = CompatibilityVerdict::ReviewRequired;
        out.summary =
            "CPUID reports a hypervisor environment. This is not proof of a conflict, but review "
            "and kernel-mode verification are required.";
    } else {
        out.verdict = CompatibilityVerdict::NoConflictDetected;
        out.summary =
            "No known user-mode virtualization conflict was detected. This is not proof that "
            "VMXON will succeed; kernel-mode checks remain required.";
    }

    last_error_.clear();
    return Result::Ok();
}

std::string PlatformCompatibilityManager::GetLastError() const {
    std::lock_guard lock(mutex_);
    return last_error_;
}

std::string FormatCompatibilityReportText(const CompatibilityReport& report) {
    std::ostringstream out;
    out << "Geraduo platform compatibility report\n";
    out << "Verdict: " << ToString(report.verdict) << '\n';
    out << "Summary: " << report.summary << '\n';
    out << "CPU vendor: " << (report.cpu_vendor_string.empty() ? "(unread)" : report.cpu_vendor_string)
        << " [" << ToString(report.cpu_vendor) << "], VMX reported="
        << (report.cpu_vmx_reported ? "yes" : "no") << '\n';
    out << "Preflight verdict: " << ToString(report.preflight_verdict) << '\n';
    out << "CPUID hypervisor: present=" << (report.hypervisor_present_bit ? "yes" : "no")
        << ", vendor=" << ToString(report.cpuid_hypervisor_vendor);
    if (!report.cpuid_hypervisor_vendor_string.empty())
        out << " (\"" << report.cpuid_hypervisor_vendor_string << "\")";
    out << '\n';
    out << "Environment: " << ToString(report.environment) << '\n';
    out << "VBS: " << ToString(report.vbs) << ", HVCI: " << ToString(report.hvci) << '\n';
    out << "WinHvPlatform API: " << (report.win_hv_platform_api_available ? "available" : "unavailable")
        << ", hypervisor reported: "
        << (report.win_hv_platform_hypervisor_present ? "yes" : "no") << '\n';
    out << "Conflict detected: " << (report.conflict_detected ? "yes" : "no") << '\n';
    out << "Checks:\n";
    for (const auto& check : report.checks)
        out << "  [" << ToString(check.status) << "] " << check.title << " - " << check.detail
            << '\n';
    out << "Kernel-mode verification required: "
        << (report.kernel_verification_required ? "yes" : "no") << '\n';
    return out.str();
}

std::string FormatCompatibilityReportJson(const CompatibilityReport& report) {
    std::ostringstream out;
    out << "{\n";
    out << "  \"verdict\": \"" << ToString(report.verdict) << "\",\n";
    out << "  \"summary\": \"" << detail::JsonEscape(report.summary) << "\",\n";
    out << "  \"cpu\": {\"vendor\": \"" << detail::JsonEscape(report.cpu_vendor_string)
        << "\", \"kind\": \"" << ToString(report.cpu_vendor) << "\", \"vmx_reported\": "
        << detail::JsonBool(report.cpu_vmx_reported) << ", \"preflight_verdict\": \""
        << ToString(report.preflight_verdict) << "\"},\n";
    out << "  \"cpuid_hypervisor\": {\"present\": "
        << detail::JsonBool(report.hypervisor_present_bit) << ", \"vendor_kind\": \""
        << ToString(report.cpuid_hypervisor_vendor) << "\", \"vendor\": \""
        << detail::JsonEscape(report.cpuid_hypervisor_vendor_string) << "\"},\n";
    out << "  \"environment\": \"" << ToString(report.environment) << "\",\n";
    out << "  \"vbs\": \"" << ToString(report.vbs) << "\", \"hvci\": \"" << ToString(report.hvci)
        << "\",\n";
    out << "  \"whpx\": {\"api_available\": "
        << detail::JsonBool(report.win_hv_platform_api_available)
        << ", \"hypervisor_present\": "
        << detail::JsonBool(report.win_hv_platform_hypervisor_present) << "},\n";
    out << "  \"conflict_detected\": " << detail::JsonBool(report.conflict_detected) << ",\n";
    out << "  \"kernel_verification_required\": "
        << detail::JsonBool(report.kernel_verification_required) << ",\n";
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
