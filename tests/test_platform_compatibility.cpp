#include "fakes/fake_windows_platform.hpp"
#include "hv/platform/cpu_capability_manager.hpp"
#include "hv/platform/fake_cpuid_provider.hpp"
#include "hv/platform/platform_compatibility_manager.hpp"
#include "test_support.hpp"
#include <cstdint>
#include <string>

using test::Check;

namespace {

constexpr std::uint32_t kVmxBit = 1u << 5;
constexpr std::uint32_t kHypervisorBit = 1u << 31;

hv::FakeCpuidProvider MakeIntel(std::uint32_t leaf1_ecx = kVmxBit) {
    hv::FakeCpuidProvider fake;
    fake.Set(0, 0, hv::FakeCpuidProvider::CpuVendorLeaf(0x16, "GenuineIntel"));
    fake.Set(1, 0, {0x000906EA, 0, leaf1_ecx, 0});
    if ((leaf1_ecx & kHypervisorBit) != 0) {
        fake.Set(0x40000000u, 0,
                 hv::FakeCpuidProvider::HypervisorVendorLeaf(0x4000000Au, "Microsoft Hv"));
    }
    fake.Set(0x80000000u, 0, {0x80000004u, 0, 0, 0});
    return fake;
}

const hv::CompatibilityCheck* Find(const hv::PlatformCompatibilityReport& report,
                                   const std::string& id) {
    for (const auto& check : report.checks) if (check.id == id) return &check;
    return nullptr;
}

void TestVbsHvciAndHyperVReview() {
    FakePlatformProvider platform;
    platform.mitigations = {true, true, false,
                            "Test source reports registry configuration only."};
    auto cpuid = MakeIntel(kVmxBit | kHypervisorBit);
    hv::CpuCapabilityManager cpu(cpuid);
    FakeKernelDriverInterface driver;
    hv::PlatformCompatibilityManager manager(platform, cpu, driver);
    hv::PlatformCompatibilityReport report;

    Check(manager.Evaluate(report).Succeeded(), "compatibility report evaluates from fake providers");
    Check(report.verdict == hv::CompatibilityVerdict::ReviewRequired,
          "existing Hyper-V/VBS/HVCI signals require review, not an assumed conflict");
    Check(Find(report, "virtualization.hypervisor") != nullptr &&
              Find(report, "virtualization.hypervisor")->status == hv::CompatibilityCheckStatus::Warning,
          "CPUID hypervisor signal is represented as a warning");
    Check(Find(report, "windows.vbs") != nullptr && Find(report, "windows.hvci") != nullptr &&
              Find(report, "windows.vbs")->detail.find("does not prove") != std::string::npos,
          "VBS and HVCI configuration are explicit and not mislabeled as runtime state");
    Check(Find(report, "driver.connection") != nullptr &&
              Find(report, "driver.connection")->status == hv::CompatibilityCheckStatus::NotChecked,
          "unconnected driver is reported without blocking user-mode diagnostics");
    Check(Find(report, "kernel.privileged_checks") != nullptr && report.kernel_verification_required,
          "kernel-only checks remain not checked and no ready verdict is exposed");
}

void TestUnsupportedSystemAndCpu() {
    FakePlatformProvider platform;
    platform.system.architecture = hv::ProcessorArchitecture::Arm64;
    auto cpuid = MakeIntel(0);
    hv::CpuCapabilityManager cpu(cpuid);
    FakeKernelDriverInterface driver;
    hv::PlatformCompatibilityManager manager(platform, cpu, driver);
    hv::PlatformCompatibilityReport report;

    Check(manager.Evaluate(report).Succeeded() && report.verdict == hv::CompatibilityVerdict::Unsupported,
          "non-x64 systems are outside the Windows driver target");
    Check(Find(report, "windows.architecture") != nullptr &&
              Find(report, "windows.architecture")->status == hv::CompatibilityCheckStatus::Fail,
          "architecture restriction is included in the structured report");

    FakePlatformProvider platform2;
    auto no_vmx = MakeIntel(0);
    hv::CpuCapabilityManager cpu2(no_vmx);
    FakeKernelDriverInterface driver2;
    hv::PlatformCompatibilityManager manager2(platform2, cpu2, driver2);
    hv::PlatformCompatibilityReport report2;
    Check(manager2.Evaluate(report2).Succeeded() && report2.verdict == hv::CompatibilityVerdict::Unsupported,
          "missing Intel VMX CPUID capability is unsupported, not ready");
}

void TestDriverQueryAndProviderErrors() {
    FakePlatformProvider platform;
    auto cpuid = MakeIntel();
    hv::CpuCapabilityManager cpu(cpuid);
    FakeKernelDriverInterface driver;
    driver.connected = true;
    driver.query_fails = true;
    hv::PlatformCompatibilityManager manager(platform, cpu, driver);
    hv::PlatformCompatibilityReport report;
    Check(manager.Evaluate(report).Succeeded() &&
              Find(report, "driver.platform_info") != nullptr &&
              Find(report, "driver.platform_info")->status == hv::CompatibilityCheckStatus::Warning,
          "kernel query failure is retained as a diagnostic warning");

    FakePlatformProvider failing_platform;
    failing_platform.system_result = hv::Result::Error(hv::Status::InitializationFailed, "system provider fault");
    auto cpuid2 = MakeIntel();
    hv::CpuCapabilityManager cpu2(cpuid2);
    FakeKernelDriverInterface driver2;
    hv::PlatformCompatibilityManager failing_manager(failing_platform, cpu2, driver2);
    hv::PlatformCompatibilityReport failed_report;
    Check(failing_manager.Evaluate(failed_report).status == hv::Status::InitializationFailed &&
              failed_report.verdict == hv::CompatibilityVerdict::Inconclusive &&
              Find(failed_report, "windows.system") != nullptr,
          "system provider failure produces an explicit inconclusive report");
}

} // namespace

int main() {
    TestVbsHvciAndHyperVReview();
    TestUnsupportedSystemAndCpu();
    TestDriverQueryAndProviderErrors();
    if (test::failures != 0) return EXIT_FAILURE;
    std::cout << "All platform compatibility tests passed\n";
    return EXIT_SUCCESS;
}