#include "hv/virtualization/vmx_diagnostics.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <sstream>
#include <utility>

namespace hv {

// ---------------------------------------------------------------------------------------------
// Enum names (declared in vmx_status.hpp and vmx_diagnostics.hpp)
// ---------------------------------------------------------------------------------------------

const char* ToString(VmxLifecycleState value) noexcept {
    switch (value) {
    case VmxLifecycleState::Uninitialized: return "Uninitialized";
    case VmxLifecycleState::CheckingCapabilities: return "CheckingCapabilities";
    case VmxLifecycleState::NotSupported: return "NotSupported";
    case VmxLifecycleState::Blocked: return "Blocked";
    case VmxLifecycleState::ReadyForReview: return "ReadyForReview";
    case VmxLifecycleState::Failed: return "Failed";
    case VmxLifecycleState::Stopped: return "Stopped";
    }
    return "Unknown";
}

const char* ToString(VmxCapabilityState value) noexcept {
    switch (value) {
    case VmxCapabilityState::NotChecked: return "NotChecked";
    case VmxCapabilityState::CpuidReported: return "CpuidReported";
    case VmxCapabilityState::NotReported: return "NotReported";
    case VmxCapabilityState::UnsupportedVendor: return "UnsupportedVendor";
    }
    return "Unknown";
}

const char* ToString(VmxCapabilityVerdict value) noexcept {
    switch (value) {
    case VmxCapabilityVerdict::CpuidReportedOnAllProcessors: return "CpuidReportedOnAllProcessors";
    case VmxCapabilityVerdict::NotSupported: return "NotSupported";
    case VmxCapabilityVerdict::Inconclusive: return "Inconclusive";
    }
    return "Unknown";
}

const char* ToString(VmxReadinessVerdict value) noexcept {
    switch (value) {
    case VmxReadinessVerdict::NotSupported: return "NotSupported";
    case VmxReadinessVerdict::Blocked: return "Blocked";
    case VmxReadinessVerdict::ReadyForReview: return "ReadyForReview";
    case VmxReadinessVerdict::Failed: return "Failed";
    }
    return "Unknown";
}

const char* ToString(VmxFailureReason value) noexcept {
    switch (value) {
    case VmxFailureReason::None: return "None";
    case VmxFailureReason::UnsupportedPlatform: return "UnsupportedPlatform";
    case VmxFailureReason::UnsupportedArchitecture: return "UnsupportedArchitecture";
    case VmxFailureReason::UnsupportedVendor: return "UnsupportedVendor";
    case VmxFailureReason::VmxNotReported: return "VmxNotReported";
    case VmxFailureReason::PlatformUnsupported: return "PlatformUnsupported";
    case VmxFailureReason::ProcessorNotChecked: return "ProcessorNotChecked";
    case VmxFailureReason::DriverNotFound: return "DriverNotFound";
    case VmxFailureReason::DriverAccessDenied: return "DriverAccessDenied";
    case VmxFailureReason::DriverConnectionFailed: return "DriverConnectionFailed";
    case VmxFailureReason::DriverProtocolMismatch: return "DriverProtocolMismatch";
    case VmxFailureReason::DriverQueryFailed: return "DriverQueryFailed";
    case VmxFailureReason::DriverDataInconsistent: return "DriverDataInconsistent";
    case VmxFailureReason::CpuProbeFailed: return "CpuProbeFailed";
    case VmxFailureReason::ProcessorEnumerationFailed: return "ProcessorEnumerationFailed";
    case VmxFailureReason::PlatformBackendFailed: return "PlatformBackendFailed";
    case VmxFailureReason::PlatformCompatibilityFailed: return "PlatformCompatibilityFailed";
    case VmxFailureReason::DiagnosticsFailed: return "DiagnosticsFailed";
    case VmxFailureReason::InternalError: return "InternalError";
    }
    return "Unknown";
}

const char* VmxCheckToken(CheckStatus value) noexcept {
    switch (value) {
    case CheckStatus::Pass: return "PASS";
    case CheckStatus::Fail: return "FAIL";
    case CheckStatus::Warn: return "WARN";
    case CheckStatus::Info: return "INFO";
    case CheckStatus::NotChecked: return "NOT_CHECKED";
    }
    return "NOT_CHECKED";
}

VmxReadinessVerdict VerdictFor(VmxFailureReason reason) noexcept {
    switch (reason) {
    case VmxFailureReason::None:
        return VmxReadinessVerdict::ReadyForReview;
    case VmxFailureReason::UnsupportedPlatform:
    case VmxFailureReason::UnsupportedArchitecture:
    case VmxFailureReason::UnsupportedVendor:
    case VmxFailureReason::VmxNotReported:
    case VmxFailureReason::PlatformUnsupported:
        return VmxReadinessVerdict::NotSupported;
    case VmxFailureReason::ProcessorNotChecked:
    case VmxFailureReason::DriverNotFound:
    case VmxFailureReason::DriverAccessDenied:
    case VmxFailureReason::DriverConnectionFailed:
    case VmxFailureReason::DriverProtocolMismatch:
    case VmxFailureReason::DriverQueryFailed:
    case VmxFailureReason::DriverDataInconsistent:
        return VmxReadinessVerdict::Blocked;
    case VmxFailureReason::CpuProbeFailed:
    case VmxFailureReason::ProcessorEnumerationFailed:
    case VmxFailureReason::PlatformBackendFailed:
    case VmxFailureReason::PlatformCompatibilityFailed:
    case VmxFailureReason::DiagnosticsFailed:
    case VmxFailureReason::InternalError:
        return VmxReadinessVerdict::Failed;
    }
    return VmxReadinessVerdict::Failed;
}

const char* ToString(VmxEventLevel value) noexcept {
    switch (value) {
    case VmxEventLevel::Info: return "Info";
    case VmxEventLevel::Warning: return "Warning";
    case VmxEventLevel::Error: return "Error";
    }
    return "Unknown";
}

const char* ToString(VmxEventCode value) noexcept {
    switch (value) {
    case VmxEventCode::CheckStarted: return "CheckStarted";
    case VmxEventCode::CheckCompleted: return "CheckCompleted";
    case VmxEventCode::ProcessorNotChecked: return "ProcessorNotChecked";
    case VmxEventCode::DriverConnected: return "DriverConnected";
    case VmxEventCode::FailureReason: return "FailureReason";
    case VmxEventCode::StateTransition: return "StateTransition";
    case VmxEventCode::TransitionRejected: return "TransitionRejected";
    case VmxEventCode::StartRejected: return "StartRejected";
    case VmxEventCode::BackendStopped: return "BackendStopped";
    case VmxEventCode::InternalError: return "InternalError";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------------------------
// Event log
// ---------------------------------------------------------------------------------------------

VmxDiagnostics::VmxDiagnostics(std::size_t capacity) : capacity_(std::max<std::size_t>(1, capacity)) {}

void VmxDiagnostics::Push(VmxDiagnosticEvent event) {
    event.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
    std::lock_guard<std::mutex> lock(mutex_);
    event.sequence = next_sequence_++;
    events_.push_back(std::move(event));
    while (events_.size() > capacity_) events_.pop_front();
}

void VmxDiagnostics::Record(VmxEventLevel level, VmxEventCode code, std::string component,
                            std::string message, Status error_code) {
    VmxDiagnosticEvent event;
    event.level = level;
    event.code = code;
    event.component = std::move(component);
    event.message = std::move(message);
    event.error_code = error_code;
    Push(std::move(event));
}

void VmxDiagnostics::RecordProcessor(VmxEventLevel level, VmxEventCode code, std::string component,
                                     std::string message, Status error_code,
                                     std::uint32_t cpu_index) {
    VmxDiagnosticEvent event;
    event.level = level;
    event.code = code;
    event.component = std::move(component);
    event.message = std::move(message);
    event.error_code = error_code;
    event.has_cpu_index = true;
    event.cpu_index = cpu_index;
    Push(std::move(event));
}

std::vector<VmxDiagnosticEvent> VmxDiagnostics::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::vector<VmxDiagnosticEvent>(events_.begin(), events_.end());
}

void VmxDiagnostics::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    events_.clear();
}

// ---------------------------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------------------------

std::string JsonEscape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char raw : text) {
        const auto ch = static_cast<unsigned char>(raw);
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (ch < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned int>(ch));
                out += buffer;
            } else {
                out.push_back(raw);
            }
        }
    }
    return out;
}

