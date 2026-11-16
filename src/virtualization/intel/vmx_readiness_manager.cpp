#include "hv/virtualization/vmx_readiness_manager.hpp"

#include <exception>
#include <string>
#include <utility>

namespace hv {
namespace {

constexpr const char* kComponent = "VmxReadinessManager";

CheckStatus ToCheckStatus(CompatibilityCheckStatus status) noexcept {
    switch (status) {
    case CompatibilityCheckStatus::Pass: return CheckStatus::Pass;
    case CompatibilityCheckStatus::Fail: return CheckStatus::Fail;
    case CompatibilityCheckStatus::Warning: return CheckStatus::Warn;
    case CompatibilityCheckStatus::Info: return CheckStatus::Info;
    case CompatibilityCheckStatus::NotChecked: return CheckStatus::NotChecked;
    }
    return CheckStatus::NotChecked;
}

int Rank(VmxReadinessVerdict verdict) noexcept {
    switch (verdict) {
    case VmxReadinessVerdict::ReadyForReview: return 0;
    case VmxReadinessVerdict::Blocked: return 1;
    case VmxReadinessVerdict::NotSupported: return 2;
    case VmxReadinessVerdict::Failed: return 3;
    }
    return 3;
}

void AddCondition(VmxReadinessReport& report, std::string id, std::string title, CheckStatus status,
                  std::string detail) {
    report.conditions.push_back(
        VmxCondition{std::move(id), std::move(title), status, std::move(detail)});
}

void AddReason(VmxReadinessReport& report, VmxFailureReason reason, Status status,
               std::string message, std::string component = kComponent) {
    for (const auto& existing : report.reasons) {
        if (existing.reason == reason && existing.message == message) return;
    }
    report.reasons.push_back(
        VmxFailureDetail{reason, status, std::move(message), std::move(component)});
}

VmxFailureReason ReasonForConnectStatus(Status status) noexcept {
    switch (status) {
    case Status::DriverNotFound: return VmxFailureReason::DriverNotFound;
    case Status::AccessDenied: return VmxFailureReason::DriverAccessDenied;
    case Status::ProtocolMismatch: return VmxFailureReason::DriverProtocolMismatch;
    default: return VmxFailureReason::DriverConnectionFailed;
    }
}

std::uint32_t ArchitectureCode(ProcessorArchitecture architecture) noexcept {
    switch (architecture) {
    case ProcessorArchitecture::X86: return static_cast<std::uint32_t>(GERADUO_ARCHITECTURE_X86);
    case ProcessorArchitecture::X64: return static_cast<std::uint32_t>(GERADUO_ARCHITECTURE_X64);
    case ProcessorArchitecture::Arm64: return static_cast<std::uint32_t>(GERADUO_ARCHITECTURE_ARM64);
    case ProcessorArchitecture::Unknown: break;
    }
    return static_cast<std::uint32_t>(GERADUO_ARCHITECTURE_UNKNOWN);
}

} // namespace

std::vector<VmxCondition> VmxReadinessReport::Unverified() const {
    std::vector<VmxCondition> result;
    for (const auto& condition : conditions) {
        if (condition.status == CheckStatus::NotChecked) result.push_back(condition);
    }
    return result;
}

Result VmxReadinessManager::Evaluate(VmxReadinessReport& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    out = VmxReadinessReport{};

    // A retained connection may have been closed by someone else in the meantime.
    if (owns_connection_ && !driver_.IsConnected()) owns_connection_ = false;

    try {
        CollectCapability(out);
        CollectPlatform(out);
        CollectDriver(out);
        CollectCompatibility(out);
    } catch (const std::exception& error) {
        AddReason(out, VmxFailureReason::InternalError, Status::InternalError,
                  std::string("Unexpected exception during readiness evaluation: ") + error.what());
    } catch (...) {
        AddReason(out, VmxFailureReason::InternalError, Status::InternalError,
                  "Unknown exception during readiness evaluation.");
    }
    AddKernelConditions(out);
    return Finalize(out);
}

void VmxReadinessManager::ReleaseDriver() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (owns_connection_) {
        driver_.Disconnect();
        owns_connection_ = false;
    }
}

