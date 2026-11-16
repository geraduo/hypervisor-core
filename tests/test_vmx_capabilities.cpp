#include "fakes/fake_vmx.hpp"
#include "test_support.hpp"

#include <cstdlib>
#include <iostream>

using test::Check;

namespace {

void TestVmxCapableCpu() {
    fakes::VmxRig rig(4, true);
    hv::VmxCapabilityReport report;
    const hv::Result result = rig.capability.Query(report);

    Check(result.Succeeded(), "VMX-capable CPU: query succeeds");
    Check(report.cpu_collected, "VMX-capable CPU: calling-processor data is collected");
    Check(report.verdict == hv::VmxCapabilityVerdict::CpuidReportedOnAllProcessors,
          "VMX-capable CPU: verdict is CpuidReportedOnAllProcessors");
    Check(report.processors.size() == 4 && report.processors_expected == 4 &&
              report.processors_checked == 4 && report.processors_vmx_reported == 4,
          "VMX-capable CPU: every logical processor has its own entry");
    bool all_ok = true;
    for (std::uint32_t i = 0; i < report.processors.size(); ++i) {
        const auto& p = report.processors[i];
        all_ok = all_ok && p.cpu_index == i && p.vendor == "GenuineIntel" &&
                 p.vmx_capability == hv::VmxCapabilityState::CpuidReported &&
                 p.check_status == hv::CheckStatus::Pass && p.error_code == hv::Status::Success;
    }
    Check(all_ok, "VMX-capable CPU: per-processor index, vendor, VMX, status and error code");
    Check(report.kernel_verification_required && report.reasons.empty(),
          "VMX-capable CPU: kernel verification remains required, no failure reasons");
    Check(rig.processors.query_calls > 0, "VMX-capable CPU: processors were probed individually");
}

void TestCpuWithoutVmx() {
    fakes::VmxRig rig(2, false);
    hv::VmxCapabilityReport report;
    Check(rig.capability.Query(report).Succeeded(), "CPU without VMX: query itself succeeds");
    Check(report.verdict == hv::VmxCapabilityVerdict::NotSupported,
          "CPU without VMX: verdict is NotSupported");
    Check(fakes::HasReason(report.reasons, hv::VmxFailureReason::VmxNotReported),
          "CPU without VMX: reason VmxNotReported is recorded");
    Check(report.processors.size() == 2 &&
              report.processors[0].vmx_capability == hv::VmxCapabilityState::NotReported &&
              report.processors[0].check_status == hv::CheckStatus::Fail &&
              report.processors[1].error_code == hv::Status::Unsupported,
          "CPU without VMX: per-processor result is NotReported / FAIL / Unsupported");
}

void TestUnsupportedVendor() {
    fakes::VmxRig rig(2, true);
    rig.current_cpuid = fakes::MakeAmdCpuid();
    rig.processors.cpus.assign(2, fakes::MakeAmdCpuid());
    hv::VmxCapabilityReport report;
    Check(rig.capability.Query(report).Succeeded(), "AMD CPU: query succeeds");
    Check(report.verdict == hv::VmxCapabilityVerdict::NotSupported,
          "AMD CPU: NotSupported (no SVM backend, Intel VT-x only)");
    Check(fakes::HasReason(report.reasons, hv::VmxFailureReason::UnsupportedVendor),
          "AMD CPU: reason UnsupportedVendor");
    Check(report.processors[0].vmx_capability == hv::VmxCapabilityState::UnsupportedVendor,
          "AMD CPU: per-processor UnsupportedVendor");
}

void TestProcessorsAreIndependent() {
    fakes::VmxRig rig(4, true);
    rig.processors.cpus[2] = fakes::MakeIntelCpuid(false);
    hv::VmxCapabilityReport report;
    Check(rig.capability.Query(report).Succeeded(), "mixed processors: query succeeds");
    Check(report.verdict == hv::VmxCapabilityVerdict::NotSupported,
          "mixed processors: one confirmed negative makes the system NotSupported");
    Check(report.processors[0].vmx_capability == hv::VmxCapabilityState::CpuidReported &&
              report.processors[1].vmx_capability == hv::VmxCapabilityState::CpuidReported &&
              report.processors[2].vmx_capability == hv::VmxCapabilityState::NotReported &&
              report.processors[3].vmx_capability == hv::VmxCapabilityState::CpuidReported,
          "mixed processors: the result of one processor is not propagated to the others");
    Check(report.processors_vmx_reported == 3, "mixed processors: VMX counted only where confirmed");
}

void TestNotCheckedProcessor() {
    fakes::VmxRig rig(4, true);
    rig.processors.failing[3] =
        hv::Result::Error(hv::Status::TransportError, "affinity change was refused");
    hv::VmxCapabilityReport report;
    Check(rig.capability.Query(report).Succeeded(), "NOT_CHECKED: query succeeds with partial data");
    Check(report.verdict == hv::VmxCapabilityVerdict::Inconclusive,
          "NOT_CHECKED: verdict is Inconclusive, never 'supported'");
    const auto& unchecked = report.processors[3];
    Check(unchecked.check_status == hv::CheckStatus::NotChecked &&
              unchecked.vmx_capability == hv::VmxCapabilityState::NotChecked &&
              unchecked.error_code == hv::Status::TransportError &&
              fakes::Contains(unchecked.diagnostic_result, "NOT_CHECKED"),
          "NOT_CHECKED: failed processor keeps NOT_CHECKED and its error code");
    Check(report.processors[0].check_status == hv::CheckStatus::Pass && report.processors_checked == 3,
          "NOT_CHECKED: other processors keep their own confirmed results");
    Check(fakes::HasReason(report.reasons, hv::VmxFailureReason::ProcessorNotChecked),
          "NOT_CHECKED: reason ProcessorNotChecked is recorded");
}

void TestCpuProbeFailure() {
    fakes::VmxRig rig(2, true);
    rig.current_cpuid.FailAllWith(hv::Status::TransportError, "cpuid fault");
    hv::VmxCapabilityReport report;
    const hv::Result result = rig.capability.Query(report);
    Check(!result.Succeeded() && result.status == hv::Status::TransportError,
          "CPU probe failure: error is returned with the original status");
    Check(!report.cpu_collected && report.processors.empty() &&
              fakes::HasReason(report.reasons, hv::VmxFailureReason::CpuProbeFailed),
          "CPU probe failure: nothing is concluded and CpuProbeFailed is recorded");
    Check(rig.processors.query_calls == 0, "CPU probe failure: no processor result is invented");
}

void TestEnumerationFailures() {
    {
        fakes::VmxRig rig(2, true);
        rig.processors.count_result = hv::Result::Error(hv::Status::InternalError, "enumeration fault");
        hv::VmxCapabilityReport report;
        const hv::Result result = rig.capability.Query(report);
        Check(!result.Succeeded() && report.verdict == hv::VmxCapabilityVerdict::Inconclusive &&
                  fakes::HasReason(report.reasons, hv::VmxFailureReason::ProcessorEnumerationFailed),
              "enumeration failure: error, Inconclusive, ProcessorEnumerationFailed");
    }
    {
        fakes::VmxRig rig(0, true);
        hv::VmxCapabilityReport report;
        Check(!rig.capability.Query(report).Succeeded() &&
                  fakes::HasReason(report.reasons, hv::VmxFailureReason::ProcessorEnumerationFailed),
              "zero processors is an enumeration failure, not a success");
    }
    {
        fakes::VmxRig rig(2, true);
        rig.processors.count_result = hv::Result::Error(hv::Status::Unsupported, "not Windows");
        hv::VmxCapabilityReport report;
        Check(rig.capability.Query(report).Succeeded() &&
                  report.verdict == hv::VmxCapabilityVerdict::NotSupported &&
                  fakes::HasReason(report.reasons, hv::VmxFailureReason::UnsupportedPlatform),
              "unsupported enumeration provider maps to NotSupported / UnsupportedPlatform");
    }
}

void TestRepeatedQueries() {
    fakes::VmxRig rig(3, true);
    hv::VmxCapabilityReport first;
    hv::VmxCapabilityReport second;
    Check(rig.capability.Query(first).Succeeded() && rig.capability.Query(second).Succeeded(),
          "repeated queries succeed");
    Check(first.verdict == second.verdict && first.processors.size() == second.processors.size() &&
              first.summary == second.summary,
          "repeated queries return identical results and do not accumulate state");
}

void TestNativeProviderOnThisHost() {
    hv::NativeLogicalProcessorCpuidProvider native;
    std::uint32_t count = 0;
    const hv::Result result = native.GetLogicalProcessorCount(count);
    // Runs on any host. Where the provider is available it must report a positive count; where it
    // is not (non-Windows) it must say Unsupported rather than invent processors.
    Check((result.Succeeded() && count > 0) || result.status == hv::Status::Unsupported,
          "native provider: positive processor count or explicit Unsupported");
}

} // namespace

int main() {
    TestVmxCapableCpu();
    TestCpuWithoutVmx();
    TestUnsupportedVendor();
    TestProcessorsAreIndependent();
    TestNotCheckedProcessor();
    TestCpuProbeFailure();
    TestEnumerationFailures();
    TestRepeatedQueries();
    TestNativeProviderOnThisHost();
    if (test::failures != 0) return EXIT_FAILURE;
    std::cout << "All VMX capability tests passed\n";
    return EXIT_SUCCESS;
}
