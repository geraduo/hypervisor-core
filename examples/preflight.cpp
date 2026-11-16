// hv_preflight: prints the user-mode platform preflight report.
// Executes only CPUID (unprivileged). It does NOT touch MSRs, VMX, or the driver.
// Exit codes: 0 PendingKernelVerification (NOT "ready"), 1 probe failed, 2 usage,
//             10 UnsupportedPlatform, 11 VmxNotReported, 12 HypervisorPresentReviewRequired.
#include "hv/platform/preflight.hpp"
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    bool json = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--json") json = true;
        else { std::cerr << "Usage: hv_preflight [--json]\n"; return 2; }
    }
    hv::NativeCpuidProvider cpuid;
    hv::PreflightReport report;
    const auto result = hv::RunPlatformPreflight(cpuid, report);
    std::cout << (json ? hv::FormatReportJson(report) : hv::FormatReportText(report));
    if (!result.Succeeded()) { std::cerr << "Preflight error: " << result.message << '\n'; return 1; }
    switch (report.verdict) {
    case hv::PreflightVerdict::PendingKernelVerification: return 0;
    case hv::PreflightVerdict::UnsupportedPlatform: return 10;
    case hv::PreflightVerdict::VmxNotReported: return 11;
    case hv::PreflightVerdict::HypervisorPresentReviewRequired: return 12;
    case hv::PreflightVerdict::ProbeFailed: return 1;
    }
    return 1;
}
