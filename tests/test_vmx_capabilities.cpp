#include "hv/virtualization/fake_vmx_providers.hpp"
#include "hv/virtualization/vmx_capability_manager.hpp"
#include "test_support.hpp"
#include "vmx_test_fixture.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using test::Check;

namespace {

const hv::PreflightCheck* FindCheck(const hv::VmxCapabilityReport& report,
                                   const std::string& id) {
    for (const auto& check : report.checks)
        if (check.id == id) return &check;
    return nullptr;
}

void TestSupportedOnEveryProcessor() {
    auto cpuid = vmx_test::MakeIntel(true);
    hv::FakeLogicalProcessorProbe probe;
    probe.SetObservations(hv::MakeProcessorSet(4, true));
    hv::VmxCapabilityManager manager(cpuid, &probe);
    hv::VmxCapabilityReport report;

    const hv::Result result = manager.Evaluate(report);
    Check(result.Succeeded(), "capability: supported evaluation succeeds");
    Check(report.status == hv::CheckStatus::Pass, "capability: supported status passes");
    Check(report.support == hv::VmxSupportLevel::Supported, "capability: support is Supported");
    Check(report.reason == hv::VmxBlockReason::None, "capability: supported has no block reason");
    Check(report.error_class == hv::VmxErrorClass::None,
          "capability: supported has no internal error class");
    Check(report.cpu_report_valid, "capability: CPU report is valid");
    Check(report.cpu.vendor == hv::CpuVendor::Intel, "capability: CPU vendor is Intel");
    Check(report.cpu.vmx_cpuid_bit, "capability: CPUID VMX bit is retained");
    Check(report.per_processor_checked && report.all_processors_checked,
          "capability: every logical processor was observed");
    Check(report.observed_processor_count == 4 && report.vmx_enabled_processor_count == 4,
          "capability: all four processors report VMX");
    Check(report.processors.size() == 4, "capability: per-processor entries are present");
    Check(!report.hypervisor_present, "capability: no hypervisor is reported");
    Check(!report.hardware_vmx_started, "capability: hardware VMX was not started");
    Check(report.kernel_verification_required,
          "capability: kernel verification is still required");
    Check(probe.ProbeCalls() == 1, "capability: the probe ran exactly once");
    const hv::PreflightCheck* processors = FindCheck(report, "vmx.processors");
    Check(processors != nullptr && processors->status == hv::CheckStatus::Pass,
          "capability: per-processor check passes");
    Check(report.summary.find("does not prove") != std::string::npos,
          "capability: summary does not claim VMXON readiness");
    Check(manager.GetLastError().empty(), "capability: no stale error is retained");
}

void TestUnsupportedVendor() {
    auto cpuid = vmx_test::MakeAmd();
    hv::FakeLogicalProcessorProbe probe;
    probe.SetObservations(hv::MakeProcessorSet(4, false));
    hv::VmxCapabilityManager manager(cpuid, &probe);
    hv::VmxCapabilityReport report;

    const hv::Result result = manager.Evaluate(report);
    Check(result.Succeeded(), "capability: AMD is a diagnostic verdict, not an error");
    Check(report.support == hv::VmxSupportLevel::NotSupported,
          "capability: AMD is not supported");
    Check(report.reason == hv::VmxBlockReason::UnsupportedCpuVendor,
          "capability: AMD reason is UnsupportedCpuVendor");
    Check(report.error_class == hv::VmxErrorClass::None,
          "capability: AMD is not an internal error");
    Check(report.status == hv::CheckStatus::Fail, "capability: AMD status fails");
    Check(report.processors.empty(), "capability: AMD is rejected before the processor probe");
    Check(probe.ProbeCalls() == 0, "capability: no per-processor probe is attempted for AMD");
    Check(report.summary.find("no implemented virtualization backend") != std::string::npos,
          "capability: summary states that the vendor backend is not implemented");
}

void TestVmxNotReported() {
    auto cpuid = vmx_test::MakeIntel(false);
    hv::FakeLogicalProcessorProbe probe;
    probe.SetObservations(hv::MakeProcessorSet(4, false));
    hv::VmxCapabilityManager manager(cpuid, &probe);
    hv::VmxCapabilityReport report;

    const hv::Result result = manager.Evaluate(report);
    Check(result.Succeeded(), "capability: missing VMX is a verdict, not an error");
    Check(report.support == hv::VmxSupportLevel::NotSupported,
          "capability: missing VMX is not supported");
    Check(report.reason == hv::VmxBlockReason::VmxNotReported,
          "capability: missing VMX reason is VmxNotReported");
    Check(report.observed_processor_count == 4 && report.vmx_enabled_processor_count == 0,
          "capability: no processor reports VMX");
}

void TestPartialVmxCoverage() {
    auto cpuid = vmx_test::MakeIntel(true);
    hv::FakeLogicalProcessorProbe probe;
    std::vector<hv::LogicalProcessorObservation> observations =
        hv::MakeProcessorSet(3, true);
    observations.push_back(hv::MakeProcessorObservation(3, false));
    probe.SetObservations(observations);
    hv::VmxCapabilityManager manager(cpuid, &probe);
    hv::VmxCapabilityReport report;

    Check(manager.Evaluate(report).Succeeded(), "capability: partial coverage evaluates");
    Check(report.support == hv::VmxSupportLevel::NotSupported,
          "capability: partial CPU coverage is not supported");
    Check(report.reason == hv::VmxBlockReason::VmxNotReported,
          "capability: partial coverage reason is VmxNotReported");
    Check(report.all_processors_checked, "capability: every processor was observed");
    Check(report.observed_processor_count == 4 && report.vmx_enabled_processor_count == 3,
          "capability: three of four processors report VMX");
    Check(report.processors.size() == 4 &&
              report.processors[3].status == hv::CheckStatus::Fail,
          "capability: the non-VMX processor is marked FAIL");
}

void TestHypervisorReviewRequired() {
    auto cpuid = vmx_test::MakeIntel(true, true);
    hv::FakeLogicalProcessorProbe probe;
    probe.SetObservations(hv::MakeProcessorSet(4, true, true));
    hv::VmxCapabilityManager manager(cpuid, &probe);
    hv::VmxCapabilityReport report;

    Check(manager.Evaluate(report).Succeeded(), "capability: hypervisor case evaluates");
    Check(report.support == hv::VmxSupportLevel::ReviewRequired,
          "capability: hypervisor presence requires review");
    Check(report.reason == hv::VmxBlockReason::HypervisorPresentReviewRequired,
          "capability: hypervisor reason is HypervisorPresentReviewRequired");
    Check(report.status == hv::CheckStatus::Warn,
          "capability: hypervisor case warns instead of passing");
    Check(report.hypervisor_present, "capability: hypervisor presence is retained");
    Check(report.summary.find("nothing was started") != std::string::npos,
          "capability: summary states that nothing was started");
}

void TestNoPerProcessorProbe() {
    auto cpuid = vmx_test::MakeIntel(true);
    hv::VmxCapabilityManager manager(cpuid, nullptr);
    hv::VmxCapabilityReport report;

    Check(manager.Evaluate(report).Succeeded(), "capability: no-probe case evaluates");
    Check(report.support == hv::VmxSupportLevel::NotChecked,
          "capability: no probe yields NOT_CHECKED");
    Check(report.reason == hv::VmxBlockReason::PerProcessorCheckIncomplete,
          "capability: no probe is explicitly incomplete");
    Check(report.status == hv::CheckStatus::NotChecked,
          "capability: no-probe status is NOT_CHECKED");
    Check(!report.per_processor_checked && !report.all_processors_checked,
          "capability: per-processor flags stay false");
    Check(report.observed_processor_count == 1 && report.processors.size() == 1,
          "capability: only the calling processor is recorded");
    Check(report.kernel_verification_required,
          "capability: kernel verification is still required");
}

void TestProbeFailure() {
    auto cpuid = vmx_test::MakeIntel(true);
    hv::FakeLogicalProcessorProbe probe;
    probe.FailWith(hv::Result::Error(hv::Status::AccessDenied,
                                     "test: affinity could not be set"));
    hv::VmxCapabilityManager manager(cpuid, &probe);
    hv::VmxCapabilityReport report;

    const hv::Result result = manager.Evaluate(report);
    Check(!result.Succeeded(), "capability: probe failure returns an error");
    Check(result.status == hv::Status::AccessDenied,
          "capability: probe error status is preserved");
    Check(report.support == hv::VmxSupportLevel::NotChecked,
          "capability: probe failure yields NOT_CHECKED");
    Check(report.reason == hv::VmxBlockReason::PerProcessorCheckIncomplete,
          "capability: probe failure reason is incomplete coverage");
    Check(report.error_class == hv::VmxErrorClass::CpuProbe,
          "capability: probe failure is classified as CpuProbe");
    Check(probe.ProbeCalls() == 1, "capability: failed probe was attempted once");
}

void TestMalformedProbeResults() {
    {
        auto cpuid = vmx_test::MakeIntel(true);
        hv::FakeLogicalProcessorProbe probe;
        probe.SetObservations({});
        probe.ClearFailure();
        hv::VmxCapabilityManager manager(cpuid, &probe);
        hv::VmxCapabilityReport report;

        const hv::Result result = manager.Evaluate(report);
        Check(!result.Succeeded() && result.status == hv::Status::InvalidArgument,
              "capability: empty observations are rejected");
        Check(report.reason == hv::VmxBlockReason::CpuidReportInvalid,
              "capability: empty observations map to CpuidReportInvalid");
        Check(report.error_class == hv::VmxErrorClass::CpuProbe,
              "capability: empty observations are a CpuProbe error");
    }
    {
        auto cpuid = vmx_test::MakeIntel(true);
        hv::FakeLogicalProcessorProbe probe;
        std::vector<hv::LogicalProcessorObservation> observations;
        observations.push_back(hv::MakeProcessorObservation(0, true));
        observations.push_back(hv::MakeProcessorObservation(0, true));
        probe.SetObservations(observations);
        hv::VmxCapabilityManager manager(cpuid, &probe);
        hv::VmxCapabilityReport report;

        const hv::Result result = manager.Evaluate(report);
        Check(!result.Succeeded() && result.status == hv::Status::InvalidArgument,
              "capability: duplicate processor indices are rejected");
        Check(report.reason == hv::VmxBlockReason::CpuidReportInvalid,
              "capability: duplicates map to CpuidReportInvalid");
        Check(report.support == hv::VmxSupportLevel::NotChecked,
              "capability: duplicate observations cannot be classified");
    }
}

void TestIndividualProcessorFailure() {
    auto cpuid = vmx_test::MakeIntel(true);
    hv::FakeLogicalProcessorProbe probe;
    std::vector<hv::LogicalProcessorObservation> observations;
    observations.push_back(hv::MakeProcessorObservation(0, true));
    hv::LogicalProcessorObservation failed = hv::MakeProcessorObservation(1, false);
    failed.cpuid_result =
        hv::Result::Error(hv::Status::AccessDenied, "test: CPUID failed on CPU 1");
    failed.detail = failed.cpuid_result.message;
    observations.push_back(failed);
    observations.push_back(hv::MakeProcessorObservation(2, true));
    probe.SetObservations(observations);
    hv::VmxCapabilityManager manager(cpuid, &probe);
    hv::VmxCapabilityReport report;

    Check(manager.Evaluate(report).Succeeded(),
          "capability: a single CPUID failure is not a fatal error");
    Check(report.support == hv::VmxSupportLevel::NotChecked,
          "capability: a single CPUID failure yields NOT_CHECKED");
    Check(report.reason == hv::VmxBlockReason::PerProcessorCheckIncomplete,
          "capability: single CPUID failure is incomplete coverage");
    Check(report.observed_processor_count == 2 && report.processors.size() == 3,
          "capability: failed processors are not counted as observed");
    Check(report.processors.size() == 3 &&
              report.processors[1].status == hv::CheckStatus::NotChecked,
          "capability: the failed processor is NOT_CHECKED, never FAIL");
    Check(!report.all_processors_checked,
          "capability: incomplete coverage is reported");
    Check(report.summary.find("cannot be established") != std::string::npos,
          "capability: summary explains the incomplete verdict");
}

void TestCpuProbeFailure() {
    hv::FakeCpuidProvider cpuid;
    cpuid.FailAllWith(hv::Status::AccessDenied, "test: CPUID is not available");
    hv::VmxCapabilityManager manager(cpuid, nullptr);
    hv::VmxCapabilityReport report;

    const hv::Result result = manager.Evaluate(report);
    Check(!result.Succeeded(), "capability: CPUID probe failure returns an error");
    Check(report.support == hv::VmxSupportLevel::NotChecked,
          "capability: CPUID probe failure is NOT_CHECKED");
    Check(report.reason == hv::VmxBlockReason::CpuidProbeFailed,
          "capability: CPUID probe failure reason is CpuidProbeFailed");
    Check(report.error_class == hv::VmxErrorClass::CpuProbe,
          "capability: CPUID probe failure is classified as CpuProbe");
    Check(!manager.GetLastError().empty(), "capability: CPUID probe error is retained");
}

void TestInvalidCpuReport() {
    {
        hv::CpuCapabilityReport cpu;
        cpu.vendor = hv::CpuVendor::Amd;
        cpu.vendor_string = "AuthenticAMD";
        cpu.leaf1_read = true;
        cpu.max_basic_leaf = 1;
        cpu.vmx_cpuid_bit = true;
        hv::FakeCpuidProvider cpuid;
        hv::VmxCapabilityManager manager(cpuid, nullptr);
        hv::VmxCapabilityReport report;

        const hv::Result result = manager.EvaluateFromCpuReport(cpu, report);
        Check(!result.Succeeded() && result.status == hv::Status::InvalidArgument,
              "capability: Intel VMX bit on a non-Intel vendor is rejected");
        Check(report.reason == hv::VmxBlockReason::CpuidReportInvalid,
              "capability: contradictory vendor report is CpuidReportInvalid");
        Check(FindCheck(report, "vmx.cpu_report") != nullptr,
              "capability: contradiction adds an explicit failing check");
    }
    {
        hv::CpuCapabilityReport cpu;
        cpu.vendor = hv::CpuVendor::Intel;
        cpu.vendor_string = "GenuineIntel";
        cpu.leaf1_read = false;
        cpu.max_basic_leaf = 0x16;
        hv::FakeCpuidProvider cpuid;
        hv::VmxCapabilityManager manager(cpuid, nullptr);
        hv::VmxCapabilityReport report;

        const hv::Result result = manager.EvaluateFromCpuReport(cpu, report);
        Check(!result.Succeeded() && result.status == hv::Status::InvalidArgument,
              "capability: a report without leaf 1 is rejected");
        Check(report.reason == hv::VmxBlockReason::CpuidReportInvalid,
              "capability: missing leaf 1 is CpuidReportInvalid");
    }
}

void TestFormatting() {
    hv::VmxCapabilityReport report;
    report.status = hv::CheckStatus::Pass;
    report.support = hv::VmxSupportLevel::Supported;
    report.reason = hv::VmxBlockReason::None;
    report.error_class = hv::VmxErrorClass::None;
    report.summary = "quote: \" slash: \\ newline:\n tab:\t control:\x01";
    report.cpu.vendor = hv::CpuVendor::Intel;
    report.cpu.vendor_string = "GenuineIntel";
    report.cpu.brand_string = "Test CPU";
    report.cpu.family = 6;
    report.cpu.model = 158;
    report.cpu.stepping = 10;
    report.cpu.vmx_cpuid_bit = true;
    report.cpu_report_valid = true;
    report.per_processor_checked = true;
    report.all_processors_checked = true;
    report.observed_processor_count = 1;
    report.vmx_enabled_processor_count = 1;
    hv::VmxProcessorCapability processor;
    processor.processor_index = 0;
    processor.status = hv::CheckStatus::Pass;
    processor.vmx_reported = true;
    processor.detail = "detail: \"quoted\" \\ line\n";
    report.processors.push_back(processor);
    report.checks.push_back(
        {"check\n", "title\"", hv::CheckStatus::Warn, "detail\\\t"});

    const std::string json = hv::FormatVmxCapabilityReportJson(report);
    Check(json.find("quote: \\\" slash: \\\\ newline:\\n tab:\\t control:\\u0001") !=
              std::string::npos,
          "capability JSON: summary text is escaped");
    Check(json.find("detail: \\\"quoted\\\" \\\\ line\\n") != std::string::npos,
          "capability JSON: processor detail is escaped");
    Check(json.find("check\\n") != std::string::npos &&
              json.find("title\\\"") != std::string::npos,
          "capability JSON: check identifiers are escaped");
    Check(json.find("\"support\": \"Supported\"") != std::string::npos,
          "capability JSON: support is serialized");
    Check(json.find("\"hardware_vmx_started\": false") != std::string::npos,
          "capability JSON: hardware VMX boundary is false");
    Check(json.find("\"kernel_verification_required\": true") != std::string::npos,
          "capability JSON: kernel verification is still required");

    const std::string text = hv::FormatVmxCapabilityReportText(report);
    Check(text.find("Hardware VMX started: no") != std::string::npos,
          "capability text: hardware boundary is explicit");
    Check(text.find("Kernel-mode verification required: yes") != std::string::npos,
          "capability text: kernel verification is explicit");
    Check(std::string(hv::ToString(hv::VmxSupportLevel::ReviewRequired)) == "ReviewRequired",
          "capability: support enum text is stable");
    Check(std::string(hv::ToString(hv::VmxBlockReason::CpuidReportInvalid)) ==
              "CpuidReportInvalid",
          "capability: reason enum text is stable");
}

} // namespace

int main() {
    TestSupportedOnEveryProcessor();
    TestUnsupportedVendor();
    TestVmxNotReported();
    TestPartialVmxCoverage();
    TestHypervisorReviewRequired();
    TestNoPerProcessorProbe();
    TestProbeFailure();
    TestMalformedProbeResults();
    TestIndividualProcessorFailure();
    TestCpuProbeFailure();
    TestInvalidCpuReport();
    TestFormatting();

    if (test::failures != 0) {
        std::cerr << test::failures << " VT-x capability test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All VT-x capability tests passed\n";
    return EXIT_SUCCESS;
}
