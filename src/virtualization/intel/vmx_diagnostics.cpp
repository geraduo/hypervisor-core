#include "hv/virtualization/vmx_diagnostics.hpp"

#include "hv/detail/report_json.hpp"
#include "hv/platform/driver_diagnostics.hpp"

#include <chrono>
#include <cstdint>
#include <sstream>
#include <utility>

namespace hv {
namespace {

using Clock = std::chrono::system_clock;

std::int64_t UnixMillis(Clock::time_point point) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(point.time_since_epoch())
        .count();
}

std::string EventCodeText(VmxDiagnosticCode code) { return ToString(code); }

} // namespace

VmxDiagnostics::VmxDiagnostics(std::size_t capacity)
    : capacity_(capacity == 0 ? 1 : capacity) {}

void VmxDiagnostics::Record(EventLevel level, VmxDiagnosticCode code, std::string message) {
    std::lock_guard lock(mutex_);
    VmxDiagnosticEvent event;
    event.level = level;
    event.code = code;
    event.message = std::move(message);
    event.timestamp = Clock::now();
    events_.push_back(std::move(event));
    TrimLocked();
}

std::vector<VmxDiagnosticEvent> VmxDiagnostics::Snapshot() const {
    std::lock_guard lock(mutex_);
    return events_;
}

void VmxDiagnostics::Clear() {
    std::lock_guard lock(mutex_);
    events_.clear();
}

std::size_t VmxDiagnostics::Size() const {
    std::lock_guard lock(mutex_);
    return events_.size();
}

std::size_t VmxDiagnostics::Capacity() const {
    std::lock_guard lock(mutex_);
    return capacity_;
}

void VmxDiagnostics::TrimLocked() {
    if (events_.size() <= capacity_) return;
    const std::size_t excess = events_.size() - capacity_;
    events_.erase(events_.begin(), events_.begin() + static_cast<std::ptrdiff_t>(excess));
}

const char* ToString(VmxDiagnosticCode value) noexcept {
    switch (value) {
    case VmxDiagnosticCode::None: return "None";
    case VmxDiagnosticCode::LifecycleInitialized: return "LifecycleInitialized";
    case VmxDiagnosticCode::LifecycleRefreshed: return "LifecycleRefreshed";
    case VmxDiagnosticCode::LifecycleStopped: return "LifecycleStopped";
    case VmxDiagnosticCode::LifecycleRejected: return "LifecycleRejected";
    case VmxDiagnosticCode::CapabilityEvaluated: return "CapabilityEvaluated";
    case VmxDiagnosticCode::ReadinessEvaluated: return "ReadinessEvaluated";
    case VmxDiagnosticCode::DriverUnavailable: return "DriverUnavailable";
    case VmxDiagnosticCode::ProtocolMismatch: return "ProtocolMismatch";
    case VmxDiagnosticCode::NotChecked: return "NotChecked";
    case VmxDiagnosticCode::EvaluationFailed: return "EvaluationFailed";
    case VmxDiagnosticCode::ReportGenerated: return "ReportGenerated";
    }
    return "None";
}

const char* ToString(VirtualizationBackendState value) noexcept {
    switch (value) {
    case VirtualizationBackendState::Uninitialized: return "Uninitialized";
    case VirtualizationBackendState::CheckingCapabilities: return "CheckingCapabilities";
    case VirtualizationBackendState::NotSupported: return "NotSupported";
    case VirtualizationBackendState::Blocked: return "Blocked";
    case VirtualizationBackendState::ReadyForReview: return "ReadyForReview";
    case VirtualizationBackendState::Failed: return "Failed";
    case VirtualizationBackendState::Stopped: return "Stopped";
    }
    return "Uninitialized";
}

bool IsTerminalBackendState(VirtualizationBackendState state) noexcept {
    return state == VirtualizationBackendState::Stopped;
}

std::string FormatVirtualizationBackendStatusText(const VirtualizationBackendStatus& status) {
    std::ostringstream out;
    out << "Geraduo virtualization backend status\n";
    out << "State: " << ToString(status.state) << '\n';
    out << "Reason: " << ToString(status.reason) << '\n';
    out << "Error class: " << ToString(status.error_class) << '\n';
    out << "Detail: " << status.detail << '\n';
    out << "Generation: " << status.generation
        << ", successful evaluations: " << status.successful_evaluations << '\n';
    out << "Initialized: " << (status.initialized ? "yes" : "no")
        << ", stopped: " << (status.stopped ? "yes" : "no") << '\n';
    out << "Last transition failed: " << (status.last_transition_failed ? "yes" : "no") << '\n';
    out << "Last transition error: "
        << (status.last_transition_error.empty() ? "(none)" : status.last_transition_error) << '\n';
    out << "Hardware VMX started: " << (status.hardware_vmx_started ? "yes" : "no") << '\n';
    out << "Hardware VMX start supported: "
        << (status.hardware_start_supported ? "yes" : "no") << '\n';
    return out.str();
}

