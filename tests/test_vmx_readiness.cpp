#include "fakes/fake_vmx.hpp"
#include "test_support.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

using test::Check;

namespace {

void TestReadyForReview() {
    fakes::VmxRig rig(4, true);
    hv::VmxReadinessReport report;
    const hv::Result result = rig.readiness.Evaluate(report);

    Check(result.Succeeded() && report.verdict == hv::VmxReadinessVerdict::ReadyForReview,
          "ready: verdict is ReadyForReview");
    Check(report.reasons.empty() && report.platform_collected && report.capability_collected &&
              report.compatibility_collected,
          "ready: all stages collected, no failure reasons");
    Check(report.driver.connected && report.driver.connected_by_readiness &&
              report.driver.protocol_verified && report.driver.status == hv::CheckStatus::Pass,
          "ready: driver connected, protocol verified");
    Check(report.kernel_verification_required, "ready: kernel verification is still required");

    const auto unverified = report.Unverified();
    bool has_vmxon = false;
    bool has_feature_control = false;
    for (const auto& condition : unverified) {
        if (condition.id == "vmx.kernel.vmxon") has_vmxon = true;
        if (condition.id == "vmx.kernel.feature_control") has_feature_control = true;
    }
    Check(has_vmxon && has_feature_control,
          "ready: kernel-only conditions are listed explicitly as NOT_CHECKED");
    Check(rig.driver.connected, "ready: the driver connection is retained for ReadyForReview");
}

void TestCpuWithoutVmxIsNotSupported() {
    fakes::VmxRig rig(2, false);
    hv::VmxReadinessReport report;
    const hv::Result result = rig.readiness.Evaluate(report);
    Check(result.Succeeded() && report.verdict == hv::VmxReadinessVerdict::NotSupported,
          "no VMX: NotSupported is a valid, successful evaluation");
    Check(fakes::HasReason(report.reasons, hv::VmxFailureReason::VmxNotReported),
          "no VMX: reason VmxNotReported");
    Check(!rig.driver.connected && rig.driver.disconnect_calls >= 1,
          "no VMX: the driver connection opened for the check is closed again");
}

void TestUnsupportedPlatform() {
    {
        fakes::VmxRig rig(4, true);
        rig.platform.system.architecture = hv::ProcessorArchitecture::Arm64;
        rig.driver.platform_info.architecture = static_cast<std::uint32_t>(GERADUO_ARCHITECTURE_ARM64);
        hv::VmxReadinessReport report;
        Check(rig.readiness.Evaluate(report).Succeeded() &&
                  report.verdict == hv::VmxReadinessVerdict::NotSupported &&
                  fakes::HasReason(report.reasons, hv::VmxFailureReason::PlatformUnsupported),
              "unsupported platform (Arm64): NotSupported / PlatformUnsupported");
    }
    {
        fakes::VmxRig rig(4, true);
        rig.platform.system.is_windows = false;
        hv::VmxReadinessReport report;
        Check(rig.readiness.Evaluate(report).Succeeded() &&
                  report.verdict == hv::VmxReadinessVerdict::NotSupported &&
                  fakes::HasReason(report.reasons, hv::VmxFailureReason::UnsupportedPlatform),
              "unsupported platform (not Windows): NotSupported / UnsupportedPlatform");
    }
}

void TestMissingDriver() {
    fakes::VmxRig rig(4, true);
    rig.driver.connect_status = hv::Status::DriverNotFound;
    hv::VmxReadinessReport report;
    const hv::Result result = rig.readiness.Evaluate(report);
    Check(result.Succeeded() && report.verdict == hv::VmxReadinessVerdict::Blocked,
          "missing driver: Blocked");
    Check(fakes::HasReason(report.reasons, hv::VmxFailureReason::DriverNotFound) &&
              !report.driver.connected && report.driver.error_code == hv::Status::DriverNotFound,
          "missing driver: reason DriverNotFound with the original status");
    const auto* protocol = fakes::FindCondition(report, "vmx.driver.protocol");
    Check(protocol != nullptr && protocol->status == hv::CheckStatus::NotChecked,
          "missing driver: protocol compatibility is NOT_CHECKED, not assumed");

    // The same situation is only a warning when the driver is optional.
    hv::VmxReadinessOptions options;
    options.require_driver = false;
    hv::VmxReadinessManager lenient(rig.backend, rig.capability, rig.compatibility, rig.driver, options);
    hv::VmxReadinessReport lenient_report;
    Check(lenient.Evaluate(lenient_report).Succeeded() &&
              lenient_report.verdict == hv::VmxReadinessVerdict::ReadyForReview &&
              lenient_report.driver.status == hv::CheckStatus::Warn,
          "optional driver: missing driver is a WARN and kernel checks stay NOT_CHECKED");
}

void TestProtocolMismatch() {
    {
        fakes::VmxRig rig(4, true);
        rig.driver.connect_status = hv::Status::ProtocolMismatch;
        hv::VmxReadinessReport report;
        Check(rig.readiness.Evaluate(report).Succeeded() &&
                  report.verdict == hv::VmxReadinessVerdict::Blocked &&
                  fakes::HasReason(report.reasons, hv::VmxFailureReason::DriverProtocolMismatch),
              "protocol mismatch at connect: Blocked / DriverProtocolMismatch");
    }
    {
        fakes::VmxRig rig(4, true);
        rig.driver.query_status = hv::Status::ProtocolMismatch;
        hv::VmxReadinessReport report;
        Check(rig.readiness.Evaluate(report).Succeeded() &&
                  report.verdict == hv::VmxReadinessVerdict::Blocked &&
                  fakes::HasReason(report.reasons, hv::VmxFailureReason::DriverProtocolMismatch),
              "protocol mismatch at query: Blocked / DriverProtocolMismatch");
        const auto* protocol = fakes::FindCondition(report, "vmx.driver.protocol");
        Check(protocol != nullptr && protocol->status == hv::CheckStatus::Fail && !rig.driver.connected,
              "protocol mismatch at query: FAIL condition and the connection is closed");
    }
}

void TestPlatformBackendFailure() {
    fakes::VmxRig rig(4, true);
    rig.platform.system_result =
        hv::Result::Error(hv::Status::InitializationFailed, "system provider fault");
    hv::VmxReadinessReport report;
    const hv::Result result = rig.readiness.Evaluate(report);
    Check(!result.Succeeded() && result.status == hv::Status::InitializationFailed,
          "platform backend failure: Evaluate returns the error");
    Check(report.verdict == hv::VmxReadinessVerdict::Failed &&
              fakes::HasReason(report.reasons, hv::VmxFailureReason::PlatformBackendFailed) &&
              !report.platform_collected &&
              report.backend_state == hv::WindowsBackendState::Failed,
          "platform backend failure: Failed / PlatformBackendFailed, no platform data invented");
    const auto* compat = fakes::FindCondition(report, "vmx.compatibility");
    Check(compat != nullptr && compat->status == hv::CheckStatus::NotChecked,
          "platform backend failure: dependent compatibility check is NOT_CHECKED");
    Check(!rig.driver.connected, "platform backend failure: no driver connection is leaked");
}

void TestDiagnosticQueryFailure() {
    fakes::VmxRig rig(4, true);
    rig.driver.query_status = hv::Status::TransportError;
    hv::VmxReadinessReport report;
    Check(rig.readiness.Evaluate(report).Succeeded() &&
              report.verdict == hv::VmxReadinessVerdict::Blocked &&
              fakes::HasReason(report.reasons, hv::VmxFailureReason::DriverQueryFailed),
          "diagnostic query failure: Blocked / DriverQueryFailed");
    Check(report.driver.error_code == hv::Status::TransportError && !report.driver.platform_info_read,
          "diagnostic query failure: error code kept, no driver data invented");
    Check(!rig.driver.connected && rig.driver.disconnect_calls >= 1,
          "diagnostic query failure: connection closed after partial failure");
}

void TestInconsistentDriverData() {
    fakes::VmxRig rig(4, true);
    rig.driver.platform_info.processor_count = 8;
    hv::VmxReadinessReport report;
    Check(rig.readiness.Evaluate(report).Succeeded() &&
              report.verdict == hv::VmxReadinessVerdict::Blocked &&
              fakes::HasReason(report.reasons, hv::VmxFailureReason::DriverDataInconsistent),
          "inconsistent driver data: Blocked / DriverDataInconsistent");
}

void TestHypervisorAndVbsAreWarnings() {
    fakes::VmxRig rig(4, true);
    rig.current_cpuid = fakes::MakeIntelCpuid(true, true);
    rig.platform.mitigations.vbs_configured = true;
    hv::VmxReadinessReport report;
    Check(rig.readiness.Evaluate(report).Succeeded() &&
              report.verdict == hv::VmxReadinessVerdict::ReadyForReview,
          "hypervisor + VBS: a review signal does not block (and is not a claimed conflict)");
    const auto* hypervisor = fakes::FindCondition(report, "vmx.hypervisor_present");
    const auto* vbs = fakes::FindCondition(report, "compat.windows.vbs");
    Check(hypervisor != nullptr && hypervisor->status == hv::CheckStatus::Warn && vbs != nullptr &&
              vbs->status == hv::CheckStatus::Warn,
          "hypervisor + VBS: both are reported as WARN");
}

void TestNotCheckedProcessorBlocks() {
    fakes::VmxRig rig(4, true);
    rig.processors.failing[1] = hv::Result::Error(hv::Status::AccessDenied, "denied");
    hv::VmxReadinessReport report;
    Check(rig.readiness.Evaluate(report).Succeeded() &&
              report.verdict == hv::VmxReadinessVerdict::Blocked &&
              fakes::HasReason(report.reasons, hv::VmxFailureReason::ProcessorNotChecked),
          "NOT_CHECKED processor: readiness is Blocked, not ReadyForReview");
    bool listed = false;
    for (const auto& condition : report.Unverified()) {
        if (condition.id == "vmx.processors") listed = true;
    }
    Check(listed, "NOT_CHECKED processor: listed among unverified conditions");
}

void TestCpuProbeFailureIsFailed() {
    fakes::VmxRig rig(4, true);
    rig.current_cpuid.FailAllWith(hv::Status::TransportError, "cpuid fault");
    hv::VmxReadinessReport report;
    const hv::Result result = rig.readiness.Evaluate(report);
    Check(!result.Succeeded() && report.verdict == hv::VmxReadinessVerdict::Failed &&
              fakes::HasReason(report.reasons, hv::VmxFailureReason::CpuProbeFailed),
          "CPU probe failure: Failed / CpuProbeFailed");
    Check(!rig.driver.connected, "CPU probe failure: driver connection is not leaked");
}

void TestExternalConnectionIsNotClosed() {
    fakes::VmxRig rig(2, false);
    rig.driver.connected = true;  // connected by someone else
    hv::VmxReadinessReport report;
    Check(rig.readiness.Evaluate(report).Succeeded() && rig.driver.connected &&
              rig.driver.disconnect_calls == 0 && !report.driver.connected_by_readiness,
          "external connection: never closed by the readiness manager");
}

void TestJsonSerialization() {
    Check(hv::JsonEscape("a\"b\\c\n\t\x01") == "a\\\"b\\\\c\\n\\t\\u0001",
          "JSON: escaping of quotes, backslashes and control characters");

    fakes::VmxRig rig(2, true);
    rig.processors.failing[1] = hv::Result::Error(hv::Status::TransportError, "refused \"here\"");
    hv::IntelVirtualizationBackend backend(rig.readiness, rig.diagnostics);
    static_cast<void>(backend.Initialize());
    hv::VmxDiagnosticReport report;
    Check(backend.RunDiagnostics(report).Succeeded(), "JSON: diagnostic report is produced");
    const std::string json = hv::FormatVmxReportJson(report);

    Check(fakes::Contains(json, "\"version\":\"0.5.0\"") &&
              fakes::Contains(json, "\"hypervisor_running\":false") &&
              fakes::Contains(json, "\"hardware_virtualization_started\":false"),
          "JSON: never claims a running hypervisor");
    Check(fakes::Contains(json, "\"state\":\"Blocked\"") &&
              fakes::Contains(json, "\"kernel_verification_required\":true"),
          "JSON: state and kernel verification flag");
    Check(fakes::Contains(json, "\"cpu_index\":0") && fakes::Contains(json, "\"cpu_index\":1") &&
              fakes::Contains(json, "\"vmx_capability\":\"CpuidReported\"") &&
              fakes::Contains(json, "\"check_status\":\"NOT_CHECKED\"") &&
              fakes::Contains(json, "\"error_code\":\"TransportError\""),
          "JSON: per-processor entries with NOT_CHECKED and error code");
    Check(fakes::Contains(json, "refused \\\"here\\\""), "JSON: message text is escaped");
    Check(fakes::Contains(json, "\"reason\":\"ProcessorNotChecked\"") &&
              fakes::Contains(json, "\"unverified\":["),
          "JSON: failure reasons and unverified list");

    int braces = 0;
    int brackets = 0;
    bool in_string = false;
    bool escaped = false;
    for (const char ch : json) {
        if (in_string) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') in_string = false;
            continue;
        }
        if (ch == '"') in_string = true;
        else if (ch == '{') ++braces;
        else if (ch == '}') --braces;
        else if (ch == '[') ++brackets;
        else if (ch == ']') --brackets;
    }
    Check(braces == 0 && brackets == 0 && !in_string && json.front() == '{' && json.back() == '}',
          "JSON: braces and brackets are balanced");

    const std::string text = hv::FormatVmxReportText(report);
    Check(fakes::Contains(text, "Hypervisor running: no") && fakes::Contains(text, "[CPU information]") &&
              fakes::Contains(text, "[Driver status]") && fakes::Contains(text, "NOT_CHECKED"),
          "text report: contains required sections and never claims a running hypervisor");
}

} // namespace

int main() {
    TestReadyForReview();
    TestCpuWithoutVmxIsNotSupported();
    TestUnsupportedPlatform();
    TestMissingDriver();
    TestProtocolMismatch();
    TestPlatformBackendFailure();
    TestDiagnosticQueryFailure();
    TestInconsistentDriverData();
    TestHypervisorAndVbsAreWarnings();
    TestNotCheckedProcessorBlocks();
    TestCpuProbeFailureIsFailed();
    TestExternalConnectionIsNotClosed();
    TestJsonSerialization();
    if (test::failures != 0) return EXIT_FAILURE;
    std::cout << "All VMX readiness tests passed\n";
    return EXIT_SUCCESS;
}