namespace {

const char* StatusName(Status status) noexcept {
    switch (status) {
    case Status::Success: return "Success";
    case Status::InvalidArgument: return "InvalidArgument";
    case Status::InvalidState: return "InvalidState";
    case Status::AlreadyMapped: return "AlreadyMapped";
    case Status::NotMapped: return "NotMapped";
    case Status::NotFound: return "NotFound";
    case Status::Timeout: return "Timeout";
    case Status::AccessDenied: return "AccessDenied";
    case Status::InitializationFailed: return "InitializationFailed";
    case Status::ShutdownFailed: return "ShutdownFailed";
    case Status::InternalError: return "InternalError";
    case Status::Unsupported: return "Unsupported";
    case Status::DriverNotFound: return "DriverNotFound";
    case Status::DeviceNotConnected: return "DeviceNotConnected";
    case Status::ProtocolMismatch: return "ProtocolMismatch";
    case Status::BufferTooSmall: return "BufferTooSmall";
    case Status::InvalidResponse: return "InvalidResponse";
    case Status::TransportError: return "TransportError";
    }
    return "Unknown";
}

// Minimal compact JSON writer. Keys are compile-time literals and need no escaping.
class JsonWriter {
public:
    void BeginObject(const char* key = nullptr) {
        Prefix(key);
        out_ << '{';
        first_.push_back(true);
    }
    void EndObject() {
        first_.pop_back();
        out_ << '}';
    }
    void BeginArray(const char* key = nullptr) {
        Prefix(key);
        out_ << '[';
        first_.push_back(true);
    }
    void EndArray() {
        first_.pop_back();
        out_ << ']';
    }
    void String(const char* key, const std::string& value) {
        Prefix(key);
        out_ << '"' << JsonEscape(value) << '"';
    }
    void Bool(const char* key, bool value) {
        Prefix(key);
        out_ << (value ? "true" : "false");
    }
    void Number(const char* key, long long value) {
        Prefix(key);
        out_ << value;
    }
    [[nodiscard]] std::string Str() const { return out_.str(); }

private:
    void Prefix(const char* key) {
        if (!first_.empty()) {
            if (!first_.back()) out_ << ',';
            first_.back() = false;
        }
        if (key != nullptr) out_ << '"' << key << "\":";
    }