void VmxReadinessManager::CollectCapability(VmxReadinessReport& out) {
    const Result result = capability_.Query(out.capability);
    out.capability_collected = result.Succeeded();
    for (const auto& reason : out.capability.reasons) {
        AddReason(out, reason.reason, reason.status, reason.message, reason.component);
    }
    const VmxCapabilityReport& capability = out.capability;

    if (!result.Succeeded()) {
        if (out.capability.reasons.empty()) {
            AddReason(out, VmxFailureReason::CpuProbeFailed, result.status, result.message);
        }
        AddCondition(out, "vmx.cpuid", "CPUID VMX capability", CheckStatus::NotChecked,
                     "NOT_CHECKED: " + result.message);
        AddCondition(out, "vmx.processors", "Per-processor VMX diagnostics", CheckStatus::NotChecked,
                     "NOT_CHECKED: no logical processor was probed.");
        return;
    }

    CheckStatus cpuid_status = CheckStatus::NotChecked;
    switch (capability.verdict) {
    case VmxCapabilityVerdict::CpuidReportedOnAllProcessors: cpuid_status = CheckStatus::Pass; break;
    case VmxCapabilityVerdict::NotSupported: cpuid_status = CheckStatus::Fail; break;
    case VmxCapabilityVerdict::Inconclusive: cpuid_status = CheckStatus::NotChecked; break;
    }
    AddCondition(out, "vmx.cpuid", "CPUID VMX capability", cpuid_status, capability.summary);

    const bool all_checked = capability.processors_expected != 0 &&
                             capability.processors_checked == capability.processors_expected;
    AddCondition(out, "vmx.processors", "Per-processor VMX diagnostics",
                 all_checked ? CheckStatus::Pass : CheckStatus::NotChecked,
                 std::to_string(capability.processors_checked) + " of " +
                     std::to_string(capability.processors_expected) +
                     " logical processors were checked individually" +
                     (all_checked ? "." : "; the remainder are NOT_CHECKED."));

    if (capability.cpu_collected) {
        const PreflightReport& preflight = capability.cpu.preflight;
        if (preflight.hypervisor_present_bit) {
            const std::string vendor = preflight.hypervisor_vendor_string.empty()
                                           ? std::string("vendor unavailable")
                                           : preflight.hypervisor_vendor_string;
            AddCondition(out, "vmx.hypervisor_present", "Existing hypervisor (CPUID)",
                         CheckStatus::Warn,
                         "CPUID reports a hypervisor environment (" + vendor +
                             "). This is a review signal, not proof of a conflict.");
        } else {
            AddCondition(out, "vmx.hypervisor_present", "Existing hypervisor (CPUID)",
                         CheckStatus::Info,
                         "The CPUID hypervisor-present bit is clear on the calling processor.");
        }
    }
}

void VmxReadinessManager::CollectPlatform(VmxReadinessReport& out) {
    if (backend_.GetState() != WindowsBackendState::Ready) {
        const Result init = backend_.Initialize();
        if (!init.Succeeded()) {
            out.backend_state = backend_.GetState();
            const bool unsupported = init.status == Status::Unsupported;
            AddReason(out,
                      unsupported ? VmxFailureReason::UnsupportedPlatform
                                  : VmxFailureReason::PlatformBackendFailed,
                      init.status, "Windows Platform Backend: " + init.message);
            AddCondition(out, "vmx.platform", "Windows Platform Backend", CheckStatus::Fail,
                         init.message);
            return;
        }
    }
    out.backend_state = backend_.GetState();
    const Result snapshot = backend_.GetSnapshot(out.platform);
    if (!snapshot.Succeeded()) {
        AddReason(out, VmxFailureReason::PlatformBackendFailed, snapshot.status,
                  "Windows Platform Backend snapshot: " + snapshot.message);
        AddCondition(out, "vmx.platform", "Windows Platform Backend", CheckStatus::Fail,
                     snapshot.message);
        return;
    }
    out.platform_collected = true;
    AddCondition(out, "vmx.platform", "Windows Platform Backend", CheckStatus::Pass,
                 FormatWindowsVersion(out.platform.system) + ", " +
                     ToString(out.platform.system.architecture) + ", " +
                     std::to_string(out.platform.system.logical_processor_count) +
                     " logical processors.");
}

