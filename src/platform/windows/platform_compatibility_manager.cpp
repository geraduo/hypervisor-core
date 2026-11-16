#include "hv/platform/platform_compatibility_manager.hpp"
#include <utility>

namespace hv {
namespace {

void Add(PlatformCompatibilityReport& report, std::string id, std::string title,
         CompatibilityCheckStatus status, std::string detail) {
    report.checks.push_back({std::move(id), std::move(title), status, std::move(detail)});
}

void AddConfiguredState(PlatformCompatibilityReport& report, const char* id, const char* title,
                        const std::optional<bool>& configured, bool& review_required,
                        const std::string& provider_note) {
    if (!configured.has_value()) {
        Add(report, id, title, CompatibilityCheckStatus::NotChecked,
            "Configuration value was not available. " + provider_note);
        return;
    }
    if (*configured) {
        review_required = true;
        Add(report, id, title, CompatibilityCheckStatus::Warning,
            "Configured in the registry; this does not prove that the feature is running. " + provider_note);
        return;
    }
    Add(report, id, title, CompatibilityCheckStatus::Pass,
        "The configuration value is not enabled; this does not prove the feature is inactive at runtime.");
}

} // namespace

Result PlatformCompatibilityManager::Evaluate(PlatformCompatibilityReport& out) {
    out = PlatformCompatibilityReport{};
    bool unsupported = false;
    bool review_required = false;
    bool inconclusive = false;

    auto result = platform_.QuerySystemInformation(out.system);
    if (!result.Succeeded()) {
        Add(out, "windows.system", "Windows system information", CompatibilityCheckStatus::Fail,
            result.message);
        out.verdict = CompatibilityVerdict::Inconclusive;
        out.summary = "Windows system information could not be collected.";
        return result;
    }

    if (!out.system.is_windows) {
        Add(out, "windows.system", "Windows operating system", CompatibilityCheckStatus::Fail,
            "The active platform provider did not identify a Windows host.");
        unsupported = true;
    } else {
        Add(out, "windows.system", "Windows operating system", CompatibilityCheckStatus::Pass,
            FormatWindowsVersion(out.system));
    }

    if (out.system.architecture != ProcessorArchitecture::X64) {
        Add(out, "windows.architecture", "Windows x64 architecture", CompatibilityCheckStatus::Fail,
            "Detected " + std::string(ToString(out.system.architecture)) +
                "; this release has no kernel backend for that architecture.");
        unsupported = true;
    } else {
        Add(out, "windows.architecture", "Windows x64 architecture", CompatibilityCheckStatus::Pass,
            "The diagnostic driver target is Windows x64.");
    }

    if (out.system.major_version < 10) {
        Add(out, "windows.version", "Supported Windows version", CompatibilityCheckStatus::Fail,
            "Windows 10 or newer is required by the v0.4.0 backend.");
        unsupported = true;
    } else {
        Add(out, "windows.version", "Supported Windows version", CompatibilityCheckStatus::Pass,
            "Windows 10/11 version family detected.");
    }

    result = cpu_.Query(out.cpu);
    if (!result.Succeeded()) {
        Add(out, "cpu.cpuid", "CPU capability probe", CompatibilityCheckStatus::Fail,
            result.message);
        inconclusive = true;
    } else {
        const auto& preflight = out.cpu.preflight;
        const bool supported_cpu = preflight.cpu_vendor == CpuVendor::Intel && preflight.leaf1_read &&
                                   preflight.vmx_cpuid_bit;
        if (preflight.verdict == PreflightVerdict::ProbeFailed) {
            Add(out, "cpu.cpuid", "CPU capability probe", CompatibilityCheckStatus::Fail,
                preflight.summary);
            inconclusive = true;
        } else if (!supported_cpu) {
            Add(out, "cpu.cpuid", "Intel VMX CPUID capability", CompatibilityCheckStatus::Fail,
                preflight.summary);
            unsupported = true;
        } else if (preflight.hypervisor_present_bit) {
            const std::string vendor = preflight.hypervisor_vendor_string.empty()
                                           ? "vendor unavailable"
                                           : preflight.hypervisor_vendor_string;
            Add(out, "virtualization.hypervisor", "Existing hypervisor environment",
                CompatibilityCheckStatus::Warning,
                "CPUID reports a hypervisor environment (" + vendor +
                    "). This is a review signal, not proof of a conflict.");
            review_required = true;
        } else {
            Add(out, "cpu.cpuid", "Intel VMX CPUID capability", CompatibilityCheckStatus::Pass,
                "VMX is reported by CPUID. This does not establish that VMXON is permitted.");
        }
    }

    WindowsMitigationInformation mitigations;
    const auto mitigation_result = platform_.QueryMitigationInformation(mitigations);
    if (mitigation_result.Succeeded()) {
        AddConfiguredState(out, "windows.vbs", "Virtualization-based Security (VBS)",
                           mitigations.vbs_configured, review_required, mitigations.note);
        AddConfiguredState(out, "windows.hvci", "Hypervisor-protected Code Integrity (HVCI)",
                           mitigations.hvci_configured, review_required, mitigations.note);
        AddConfiguredState(out, "windows.credential_guard", "Credential Guard configuration",
                           mitigations.credential_guard_configured, review_required, mitigations.note);
    } else {
        Add(out, "windows.vbs", "VBS and HVCI configuration", CompatibilityCheckStatus::NotChecked,
            mitigation_result.message + " Runtime state remains unknown.");
        inconclusive = true;
    }

    if (driver_.IsConnected()) {
        DriverPlatformInfo driver_information;
        const auto driver_result = driver_.QueryPlatformInfo(driver_information);
        if (!driver_result.Succeeded()) {
            Add(out, "driver.platform_info", "Kernel diagnostic response",
                CompatibilityCheckStatus::Warning, driver_result.message);
            review_required = true;
        } else {
            const bool version_matches = driver_information.os_major_version == out.system.major_version &&
                                         driver_information.os_minor_version == out.system.minor_version &&
                                         driver_information.os_build_number == out.system.build_number;
            Add(out, "driver.platform_info", "Kernel diagnostic response",
                version_matches ? CompatibilityCheckStatus::Pass : CompatibilityCheckStatus::Warning,
                version_matches ? "Driver and user-mode OS version reports match."
                                : "Driver and user-mode OS version reports differ; review the environment.");
            if (!version_matches || driver_information.architecture != GERADUO_ARCHITECTURE_X64)
                review_required = true;
        }
    } else {
        Add(out, "driver.connection", "Kernel diagnostic driver", CompatibilityCheckStatus::NotChecked,
            "Driver is not connected. Kernel-only platform checks are unavailable; the user-mode report stays advisory.");
    }

    Add(out, "kernel.privileged_checks", "Privileged VMX and kernel checks",
        CompatibilityCheckStatus::NotChecked,
        "This diagnostic backend does not execute VMX instructions or claim hardware virtualization readiness.");

    if (unsupported) {
        out.verdict = CompatibilityVerdict::Unsupported;
        out.summary = "The current host does not meet the implemented Windows x64 / Intel VMX diagnostic scope.";
    } else if (inconclusive) {
        out.verdict = CompatibilityVerdict::Inconclusive;
        out.summary = "Some platform evidence could not be collected; no readiness claim is made.";
    } else if (review_required) {
        out.verdict = CompatibilityVerdict::ReviewRequired;
        out.summary = "Windows virtualization configuration needs review; this is not a VMX-ready verdict.";
    } else {
        out.verdict = CompatibilityVerdict::CompatibleForDiagnostics;
        out.summary = "Host evidence is consistent with the diagnostic scope; kernel verification is still required.";
    }
    return Result::Ok();
}

const char* ToString(CompatibilityCheckStatus status) noexcept {
    switch (status) {
    case CompatibilityCheckStatus::Pass: return "PASS";
    case CompatibilityCheckStatus::Fail: return "FAIL";
    case CompatibilityCheckStatus::Warning: return "WARN";
    case CompatibilityCheckStatus::Info: return "INFO";
    case CompatibilityCheckStatus::NotChecked: return "NOT CHECKED";
    }
    return "NOT CHECKED";
}

const char* ToString(CompatibilityVerdict verdict) noexcept {
    switch (verdict) {
    case CompatibilityVerdict::CompatibleForDiagnostics: return "CompatibleForDiagnostics";
    case CompatibilityVerdict::ReviewRequired: return "ReviewRequired";
    case CompatibilityVerdict::Unsupported: return "Unsupported";
    case CompatibilityVerdict::Inconclusive: return "Inconclusive";
    }
    return "Inconclusive";
}

std::string FormatCompatibilityReport(const PlatformCompatibilityReport& report) {
    std::string text = "Platform compatibility report\nVerdict: ";
    text += ToString(report.verdict);
    text += "\nSummary: ";
    text += report.summary;
    text += "\nChecks:\n";
    for (const auto& check : report.checks) {
        text += "  [";
        text += ToString(check.status);
        text += "] ";
        text += check.title;
        text += " - ";
        text += check.detail;
        text += '\n';
    }
    text += "Kernel verification required: yes\n";
    return text;
}

} // namespace hv