#include "hv/virtualization/virtualization_backend.hpp"

#include <exception>
#include <string>
#include <utility>

namespace hv {
namespace {

constexpr const char* kComponent = "IntelVirtualizationBackend";

VmxLifecycleState StateFor(VmxReadinessVerdict verdict) noexcept {
    switch (verdict) {
    case VmxReadinessVerdict::NotSupported: return VmxLifecycleState::NotSupported;
    case VmxReadinessVerdict::Blocked: return VmxLifecycleState::Blocked;
    case VmxReadinessVerdict::ReadyForReview: return VmxLifecycleState::ReadyForReview;
    case VmxReadinessVerdict::Failed: return VmxLifecycleState::Failed;
    }
    return VmxLifecycleState::Failed;
}

} // namespace

IntelVirtualizationBackend::IntelVirtualizationBackend(VmxReadinessManager& readiness,
                                                       VmxDiagnostics& diagnostics,
                                                       DriverDiagnostics* driver_diagnostics)
    : readiness_(readiness),
      diagnostics_(diagnostics),
      driver_diagnostics_(driver_diagnostics),
      lifecycle_(&diagnostics) {}

std::string IntelVirtualizationBackend::Name() const { return "Intel VT-x (diagnostic only)"; }

Result IntelVirtualizationBackend::Initialize() {
    std::lock_guard<std::mutex> operation(operation_mutex_);

    std::uint64_t token = 0;
    const Result begin = lifecycle_.BeginCheck(token);
    if (!begin.Succeeded()) {
        SetLastError(begin.message);
        return begin;
    }
    diagnostics_.Record(VmxEventLevel::Info, VmxEventCode::CheckStarted, kComponent,
                        "Capability and readiness check started.");

    VmxReadinessReport report;
    Result evaluation;
    try {
        evaluation = readiness_.Evaluate(report);
    } catch (const std::exception& error) {
        report.verdict = VmxReadinessVerdict::Failed;
        report.summary = std::string("Evaluation threw an exception: ") + error.what();
        report.reasons.push_back(VmxFailureDetail{VmxFailureReason::InternalError,
                                                  Status::InternalError, report.summary, kComponent});
        evaluation = Result::Error(Status::InternalError, report.summary);
    } catch (...) {
        report.verdict = VmxReadinessVerdict::Failed;
        report.summary = "Evaluation threw an unknown exception.";
        report.reasons.push_back(VmxFailureDetail{VmxFailureReason::InternalError,
                                                  Status::InternalError, report.summary, kComponent});
        evaluation = Result::Error(Status::InternalError, report.summary);
    }

    const VmxLifecycleState outcome = StateFor(report.verdict);
    const std::string summary = report.summary;
    RecordReport(report);

    // Resource cleanup: only a ReadyForReview result keeps the driver connection.
    if (outcome != VmxLifecycleState::ReadyForReview) readiness_.ReleaseDriver();

    Result result = Result::Ok();
    switch (outcome) {
    case VmxLifecycleState::ReadyForReview: break;
    case VmxLifecycleState::NotSupported:
        result = Result::Error(Status::Unsupported, summary);
        break;
    case VmxLifecycleState::Failed:
        result = evaluation.Succeeded() ? Result::Error(Status::InitializationFailed, summary)
                                        : evaluation;
        break;
    default:
        result = Result::Error(Status::InitializationFailed, summary);
        break;
    }

    {
        std::lock_guard<std::mutex> lock(report_mutex_);
        last_report_ = std::move(report);
        has_report_ = true;
        last_error_ = result.Succeeded() ? std::string() : result.message;
    }

    const Result completed = lifecycle_.CompleteCheck(token, outcome, summary);
    if (!completed.Succeeded()) {
        readiness_.ReleaseDriver();
        SetLastError(completed.message);
        return completed;
    }
    return result;
}

Result IntelVirtualizationBackend::RunDiagnostics(VmxDiagnosticReport& out) {
    out = VmxDiagnosticReport{};
    out.state = lifecycle_.State();
    out.transitions = lifecycle_.History();
    out.events = diagnostics_.Snapshot();
    if (driver_diagnostics_ != nullptr) out.driver_events = driver_diagnostics_->Snapshot();
    {
        std::lock_guard<std::mutex> lock(report_mutex_);
        if (has_report_) {
            out.readiness = last_report_;
            out.readiness_available = true;
        }
        out.last_error = last_error_;
    }
    // hypervisor_running / hardware_virtualization_started stay false: nothing is ever started.
    if (!out.readiness_available) {
        return Result::Error(Status::InvalidState,
                             std::string("no readiness evaluation has completed (state ") +
                                 ToString(out.state) + ")");
    }
    return Result::Ok();
}

Result IntelVirtualizationBackend::StartVirtualization() {
    const std::string message =
        "Starting hardware virtualization is not implemented in v0.5.0: VMXON, VMCS and VM entry "
        "are never executed.";
    diagnostics_.Record(VmxEventLevel::Warning, VmxEventCode::StartRejected, kComponent, message,
                        Status::Unsupported);
    SetLastError(message);
    return Result::Error(Status::Unsupported, message);
}

Result IntelVirtualizationBackend::Shutdown() {
    std::lock_guard<std::mutex> operation(operation_mutex_);
    const Result stopped = lifecycle_.Stop("shutdown requested");
    readiness_.ReleaseDriver();  // idempotent; closes only a connection this layer opened
    if (stopped.Succeeded()) {
        diagnostics_.Record(VmxEventLevel::Info, VmxEventCode::BackendStopped, kComponent,
                            "Backend stopped and resources released.");
    }
    return stopped;
}

VirtualizationBackendState IntelVirtualizationBackend::GetState() const { return lifecycle_.State(); }

bool IntelVirtualizationBackend::IsHypervisorRunning() const noexcept { return false; }

std::string IntelVirtualizationBackend::GetLastError() const {
    std::lock_guard<std::mutex> lock(report_mutex_);
    return last_error_;
}

void IntelVirtualizationBackend::SetLastError(std::string message) {
    std::lock_guard<std::mutex> lock(report_mutex_);
    last_error_ = std::move(message);
}

void IntelVirtualizationBackend::RecordReport(const VmxReadinessReport& report) {
    for (const auto& processor : report.capability.processors) {
        if (processor.check_status == CheckStatus::NotChecked) {
            diagnostics_.RecordProcessor(VmxEventLevel::Warning, VmxEventCode::ProcessorNotChecked,
                                         "VmxCapabilityManager", processor.diagnostic_result,
                                         processor.error_code, processor.cpu_index);
        }
    }
    if (report.driver.connected && report.driver.connect_attempted) {
        diagnostics_.Record(VmxEventLevel::Info, VmxEventCode::DriverConnected,
                            "KernelDriverInterface", "Diagnostic driver connected.");
    }
    for (const auto& reason : report.reasons) {
        const bool failure = VerdictFor(reason.reason) == VmxReadinessVerdict::Failed;
        diagnostics_.Record(failure ? VmxEventLevel::Error : VmxEventLevel::Warning,
                            VmxEventCode::FailureReason, reason.component,
                            std::string("[") + ToString(reason.reason) + "] " + reason.message,
                            reason.status);
    }
    diagnostics_.Record(report.verdict == VmxReadinessVerdict::ReadyForReview
                            ? VmxEventLevel::Info
                            : VmxEventLevel::Warning,
                        VmxEventCode::CheckCompleted, kComponent,
                        std::string("Verdict ") + ToString(report.verdict) + ": " + report.summary);
}

} // namespace hv
