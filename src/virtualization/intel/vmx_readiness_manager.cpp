#include "hv/virtualization/vmx_readiness_manager.hpp"

#include "hv/detail/report_json.hpp"

#include <cstdint>
#include <sstream>
#include <string>
#include <utility>

namespace hv {
namespace {

void AddCheck(VmxReadinessReport& report, std::string id, std::string title,
              CheckStatus status, std::string detail) {
    report.checks.push_back({std::move(id), std::move(title), status, std::move(detail)});
}

CheckStatus PlatformCheckStatus(PlatformSupport support) {
    switch (support) {
    case PlatformSupport::Supported: return CheckStatus::Pass;
    case PlatformSupport::SupportedWithWarnings: return CheckStatus::Warn;
    case PlatformSupport::Unsupported: return CheckStatus::Fail;
    }
    return CheckStatus::NotChecked;
}

CheckStatus CompatibilityCheckStatus(CompatibilityVerdict verdict) {
    switch (verdict) {
    case CompatibilityVerdict::NoConflictDetected: return CheckStatus::Pass;
    case CompatibilityVerdict::KnownConflict: return CheckStatus::Warn;
    case CompatibilityVerdict::ReviewRequired: return CheckStatus::Warn;
    case CompatibilityVerdict::UnsupportedPlatform: return CheckStatus::Fail;
    case CompatibilityVerdict::Unknown: return CheckStatus::NotChecked;
    }
    return CheckStatus::NotChecked;
}

std::string DriverDetail(const DriverPlatformInfoResponse& response) {
    return "driver " + std::to_string(response.driver_version_major) + "." +
           std::to_string(response.driver_version_minor) + "." +
           std::to_string(response.driver_version_patch) + ", protocol " +
           std::to_string(response.protocol_version) + ", response " +
           std::to_string(response.structure_size) + " bytes, architecture " +
           ToString(static_cast<DriverArchitecture>(response.architecture)) + ".";
}

} // namespace

const char* ToString(VmxReadinessState value) noexcept {
    switch (value) {
    case VmxReadinessState::NotEvaluated: return "NotEvaluated";
    case VmxReadinessState::NotSupported: return "NotSupported";
    case VmxReadinessState::Blocked: return "Blocked";
    case VmxReadinessState::ReadyForReview: return "ReadyForReview";
    case VmxReadinessState::Failed: return "Failed";
    }
    return "NotEvaluated";
}

Result VmxReadinessManager::Evaluate(const VmxCapabilityReport& capability,
                                     VmxReadinessReport& out) {
    std::lock_guard lock(mutex_);
    return EvaluateLocked(capability, out, options_);
}

Result VmxReadinessManager::Evaluate(const VmxCapabilityReport& capability,
                                     VmxReadinessReport& out,
                                     const VmxReadinessOptions& options) {
    std::lock_guard lock(mutex_);
    return EvaluateLocked(capability, out, options);
}

void VmxReadinessManager::SetOptions(const VmxReadinessOptions& options) {
    std::lock_guard lock(mutex_);
    options_ = options;
}

VmxReadinessOptions VmxReadinessManager::GetOptions() const {
    std::lock_guard lock(mutex_);
    return options_;
}

void VmxReadinessManager::ReleaseConnections() {
    std::lock_guard lock(mutex_);
    driver_.Disconnect();
}

std::string VmxReadinessManager::GetLastError() const {
    std::lock_guard lock(mutex_);
    return last_error_;
}

Result VmxReadinessManager::EvaluateLocked(const VmxCapabilityReport& capability,
                                           VmxReadinessReport& out,
                                           const VmxReadinessOptions& options) {
    out = VmxReadinessReport{};
    out.vmx_support = capability.support;
    out.kernel_verification_required = true;
    out.hardware_vmx_started = false;
    out.hardware_start_supported = false;
    out.driver.expected_protocol_version = kGeraduoProtocolVersion;
    out.driver.expected_response_size =
        static_cast<std::uint32_t>(sizeof(DriverPlatformInfoResponse));

    bool failed = false;
    VmxBlockReason failed_reason = VmxBlockReason::InternalError;
    VmxErrorClass failed_class = VmxErrorClass::Internal;
    Status failed_status = Status::InternalError;
    std::string failed_detail;

    bool unsupported = false;
    VmxBlockReason unsupported_reason = VmxBlockReason::None;
    std::string unsupported_detail;

    bool blocked = false;
    VmxBlockReason blocked_reason = VmxBlockReason::None;
    VmxErrorClass blocked_class = VmxErrorClass::None;
    std::string blocked_detail;

    const auto mark_failed = [&](VmxBlockReason reason, VmxErrorClass error_class,
                                 Status status, std::string detail) {
        if (failed) return;
        failed = true;
        failed_reason = reason;
        failed_class = error_class;
        failed_status = status;
        failed_detail = std::move(detail);
    };
    const auto mark_unsupported = [&](VmxBlockReason reason, std::string detail) {
        if (unsupported) return;
        unsupported = true;
        unsupported_reason = reason;
        unsupported_detail = std::move(detail);
    };
    const auto mark_blocked = [&](VmxBlockReason reason, std::string detail,
                                  VmxErrorClass error_class = VmxErrorClass::None) {
        if (blocked) return;
        blocked = true;
        blocked_reason = reason;
        blocked_class = error_class;
        blocked_detail = std::move(detail);
    };

    // 1. VT-x capability, produced by VmxCapabilityManager.
    AddCheck(out, "readiness.capability", "Intel VT-x capability", capability.status,
             capability.summary);
    if (capability.error_class != VmxErrorClass::None ||
        capability.reason == VmxBlockReason::CpuidProbeFailed ||
        capability.reason == VmxBlockReason::CpuidReportInvalid) {
        mark_failed(capability.reason == VmxBlockReason::None
                        ? VmxBlockReason::CpuidProbeFailed
                        : capability.reason,
                    VmxErrorClass::CpuProbe, Status::InvalidArgument,
                    capability.summary.empty() ? "The CPU capability report is unusable."
                                               : capability.summary);
    } else {
        switch (capability.support) {
        case VmxSupportLevel::NotSupported:
            mark_unsupported(capability.reason == VmxBlockReason::None
                                 ? VmxBlockReason::UnsupportedCpuVendor
                                 : capability.reason,
                             capability.summary);
            break;
        case VmxSupportLevel::ReviewRequired:
            mark_blocked(VmxBlockReason::HypervisorPresentReviewRequired, capability.summary);
            break;
        case VmxSupportLevel::NotChecked:
            mark_blocked(capability.reason == VmxBlockReason::None
                             ? VmxBlockReason::PerProcessorCheckIncomplete
                             : capability.reason,
                         capability.summary);
            break;
        case VmxSupportLevel::Supported:
            break;
        }
    }

    // 2. Windows platform backend.
    std::uint32_t platform_processor_count = 0;
    bool platform_processor_count_known = false;
    if (!options.query_platform) {
        out.platform_state = platform_.GetState();
        AddCheck(out, "readiness.platform", "Windows platform backend", CheckStatus::NotChecked,
                 "Platform probing was disabled by the caller; the Windows state was not "
                 "verified.");
        mark_blocked(VmxBlockReason::PlatformNotChecked,
                     "Windows platform probing was disabled; readiness cannot be established.");
    } else {
        const Result platform_result =
            platform_.IsInitialized() ? platform_.Refresh() : platform_.Initialize();
        out.platform_state = platform_.GetState();
        if (!platform_result.Succeeded()) {
            const std::string detail =
                platform_result.message.empty() ? platform_.GetLastError()
                                                : platform_result.message;
            AddCheck(out, "readiness.platform", "Windows platform backend", CheckStatus::Fail,
                     detail);
            mark_failed(VmxBlockReason::PlatformBackendFailed, VmxErrorClass::PlatformProbe,
                        platform_result.status, detail);
        } else {
            SystemInfo system;
            PlatformCapabilityReport platform_report;
            const Result system_result = platform_.GetSystemInfo(system);
            const Result capability_result = platform_.GetCapabilities(platform_report);
            if (!system_result.Succeeded() || !capability_result.Succeeded()) {
                const std::string detail =
                    !system_result.Succeeded() ? system_result.message
                                               : capability_result.message;
                AddCheck(out, "readiness.platform", "Windows platform backend", CheckStatus::Fail,
                         detail);
                mark_failed(VmxBlockReason::PlatformBackendFailed, VmxErrorClass::PlatformProbe,
                            Status::InitializationFailed, detail);
            } else {
                out.platform_support = platform_report.support;
                platform_processor_count = system.logical_processor_count;
                platform_processor_count_known = platform_processor_count > 0;
                AddCheck(out, "readiness.platform", "Windows platform backend",
                         PlatformCheckStatus(platform_report.support), platform_report.summary);
                if (platform_report.support == PlatformSupport::Unsupported) {
                    mark_unsupported(VmxBlockReason::PlatformUnsupported,
                                     platform_report.summary);
                }
            }
        }
    }

    // 3. Windows compatibility policy (Hyper-V, VBS, HVCI, WHPX).
    if (!options.query_compatibility) {
        AddCheck(out, "readiness.compatibility", "Windows compatibility policy",
                 CheckStatus::NotChecked,
                 "Compatibility probing was disabled by the caller; the verdict is unknown.");
        mark_blocked(VmxBlockReason::CompatibilityNotChecked,
                     "Windows compatibility was not evaluated; readiness cannot be established.");
    } else {
        CompatibilityReport compatibility;
        const Result compatibility_result = compatibility_.Evaluate(compatibility);
        out.compatibility_verdict = compatibility.verdict;
        if (!compatibility_result.Succeeded()) {
            const std::string detail =
                compatibility_result.message.empty() ? compatibility_.GetLastError()
                                                     : compatibility_result.message;
            AddCheck(out, "readiness.compatibility", "Windows compatibility policy",
                     CheckStatus::Fail, detail);
            mark_failed(VmxBlockReason::CompatibilityEvaluationFailed,
                        VmxErrorClass::PlatformProbe, compatibility_result.status, detail);
        } else {
            AddCheck(out, "readiness.compatibility", "Windows compatibility policy",
                     CompatibilityCheckStatus(compatibility.verdict), compatibility.summary);
            switch (compatibility.verdict) {
            case CompatibilityVerdict::NoConflictDetected:
                break;
            case CompatibilityVerdict::UnsupportedPlatform:
                mark_unsupported(VmxBlockReason::CompatibilityUnsupported,
                                 compatibility.summary);
                break;
            case CompatibilityVerdict::KnownConflict:
                mark_blocked(VmxBlockReason::HypervisorConflict, compatibility.summary);
                break;
            case CompatibilityVerdict::ReviewRequired:
            case CompatibilityVerdict::Unknown:
                mark_blocked(VmxBlockReason::CompatibilityReviewRequired,
                             compatibility.summary.empty()
                                 ? "The compatibility verdict requires review."
                                 : compatibility.summary);
                break;
            }
        }
    }

    // 4. Diagnostic driver and protocol compatibility.
    if (!options.query_driver) {
        out.driver.status = CheckStatus::NotChecked;
        out.driver.detail =
            "Driver probing was disabled by the caller; protocol compatibility is NOT_CHECKED.";
        AddCheck(out, "readiness.driver", "Geraduo diagnostic driver", CheckStatus::NotChecked,
                 out.driver.detail);
        mark_blocked(VmxBlockReason::DriverNotChecked,
                     "The diagnostic driver was not queried; protocol compatibility is unknown.");
    } else {
        out.driver.query_attempted = true;
        const Result connect_result = driver_.Connect();
        if (!connect_result.Succeeded()) {
            out.driver.detail = connect_result.message.empty() ? driver_.GetLastError()
                                                               : connect_result.message;
            if (connect_result.status == Status::NotFound ||
                connect_result.status == Status::Unsupported) {
                out.driver.status = CheckStatus::NotChecked;
                AddCheck(out, "readiness.driver", "Geraduo diagnostic driver",
                         CheckStatus::Warn, out.driver.detail);
                mark_blocked(VmxBlockReason::DriverNotConnected, out.driver.detail);
            } else if (connect_result.status == Status::AccessDenied) {
                out.driver.status = CheckStatus::NotChecked;
                AddCheck(out, "readiness.driver", "Geraduo diagnostic driver",
                         CheckStatus::Warn, out.driver.detail);
                mark_blocked(VmxBlockReason::DriverAccessDenied, out.driver.detail);
            } else {
                out.driver.status = CheckStatus::Fail;
                AddCheck(out, "readiness.driver", "Geraduo diagnostic driver", CheckStatus::Fail,
                         out.driver.detail);
                mark_failed(VmxBlockReason::DriverQueryFailed, VmxErrorClass::Driver,
                            connect_result.status, out.driver.detail);
            }
        } else {
            out.driver.connected = true;
            DriverPlatformInfoResponse response{};
            const Result query_result = driver_.QueryPlatformInfo(response);
            if (!query_result.Succeeded()) {
                out.driver.detail = query_result.message.empty() ? driver_.GetLastError()
                                                                 : query_result.message;
                if (query_result.status == Status::Unsupported) {
                    out.driver.status = CheckStatus::Fail;
                    out.driver.protocol_version_checked = true;
                    AddCheck(out, "readiness.driver", "Geraduo diagnostic driver",
                             CheckStatus::Fail, out.driver.detail);
                    mark_blocked(VmxBlockReason::DriverProtocolMismatch,
                                 out.driver.detail, VmxErrorClass::Protocol);
                } else if (query_result.status == Status::InvalidArgument) {
                    out.driver.status = CheckStatus::Fail;
                    AddCheck(out, "readiness.driver", "Geraduo diagnostic driver",
                             CheckStatus::Fail, out.driver.detail);
                    mark_blocked(VmxBlockReason::DriverResponseInvalid, out.driver.detail,
                                 VmxErrorClass::Protocol);
                } else if (query_result.status == Status::NotFound) {
                    out.driver.status = CheckStatus::NotChecked;
                    AddCheck(out, "readiness.driver", "Geraduo diagnostic driver",
                             CheckStatus::Warn, out.driver.detail);
                    mark_blocked(VmxBlockReason::DriverNotConnected, out.driver.detail);
                } else if (query_result.status == Status::AccessDenied) {
                    out.driver.status = CheckStatus::NotChecked;
                    AddCheck(out, "readiness.driver", "Geraduo diagnostic driver",
                             CheckStatus::Warn, out.driver.detail);
                    mark_blocked(VmxBlockReason::DriverAccessDenied, out.driver.detail);
                } else {
                    out.driver.status = CheckStatus::Fail;
                    AddCheck(out, "readiness.driver", "Geraduo diagnostic driver",
                             CheckStatus::Fail, out.driver.detail);
                    mark_failed(VmxBlockReason::DriverQueryFailed, VmxErrorClass::Driver,
                                query_result.status, out.driver.detail);
                }
            } else {
                out.driver.query_succeeded = true;
                out.driver.detail = DriverDetail(response);
                out.driver.protocol_version_checked = true;
                out.driver.reported_protocol_version = response.protocol_version;
                out.driver.protocol_version_compatible =
                    response.protocol_version == kGeraduoProtocolVersion;
                out.driver.structure_layout_checked = true;
                out.driver.reported_structure_size = response.structure_size;
                out.driver.structure_layout_compatible =
                    response.structure_size == sizeof(DriverPlatformInfoResponse);
                out.driver.driver_version_checked = true;
                out.driver.driver_version_major = response.driver_version_major;
                out.driver.driver_version_minor = response.driver_version_minor;
                out.driver.driver_version_patch = response.driver_version_patch;
                out.driver.driver_version_compatible = IsDriverVersionCompatible(
                    response.driver_version_major, response.driver_version_minor);
                out.driver.architecture_checked = true;
                out.driver.driver_architecture =
                    static_cast<DriverArchitecture>(response.architecture);
                out.driver.architecture_compatible =
                    response.architecture == static_cast<std::uint32_t>(DriverArchitecture::X64);
                out.driver.processor_count_checked = platform_processor_count_known;
                out.driver.driver_processor_count = response.active_processor_count;
                out.driver.platform_processor_count = platform_processor_count;
                out.driver.processor_count_matches =
                    platform_processor_count_known &&
                    response.active_processor_count == platform_processor_count;

                if (response.magic != kGeraduoResponseMagic) {
                    out.driver.status = CheckStatus::Fail;
                    AddCheck(out, "readiness.driver", "Geraduo diagnostic driver",
                             CheckStatus::Fail,
                             "The driver response magic does not match the Geraduo protocol.");
                    mark_failed(VmxBlockReason::DriverResponseInvalid, VmxErrorClass::Protocol,
                                Status::InternalError,
                                "The driver response magic does not match the Geraduo protocol.");
                } else if (!out.driver.protocol_version_compatible) {
                    out.driver.status = CheckStatus::Fail;
                    const std::string detail =
                        "Driver protocol version " +
                        std::to_string(response.protocol_version) +
                        " does not match the expected version " +
                        std::to_string(kGeraduoProtocolVersion) + ".";
                    AddCheck(out, "readiness.driver", "Geraduo diagnostic driver",
                             CheckStatus::Fail, detail);
                    mark_blocked(VmxBlockReason::DriverProtocolMismatch, detail,
                                 VmxErrorClass::Protocol);
                } else if (!out.driver.structure_layout_compatible) {
                    out.driver.status = CheckStatus::Fail;
                    const std::string detail =
                        "Driver response structure size " +
                        std::to_string(response.structure_size) + " does not match " +
                        std::to_string(sizeof(DriverPlatformInfoResponse)) + ".";
                    AddCheck(out, "readiness.driver", "Geraduo diagnostic driver",
                             CheckStatus::Fail, detail);
                    mark_blocked(VmxBlockReason::DriverProtocolMismatch, detail,
                                 VmxErrorClass::Protocol);
                } else if (static_cast<DriverProtocolStatus>(response.status) !=
                           DriverProtocolStatus::Success) {
                    out.driver.status = CheckStatus::Fail;
                    const std::string detail =
                        std::string("The driver reported protocol status ") +
                        ToString(static_cast<DriverProtocolStatus>(response.status)) + ".";
                    AddCheck(out, "readiness.driver", "Geraduo diagnostic driver",
                             CheckStatus::Fail, detail);
                    mark_failed(VmxBlockReason::DriverQueryFailed, VmxErrorClass::Driver,
                                Status::InternalError, detail);
                } else if (response.architecture ==
                               static_cast<std::uint32_t>(DriverArchitecture::Unknown) ||
                           !IsKnownDriverArchitecture(response.architecture)) {
                    out.driver.status = CheckStatus::Fail;
                    const std::string detail =
                        "The driver reported an unknown processor architecture.";
                    AddCheck(out, "readiness.driver", "Geraduo diagnostic driver",
                             CheckStatus::Fail, detail);
                    mark_failed(VmxBlockReason::DriverResponseInvalid, VmxErrorClass::Protocol,
                                Status::InternalError, detail);
                } else if (!out.driver.architecture_compatible) {
                    out.driver.status = CheckStatus::Fail;
                    const std::string detail =
                        std::string("The driver architecture is ") + ToString(out.driver.driver_architecture) +
                        "; the foundation requires an x64 driver.";
                    AddCheck(out, "readiness.driver", "Geraduo diagnostic driver",
                             CheckStatus::Fail, detail);
                    mark_blocked(VmxBlockReason::DriverArchitectureMismatch, detail,
                                 VmxErrorClass::Protocol);
                } else if (!out.driver.driver_version_compatible) {
                    out.driver.status = CheckStatus::Fail;
                    const std::string detail =
                        "Driver version " + std::to_string(response.driver_version_major) + "." +
                        std::to_string(response.driver_version_minor) +
                        " is older than the minimum compatible version " +
                        std::to_string(kGeraduoMinimumCompatibleDriverMajor) + "." +
                        std::to_string(kGeraduoMinimumCompatibleDriverMinor) + ".";
                    AddCheck(out, "readiness.driver", "Geraduo diagnostic driver",
                             CheckStatus::Fail, detail);
                    mark_blocked(VmxBlockReason::DriverVersionIncompatible, detail,
                                 VmxErrorClass::Protocol);
                } else if (response.reserved != 0 || response.active_processor_count == 0) {
                    out.driver.status = CheckStatus::Fail;
                    const std::string detail =
                        "The driver response has an invalid reserved field or reports zero "
                        "active processors.";
                    AddCheck(out, "readiness.driver", "Geraduo diagnostic driver",
                             CheckStatus::Fail, detail);
                    mark_failed(VmxBlockReason::DriverResponseInvalid, VmxErrorClass::Protocol,
                                Status::InternalError, detail);
                } else if (out.driver.processor_count_checked &&
                           !out.driver.processor_count_matches) {
                    out.driver.status = CheckStatus::Warn;
                    AddCheck(out, "readiness.driver", "Geraduo diagnostic driver",
                             CheckStatus::Warn,
                             "The driver reports " +
                                 std::to_string(response.active_processor_count) +
                                 " active processors, while the platform reports " +
                                 std::to_string(platform_processor_count) +
                                 ". This can be a dynamic processor change; it does not block "
                                 "review but must be rechecked before kernel work.");
                } else {
                    out.driver.status = CheckStatus::Pass;
                    out.driver.detail = DriverDetail(response);
                    AddCheck(out, "readiness.driver", "Geraduo diagnostic driver",
                             CheckStatus::Pass, out.driver.detail);
                }
            }
        }
    }

    // 5. Kernel-stage and boundary checks. These stay NOT_CHECKED by design;
    // they do not block ReadyForReview because that verdict only authorizes a
    // review, never a VMXON.
    AddCheck(out, "readiness.kernel.feature_control",
             "IA32_FEATURE_CONTROL lock and VMXON enable bits", CheckStatus::NotChecked,
             "Requires RDMSR/WRMSR in kernel mode; not verified by this user-mode layer.");
    AddCheck(out, "readiness.kernel.vmx_capability",
             "VMX capability MSRs and CR0/CR4 fixed bits", CheckStatus::NotChecked,
             "Requires privileged MSR and control-register access; not verified.");
    AddCheck(out, "readiness.kernel.ept_vpid", "EPT/VPID capability MSRs", CheckStatus::NotChecked,
             "Requires RDMSR; hardware EPT is out of scope for v0.5.0.");
    AddCheck(out, "readiness.kernel.vmxon", "VMXON, VMCS, VM-entry and VM-exit",
             CheckStatus::NotChecked,
             "Not implemented in v0.5.0. No VMXON, VMCS, guest entry or exit was attempted.");
    AddCheck(out, "readiness.boundary", "Hardware virtualization boundary", CheckStatus::Info,
             "The virtualization foundation is read-only. It never accesses physical memory, "
             "another process, an MSR or a VMX instruction.");

    if (failed) {
        out.state = VmxReadinessState::Failed;
        out.status = CheckStatus::Fail;
        out.reason = failed_reason;
        out.error_class = failed_class;
        out.summary = failed_detail.empty() ? "The readiness evaluation failed." : failed_detail;
        last_error_ = out.summary;
        return Result::Error(failed_status, last_error_);
    }

    if (unsupported) {
        out.state = VmxReadinessState::NotSupported;
        out.status = CheckStatus::Fail;
        out.reason = unsupported_reason;
        out.error_class = VmxErrorClass::None;
        out.summary = unsupported_detail.empty()
                          ? "This configuration is not supported by the implemented backend."
                          : unsupported_detail;
        last_error_.clear();
        return Result::Ok();
    }

    if (blocked) {
        out.state = VmxReadinessState::Blocked;
        out.status = CheckStatus::Warn;
        out.reason = blocked_reason;
        out.error_class = blocked_class;
        out.summary = blocked_detail.empty() ? "A required component is not ready."
                                             : blocked_detail;
        last_error_.clear();
        return Result::Ok();
    }

    out.state = VmxReadinessState::ReadyForReview;
    out.status = CheckStatus::Pass;
    out.reason = VmxBlockReason::None;
    out.error_class = VmxErrorClass::None;
    out.summary =
        "All user-mode prerequisites are satisfied. This means ready for kernel-stage review "
        "only, not ready for hardware VMXON. IA32_FEATURE_CONTROL, VMX capability MSRs, CR0/CR4 "
        "fixed bits, VMCS setup and EPT are still unverified, and no VMX instruction was "
        "executed.";
    last_error_.clear();
    return Result::Ok();
}

std::string FormatVmxReadinessReportText(const VmxReadinessReport& report) {
    std::ostringstream out;
    out << "Geraduo VT-x readiness report\n";
    out << "State: " << ToString(report.state) << '\n';
    out << "Status: " << ToString(report.status) << '\n';
    out << "Reason: " << ToString(report.reason) << '\n';
    out << "Error class: " << ToString(report.error_class) << '\n';
    out << "Summary: " << report.summary << '\n';
    out << "VMX support: " << ToString(report.vmx_support) << '\n';
    out << "Platform: state=" << ToString(report.platform_state)
        << ", support=" << ToString(report.platform_support) << '\n';
    out << "Compatibility verdict: " << ToString(report.compatibility_verdict) << '\n';
    out << "Driver compatibility:\n";
    out << "  Status: " << ToString(report.driver.status) << '\n';
    out << "  Attempted: " << (report.driver.query_attempted ? "yes" : "no")
        << ", connected: " << (report.driver.connected ? "yes" : "no")
        << ", query succeeded: " << (report.driver.query_succeeded ? "yes" : "no") << '\n';
    out << "  Protocol: checked=" << (report.driver.protocol_version_checked ? "yes" : "no")
        << ", compatible=" << (report.driver.protocol_version_compatible ? "yes" : "no")
        << ", expected=" << report.driver.expected_protocol_version
        << ", reported=" << report.driver.reported_protocol_version << '\n';
    out << "  Structure: checked=" << (report.driver.structure_layout_checked ? "yes" : "no")
        << ", compatible=" << (report.driver.structure_layout_compatible ? "yes" : "no")
        << ", expected=" << report.driver.expected_response_size
        << ", reported=" << report.driver.reported_structure_size << '\n';
    out << "  Driver version: checked=" << (report.driver.driver_version_checked ? "yes" : "no")
        << ", compatible=" << (report.driver.driver_version_compatible ? "yes" : "no") << ", "
        << report.driver.driver_version_major << '.' << report.driver.driver_version_minor << '.'
        << report.driver.driver_version_patch << " (minimum "
        << kGeraduoMinimumCompatibleDriverMajor << '.'
        << kGeraduoMinimumCompatibleDriverMinor << ")\n";
    out << "  Architecture: checked=" << (report.driver.architecture_checked ? "yes" : "no")
        << ", compatible=" << (report.driver.architecture_compatible ? "yes" : "no") << ", "
        << ToString(report.driver.driver_architecture) << '\n';
    out << "  Processor count: checked=" << (report.driver.processor_count_checked ? "yes" : "no")
        << ", matches=" << (report.driver.processor_count_matches ? "yes" : "no")
        << ", driver=" << report.driver.driver_processor_count
        << ", platform=" << report.driver.platform_processor_count << '\n';
    out << "  Detail: " << report.driver.detail << '\n';
    out << "Checks:\n";
    for (const auto& check : report.checks)
        out << "  [" << ToString(check.status) << "] " << check.title << " - " << check.detail
            << '\n';
    out << "Kernel-mode verification required: "
        << (report.kernel_verification_required ? "yes" : "no") << '\n';
    out << "Hardware VMX started: " << (report.hardware_vmx_started ? "yes" : "no") << '\n';
    out << "Hardware VMX start supported: "
        << (report.hardware_start_supported ? "yes" : "no") << '\n';
    return out.str();
}

std::string FormatVmxReadinessReportJson(const VmxReadinessReport& report) {
    std::ostringstream out;
    out << "{\n";
    out << "  \"state\": \"" << ToString(report.state) << "\",\n";
    out << "  \"status\": \"" << ToString(report.status) << "\",\n";
    out << "  \"reason\": \"" << ToString(report.reason) << "\",\n";
    out << "  \"error_class\": \"" << ToString(report.error_class) << "\",\n";
    out << "  \"summary\": \"" << detail::JsonEscape(report.summary) << "\",\n";
    out << "  \"vmx_support\": \"" << ToString(report.vmx_support) << "\",\n";
    out << "  \"platform\": {\"state\": \"" << ToString(report.platform_state)
        << "\", \"support\": \"" << ToString(report.platform_support) << "\"},\n";
    out << "  \"compatibility_verdict\": \"" << ToString(report.compatibility_verdict)
        << "\",\n";
    out << "  \"driver\": {\n";
    out << "    \"status\": \"" << ToString(report.driver.status) << "\",\n";
    out << "    \"query_attempted\": " << detail::JsonBool(report.driver.query_attempted)
        << ", \"connected\": " << detail::JsonBool(report.driver.connected)
        << ", \"query_succeeded\": " << detail::JsonBool(report.driver.query_succeeded)
        << ",\n";
    out << "    \"protocol_version_checked\": "
        << detail::JsonBool(report.driver.protocol_version_checked)
        << ", \"protocol_version_compatible\": "
        << detail::JsonBool(report.driver.protocol_version_compatible)
        << ", \"expected_protocol_version\": " << report.driver.expected_protocol_version
        << ", \"reported_protocol_version\": " << report.driver.reported_protocol_version
        << ",\n";
    out << "    \"structure_layout_checked\": "
        << detail::JsonBool(report.driver.structure_layout_checked)
        << ", \"structure_layout_compatible\": "
        << detail::JsonBool(report.driver.structure_layout_compatible)
        << ", \"expected_response_size\": " << report.driver.expected_response_size
        << ", \"reported_structure_size\": " << report.driver.reported_structure_size << ",\n";
    out << "    \"driver_version_checked\": "
        << detail::JsonBool(report.driver.driver_version_checked)
        << ", \"driver_version_compatible\": "
        << detail::JsonBool(report.driver.driver_version_compatible) << ",\n";
    out << "    \"driver_version\": \"" << report.driver.driver_version_major << '.'
        << report.driver.driver_version_minor << '.' << report.driver.driver_version_patch
        << "\",\n";
    out << "    \"minimum_driver_version\": \""
        << kGeraduoMinimumCompatibleDriverMajor << '.'
        << kGeraduoMinimumCompatibleDriverMinor << "\",\n";
    out << "    \"architecture_checked\": "
        << detail::JsonBool(report.driver.architecture_checked)
        << ", \"architecture_compatible\": "
        << detail::JsonBool(report.driver.architecture_compatible) << ", \"architecture\": \""
        << ToString(report.driver.driver_architecture) << "\",\n";
    out << "    \"processor_count_checked\": "
        << detail::JsonBool(report.driver.processor_count_checked)
        << ", \"processor_count_matches\": "
        << detail::JsonBool(report.driver.processor_count_matches)
        << ", \"driver_processor_count\": " << report.driver.driver_processor_count
        << ", \"platform_processor_count\": " << report.driver.platform_processor_count
        << ",\n";
    out << "    \"detail\": \"" << detail::JsonEscape(report.driver.detail) << "\"\n";
    out << "  },\n";
    out << "  \"kernel_verification_required\": "
        << detail::JsonBool(report.kernel_verification_required) << ",\n";
    out << "  \"hardware_vmx_started\": " << detail::JsonBool(report.hardware_vmx_started)
        << ",\n";
    out << "  \"hardware_start_supported\": "
        << detail::JsonBool(report.hardware_start_supported) << ",\n";
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