void VmxReadinessManager::CollectDriver(VmxReadinessReport& out) {
    VmxDriverStatus& driver = out.driver;
    driver.expected_protocol_version = GERADUO_DRIVER_PROTOCOL_VERSION;
    const bool required = options_.require_driver;
    const CheckStatus problem = required ? CheckStatus::Fail : CheckStatus::Warn;
    const auto blocker = [&](VmxFailureReason reason, Status status, const std::string& message) {
        if (required) AddReason(out, reason, status, message, "KernelDriverInterface");
    };

    if (!driver_.IsConnected()) {
        driver.connect_attempted = true;
        const Result connect = driver_.Connect();
        if (!connect.Succeeded()) {
            driver_.Disconnect();  // clean up a possible partial connection
            driver.connected = false;
            driver.status = problem;
            driver.error_code = connect.status;
            driver.message = connect.message;
            blocker(ReasonForConnectStatus(connect.status), connect.status,
                    "Diagnostic driver connection failed: " + connect.message);
            AddCondition(out, "vmx.driver.connection", "Diagnostic driver connection", problem,
                         connect.message);
            AddCondition(out, "vmx.driver.protocol", "Driver protocol compatibility",
                         CheckStatus::NotChecked,
                         "NOT_CHECKED: the driver connection could not be established.");
            AddCondition(out, "vmx.driver.platform_info", "Driver platform information",
                         CheckStatus::NotChecked, "NOT_CHECKED: the driver is not connected.");
            AddCondition(out, "vmx.driver.consistency", "Driver / user-mode consistency",
                         CheckStatus::NotChecked, "NOT_CHECKED: the driver is not connected.");
            return;
        }
        owns_connection_ = true;
    }
    driver.connected = true;
    driver.connected_by_readiness = owns_connection_;
    AddCondition(out, "vmx.driver.connection", "Diagnostic driver connection", CheckStatus::Pass,
                 "The read-only diagnostic device is connected.");

    DriverPlatformInfo info{};
    const Result query = driver_.QueryPlatformInfo(info);
    if (!query.Succeeded()) {
        driver.status = problem;
        driver.error_code = query.status;
        driver.message = query.message;
        if (query.status == Status::ProtocolMismatch) {
            blocker(VmxFailureReason::DriverProtocolMismatch, query.status,
                    "Driver protocol mismatch: " + query.message);
            AddCondition(out, "vmx.driver.protocol", "Driver protocol compatibility", problem,
                         query.message);
        } else {
            blocker(VmxFailureReason::DriverQueryFailed, query.status,
                    "Diagnostic query failed: " + query.message);
            AddCondition(out, "vmx.driver.protocol", "Driver protocol compatibility",
                         CheckStatus::NotChecked,
                         "NOT_CHECKED: the diagnostic query failed before the response could be "
                         "validated.");
        }
        AddCondition(out, "vmx.driver.platform_info", "Driver platform information", problem,
                     query.message);
        AddCondition(out, "vmx.driver.consistency", "Driver / user-mode consistency",
                     CheckStatus::NotChecked, "NOT_CHECKED: no driver platform data is available.");
        return;
    }

    driver.platform_info = info;
    driver.platform_info_read = true;
    driver.protocol_verified = true;
    AddCondition(out, "vmx.driver.protocol", "Driver protocol compatibility", CheckStatus::Pass,
                 "Protocol version " + std::to_string(driver.expected_protocol_version) +
                     " accepted.");
    AddCondition(out, "vmx.driver.platform_info", "Driver platform information", CheckStatus::Pass,
                 "The driver returned a validated platform information response.");

    if (!out.platform_collected) {
        driver.status = CheckStatus::Pass;
        AddCondition(out, "vmx.driver.consistency", "Driver / user-mode consistency",
                     CheckStatus::NotChecked,
                     "NOT_CHECKED: no user-mode platform snapshot to compare with.");
        return;
    }

    const WindowsSystemInformation& system = out.platform.system;
    std::string failures;
    std::string warnings;
    if (info.architecture != ArchitectureCode(system.architecture)) {
        failures += "architecture differs (driver " + std::to_string(info.architecture) +
                    ", user mode " + ToString(system.architecture) + "); ";
    }
    if (info.processor_count != system.logical_processor_count) {
        failures += "logical processor count differs (driver " +
                    std::to_string(info.processor_count) + ", user mode " +
                    std::to_string(system.logical_processor_count) + "); ";
    }
    if (info.page_size != system.page_size) {
        failures += "page size differs (driver " + std::to_string(info.page_size) +
                    ", user mode " + std::to_string(system.page_size) + "); ";
    }
    if (info.os_major_version != system.major_version ||
        info.os_minor_version != system.minor_version ||
        info.os_build_number != system.build_number) {
        warnings += "Windows version reports differ; ";
    }
    const VmxCapabilityReport& capability = out.capability;
    if (out.capability_collected &&
        capability.verdict != VmxCapabilityVerdict::Inconclusive) {
        const bool user_vmx = capability.verdict == VmxCapabilityVerdict::CpuidReportedOnAllProcessors;
        if (info.vmx_reported != user_vmx) {
            warnings += "driver VMX flag differs from the per-processor CPUID result; ";
        }
    }
    if (capability.processors_expected != 0 &&
        capability.processors_expected != system.logical_processor_count) {
        warnings += "enumerated processor count (" + std::to_string(capability.processors_expected) +
                    ") differs from the platform snapshot; ";
    }

    if (!failures.empty()) {
        driver.status = problem;
        driver.message = failures;
        blocker(VmxFailureReason::DriverDataInconsistent, Status::InvalidResponse,
                "Driver data is inconsistent with the user-mode snapshot: " + failures);
        AddCondition(out, "vmx.driver.consistency", "Driver / user-mode consistency", problem,
                     failures + warnings);
    } else if (!warnings.empty()) {
        driver.status = CheckStatus::Warn;
        driver.message = warnings;
        AddCondition(out, "vmx.driver.consistency", "Driver / user-mode consistency",
                     CheckStatus::Warn, warnings);
    } else {
        driver.status = CheckStatus::Pass;
        AddCondition(out, "vmx.driver.consistency", "Driver / user-mode consistency",
                     CheckStatus::Pass,
                     "Architecture, processor count, page size and OS version match.");
    }
}

