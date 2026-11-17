#include "hv/virtualization/vmx_lifecycle_controller.hpp"

#include <cstdint>
#include <string>
#include <utility>

namespace hv {
namespace {

constexpr const char* kBackendName = "Intel VT-x diagnostic lifecycle";

bool IsDriverUnavailableReason(VmxBlockReason reason) noexcept {
    switch (reason) {
    case VmxBlockReason::DriverNotChecked:
    case VmxBlockReason::DriverNotConnected:
    case VmxBlockReason::DriverAccessDenied:
        return true;
    default:
        return false;
    }
}

bool IsProtocolReason(VmxBlockReason reason) noexcept {
    switch (reason) {
    case VmxBlockReason::DriverProtocolMismatch:
    case VmxBlockReason::DriverResponseInvalid:
    case VmxBlockReason::DriverArchitectureMismatch:
    case VmxBlockReason::DriverVersionIncompatible:
        return true;
    default:
        return false;
    }
}

} // namespace

Result VmxLifecycleController::Initialize() {
    std::lock_guard lock(mutex_);
    if (state_ == VirtualizationBackendState::Stopped) {
        const std::string detail =
            "Initialize() was rejected: the backend is stopped and Stop() is terminal.";
        last_transition_failed_ = true;
        last_transition_error_ = detail;
        RecordLocked(EventLevel::Warning, VmxDiagnosticCode::LifecycleRejected, detail);
        return Result::Error(Status::InvalidState, detail);
    }
    if (state_ != VirtualizationBackendState::Uninitialized) {
        const std::string detail =
            "Initialize() was rejected: the backend is already initialized. Use Refresh() to "
            "re-evaluate the diagnostics.";
        last_transition_failed_ = true;
        last_transition_error_ = detail;
        RecordLocked(EventLevel::Warning, VmxDiagnosticCode::LifecycleRejected, detail);
        return Result::Error(Status::InvalidState, detail);
    }

    ++generation_;
    RecordLocked(EventLevel::Info, VmxDiagnosticCode::LifecycleInitialized,
                 "Initialize() started the Intel VT-x diagnostic evaluation.");
    const Result result = RunEvaluationLocked();
    if (!result.Succeeded()) {
        last_transition_failed_ = true;
        last_transition_error_ = detail_;
        return result;
    }

    RecordLocked(EventLevel::Info, VmxDiagnosticCode::LifecycleInitialized,
                 std::string("Initialize() committed backend state ") + ToString(state_) + ".");
    return Result::Ok();
}

Result VmxLifecycleController::Refresh() {
    std::lock_guard lock(mutex_);
    if (state_ == VirtualizationBackendState::Stopped) {
        const std::string detail =
            "Refresh() was rejected: the backend is stopped and Stop() is terminal.";
        last_transition_failed_ = true;
        last_transition_error_ = detail;
        RecordLocked(EventLevel::Warning, VmxDiagnosticCode::LifecycleRejected, detail);
        return Result::Error(Status::InvalidState, detail);
    }
    if (state_ == VirtualizationBackendState::Uninitialized) {
        const std::string detail =
            "Refresh() was rejected: the backend has not been initialized. Call Initialize() "
            "first.";
        last_transition_failed_ = true;
        last_transition_error_ = detail;
        RecordLocked(EventLevel::Warning, VmxDiagnosticCode::LifecycleRejected, detail);
        return Result::Error(Status::InvalidState, detail);
    }

    // Snapshot the last committed state. Refresh() is transactional: if the
    // evaluation fails internally, the previous committed verdict survives.
    const VirtualizationBackendState previous_state = state_;
    const VmxBlockReason previous_reason = reason_;
    const VmxErrorClass previous_error_class = error_class_;
    const std::string previous_detail = detail_;
    const bool previous_has_report = has_report_;
    const VmxCapabilityReport previous_capability = capability_report_;
    const VmxReadinessReport previous_readiness = readiness_report_;

    ++generation_;
    RecordLocked(EventLevel::Info, VmxDiagnosticCode::LifecycleRefreshed,
                 "Refresh() started a new Intel VT-x diagnostic evaluation.");
    const Result result = RunEvaluationLocked();
    if (!result.Succeeded()) {
        state_ = previous_state;
        reason_ = previous_reason;
        error_class_ = previous_error_class;
        detail_ = previous_detail;
        has_report_ = previous_has_report;
        capability_report_ = previous_capability;
        readiness_report_ = previous_readiness;
        last_transition_failed_ = true;
        last_transition_error_ =
            result.message.empty() ? std::string("The refresh evaluation failed.") : result.message;
        RecordLocked(EventLevel::Error, VmxDiagnosticCode::EvaluationFailed,
                     "Refresh() failed and was rolled back to " +
                         std::string(ToString(previous_state)) + ": " + last_transition_error_);
        return result;
    }

    RecordLocked(EventLevel::Info, VmxDiagnosticCode::LifecycleRefreshed,
                 std::string("Refresh() committed backend state ") + ToString(state_) + ".");
    return Result::Ok();
}

Result VmxLifecycleController::Stop() {
    std::lock_guard lock(mutex_);
    if (state_ == VirtualizationBackendState::Uninitialized) {
        const std::string detail =
            "Stop() was rejected: the backend has not been initialized.";
        last_transition_failed_ = true;
        last_transition_error_ = detail;
        RecordLocked(EventLevel::Warning, VmxDiagnosticCode::LifecycleRejected, detail);
        return Result::Error(Status::InvalidState, detail);
    }
    if (state_ == VirtualizationBackendState::Stopped) {
        const std::string detail =
            "Stop() was rejected: the backend is already stopped.";
        last_transition_failed_ = true;
        last_transition_error_ = detail;
        RecordLocked(EventLevel::Warning, VmxDiagnosticCode::LifecycleRejected, detail);
        return Result::Error(Status::InvalidState, detail);
    }

    readiness_.ReleaseConnections();
    ++generation_;
    state_ = VirtualizationBackendState::Stopped;
    reason_ = VmxBlockReason::None;
    error_class_ = VmxErrorClass::None;
    detail_ = "The diagnostic backend is stopped; the driver connection was released. "
              "Stop() is terminal and no diagnostic evaluation may resume.";
    last_transition_failed_ = false;
    last_transition_error_.clear();
    RecordLocked(EventLevel::Info, VmxDiagnosticCode::LifecycleStopped, detail_);
    return Result::Ok();
}

const char* VmxLifecycleController::Name() const noexcept { return kBackendName; }

VirtualizationBackendState VmxLifecycleController::GetState() const {
    std::lock_guard lock(mutex_);
    return state_;
}

VirtualizationBackendStatus VmxLifecycleController::GetStatus() const {
    std::lock_guard lock(mutex_);
    return BuildStatusLocked();
}

bool VmxLifecycleController::HardwareVmxStarted() const noexcept { return false; }

VmxDiagnosticsReport VmxLifecycleController::CaptureReport() const {
    std::lock_guard lock(mutex_);
    VmxDiagnosticsReport report;
    report.backend = BuildStatusLocked();
    if (has_report_) {
        report.capability = capability_report_;
        report.readiness = readiness_report_;
    }
    report.events = diagnostics_.Snapshot();
    return report;
}

VmxDiagnostics& VmxLifecycleController::Diagnostics() noexcept { return diagnostics_; }

const VmxDiagnostics& VmxLifecycleController::Diagnostics() const noexcept {
    return diagnostics_;
}

Result VmxLifecycleController::RunEvaluationLocked() {
    state_ = VirtualizationBackendState::CheckingCapabilities;
    reason_ = VmxBlockReason::None;
    error_class_ = VmxErrorClass::None;
    detail_ = "The Intel VT-x diagnostic evaluation is in progress.";
    last_transition_failed_ = false;
    last_transition_error_.clear();

    // 1. CPU capability and per-logical-processor VMX coverage.
    VmxCapabilityReport capability;
    const Result capability_result = capability_.Evaluate(capability);
    capability_report_ = capability;
    if (!capability_result.Succeeded()) {
        has_report_ = true;
        readiness_report_ = VmxReadinessReport{};
        state_ = VirtualizationBackendState::Failed;
        reason_ = capability.reason != VmxBlockReason::None
                      ? capability.reason
                      : VmxBlockReason::CpuidProbeFailed;
        error_class_ = capability.error_class != VmxErrorClass::None
                           ? capability.error_class
                           : VmxErrorClass::CpuProbe;
        detail_ = capability_result.message.empty()
                      ? (capability.summary.empty() ? std::string("The CPU capability probe failed.")
                                                    : capability.summary)
                      : capability_result.message;
        RecordLocked(EventLevel::Error, VmxDiagnosticCode::EvaluationFailed,
                     "VT-x capability evaluation failed: " + detail_);
        return capability_result;
    }

    RecordLocked(EventLevel::Info, VmxDiagnosticCode::CapabilityEvaluated,
                 std::string("VT-x capability support: ") + ToString(capability.support) +
                     "; reason: " + ToString(capability.reason) + ".");
    if (capability.support == VmxSupportLevel::NotChecked) {
        RecordLocked(EventLevel::Warning, VmxDiagnosticCode::NotChecked,
                     "VT-x capability is NOT_CHECKED. " + capability.summary);
    }

    // 2. Readiness: platform, compatibility, driver protocol and kernel-stage
    // checks. This call does not execute any privileged instruction.
    VmxReadinessReport readiness;
    const Result readiness_result = readiness_.Evaluate(capability, readiness);
    readiness_report_ = readiness;
    has_report_ = true;
    if (!readiness_result.Succeeded()) {
        state_ = VirtualizationBackendState::Failed;
        reason_ = readiness.reason != VmxBlockReason::None ? readiness.reason
                                                           : VmxBlockReason::InternalError;
        error_class_ = readiness.error_class != VmxErrorClass::None
                           ? readiness.error_class
                           : VmxErrorClass::Internal;
        detail_ = readiness_result.message.empty()
                      ? (readiness.summary.empty() ? std::string("The readiness evaluation failed.")
                                                   : readiness.summary)
                      : readiness_result.message;
        RecordLocked(EventLevel::Error, VmxDiagnosticCode::EvaluationFailed,
                     "VT-x readiness evaluation failed: " + detail_);
        return readiness_result;
    }

    RecordLocked(EventLevel::Info, VmxDiagnosticCode::ReadinessEvaluated,
                 std::string("VT-x readiness state: ") + ToString(readiness.state) +
                     "; reason: " + ToString(readiness.reason) + ".");
    if (IsDriverUnavailableReason(readiness.reason)) {
        RecordLocked(EventLevel::Warning, VmxDiagnosticCode::DriverUnavailable,
                     "The diagnostic driver is not available: " + readiness.summary);
    }
    if (IsProtocolReason(readiness.reason)) {
        RecordLocked(EventLevel::Warning, VmxDiagnosticCode::ProtocolMismatch,
                     "The diagnostic driver protocol is not compatible: " + readiness.summary);
    }
    if (readiness.reason == VmxBlockReason::PerProcessorCheckIncomplete ||
        readiness.reason == VmxBlockReason::CompatibilityNotChecked ||
        readiness.reason == VmxBlockReason::PlatformNotChecked ||
        readiness.reason == VmxBlockReason::DriverNotChecked) {
        RecordLocked(EventLevel::Warning, VmxDiagnosticCode::NotChecked,
                     "A readiness component is NOT_CHECKED: " + readiness.summary);
    }

    reason_ = readiness.reason;
    error_class_ = readiness.error_class;
    detail_ = readiness.summary;
    switch (readiness.state) {
    case VmxReadinessState::NotSupported:
        state_ = VirtualizationBackendState::NotSupported;
        break;
    case VmxReadinessState::Blocked:
        state_ = VirtualizationBackendState::Blocked;
        break;
    case VmxReadinessState::ReadyForReview:
        state_ = VirtualizationBackendState::ReadyForReview;
        break;
    case VmxReadinessState::Failed:
    case VmxReadinessState::NotEvaluated: {
        // Defensive: a readiness manager must never return Ok() with these
        // states. Treat the contradiction as an internal failure instead of
        // claiming a usable verdict.
        state_ = VirtualizationBackendState::Failed;
        reason_ = VmxBlockReason::InternalError;
        error_class_ = VmxErrorClass::Internal;
        detail_ = "The readiness layer returned an unusable verdict with a success result.";
        RecordLocked(EventLevel::Error, VmxDiagnosticCode::EvaluationFailed, detail_);
        return Result::Error(Status::InternalError, detail_);
    }
    }

    ++successful_evaluations_;
    return Result::Ok();
}

VirtualizationBackendStatus VmxLifecycleController::BuildStatusLocked() const {
    VirtualizationBackendStatus status;
    status.state = state_;
    status.reason = reason_;
    status.error_class = error_class_;
    status.detail = detail_;
    status.generation = generation_;
    status.successful_evaluations = successful_evaluations_;
    status.initialized = state_ != VirtualizationBackendState::Uninitialized;
    status.stopped = state_ == VirtualizationBackendState::Stopped;
    status.last_transition_failed = last_transition_failed_;
    status.last_transition_error = last_transition_error_;
    status.hardware_vmx_started = false;
    status.hardware_start_supported = false;
    return status;
}

void VmxLifecycleController::RecordLocked(EventLevel level, VmxDiagnosticCode code,
                                          std::string message) {
    diagnostics_.Record(level, code, std::move(message));
}

} // namespace hv
