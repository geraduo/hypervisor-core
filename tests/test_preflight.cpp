#include "hv/platform/fake_cpuid_provider.hpp"
#include "hv/platform/preflight.hpp"
#include "test_support.hpp"
#include <cstdlib>
#include <iostream>
#include <string>

using test::Check;
using hv::CheckStatus;
using hv::PreflightVerdict;

namespace {
constexpr std::uint32_t kVmxBit = 1u << 5;
constexpr std::uint32_t kHypervisorBit = 1u << 31;
constexpr std::uint32_t kSvmBit = 1u << 2;
constexpr std::uint32_t kExtendedLeaf0 = 0x80000000u;
constexpr std::uint32_t kExtendedLeaf1 = 0x80000001u;

hv::FakeCpuidProvider MakeCpu(const std::string& vendor, std::uint32_t leaf1_ecx,
                              std::uint32_t max_leaf = 0x16, std::uint32_t leaf1_eax = 0x000906EA) {
    hv::FakeCpuidProvider fake;
    fake.Set(0, 0, hv::FakeCpuidProvider::CpuVendorLeaf(max_leaf, vendor));
    fake.Set(1, 0, {leaf1_eax, 0, leaf1_ecx, 0});
    return fake;
}

void AddExtendedLeaves(hv::FakeCpuidProvider& fake, std::uint32_t max_leaf, std::uint32_t leaf1_ecx) {
    fake.Set(kExtendedLeaf0, 0, {max_leaf, 0, 0, 0});
    if (max_leaf >= kExtendedLeaf1) fake.Set(kExtendedLeaf1, 0, {0, 0, leaf1_ecx, 0});
}

const hv::PreflightCheck* Find(const hv::PreflightReport& r, const std::string& id) {
    for (const auto& c : r.checks) if (c.id == id) return &c;
    return nullptr;
}

void CheckKernelChecksNotChecked(const hv::PreflightReport& r) {
    for (const char* id : {"msr.ia32_feature_control", "msr.vmx_capabilities", "msr.ept_vpid_cap", "windows.hyperv_vbs"}) {
        const auto* check = Find(r, id);
        Check(check != nullptr && check->status == CheckStatus::NotChecked,
              std::string("kernel-only check remains NOT CHECKED: ") + id);
    }
}

void TestIntelWithAndWithoutVmx() {
    auto fake = MakeCpu("GenuineIntel", kVmxBit);
    hv::PreflightReport r;
    Check(hv::RunPlatformPreflight(fake, r).Succeeded(), "Intel + VMX: preflight succeeds");
    Check(r.cpu_vendor == hv::CpuVendor::Intel && r.cpu_vendor_string == "GenuineIntel",
          "Intel vendor decoded from EBX, EDX, ECX");
    Check(r.vmx_cpuid_bit && !r.hypervisor_present_bit, "Intel feature bits decoded");
    Check(r.verdict == PreflightVerdict::PendingKernelVerification,
          "Intel + VMX remains pending kernel verification, never ready");
    Check(r.family == 6 && r.model == 0x9E && r.stepping == 10,
          "signature 0x906EA decodes to family 6, model 0x9E, stepping 10");
    CheckKernelChecksNotChecked(r);
    Check(!fake.WasQueried(0x40000000u) && !fake.WasQueried(kExtendedLeaf0) &&
          !fake.WasQueried(kExtendedLeaf1), "Intel does not read unrelated hypervisor or AMD leaves");

    auto no_vmx = MakeCpu("GenuineIntel", 0);
    hv::PreflightReport no_vmx_report;
    Check(hv::RunPlatformPreflight(no_vmx, no_vmx_report).Succeeded(), "Intel without VMX: preflight succeeds");
    Check(no_vmx_report.verdict == PreflightVerdict::VmxNotReported, "Intel without VMX has VmxNotReported verdict");
    const auto* vmx = Find(no_vmx_report, "cpuid.01h.ecx5.vmx");
    Check(vmx != nullptr && vmx->status == CheckStatus::Fail, "Intel without VMX fails the VMX CPUID check");
}

void TestFamilyModelBoundaries() {
    const std::uint32_t family15_signature = (0xFu << 8) | (0xBu << 4) | 0xFu | (0xAu << 16) | (2u << 20);
    auto family15 = MakeCpu("GenuineIntel", kVmxBit, 1, family15_signature);
    hv::PreflightReport r15;
    Check(hv::RunPlatformPreflight(family15, r15).Succeeded() && r15.family == 17 &&
          r15.model == 0xAB && r15.stepping == 15, "family 0xF folds extended family and model fields");

    const std::uint32_t family5_signature = (5u << 8) | (3u << 4) | 7u | (0xFu << 16) | (0x55u << 20);
    auto family5 = MakeCpu("GenuineIntel", kVmxBit, 1, family5_signature);
    hv::PreflightReport r5;
    Check(hv::RunPlatformPreflight(family5, r5).Succeeded() && r5.family == 5 &&
          r5.model == 3 && r5.stepping == 7, "non-6/non-0xF families ignore extended family and model fields");
}

void TestAmdHygonAndUnknownCpu() {
    auto amd = MakeCpu("AuthenticAMD", kVmxBit);
    AddExtendedLeaves(amd, 0x80000008u, kSvmBit);
    hv::PreflightReport r;
    Check(hv::RunPlatformPreflight(amd, r).Succeeded(), "AMD CPUID probe succeeds");
    Check(r.cpu_vendor == hv::CpuVendor::Amd && r.svm_read && r.svm_cpuid_bit,
          "AMD SVM bit is read only after validating the extended maximum leaf");
    Check(!r.vmx_cpuid_bit && Find(r, "cpuid.01h.ecx5.vmx") == nullptr,
          "Intel VMX bit is not interpreted for AMD even if ECX[5] is set");
    Check(r.verdict == PreflightVerdict::UnsupportedPlatform, "AMD remains unsupported until an SVM backend exists");
    Check(amd.WasQueried(kExtendedLeaf0) && amd.WasQueried(kExtendedLeaf1), "AMD extended leaves queried in order");
    Check(amd.Calls().size() >= 4 && amd.Calls()[2].first == kExtendedLeaf0 &&
          amd.Calls()[3].first == kExtendedLeaf1, "AMD query order gates leaf 0x80000001 with leaf 0x80000000");

    auto hygon = MakeCpu("HygonGenuine", 0);
    AddExtendedLeaves(hygon, 0x80000001u, kSvmBit);
    hv::PreflightReport rh;
    Check(hv::RunPlatformPreflight(hygon, rh).Succeeded() && rh.cpu_vendor == hv::CpuVendor::Hygon &&
          rh.svm_read && rh.svm_cpuid_bit && rh.verdict == PreflightVerdict::UnsupportedPlatform,
          "Hygon is recognized and its SVM bit remains informational");

    auto unknown = MakeCpu("SomethingElse!", kVmxBit);
    hv::PreflightReport ru;
    Check(hv::RunPlatformPreflight(unknown, ru).Succeeded() && ru.cpu_vendor == hv::CpuVendor::Other &&
          !ru.vmx_cpuid_bit && ru.verdict == PreflightVerdict::UnsupportedPlatform,
          "unknown vendor is not mistaken for Intel when ECX[5] is set");
    Check(!unknown.WasQueried(kExtendedLeaf0), "unknown vendor does not probe AMD extended leaves");

    auto empty_vendor = MakeCpu("", kVmxBit);
    hv::PreflightReport empty_report;
    Check(hv::RunPlatformPreflight(empty_vendor, empty_report).Succeeded() &&
          empty_report.cpu_vendor == hv::CpuVendor::Unknown && !empty_report.vmx_cpuid_bit &&
          empty_report.verdict == PreflightVerdict::UnsupportedPlatform,
          "empty CPU vendor data remains Unknown and cannot claim Intel VMX");
}

void TestExtendedLeafAbsenceAndMalformedData() {
    auto no_max = MakeCpu("AuthenticAMD", 0);
    hv::PreflightReport no_max_report;
    Check(hv::RunPlatformPreflight(no_max, no_max_report).Succeeded() && !no_max_report.svm_read,
          "missing CPUID.80000000H does not fail the platform probe");
    Check(!no_max.WasQueried(kExtendedLeaf1), "missing extended maximum prevents querying CPUID.80000001H");
    const auto* no_max_check = Find(no_max_report, "cpuid.80000000h");
    Check(no_max_check != nullptr && no_max_check->status == CheckStatus::NotChecked,
          "missing extended maximum is represented as NOT CHECKED");

    auto no_leaf1 = MakeCpu("AuthenticAMD", 0);
    AddExtendedLeaves(no_leaf1, kExtendedLeaf0, kSvmBit);
    hv::PreflightReport no_leaf1_report;
    Check(hv::RunPlatformPreflight(no_leaf1, no_leaf1_report).Succeeded() && !no_leaf1_report.svm_read,
          "max extended leaf 0x80000000 means SVM leaf is absent");
    Check(!no_leaf1.WasQueried(kExtendedLeaf1), "CPUID.80000001H is not queried when not advertised");
    Check(Find(no_leaf1_report, "cpuid.80000001h")->status == CheckStatus::NotChecked,
          "unadvertised SVM leaf remains NOT CHECKED");

    auto malformed_ext = MakeCpu("AuthenticAMD", 0);
    AddExtendedLeaves(malformed_ext, 0x7FFFFFFFu, kSvmBit);
    hv::PreflightReport malformed_ext_report;
    Check(hv::RunPlatformPreflight(malformed_ext, malformed_ext_report).Succeeded() &&
          !malformed_ext_report.svm_read && !malformed_ext.WasQueried(kExtendedLeaf1),
          "malformed extended maximum is rejected without reading leaf 0x80000001");

    auto unavailable_advertised_leaf = MakeCpu("AuthenticAMD", 0);
    unavailable_advertised_leaf.Set(kExtendedLeaf0, 0, {kExtendedLeaf1, 0, 0, 0});
    hv::PreflightReport unavailable_report;
    const auto unavailable_result = hv::RunPlatformPreflight(unavailable_advertised_leaf, unavailable_report);
    Check(!unavailable_result.Succeeded() && unavailable_report.verdict == PreflightVerdict::ProbeFailed,
          "advertised but unreadable CPUID.80000001H is a source failure");
}

void TestHypervisorReporting() {
    auto microsoft = MakeCpu("GenuineIntel", kVmxBit | kHypervisorBit);
    microsoft.Set(0x40000000u, 0, hv::FakeCpuidProvider::HypervisorVendorLeaf(0x4000000Au, "Microsoft Hv"));
    hv::PreflightReport rm;
    Check(hv::RunPlatformPreflight(microsoft, rm).Succeeded(), "hypervisor-present probe succeeds");
    Check(rm.hypervisor_present_bit && rm.hypervisor_vendor == hv::HypervisorVendor::MicrosoftHyperV &&
          rm.max_hypervisor_leaf == 0x4000000Au, "Microsoft hypervisor vendor order and max leaf decode correctly");
    Check(rm.verdict == PreflightVerdict::HypervisorPresentReviewRequired,
          "hypervisor-present bit requests review and is not reported as a conflict");
    Check(Find(rm, "cpuid.01h.ecx31.hypervisor")->status == CheckStatus::Warn,
          "hypervisor-present bit is a warning, not a failure");

    auto unknown_vendor = MakeCpu("GenuineIntel", kVmxBit | kHypervisorBit);
    unknown_vendor.Set(0x40000000u, 0,
                       hv::FakeCpuidProvider::HypervisorVendorLeaf(0x40000001u, "MysteryVendor"));
    hv::PreflightReport ru;
    Check(hv::RunPlatformPreflight(unknown_vendor, ru).Succeeded() &&
          ru.hypervisor_vendor == hv::HypervisorVendor::Other &&
          Find(ru, "cpuid.40000000h.vendor")->status == CheckStatus::Warn,
          "unknown hypervisor vendor is retained and marked for review");

    auto absent_leaf = MakeCpu("GenuineIntel", kHypervisorBit);
    hv::PreflightReport ra;
    Check(hv::RunPlatformPreflight(absent_leaf, ra).Succeeded() &&
          ra.verdict == PreflightVerdict::HypervisorPresentReviewRequired &&
          ra.hypervisor_vendor == hv::HypervisorVendor::Other,
          "missing hypervisor vendor leaf does not erase the present-bit result");

    auto invalid_max = MakeCpu("GenuineIntel", kVmxBit | kHypervisorBit);
    invalid_max.Set(0x40000000u, 0,
                    hv::FakeCpuidProvider::HypervisorVendorLeaf(0x3FFFFFFFu, "Microsoft Hv"));
    hv::PreflightReport ri;
    Check(hv::RunPlatformPreflight(invalid_max, ri).Succeeded() &&
          ri.hypervisor_vendor == hv::HypervisorVendor::Other &&
          Find(ri, "cpuid.40000000h.vendor")->status == CheckStatus::Warn,
          "malformed hypervisor maximum leaf is not trusted");
}

void TestMissingBasicLeavesAndSourceErrors() {
    auto no_leaf1 = MakeCpu("GenuineIntel", kVmxBit, 0);
    hv::PreflightReport missing_report;
    Check(hv::RunPlatformPreflight(no_leaf1, missing_report).Succeeded() &&
          missing_report.verdict == PreflightVerdict::UnsupportedPlatform && !missing_report.leaf1_read,
          "missing CPUID.01H is unsupported and not interpreted as VMX absent");
    Check(!no_leaf1.WasQueried(1) && Find(missing_report, "cpuid.01h")->status == CheckStatus::NotChecked,
          "CPUID.01H is not queried or marked as a failed feature when not advertised");

    hv::FakeCpuidProvider no_cpuid;
    no_cpuid.FailAllWith(hv::Status::Unsupported, "CPUID unavailable");
    hv::PreflightReport unsupported_report;
    Check(hv::RunPlatformPreflight(no_cpuid, unsupported_report).Succeeded() &&
          unsupported_report.verdict == PreflightVerdict::UnsupportedPlatform,
          "unsupported CPUID source returns an unsupported platform report");

    hv::FakeCpuidProvider leaf0_error;
    leaf0_error.FailAllWith(hv::Status::InternalError, "source fault");
    hv::PreflightReport error_report;
    Check(!hv::RunPlatformPreflight(leaf0_error, error_report).Succeeded() &&
          error_report.verdict == PreflightVerdict::ProbeFailed,
          "CPUID leaf 0 source errors become ProbeFailed");

    auto leaf1_error = MakeCpu("GenuineIntel", kVmxBit);
    leaf1_error.FailOn(1, 0, hv::Status::InternalError, "leaf 1 read failed");
    hv::PreflightReport leaf1_error_report;
    Check(!hv::RunPlatformPreflight(leaf1_error, leaf1_error_report).Succeeded() &&
          leaf1_error_report.verdict == PreflightVerdict::ProbeFailed && !leaf1_error_report.vmx_cpuid_bit,
          "CPUID leaf 1 source errors do not produce a VMX claim");

    auto ext_error = MakeCpu("AuthenticAMD", 0);
    ext_error.FailOn(kExtendedLeaf0, 0, hv::Status::InternalError, "extended source fault");
    hv::PreflightReport ext_error_report;
    Check(!hv::RunPlatformPreflight(ext_error, ext_error_report).Succeeded() &&
          ext_error_report.verdict == PreflightVerdict::ProbeFailed,
          "errors while querying the extended maximum are surfaced");

    auto malformed_basic = MakeCpu("GenuineIntel", kVmxBit, 0xFFFFFFFFu);
    hv::PreflightReport malformed_report;
    Check(hv::RunPlatformPreflight(malformed_basic, malformed_report).Succeeded() &&
          malformed_report.verdict == PreflightVerdict::UnsupportedPlatform &&
          !malformed_basic.WasQueried(1), "reserved maximum basic leaf is rejected before further queries");
}

void TestJsonAndTextSerialization() {
    auto malformed_vendor = MakeCpu("Bad\x01\"Vend\\r!", 0);
    hv::PreflightReport r;
    Check(hv::RunPlatformPreflight(malformed_vendor, r).Succeeded(), "malformed vendor data still yields a report");
    Check(r.cpu_vendor == hv::CpuVendor::Other && r.cpu_vendor_string.find('\x01') == std::string::npos,
          "CPU vendor control bytes are sanitized and do not match a known vendor");
    CheckKernelChecksNotChecked(r);

    hv::PreflightReport json_report;
    json_report.verdict = PreflightVerdict::UnsupportedPlatform;
    json_report.summary = "quote: \" slash: \\ newline:\n tab:\t backspace:\b formfeed:\f control:\x01";
    json_report.cpu_vendor_string = "odd\\\"vendor";
    json_report.checks.push_back({"id\n", "title\"", CheckStatus::Warn, "detail\\\t"});
    const auto json = hv::FormatReportJson(json_report);
    Check(json.find("quote: \\\" slash: \\\\ newline:\\n tab:\\t backspace:\\b formfeed:\\f control:\\u0001") != std::string::npos,
          "JSON strings escape quotes, backslashes, common controls, and other control bytes");
    Check(json.find("\"max_leaf\": 0") != std::string::npos &&
          json.find("\"leaf0_read\": false") != std::string::npos,
          "JSON contains explicit extended-leaf availability data");
    Check(json.find("\"id\\n\"") != std::string::npos && json.find("\"title\\\"\"") != std::string::npos,
          "JSON escapes check IDs and titles");

    auto good = MakeCpu("GenuineIntel", kVmxBit);
    hv::PreflightReport good_report;
    Check(hv::RunPlatformPreflight(good, good_report).Succeeded(), "text report fixture succeeds");
    const auto text = hv::FormatReportText(good_report);
    Check(text.find("NOT CHECKED") != std::string::npos &&
          text.find("Kernel-mode verification required: yes") != std::string::npos,
          "text report keeps kernel-only checks explicit");
    Check(good_report.summary.find("NOT confirmed ready") != std::string::npos &&
          text.find(good_report.summary) != std::string::npos,
          "text report explicitly avoids a VMX-ready claim");
}

void TestNativeProvider() {
    hv::NativeCpuidProvider native;
    hv::PreflightReport r;
    const auto res = hv::RunPlatformPreflight(native, r);
    if (hv::NativeCpuidProvider::IsAvailableOnThisBuild()) {
        Check(res.Succeeded() && r.verdict != PreflightVerdict::ProbeFailed,
              "native CPUID probe succeeds on x86/x64");
        Check(!r.cpu_vendor_string.empty() && r.max_basic_leaf >= 1,
              "native CPUID vendor and leaf count are plausible");
        Check(r.kernel_verification_required, "native CPUID always requires kernel verification");
    } else {
        Check(res.Succeeded() && r.verdict == PreflightVerdict::UnsupportedPlatform,
              "native CPUID reports unsupported on non-x86");
    }
}
} // namespace

int main() {
    TestIntelWithAndWithoutVmx();
    TestFamilyModelBoundaries();
    TestAmdHygonAndUnknownCpu();
    TestExtendedLeafAbsenceAndMalformedData();
    TestHypervisorReporting();
    TestMissingBasicLeavesAndSourceErrors();
    TestJsonAndTextSerialization();
    TestNativeProvider();
    if (test::failures != 0) { std::cerr << test::failures << " test(s) failed\n"; return EXIT_FAILURE; }
    std::cout << "All preflight tests passed\n";
    return EXIT_SUCCESS;
}
