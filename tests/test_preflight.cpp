#include "hv/platform/fake_cpuid_provider.hpp"
#include "hv/platform/preflight.hpp"
#include "test_support.hpp"
#include <cstdlib>

using test::Check;
using hv::CheckStatus;
using hv::PreflightVerdict;

namespace {
constexpr std::uint32_t kVmxBit = 1u << 5;
constexpr std::uint32_t kHypervisorBit = 1u << 31;

hv::FakeCpuidProvider MakeCpu(const std::string& vendor, std::uint32_t leaf1_ecx,
                              std::uint32_t max_leaf = 0x16, std::uint32_t leaf1_eax = 0x000906EA) {
    hv::FakeCpuidProvider fake;
    fake.Set(0, 0, hv::FakeCpuidProvider::CpuVendorLeaf(max_leaf, vendor));
    fake.Set(1, 0, {leaf1_eax, 0, leaf1_ecx, 0});
    return fake;
}

const hv::PreflightCheck* Find(const hv::PreflightReport& r, const std::string& id) {
    for (const auto& c : r.checks) if (c.id == id) return &c;
    return nullptr;
}

void TestIntelWithVmx() {
    auto fake = MakeCpu("GenuineIntel", kVmxBit);
    hv::PreflightReport r;
    Check(hv::RunPlatformPreflight(fake, r).Succeeded(), "intel+vmx: preflight succeeds");
    Check(r.cpu_vendor == hv::CpuVendor::Intel, "intel+vmx: vendor decoded (EBX,EDX,ECX order)");
    Check(r.cpu_vendor_string == "GenuineIntel", "intel+vmx: vendor string exact");
    Check(r.vmx_cpuid_bit && !r.hypervisor_present_bit, "intel+vmx: ECX[5] set, ECX[31] clear");
    Check(r.verdict == PreflightVerdict::PendingKernelVerification,
          "intel+vmx: verdict is PendingKernelVerification, never 'ready'");
    Check(r.kernel_verification_required, "intel+vmx: kernel verification still required");
    Check(r.family == 6 && r.model == 0x9E && r.stepping == 10, "signature 0x906EA -> family 6, model 0x9E, stepping 10");
    for (const char* id : {"msr.ia32_feature_control", "msr.vmx_capabilities", "msr.ept_vpid_cap", "windows.hyperv_vbs"}) {
        const auto* c = Find(r, id);
        Check(c != nullptr && c->status == CheckStatus::NotChecked, std::string("kernel-only check is NOT CHECKED: ") + id);
    }
    Check(!fake.WasQueried(0x40000000u) && !fake.WasQueried(0x80000001u),
          "intel+vmx: hypervisor/extended leaves not read when not needed");
}

void TestIntelWithoutVmx() {
    auto fake = MakeCpu("GenuineIntel", 0);
    hv::PreflightReport r;
    Check(hv::RunPlatformPreflight(fake, r).Succeeded(), "intel-no-vmx: preflight succeeds");
    Check(r.verdict == PreflightVerdict::VmxNotReported, "intel-no-vmx: VmxNotReported");
    const auto* c = Find(r, "cpuid.01h.ecx5.vmx");
    Check(c != nullptr && c->status == CheckStatus::Fail, "intel-no-vmx: VMX check fails");
}

void TestHypervisorPresent() {
    auto fake = MakeCpu("GenuineIntel", kVmxBit | kHypervisorBit);
    fake.Set(0x40000000u, 0, hv::FakeCpuidProvider::HypervisorVendorLeaf(0x4000000A, "Microsoft Hv"));
    hv::PreflightReport r;
    Check(hv::RunPlatformPreflight(fake, r).Succeeded(), "hypervisor: preflight succeeds");
    Check(r.hypervisor_present_bit, "hypervisor: ECX[31] detected");
    Check(r.hypervisor_vendor == hv::HypervisorVendor::MicrosoftHyperV, "hypervisor: Microsoft Hv decoded (EBX,ECX,EDX order)");
    Check(r.max_hypervisor_leaf == 0x4000000A, "hypervisor: max hypervisor leaf captured");
    Check(r.verdict == PreflightVerdict::HypervisorConflict, "hypervisor: HypervisorConflict verdict");

    auto kvm = MakeCpu("GenuineIntel", kVmxBit | kHypervisorBit);
    kvm.Set(0x40000000u, 0, hv::FakeCpuidProvider::HypervisorVendorLeaf(0x40000001, "KVMKVMKVM"));
    hv::PreflightReport rk;
    Check(hv::RunPlatformPreflight(kvm, rk).Succeeded() && rk.hypervisor_vendor == hv::HypervisorVendor::Kvm,
          "hypervisor: KVM (NUL-padded) decoded");

    auto unreadable = MakeCpu("GenuineIntel", kVmxBit | kHypervisorBit);  // leaf 0x40000000 not configured
    hv::PreflightReport ru;
    Check(hv::RunPlatformPreflight(unreadable, ru).Succeeded() && ru.verdict == PreflightVerdict::HypervisorConflict &&
          ru.hypervisor_vendor == hv::HypervisorVendor::Other,
          "hypervisor: unreadable vendor leaf still reported as conflict");

    auto vmx_hidden = MakeCpu("GenuineIntel", kHypervisorBit);
    hv::PreflightReport rh;
    Check(hv::RunPlatformPreflight(vmx_hidden, rh).Succeeded() && rh.verdict == PreflightVerdict::VmxNotReported,
          "hypervisor present but VMX clear: VmxNotReported");
}

void TestAmdAndUnknownVendors() {
    auto amd = MakeCpu("AuthenticAMD", 0);
    amd.Set(0x80000001u, 0, {0, 0, 1u << 2, 0});
    hv::PreflightReport r;
    Check(hv::RunPlatformPreflight(amd, r).Succeeded(), "amd: preflight succeeds");
    Check(r.cpu_vendor == hv::CpuVendor::Amd && r.svm_read && r.svm_cpuid_bit, "amd: SVM bit recorded as information");
    Check(r.verdict == PreflightVerdict::UnsupportedPlatform, "amd: UnsupportedPlatform (no SVM backend)");

    auto other = MakeCpu("SomethingElse!", kVmxBit);
    hv::PreflightReport ro;
    Check(hv::RunPlatformPreflight(other, ro).Succeeded() && ro.cpu_vendor == hv::CpuVendor::Other &&
          ro.verdict == PreflightVerdict::UnsupportedPlatform, "unknown vendor: UnsupportedPlatform even if VMX bit set");
    Check(!other.WasQueried(0x80000001u), "unknown vendor: extended leaf not read");
}

void TestMissingLeavesAndFailures() {
    auto low = MakeCpu("GenuineIntel", kVmxBit, 0);
    hv::PreflightReport r;
    Check(hv::RunPlatformPreflight(low, r).Succeeded(), "max leaf 0: preflight succeeds");
    Check(!low.WasQueried(1), "max leaf 0: leaf 1 never queried");
    Check(r.verdict == PreflightVerdict::UnsupportedPlatform && !r.vmx_cpuid_bit, "max leaf 0: unsupported, VMX not claimed");

    hv::FakeCpuidProvider none;
    hv::PreflightReport r2;
    auto res = hv::RunPlatformPreflight(none, r2);
    Check(!res.Succeeded() && r2.verdict == PreflightVerdict::ProbeFailed, "leaf 0 failure: error result + ProbeFailed");

    hv::FakeCpuidProvider only_leaf0;
    only_leaf0.Set(0, 0, hv::FakeCpuidProvider::CpuVendorLeaf(1, "GenuineIntel"));
    hv::PreflightReport r3;
    res = hv::RunPlatformPreflight(only_leaf0, r3);
    Check(!res.Succeeded() && r3.verdict == PreflightVerdict::ProbeFailed && !r3.vmx_cpuid_bit,
          "leaf 1 failure: error result + ProbeFailed, VMX not claimed");

    hv::FakeCpuidProvider unsupported;
    unsupported.FailAllWith(hv::Status::Unsupported, "no cpuid");
    hv::PreflightReport r4;
    Check(hv::RunPlatformPreflight(unsupported, r4).Succeeded() && r4.verdict == PreflightVerdict::UnsupportedPlatform,
          "non-x86 provider: UnsupportedPlatform with successful result");
}

void TestSanitizationAndFormatting() {
    hv::FakeCpuidProvider fake;
    fake.Set(0, 0, hv::FakeCpuidProvider::CpuVendorLeaf(1, std::string("Bad\x01\"Vend\\r!", 12)));
    fake.Set(1, 0, {0, 0, 0, 0});
    hv::PreflightReport r;
    Check(hv::RunPlatformPreflight(fake, r).Succeeded(), "garbled vendor: preflight succeeds");
    Check(r.cpu_vendor_string.find('\x01') == std::string::npos, "garbled vendor: control characters sanitized");
    const auto json = hv::FormatReportJson(r);
    Check(json.find("\"verdict\": \"UnsupportedPlatform\"") != std::string::npos, "json: verdict present");
    Check(json.find("Bad?\\\"Vend\\\\r!") != std::string::npos, "json: quotes and backslashes escaped");

    auto good = MakeCpu("GenuineIntel", kVmxBit);
    hv::PreflightReport g;
    Check(hv::RunPlatformPreflight(good, g).Succeeded(), "text report: preflight succeeds");
    const auto text = hv::FormatReportText(g);
    Check(text.find("NOT CHECKED") != std::string::npos, "text: lists unverified kernel checks");
    Check(text.find("Kernel-mode verification required: yes") != std::string::npos, "text: states kernel verification required");
    Check(g.summary.find("NOT confirmed ready") != std::string::npos &&
          text.find(g.summary) != std::string::npos,
          "summary: explicitly says VMX is NOT confirmed ready (and is in the text report)");
}

void TestNativeProvider() {
    hv::NativeCpuidProvider native;
    hv::PreflightReport r;
    const auto res = hv::RunPlatformPreflight(native, r);
    if (hv::NativeCpuidProvider::IsAvailableOnThisBuild()) {
        Check(res.Succeeded() && r.verdict != PreflightVerdict::ProbeFailed, "native CPUID: probe succeeds on x86/x64");
        Check(!r.cpu_vendor_string.empty() && r.max_basic_leaf >= 1, "native CPUID: vendor and leaf count are plausible");
        Check(r.kernel_verification_required, "native CPUID: kernel verification flag stays set");
    } else {
        Check(res.Succeeded() && r.verdict == PreflightVerdict::UnsupportedPlatform, "native CPUID: unsupported on non-x86");
    }
}
} // namespace

int main() {
    TestIntelWithVmx();
    TestIntelWithoutVmx();
    TestHypervisorPresent();
    TestAmdAndUnknownVendors();
    TestMissingLeavesAndFailures();
    TestSanitizationAndFormatting();
    TestNativeProvider();
    if (test::failures != 0) { std::cerr << test::failures << " test(s) failed\n"; return EXIT_FAILURE; }
    std::cout << "All preflight tests passed\n";
    return EXIT_SUCCESS;
}
