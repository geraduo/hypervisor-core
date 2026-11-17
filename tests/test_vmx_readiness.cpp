#include "hv/platform/kernel_driver_interface.hpp"
#include "hv/virtualization/vmx_readiness_manager.hpp"
#include "test_support.hpp"
#include "vmx_test_fixture.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

using test::Check;

namespace {

const hv::PreflightCheck* FindCheck(const hv::VmxReadinessReport& report,
                                   const std::string& id) {
    for (const auto& check : report.checks)
        if (check.id == id) return &check;
    return nullptr;
}

void TestReadyForReview() {
    vmx_test::ReadinessFixture fixture;
    Check(fixture.PrepareCapability(), "readiness: capability fixture evaluates");
    Check(fixture.capability.support == hv::VmxSupportLevel::Supported,
          "readiness: fixture CPU is supported");

    hv::VmxReadinessReport report;
    Check(fixture.Evaluate(report), "readiness: clean evaluation succeeds");
    Check(report.state == hv::VmxReadinessState::ReadyForReview,
          "readiness: clean environment is ReadyForReview");
    Check(report.status == hv::CheckStatus::Pass, "readiness: clean status passes");
    Check(report.reason == hv::VmxBlockReason::None, "readiness: clean case has no block reason");
    Check(report.error_class == hv::VmxErrorClass::None,
          "readiness: clean case has no internal error");
    Check(report.vmx_support == hv::VmxSupportLevel::Supported,
          "readiness: VT-x support is retained");
    Check(report.platform_state == hv::PlatformState::Initialized,
          "readiness: platform backend is initialized");
    Check(report.compatibility_verdict == hv::CompatibilityVerdict::NoConflictDetected,
          "readiness: compatibility has no conflict");
    Check(report.driver.connected && report.driver.query_succeeded,
          "readiness: driver query succeeds");
    Check(report.driver.protocol_version_checked &&
              report.driver.protocol_version_compatible,
          "readiness: protocol version is checked and compatible");
    Check(report.driver.structure_layout_checked &&
              report.driver.structure_layout_compatible,
          "readiness: response layout is checked and compatible");
    Check(report.driver.driver_version_checked && report.driver.driver_version_compatible,
          "readiness: driver version is checked and compatible");
    Check(report.driver.architecture_checked && report.driver.architecture_compatible,
          "readiness: driver architecture is checked and compatible");
    Check(report.driver.processor_count_checked && report.driver.processor_count_matches,
          "readiness: processor counts match");
    Check(report.kernel_verification_required,
          "readiness: kernel verification is still required");
    Check(!report.hardware_vmx_started && !report.hardware_start_supported,
          "readiness: hardware VMX is not started or supported");
    const hv::PreflightCheck* vmxon = FindCheck(report, "readiness.kernel.vmxon");
    Check(vmxon != nullptr && vmxon->status == hv::CheckStatus::NotChecked,
          "readiness: VMXON check stays NOT_CHECKED");
    const hv::PreflightCheck* feature =
        FindCheck(report, "readiness.kernel.feature_control");
    Check(feature != nullptr && feature->status == hv::CheckStatus::NotChecked,
          "readiness: IA32_FEATURE_CONTROL stays NOT_CHECKED");
    Check(report.summary.find("kernel-stage review only") != std::string::npos,
          "readiness: summary limits the verdict to a review");
    Check(report.summary.find("no VMX instruction") != std::string::npos,
          "readiness: summary states that no VMX instruction ran");

    const std::string json = hv::FormatVmxReadinessReportJson(report);
    Check(json.find("\"state\": \"ReadyForReview\"") != std::string::npos,
          "readiness JSON: state is serialized");
    Check(json.find("\"hardware_vmx_started\": false") != std::string::npos,
          "readiness JSON: hardware boundary is false");
    Check(json.find("\"hardware_start_supported\": false") != std::string::npos,
          "readiness JSON: hardware support is false");
    Check(json.find("\"status\": \"NOT CHECKED\"") != std::string::npos,
          "readiness JSON: unverified checks are NOT CHECKED");
    Check(json.find("\"driver_version_compatible\": true") != std::string::npos,
          "readiness JSON: driver compatibility is serialized");

    const std::string text = hv::FormatVmxReadinessReportText(report);
    Check(text.find("Hardware VMX started: no") != std::string::npos,
          "readiness text: hardware boundary is explicit");
    Check(text.find("minimum 0.4") != std::string::npos,
          "readiness text: minimum compatible driver version is explicit");
}

void TestMissingDriverIsBlocked() {
    vmx_test::ReadinessFixture fixture;
    Check(fixture.PrepareCapability(), "driver-missing: capability fixture evaluates");
    fixture.driver.SetConnectResult(
        hv::Result::Error(hv::Status::NotFound, "test: device interface is missing"));

    hv::VmxReadinessReport report;
    Check(fixture.Evaluate(report), "driver-missing: result is a verdict, not an error");
    Check(report.state == hv::VmxReadinessState::Blocked,
          "driver-missing: readiness is Blocked");
    Check(report.reason == hv::VmxBlockReason::DriverNotConnected,
          "driver-missing: reason is DriverNotConnected");
    Check(report.error_class == hv::VmxErrorClass::None,
          "driver-missing: no internal error is reported");
    Check(report.status == hv::CheckStatus::Warn, "driver-missing: status warns");
    Check(report.driver.query_attempted && !report.driver.connected,
          "driver-missing: the connection attempt is recorded");
    Check(!report.driver.protocol_version_checked,
          "driver-missing: protocol compatibility is NOT_CHECKED");
    Check(!report.driver.structure_layout_checked &&
              !report.driver.driver_version_checked &&
              !report.driver.architecture_checked,
          "driver-missing: every driver comparison remains unchecked");
    const std::string json = hv::FormatVmxReadinessReportJson(report);
    Check(json.find("\"protocol_version_checked\": false") != std::string::npos,
          "driver-missing JSON: protocol check is false");
    Check(json.find("\"reported_protocol_version\": 0") != std::string::npos,
          "driver-missing JSON: no protocol version is invented");
}

void TestDriverAccessDenied() {
    vmx_test::ReadinessFixture fixture;
    Check(fixture.PrepareCapability(), "driver-denied: capability fixture evaluates");
    fixture.driver.SetConnectResult(
        hv::Result::Error(hv::Status::AccessDenied, "test: access denied"));

    hv::VmxReadinessReport report;
    Check(fixture.Evaluate(report), "driver-denied: result is a verdict");
    Check(report.state == hv::VmxReadinessState::Blocked,
          "driver-denied: readiness is Blocked");
    Check(report.reason == hv::VmxBlockReason::DriverAccessDenied,
          "driver-denied: reason is DriverAccessDenied");
    Check(report.error_class == hv::VmxErrorClass::None,
          "driver-denied: access denial is not an internal error");
}

void TestDriverQueryFailureIsInternal() {
    vmx_test::ReadinessFixture fixture;
    Check(fixture.PrepareCapability(), "driver-query: capability fixture evaluates");
    fixture.driver.SetQueryResult(
        hv::Result::Error(hv::Status::InitializationFailed, "test: IOCTL failed"));

    hv::VmxReadinessReport report;
    const hv::Result result = fixture.readiness.Evaluate(fixture.capability, report);
    Check(!result.Succeeded(), "driver-query: unexpected IOCTL failure is an error");
    Check(report.state == hv::VmxReadinessState::Failed,
          "driver-query: readiness is Failed");
    Check(report.reason == hv::VmxBlockReason::DriverQueryFailed,
          "driver-query: reason is DriverQueryFailed");
    Check(report.error_class == hv::VmxErrorClass::Driver,
          "driver-query: error class is Driver");
    Check(report.status == hv::CheckStatus::Fail, "driver-query: status fails");
}

void TestProtocolMismatch() {
    vmx_test::ReadinessFixture fixture;
    Check(fixture.PrepareCapability(), "protocol: capability fixture evaluates");
    hv::FakeDriverResponseOverride override;
    override.override_protocol_version = true;
    override.protocol_version = hv::kGeraduoProtocolVersion + 1u;
    fixture.driver.SetResponseOverride(override);

    hv::VmxReadinessReport report;
    Check(fixture.Evaluate(report), "protocol: mismatch is a diagnostic verdict");
    Check(report.state == hv::VmxReadinessState::Blocked,
          "protocol: mismatch blocks readiness");
    Check(report.reason == hv::VmxBlockReason::DriverProtocolMismatch,
          "protocol: reason is DriverProtocolMismatch");
    Check(report.error_class == hv::VmxErrorClass::Protocol,
          "protocol: error class is Protocol");
    Check(report.driver.protocol_version_checked &&
              !report.driver.protocol_version_compatible,
          "protocol: version is checked and incompatible");
    Check(report.driver.reported_protocol_version == hv::kGeraduoProtocolVersion + 1u,
          "protocol: reported version is retained");
}

void TestDriverQueryReportsProtocolMismatch() {
    vmx_test::ReadinessFixture fixture;
    Check(fixture.PrepareCapability(), "protocol-status: capability fixture evaluates");
    fixture.driver.SetQueryResult(hv::Result::Error(
        hv::Status::Unsupported, "test: protocol version mismatch"));

    hv::VmxReadinessReport report;
    Check(fixture.Evaluate(report), "protocol-status: mismatch is a diagnostic verdict");
    Check(report.state == hv::VmxReadinessState::Blocked,
          "protocol-status: mismatch blocks readiness");
    Check(report.reason == hv::VmxBlockReason::DriverProtocolMismatch,
          "protocol-status: reason is DriverProtocolMismatch");
    Check(report.error_class == hv::VmxErrorClass::Protocol,
          "protocol-status: error class is Protocol");
    Check(!report.driver.query_succeeded,
          "protocol-status: the query is not marked successful");
}

void TestStructureLayoutMismatch() {
    vmx_test::ReadinessFixture fixture;
    Check(fixture.PrepareCapability(), "layout: capability fixture evaluates");
    hv::FakeDriverResponseOverride override;
    override.override_structure_size = true;
    override.structure_size =
        static_cast<std::uint32_t>(sizeof(hv::DriverPlatformInfoResponse)) - 4u;
    fixture.driver.SetResponseOverride(override);

    hv::VmxReadinessReport report;
    Check(fixture.Evaluate(report), "layout: mismatch is a diagnostic verdict");
    Check(report.state == hv::VmxReadinessState::Blocked,
          "layout: mismatch blocks readiness");
    Check(report.reason == hv::VmxBlockReason::DriverProtocolMismatch,
          "layout: reason is DriverProtocolMismatch");
    Check(report.driver.structure_layout_checked &&
              !report.driver.structure_layout_compatible,
          "layout: layout is checked and incompatible");
    Check(report.driver.expected_response_size ==
              static_cast<std::uint32_t>(sizeof(hv::DriverPlatformInfoResponse)),
          "layout: expected response size is retained");
}

void TestArchitectureMismatch() {
    vmx_test::ReadinessFixture fixture;
    Check(fixture.PrepareCapability(), "arch: capability fixture evaluates");
    hv::FakeDriverResponseOverride override;
    override.override_architecture = true;
    override.architecture = static_cast<std::uint32_t>(hv::DriverArchitecture::X86);
    fixture.driver.SetResponseOverride(override);

    hv::VmxReadinessReport report;
    Check(fixture.Evaluate(report), "arch: mismatch is a diagnostic verdict");
    Check(report.state == hv::VmxReadinessState::Blocked,
          "arch: mismatch blocks readiness");
    Check(report.reason == hv::VmxBlockReason::DriverArchitectureMismatch,
          "arch: reason is DriverArchitectureMismatch");
    Check(report.error_class == hv::VmxErrorClass::Protocol,
          "arch: error class is Protocol");
    Check(report.driver.architecture_checked && !report.driver.architecture_compatible,
          "arch: architecture is checked and incompatible");
}

void TestDriverVersionCompatibility() {
    {
        vmx_test::ReadinessFixture fixture;
        Check(fixture.PrepareCapability(), "version-old: capability fixture evaluates");
        hv::FakeDriverResponseOverride override;
        override.override_driver_version = true;
        override.driver_version_major = 0;
        override.driver_version_minor = 3;
        override.driver_version_patch = 9;
        fixture.driver.SetResponseOverride(override);

        hv::VmxReadinessReport report;
        Check(fixture.Evaluate(report), "version-old: verdict is returned");
        Check(report.state == hv::VmxReadinessState::Blocked,
              "version-old: too old driver blocks readiness");
        Check(report.reason == hv::VmxBlockReason::DriverVersionIncompatible,
              "version-old: reason is DriverVersionIncompatible");
        Check(report.error_class == hv::VmxErrorClass::Protocol,
              "version-old: error class is Protocol");
        Check(report.driver.driver_version_checked &&
                  !report.driver.driver_version_compatible,
              "version-old: version is checked and incompatible");
    }
    {
        vmx_test::ReadinessFixture fixture;
        Check(fixture.PrepareCapability(), "version-0.4: capability fixture evaluates");
        hv::FakeDriverResponseOverride override;
        override.override_driver_version = true;
        override.driver_version_major = 0;
        override.driver_version_minor = 4;
        override.driver_version_patch = 0;
        fixture.driver.SetResponseOverride(override);

        hv::VmxReadinessReport report;
        Check(fixture.Evaluate(report), "version-0.4: verdict is returned");
        Check(report.state == hv::VmxReadinessState::ReadyForReview,
              "version-0.4: a 0.4.x driver remains protocol compatible");
        Check(report.driver.driver_version_compatible,
              "version-0.4: compatibility flag is set");
    }
}

void TestProcessorCountMismatchIsWarning() {
    vmx_test::ReadinessFixture fixture;
    Check(fixture.PrepareCapability(), "cpu-count: capability fixture evaluates");
    hv::FakeDriverResponseOverride override;
    override.override_processor_count = true;
    override.active_processor_count = 8;
    fixture.driver.SetResponseOverride(override);

    hv::VmxReadinessReport report;
    Check(fixture.Evaluate(report), "cpu-count: verdict is returned");
    Check(report.state == hv::VmxReadinessState::ReadyForReview,
          "cpu-count: mismatch does not block review");
    Check(report.driver.status == hv::CheckStatus::Warn,
          "cpu-count: driver check warns");
    Check(report.driver.processor_count_checked && !report.driver.processor_count_matches,
          "cpu-count: mismatch is recorded");
    Check(report.reason == hv::VmxBlockReason::None,
          "cpu-count: mismatch is not a blocking reason");
}

void TestUnsupportedCpu() {
    vmx_test::ReadinessFixture fixture;
    fixture.cpuid = vmx_test::MakeAmd();
    fixture.processor_probe.SetObservations({});
    Check(fixture.PrepareCapability(), "readiness-amd: capability fixture evaluates");

    hv::VmxReadinessReport report;
    Check(fixture.Evaluate(report), "readiness-amd: unsupported CPU is not an error");
    Check(report.state == hv::VmxReadinessState::NotSupported,
          "readiness-amd: readiness is NotSupported");
    Check(report.reason == hv::VmxBlockReason::UnsupportedCpuVendor,
          "readiness-amd: reason is UnsupportedCpuVendor");
    Check(report.error_class == hv::VmxErrorClass::None,
          "readiness-amd: unsupported CPU has no internal error");
    Check(report.status == hv::CheckStatus::Fail, "readiness-amd: status fails");
}

void TestCapabilityNotCheckedBlocks() {
    vmx_test::ReadinessFixture fixture;
    hv::VmxCapabilityReport capability;
    capability.status = hv::CheckStatus::NotChecked;
    capability.support = hv::VmxSupportLevel::NotChecked;
    capability.reason = hv::VmxBlockReason::PerProcessorCheckIncomplete;
    capability.summary = "test: per-processor coverage is incomplete";

    hv::VmxReadinessReport report;
    Check(fixture.readiness.Evaluate(capability, report).Succeeded(),
          "readiness-notchecked: verdict is returned");
    Check(report.state == hv::VmxReadinessState::Blocked,
          "readiness-notchecked: readiness is Blocked");
    Check(report.reason == hv::VmxBlockReason::PerProcessorCheckIncomplete,
          "readiness-notchecked: reason is PerProcessorCheckIncomplete");
    Check(report.error_class == hv::VmxErrorClass::None,
          "readiness-notchecked: incomplete coverage is not an internal error");
}

void TestHypervisorCapabilityRequiresReview() {
    vmx_test::ReadinessFixture fixture;
    fixture.cpuid = vmx_test::MakeIntel(true, true);
    fixture.processor_probe.SetObservations(hv::MakeProcessorSet(4, true, true));
    Check(fixture.PrepareCapability(), "readiness-hypervisor: capability fixture evaluates");
    Check(fixture.capability.support == hv::VmxSupportLevel::ReviewRequired,
          "readiness-hypervisor: capability requires review");

    hv::VmxReadinessReport report;
    Check(fixture.Evaluate(report), "readiness-hypervisor: verdict is returned");
    Check(report.state == hv::VmxReadinessState::Blocked,
          "readiness-hypervisor: readiness is Blocked");
    Check(report.reason == hv::VmxBlockReason::HypervisorPresentReviewRequired,
          "readiness-hypervisor: reason is HypervisorPresentReviewRequired");
}

void TestUnsupportedPlatformWinsOverBlocked() {
    vmx_test::ReadinessFixture fixture;
    Check(fixture.PrepareCapability(), "precedence-unsupported: capability fixture evaluates");
    hv::PlatformProviderSnapshot snapshot = vmx_test::MakePlatformSnapshot();
    snapshot.os_version = {6, 1, 7601, 0};
    snapshot.os_name = "Windows 7";
    fixture.platform_provider.SetSnapshot(snapshot);
    fixture.driver.SetConnectResult(
        hv::Result::Error(hv::Status::NotFound, "test: device interface is missing"));

    hv::VmxReadinessReport report;
    Check(fixture.Evaluate(report), "precedence-unsupported: verdict is returned");
    Check(report.state == hv::VmxReadinessState::NotSupported,
          "precedence-unsupported: unsupported platform wins over a blocked driver");
    Check(report.reason == hv::VmxBlockReason::PlatformUnsupported,
          "precedence-unsupported: reason is PlatformUnsupported");
    Check(report.error_class == hv::VmxErrorClass::None,
          "precedence-unsupported: no internal error is reported");
}

void TestPlatformFailureIsInternal() {
    vmx_test::ReadinessFixture fixture;
    Check(fixture.PrepareCapability(), "platform-fail: capability fixture evaluates");
    fixture.platform_provider.FailWith(hv::Status::InitializationFailed,
                                       "test: platform probe failed");
    fixture.driver.SetConnectResult(
        hv::Result::Error(hv::Status::NotFound, "test: device interface is missing"));

    hv::VmxReadinessReport report;
    const hv::Result result = fixture.readiness.Evaluate(fixture.capability, report);
    Check(!result.Succeeded(), "platform-fail: internal failure returns an error");
    Check(report.state == hv::VmxReadinessState::Failed,
          "platform-fail: internal failure wins over blocked readiness");
    Check(report.reason == hv::VmxBlockReason::PlatformBackendFailed,
          "platform-fail: reason is PlatformBackendFailed");
    Check(report.error_class == hv::VmxErrorClass::PlatformProbe,
          "platform-fail: error class is PlatformProbe");
    Check(report.status == hv::CheckStatus::Fail, "platform-fail: status fails");
}

void TestCompatibilityFailureIsInternal() {
    vmx_test::ReadinessFixture fixture;
    hv::VmxCapabilityReport capability;
    capability.status = hv::CheckStatus::Pass;
    capability.support = hv::VmxSupportLevel::Supported;
    capability.reason = hv::VmxBlockReason::None;
    capability.summary = "test: all observed logical processors report Intel VMX";
    // The compatibility manager runs its own CPUID preflight. Failing the CPUID
    // provider simulates that internal evaluation failure while the capability
    // report supplied above remains a supported verdict.
    fixture.cpuid.FailAllWith(hv::Status::AccessDenied, "test: compatibility CPUID failed");

    hv::VmxReadinessReport report;
    const hv::Result result = fixture.readiness.Evaluate(capability, report);
    Check(!result.Succeeded(), "compat-fail: internal failure returns an error");
    Check(report.state == hv::VmxReadinessState::Failed,
          "compat-fail: readiness is Failed");
    Check(report.reason == hv::VmxBlockReason::CompatibilityEvaluationFailed,
          "compat-fail: reason is CompatibilityEvaluationFailed");
    Check(report.error_class == hv::VmxErrorClass::PlatformProbe,
          "compat-fail: error class is PlatformProbe");
}

void TestCompatibilityEnvironmentFailureBlocks() {
    vmx_test::ReadinessFixture fixture;
    Check(fixture.PrepareCapability(), "compat-env-fail: capability fixture evaluates");
    fixture.environment.FailWith(hv::Status::AccessDenied, "test: registry query failed");

    hv::VmxReadinessReport report;
    Check(fixture.Evaluate(report),
          "compat-env-fail: an environment probe failure is a verdict, not an error");
    Check(report.state == hv::VmxReadinessState::Blocked,
          "compat-env-fail: unknown VBS/HVCI state blocks review");
    Check(report.reason == hv::VmxBlockReason::CompatibilityReviewRequired,
          "compat-env-fail: reason is CompatibilityReviewRequired");
    Check(report.error_class == hv::VmxErrorClass::None,
          "compat-env-fail: no internal error is reported");
}

void TestDisabledProbesStayNotChecked() {
    vmx_test::ReadinessFixture fixture;
    Check(fixture.PrepareCapability(), "probes-off: capability fixture evaluates");
    hv::VmxReadinessOptions options;
    options.query_platform = false;
    options.query_driver = false;
    options.query_compatibility = false;
    fixture.readiness.SetOptions(options);

    hv::VmxReadinessReport report;
    Check(fixture.readiness.Evaluate(fixture.capability, report).Succeeded(),
          "probes-off: verdict is returned");
    Check(report.state == hv::VmxReadinessState::Blocked,
          "probes-off: readiness is Blocked");
    Check(report.reason == hv::VmxBlockReason::PlatformNotChecked,
          "probes-off: first block reason is PlatformNotChecked");
    Check(report.driver.status == hv::CheckStatus::NotChecked,
          "probes-off: driver status is NOT_CHECKED");
    Check(!report.driver.query_attempted,
          "probes-off: the driver is not contacted");
    Check(report.driver.detail.find("NOT_CHECKED") != std::string::npos,
          "probes-off: driver detail says NOT_CHECKED");
    Check(fixture.driver.ConnectCalls() == 0,
          "probes-off: no driver connection attempt was made");

    const std::string json = hv::FormatVmxReadinessReportJson(report);
    Check(json.find("\"reason\": \"PlatformNotChecked\"") != std::string::npos,
          "probes-off JSON: reason is serialized");
    Check(json.find("\"protocol_version_checked\": false") != std::string::npos,
          "probes-off JSON: protocol compatibility is NOT_CHECKED");
}

void TestFormatting() {
    vmx_test::ReadinessFixture fixture;
    Check(fixture.PrepareCapability(), "formatting: capability fixture evaluates");
    hv::VmxReadinessReport report;
    Check(fixture.Evaluate(report), "formatting: readiness fixture evaluates");

    report.summary = "quote: \" slash: \\ newline:\n tab:\t control:\x01";
    report.driver.detail = "driver detail: \"quoted\" \\ slash\n";
    report.checks.push_back({"check\n", "title\"", hv::CheckStatus::Warn, "detail\\\t"});

    const std::string json = hv::FormatVmxReadinessReportJson(report);
    Check(json.find("quote: \\\" slash: \\\\ newline:\\n tab:\\t control:\\u0001") !=
              std::string::npos,
          "readiness JSON: summary is escaped");
    Check(json.find("driver detail: \\\"quoted\\\" \\\\ slash\\n") != std::string::npos,
          "readiness JSON: driver detail is escaped");
    Check(json.find("check\\n") != std::string::npos &&
              json.find("title\\\"") != std::string::npos,
          "readiness JSON: check identifiers are escaped");
    Check(json.find("\"hardware_vmx_started\": false") != std::string::npos,
          "readiness JSON: hardware VMX boundary is false");
    Check(json.find("\"kernel_verification_required\": true") != std::string::npos,
          "readiness JSON: kernel verification is required");

    const std::string text = hv::FormatVmxReadinessReportText(report);
    Check(text.find("Driver compatibility:") != std::string::npos,
          "readiness text: driver compatibility section is present");
    Check(text.find("Hardware VMX started: no") != std::string::npos,
          "readiness text: hardware boundary is explicit");
    Check(std::string(hv::ToString(hv::VmxReadinessState::ReadyForReview)) ==
              "ReadyForReview",
          "readiness: state enum text is stable");
    Check(std::string(hv::ToString(hv::VmxErrorClass::Protocol)) == "Protocol",
          "readiness: error class enum text is stable");
}

} // namespace

int main() {
    TestReadyForReview();
    TestMissingDriverIsBlocked();
    TestDriverAccessDenied();
    TestDriverQueryFailureIsInternal();
    TestProtocolMismatch();
    TestDriverQueryReportsProtocolMismatch();
    TestStructureLayoutMismatch();
    TestArchitectureMismatch();
    TestDriverVersionCompatibility();
    TestProcessorCountMismatchIsWarning();
    TestUnsupportedCpu();
    TestCapabilityNotCheckedBlocks();
    TestHypervisorCapabilityRequiresReview();
    TestUnsupportedPlatformWinsOverBlocked();
    TestPlatformFailureIsInternal();
    TestCompatibilityFailureIsInternal();
    TestCompatibilityEnvironmentFailureBlocks();
    TestDisabledProbesStayNotChecked();
    TestFormatting();

    if (test::failures != 0) {
        std::cerr << test::failures << " VT-x readiness test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All VT-x readiness tests passed\n";
    return EXIT_SUCCESS;
}