std::string FormatVirtualizationBackendStatusJson(const VirtualizationBackendStatus& status) {
    std::ostringstream out;
    out << "{\n";
    out << "  \"state\": \"" << ToString(status.state) << "\",\n";
    out << "  \"reason\": \"" << ToString(status.reason) << "\",\n";
    out << "  \"error_class\": \"" << ToString(status.error_class) << "\",\n";
    out << "  \"detail\": \"" << detail::JsonEscape(status.detail) << "\",\n";
    out << "  \"generation\": " << status.generation << ",\n";
    out << "  \"successful_evaluations\": " << status.successful_evaluations << ",\n";
    out << "  \"initialized\": " << detail::JsonBool(status.initialized) << ",\n";
    out << "  \"stopped\": " << detail::JsonBool(status.stopped) << ",\n";
    out << "  \"last_transition_failed\": " << detail::JsonBool(status.last_transition_failed)
        << ",\n";
    out << "  \"last_transition_error\": \""
        << detail::JsonEscape(status.last_transition_error) << "\",\n";
    out << "  \"hardware_vmx_started\": " << detail::JsonBool(status.hardware_vmx_started)
        << ",\n";
    out << "  \"hardware_start_supported\": "
        << detail::JsonBool(status.hardware_start_supported) << "\n";
    out << "}";
    return out.str();
}

std::string FormatVmxDiagnosticsText(const VmxDiagnosticsReport& report) {
    std::ostringstream out;
    out << "=== Geraduo Hypervisor Core v0.6.0 VT-x diagnostics ===\n";
    out << FormatVirtualizationBackendStatusText(report.backend) << '\n';
    out << FormatVmxCapabilityReportText(report.capability) << '\n';
    out << FormatVmxReadinessReportText(report.readiness) << '\n';
    out << "Diagnostic journal (" << report.events.size() << " events):\n";
    for (const auto& event : report.events) {
        out << "  [" << ToString(event.level) << "] " << ToString(event.code) << " @ "
            << UnixMillis(event.timestamp) << " ms: " << event.message << '\n';
    }
    out << "Boundary: no VMXON, VMCS access, VM-entry/exit or hardware EPT was performed. "
           "The result is a diagnostic verdict, not a hardware virtualization claim.\n";
    return out.str();
}

std::string FormatVmxDiagnosticsJson(const VmxDiagnosticsReport& report) {
    std::ostringstream out;
    out << "{\n";
    out << "  \"schema\": \"geraduo.vmx.diagnostics.v1\",\n";
    out << "  \"tool_version\": \"0.6.0\",\n";
    out << "  \"backend\": " << FormatVirtualizationBackendStatusJson(report.backend) << ",\n";
    out << "  \"capability\": " << FormatVmxCapabilityReportJson(report.capability) << ",\n";
    out << "  \"readiness\": " << FormatVmxReadinessReportJson(report.readiness) << ",\n";
    out << "  \"events\": [\n";
    for (std::size_t index = 0; index < report.events.size(); ++index) {
        const auto& event = report.events[index];
        out << "    {\"level\": \"" << ToString(event.level) << "\", \"code\": \""
            << EventCodeText(event.code) << "\", \"timestamp_unix_ms\": "
            << UnixMillis(event.timestamp) << ", \"message\": \""
            << detail::JsonEscape(event.message) << "\"}"
            << (index + 1 < report.events.size() ? "," : "") << '\n';
    }
    out << "  ],\n";
    out << "  \"boundary\": {\n";
    out << "    \"vmxon_executed\": false,\n";
    out << "    \"vmcs_accessed\": false,\n";
    out << "    \"vm_entry_exit_executed\": false,\n";
    out << "    \"hardware_ept_configured\": false,\n";
    out << "    \"privileged_instructions_executed\": false\n";
    out << "  }\n";
    out << "}\n";
    return out.str();
}

} // namespace hv