    std::ostringstream out_;
    std::vector<bool> first_;
};

void WriteReadiness(JsonWriter& w, const VmxReadinessReport& r) {
    w.BeginObject("readiness");
    w.String("verdict", ToString(r.verdict));
    w.String("summary", r.summary);
    w.Bool("kernel_verification_required", r.kernel_verification_required);

    // CPU information (calling processor, from the existing CpuCapabilityManager)
    w.BeginObject("cpu");
    w.Bool("collected", r.capability.cpu_collected);
    if (r.capability.cpu_collected) {
        const CpuCapabilityReport& cpu = r.capability.cpu;
        const PreflightReport& pf = cpu.preflight;
        w.String("manufacturer", cpu.manufacturer);
        w.String("model", cpu.model);
        w.String("vendor", ToString(pf.cpu_vendor));
        w.Number("family", pf.family);
        w.Number("model_id", pf.model);
        w.Number("stepping", pf.stepping);
        w.Bool("hypervisor_present_bit", pf.hypervisor_present_bit);
        w.String("hypervisor_vendor", pf.hypervisor_vendor_string);
        w.String("preflight_verdict", ToString(pf.verdict));
    }
    w.EndObject();

    // VMX capability
    w.BeginObject("vmx_capability");
    w.Bool("collected", r.capability_collected);
    w.String("verdict", ToString(r.capability.verdict));
    w.String("summary", r.capability.summary);
    w.Number("processors_expected", r.capability.processors_expected);
    w.Number("processors_checked", r.capability.processors_checked);
    w.Number("processors_vmx_reported", r.capability.processors_vmx_reported);
    w.EndObject();

    w.BeginArray("processors");
    for (const auto& p : r.capability.processors) {
        w.BeginObject();
        w.Number("cpu_index", p.cpu_index);
        w.String("vendor", p.vendor);
        w.String("vmx_capability", ToString(p.vmx_capability));
        w.String("check_status", VmxCheckToken(p.check_status));
        w.String("error_code", StatusName(p.error_code));
        w.String("diagnostic_result", p.diagnostic_result);
        w.EndObject();
    }
    w.EndArray();

    // Platform status
    w.BeginObject("platform");
    w.Bool("collected", r.platform_collected);
    w.String("backend_state",
             r.backend_state == WindowsBackendState::Ready          ? "Ready"
             : r.backend_state == WindowsBackendState::Initializing ? "Initializing"
             : r.backend_state == WindowsBackendState::Failed       ? "Failed"
                                                                    : "Stopped");
    if (r.platform_collected) {
        const WindowsSystemInformation& s = r.platform.system;
        w.String("os", FormatWindowsVersion(s));
        w.String("architecture", ToString(s.architecture));
        w.Number("logical_processors", s.logical_processor_count);
        w.Number("page_size", s.page_size);
    }
    w.EndObject();

    // Driver status
    w.BeginObject("driver");
    w.String("status", VmxCheckToken(r.driver.status));
    w.Bool("connect_attempted", r.driver.connect_attempted);
    w.Bool("connected", r.driver.connected);
    w.Bool("connected_by_readiness", r.driver.connected_by_readiness);
    w.Bool("protocol_verified", r.driver.protocol_verified);
    w.Number("expected_protocol_version", r.driver.expected_protocol_version);
    w.Bool("platform_info_read", r.driver.platform_info_read);
    w.String("error_code", StatusName(r.driver.error_code));
    w.String("message", r.driver.message);
    w.EndObject();

    // Compatibility status
    w.BeginObject("compatibility");
    w.Bool("collected", r.compatibility_collected);
    if (r.compatibility_collected) {
        w.String("verdict", ToString(r.compatibility.verdict));
        w.String("summary", r.compatibility.summary);
    }
    w.EndObject();

    w.BeginArray("reasons");
    for (const auto& reason : r.reasons) {
        w.BeginObject();
        w.String("reason", ToString(reason.reason));
        w.String("category", ToString(VerdictFor(reason.reason)));
        w.String("error_code", StatusName(reason.status));
        w.String("component", reason.component);
        w.String("message", reason.message);
        w.EndObject();
    }
    w.EndArray();

    w.BeginArray("conditions");
    for (const auto& c : r.conditions) {
        w.BeginObject();
        w.String("id", c.id);
        w.String("title", c.title);
        w.String("status", VmxCheckToken(c.status));
        w.String("detail", c.detail);
        w.EndObject();
    }
    w.EndArray();

    w.BeginArray("unverified");
    for (const auto& c : r.Unverified()) w.String(nullptr, c.id);
    w.EndArray();

    w.EndObject();
}

} // namespace