void VmxReadinessManager::CollectCompatibility(VmxReadinessReport& out) {
    if (!out.platform_collected) {
        AddCondition(out, "vmx.compatibility", "Platform compatibility", CheckStatus::NotChecked,
                     "NOT_CHECKED: the Windows Platform Backend did not produce a snapshot.");
        return;
    }
    const Result result = compatibility_.Evaluate(out.compatibility);
    out.compatibility_collected = true;

    if (!result.Succeeded()) {
        AddReason(out, VmxFailureReason::PlatformCompatibilityFailed, result.status,
                  "Platform compatibility evaluation failed: " + result.message);
        AddCondition(out, "vmx.compatibility", "Platform compatibility", CheckStatus::Fail,
                     result.message);
        return;
    }

    const PlatformCompatibilityReport& report = out.compatibility;
    switch (report.verdict) {
    case CompatibilityVerdict::Unsupported: {
        std::string failed;
        for (const auto& check : report.checks) {
            if (check.status == CompatibilityCheckStatus::Fail) {
                if (!failed.empty()) failed += ", ";
                failed += check.id;
            }
        }
        AddReason(out, VmxFailureReason::PlatformUnsupported, Status::Unsupported,
                  "Platform compatibility verdict is Unsupported: " + report.summary +
                      (failed.empty() ? std::string() : " [failed checks: " + failed + "]"),
                  "PlatformCompatibilityManager");
        AddCondition(out, "vmx.compatibility", "Platform compatibility", CheckStatus::Fail,
                     report.summary);
        break;
    }
    case CompatibilityVerdict::ReviewRequired:
        AddCondition(out, "vmx.compatibility", "Platform compatibility", CheckStatus::Warn,
                     report.summary);
        break;
    case CompatibilityVerdict::Inconclusive:
        AddCondition(out, "vmx.compatibility", "Platform compatibility", CheckStatus::NotChecked,
                     report.summary);
        break;
    case CompatibilityVerdict::CompatibleForDiagnostics:
        AddCondition(out, "vmx.compatibility", "Platform compatibility", CheckStatus::Pass,
                     report.summary);
        break;
    }
    for (const auto& check : report.checks) {
        AddCondition(out, "compat." + check.id, check.title, ToCheckStatus(check.status),
                     check.detail);
    }
}