std::string FormatVmxReportJson(const VmxDiagnosticReport& report) {
    JsonWriter w;
    w.BeginObject();
    w.String("product", "Geraduo Hypervisor Core");
    w.String("version", report.product_version);
    w.String("state", ToString(report.state));
    w.Bool("hypervisor_running", report.hypervisor_running);
    w.Bool("hardware_virtualization_started", report.hardware_virtualization_started);
    w.Bool("readiness_available", report.readiness_available);
    w.String("last_error", report.last_error);
    if (report.readiness_available) WriteReadiness(w, report.readiness);

    w.BeginArray("transitions");
    for (const auto& t : report.transitions) {
        w.BeginObject();
        w.Number("sequence", static_cast<long long>(t.sequence));
        w.String("from", ToString(t.from));
        w.String("to", ToString(t.to));
        w.String("reason", t.reason);
        w.EndObject();
    }
    w.EndArray();

    w.BeginArray("events");
    for (const auto& e : report.events) {
        w.BeginObject();
        w.Number("sequence", static_cast<long long>(e.sequence));
        w.Number("timestamp_ms", static_cast<long long>(e.timestamp_ms));
        w.String("level", ToString(e.level));
        w.String("code", ToString(e.code));
        w.String("component", e.component);
        w.String("error_code", StatusName(e.error_code));
        if (e.has_cpu_index) w.Number("cpu_index", e.cpu_index);
        w.String("message", e.message);
        w.EndObject();
    }
    w.EndArray();

    w.BeginArray("driver_events");
    for (const auto& e : report.driver_events) {
        w.BeginObject();
        w.Number("sequence", static_cast<long long>(e.sequence));
        w.Number("timestamp_ms",
                 static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                            e.timestamp.time_since_epoch())
                                            .count()));
        w.String("level", ToString(e.level));
        w.String("code", ToString(e.code));
        w.String("component", e.component);
        w.Number("native_error", e.native_error);
        w.Number("request_id", static_cast<long long>(e.request_id));
        w.String("message", e.message);
        w.EndObject();
    }
    w.EndArray();

    w.EndObject();
    return w.Str();
}

// ---------------------------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------------------------