void VmxReadinessManager::AddKernelConditions(VmxReadinessReport& out) {
    AddCondition(out, "vmx.kernel.feature_control", "IA32_FEATURE_CONTROL (MSR 0x3A)",
                 CheckStatus::NotChecked,
                 "NOT_CHECKED: requires kernel-mode RDMSR, which the v0.5.0 driver does not "
                 "perform. Firmware may have locked VMX off.");
    AddCondition(out, "vmx.kernel.fixed_bits", "VMX capability MSRs and CR0/CR4 fixed bits",
                 CheckStatus::NotChecked, "NOT_CHECKED: requires kernel-mode MSR/CR access.");
    AddCondition(out, "vmx.kernel.vmxon", "VMXON permission", CheckStatus::NotChecked,
                 "NOT_CHECKED: VMXON is never executed in v0.5.0.");
}

Result VmxReadinessManager::Finalize(VmxReadinessReport& out) {
    VmxReadinessVerdict verdict = VmxReadinessVerdict::ReadyForReview;
    for (const auto& reason : out.reasons) {
        const VmxReadinessVerdict candidate = VerdictFor(reason.reason);
        if (Rank(candidate) > Rank(verdict)) verdict = candidate;
    }
    out.verdict = verdict;
    out.kernel_verification_required = true;

    const VmxFailureDetail* first = nullptr;
    for (const auto& reason : out.reasons) {
        if (VerdictFor(reason.reason) == verdict) {
            first = &reason;
            break;
        }
    }

    switch (verdict) {
    case VmxReadinessVerdict::ReadyForReview:
        out.summary =
            "Every condition that can be checked from user mode and the read-only driver is "
            "consistent with Intel VT-x. Kernel-only checks are NOT_CHECKED and no hypervisor was "
            "started.";
        break;
    case VmxReadinessVerdict::NotSupported:
        out.summary = "Unsupported configuration: " + (first ? first->message : std::string());
        break;
    case VmxReadinessVerdict::Blocked:
        out.summary = "Blocked: " + (first ? first->message : std::string());
        break;
    case VmxReadinessVerdict::Failed:
        out.summary = "Evaluation failed: " + (first ? first->message : std::string());
        break;
    }

    Status failure_status = Status::InternalError;
    if (verdict == VmxReadinessVerdict::Failed && first != nullptr &&
        first->status != Status::Success) {
        failure_status = first->status;
    }

    // Resource cleanup: a retained driver connection exists only for ReadyForReview.
    if (verdict != VmxReadinessVerdict::ReadyForReview && owns_connection_) {
        driver_.Disconnect();
        owns_connection_ = false;
        out.driver.connected = false;
        out.driver.message += out.driver.message.empty() ? "" : " ";
        out.driver.message += "(connection closed: readiness verdict is not ReadyForReview)";
    }

    if (verdict == VmxReadinessVerdict::Failed) return Result::Error(failure_status, out.summary);
    return Result::Ok();
}

} // namespace hv