std::string FormatVmxReportText(const VmxDiagnosticReport& report) {
    std::ostringstream out;
    out << "Geraduo Hypervisor Core v" << report.product_version << " - VMX Diagnostics\n";
    out << "============================================================\n\n";

    out << "[VirtualizationBackend]\n";
    out << "  State: " << ToString(report.state) << '\n';
    out << "  Hypervisor running: " << (report.hypervisor_running ? "yes" : "no")
        << " (hardware virtualization launch is not implemented in this version)\n";
    if (!report.last_error.empty()) out << "  Last error: " << report.last_error << '\n';

    if (!report.readiness_available) {
        out << "\nNo readiness evaluation has completed; every condition is NOT_CHECKED.\n";
        return out.str();
    }
    const VmxReadinessReport& r = report.readiness;
    out << "  Readiness verdict: " << ToString(r.verdict) << '\n';
    out << "  Summary: " << r.summary << '\n';

    out << "\n[CPU information]\n";
    if (r.capability.cpu_collected) {
        const CpuCapabilityReport& cpu = r.capability.cpu;
        const PreflightReport& pf = cpu.preflight;
        out << "  Manufacturer: " << (cpu.manufacturer.empty() ? "(unavailable)" : cpu.manufacturer)
            << '\n';
        out << "  Model: " << (cpu.model.empty() ? "(unavailable)" : cpu.model) << '\n';
        out << "  Vendor class: " << ToString(pf.cpu_vendor) << "  family " << pf.family << " model "
            << pf.model << " stepping " << pf.stepping << '\n';
        out << "  CPUID hypervisor-present bit: " << (pf.hypervisor_present_bit ? "set" : "clear");
        if (pf.hypervisor_present_bit && !pf.hypervisor_vendor_string.empty())
            out << " (" << pf.hypervisor_vendor_string << ")";
        out << "\n  (this section describes the calling logical processor only)\n";
    } else {
        out << "  NOT_CHECKED: CPU information could not be read.\n";
    }

    out << "\n[VMX capability]\n";
    out << "  Verdict: " << ToString(r.capability.verdict) << '\n';
    out << "  " << (r.capability.summary.empty() ? "NOT_CHECKED" : r.capability.summary) << '\n';
    out << "  Processors checked: " << r.capability.processors_checked << " / "
        << r.capability.processors_expected << "; VMX reported on "
        << r.capability.processors_vmx_reported << '\n';
    for (const auto& p : r.capability.processors) {
        out << "    CPU " << p.cpu_index << ": vendor=" << (p.vendor.empty() ? "(unknown)" : p.vendor)
            << " vmx=" << ToString(p.vmx_capability) << " check=" << VmxCheckToken(p.check_status)
            << " error=" << StatusName(p.error_code) << "\n      " << p.diagnostic_result << '\n';
    }

    out << "\n[Platform status]\n";
    if (r.platform_collected) {
        out << "  OS: " << FormatWindowsVersion(r.platform.system) << '\n';
        out << "  Architecture: " << ToString(r.platform.system.architecture) << '\n';
        out << "  Logical processors: " << r.platform.system.logical_processor_count << '\n';
        out << "  Page size: " << r.platform.system.page_size << " bytes\n";
    } else {
        out << "  NOT_CHECKED: the Windows Platform Backend produced no snapshot.\n";
    }

    out << "\n[Driver status]\n";
    out << "  Status: " << VmxCheckToken(r.driver.status) << '\n';
    out << "  Connected: " << (r.driver.connected ? "yes" : "no")
        << "  Protocol verified: " << (r.driver.protocol_verified ? "yes" : "no")
        << "  Expected protocol: v" << r.driver.expected_protocol_version << '\n';
    out << "  Error code: " << StatusName(r.driver.error_code) << '\n';
    if (!r.driver.message.empty()) out << "  Detail: " << r.driver.message << '\n';
    out << "  The driver is diagnostic-only: no VMX instruction and no memory access.\n";

    out << "\n[Compatibility status]\n";
    if (r.compatibility_collected) {
        out << "  Verdict: " << ToString(r.compatibility.verdict) << '\n';
        out << "  " << r.compatibility.summary << '\n';
    } else {
        out << "  NOT_CHECKED: compatibility was not evaluated.\n";
    }

    out << "\n[Reasons for unsupported / blocked / failed configuration]\n";
    if (r.reasons.empty()) {
        out << "  none recorded\n";
    } else {
        for (const auto& reason : r.reasons) {
            out << "  - " << ToString(reason.reason) << " [" << ToString(VerdictFor(reason.reason))
                << ", " << StatusName(reason.status) << "] " << reason.message << '\n';
        }
    }

    out << "\n[Conditions]\n";
    for (const auto& c : r.conditions) {
        out << "  [" << VmxCheckToken(c.status) << "] " << c.title << " - " << c.detail << '\n';
    }
    out << "\nKernel verification required: yes\n";
    out << "No VMXON, VMCS, VM-entry, VM-exit, or hardware EPT/NPT operation was performed.\n";
    return out.str();
}

} // namespace hv
