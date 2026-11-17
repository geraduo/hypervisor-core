#include "hv/platform/fake_cpuid_provider.hpp"
#include "hv/platform/platform_compatibility_manager.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>

using test::Check;

namespace {

constexpr std::uint32_t kVmxBit = 1u << 5;
constexpr std::uint32_t kHypervisorBit = 1u << 31;
constexpr std::uint32_t kSvmBit = 1u << 2;
constexpr std::uint32_t kExtendedLeaf0 = 0x80000000u;
constexpr std::uint32_t kExtendedLeaf1 = 0x80000001u;

class FakeEnvironment final : public hv::ICompatibilityEnvironment {
public:
    void SetSnapshot(hv::CompatibilityEnvironmentSnapshot snapshot) {
        snapshot_ = std::move(snapshot);
    }
    void FailWith(hv::Status status, std::string message) {
        fail_ = true;
        fail_status_ = status;
        fail_message_ = std::move(message);
    }
    void ClearFailure() { fail_ = false; }

    [[nodiscard]] hv::Result Query(hv::CompatibilityEnvironmentSnapshot& out) override {
        ++queries_;
        out = hv::CompatibilityEnvironmentSnapshot{};
        if (fail_) return hv::Result::Error(fail_status_, fail_message_);
        out = snapshot_;
        return hv::Result::Ok();
    }

    [[nodiscard]] int QueryCount() const { return queries_; }

private:
    hv::CompatibilityEnvironmentSnapshot snapshot_{};
    bool fail_{false};
    hv::Status fail_status_{hv::Status::InitializationFailed};
    std::string fail_message_;
    int queries_{0};
};

hv::CompatibilityEnvironmentSnapshot CleanEnvironment() {
    hv::CompatibilityEnvironmentSnapshot environment{};
    environment.vbs_configuration_known = true;
    environment.vbs_configured = false;
    environment.hvci_configuration_known = true;
    environment.hvci_configured = false;
    environment.win_hv_platform_api_available = false;
    environment.vbs_detail = "test: VBS is disabled";
    environment.hvci_detail = "test: HVCI is disabled";
    environment.whpx_detail = "test: WinHvPlatform is unavailable";
    return environment;
}

hv::FakeCpuidProvider MakeIntel(bool vmx, bool hypervisor = false,
                                const std::string& vendor = "Microsoft Hv") {
    hv::FakeCpuidProvider fake;
    fake.Set(0, 0, hv::FakeCpuidProvider::CpuVendorLeaf(0x16, "GenuineIntel"));
    std::uint32_t ecx = vmx ? kVmxBit : 0u;
    if (hypervisor) ecx |= kHypervisorBit;
    fake.Set(1, 0, {0x000906EA, 0, ecx, 0});
    if (hypervisor) {
        fake.Set(0x40000000u, 0,
                 hv::FakeCpuidProvider::HypervisorVendorLeaf(0x4000000Au, vendor));
    }
    return fake;
}

hv::FakeCpuidProvider MakeAmd() {
    hv::FakeCpuidProvider fake;
    fake.Set(0, 0, hv::FakeCpuidProvider::CpuVendorLeaf(0x0D, "AuthenticAMD"));
    fake.Set(1, 0, {0x00800F12, 0, kSvmBit, 0});
    fake.Set(kExtendedLeaf0, 0, {0x80000008u, 0, 0, 0});
    fake.Set(kExtendedLeaf1, 0, {0, 0, kSvmBit, 0});
    return fake;
}

const hv::PreflightCheck* Find(const hv::CompatibilityReport& report, const std::string& id) {
    for (const auto& check : report.checks)
        if (check.id == id) return &check;
    return nullptr;
}

void TestCleanEnvironment() {
    auto cpuid = MakeIntel(true);
    FakeEnvironment environment;
    environment.SetSnapshot(CleanEnvironment());
    hv::PlatformCompatibilityManager manager(cpuid, environment);
    hv::CompatibilityReport report;

    Check(manager.Evaluate(report).Succeeded(), "clean: evaluation succeeds");
    Check(report.verdict == hv::CompatibilityVerdict::NoConflictDetected,
          "clean: no conflict is detected");
    Check(!report.conflict_detected, "clean: conflict flag is clear");
    Check(report.environment == hv::HypervisorEnvironment::NotReported,
          "clean: no hypervisor environment is reported");
    Check(report.vbs == hv::VbsState::Disabled && report.hvci == hv::HvciState::Disabled,
          "clean: VBS and HVCI are disabled");
    Check(report.kernel_verification_required,
          "clean: kernel verification is still required");
    Check(report.cpu_vendor == hv::CpuVendor::Intel && report.cpu_vmx_reported,
          "clean: CPU identity and VMX bit are retained");
    Check(report.summary.find("not proof") != std::string::npos,
          "clean: summary does not claim VMXON readiness");
    Check(Find(report, "windows.vbs") != nullptr &&
              Find(report, "windows.vbs")->status == hv::CheckStatus::Pass,
          "clean: VBS check passes");
    Check(Find(report, "windows.hvci") != nullptr &&
              Find(report, "windows.hvci")->status == hv::CheckStatus::Pass,
          "clean: HVCI check passes");
    Check(Find(report, "system.kernel_restrictions")->status == hv::CheckStatus::NotChecked,
          "clean: kernel-only restrictions remain NOT CHECKED");
    Check(Find(report, "system.boundary")->status == hv::CheckStatus::Info,
          "clean: boundary check is informational");
    Check(environment.QueryCount() == 1, "clean: environment is queried once");
}

void TestHypervisorEnvironments() {
    const auto evaluate = [](hv::FakeCpuidProvider& cpuid,
                             hv::CompatibilityEnvironmentSnapshot environment) {
        FakeEnvironment source;
        source.SetSnapshot(std::move(environment));
        hv::PlatformCompatibilityManager manager(cpuid, source);
        hv::CompatibilityReport report;
        Check(manager.Evaluate(report).Succeeded(), "environment: evaluation succeeds");
        return report;
    };

    auto microsoft_cpu = MakeIntel(true, true, "Microsoft Hv");
    hv::CompatibilityReport microsoft = evaluate(microsoft_cpu, CleanEnvironment());
    Check(microsoft.environment == hv::HypervisorEnvironment::MicrosoftHyperV,
          "environment: Microsoft hypervisor is classified");
    Check(microsoft.verdict == hv::CompatibilityVerdict::KnownConflict &&
              microsoft.conflict_detected,
          "environment: Microsoft hypervisor is a known conflict");
    Check(microsoft.cpuid_hypervisor_vendor == hv::HypervisorVendor::MicrosoftHyperV,
          "environment: CPUID vendor classification is preserved");
    Check(Find(microsoft, "compatibility.hypervisor")->status == hv::CheckStatus::Warn,
          "environment: hypervisor conflict is a warning");

    auto kvm_cpu = MakeIntel(true, true, "KVMKVMKVM");
    hv::CompatibilityReport kvm = evaluate(kvm_cpu, CleanEnvironment());
    Check(kvm.environment == hv::HypervisorEnvironment::ThirdParty,
          "environment: KVM is a third-party hypervisor");
    Check(kvm.verdict == hv::CompatibilityVerdict::KnownConflict,
          "environment: third-party hypervisor is a known conflict");

    auto mystery_cpu = MakeIntel(true, true, "MysteryVendor");
    hv::CompatibilityReport mystery = evaluate(mystery_cpu, CleanEnvironment());
    Check(mystery.environment == hv::HypervisorEnvironment::Unrecognized,
          "environment: unknown vendor is Unrecognized");
    Check(mystery.verdict == hv::CompatibilityVerdict::ReviewRequired &&
              !mystery.conflict_detected,
          "environment: unknown vendor requires review without claiming a conflict");

    auto whpx_cpu = MakeIntel(true, false);
    hv::CompatibilityEnvironmentSnapshot whpx_environment = CleanEnvironment();
    whpx_environment.win_hv_platform_api_available = true;
    whpx_environment.win_hv_platform_query_succeeded = true;
    whpx_environment.win_hv_platform_hypervisor_present = true;
    whpx_environment.whpx_detail = "test: WHvGetCapability reports a hypervisor";
    hv::CompatibilityReport whpx = evaluate(whpx_cpu, whpx_environment);
    Check(whpx.environment == hv::HypervisorEnvironment::MicrosoftHyperV,
          "environment: WHPX presence is classified as MicrosoftHyperV");
    Check(whpx.verdict == hv::CompatibilityVerdict::KnownConflict &&
              whpx.conflict_detected,
          "environment: WHPX presence is a known conflict");
    Check(Find(whpx, "windows.whpx_api")->status == hv::CheckStatus::Warn,
          "environment: WHPX conflict is a warning");

    hv::CompatibilityEnvironmentSnapshot whpx_failed = CleanEnvironment();
    whpx_failed.win_hv_platform_api_available = true;
    whpx_failed.win_hv_platform_query_succeeded = false;
    whpx_failed.whpx_detail = "test: WHvGetCapability failed";
    hv::CompatibilityReport whpx_unavailable = evaluate(whpx_cpu, whpx_failed);
    Check(whpx_unavailable.verdict == hv::CompatibilityVerdict::NoConflictDetected,
          "environment: a failed WHPX query does not invent a conflict");
    Check(Find(whpx_unavailable, "windows.whpx_api")->status == hv::CheckStatus::Info,
          "environment: a failed optional WHPX query is informational");
}

void TestVbsAndHvciStates() {
    const auto evaluate = [](hv::CompatibilityEnvironmentSnapshot environment) {
        auto cpuid = MakeIntel(true);
        FakeEnvironment source;
        source.SetSnapshot(std::move(environment));
        hv::PlatformCompatibilityManager manager(cpuid, source);
        hv::CompatibilityReport report;
        Check(manager.Evaluate(report).Succeeded(), "VBS: evaluation succeeds");
        return report;
    };

    hv::CompatibilityEnvironmentSnapshot vbs_enabled = CleanEnvironment();
    vbs_enabled.vbs_configured = true;
    hv::CompatibilityReport vbs_report = evaluate(vbs_enabled);
    Check(vbs_report.vbs == hv::VbsState::EnabledConfigured,
          "VBS: configured state is reported");
    Check(vbs_report.verdict == hv::CompatibilityVerdict::ReviewRequired,
          "VBS: configured VBS requires review");
    Check(Find(vbs_report, "windows.vbs")->status == hv::CheckStatus::Warn,
          "VBS: configured VBS is a warning");

    hv::CompatibilityEnvironmentSnapshot vbs_disabled_hvci_enabled = CleanEnvironment();
    vbs_disabled_hvci_enabled.hvci_configured = true;
    hv::CompatibilityReport hvci_report = evaluate(vbs_disabled_hvci_enabled);
    Check(hvci_report.vbs == hv::VbsState::Disabled &&
              hvci_report.hvci == hv::HvciState::Disabled,
          "HVCI: HVCI is inactive when VBS is disabled");
    Check(hvci_report.verdict == hv::CompatibilityVerdict::NoConflictDetected,
          "HVCI: configured HVCI alone does not create a conflict");
    Check(Find(hvci_report, "windows.hvci")->detail.find("prerequisite") !=
              std::string::npos,
          "HVCI: detail explains the VBS prerequisite");

    hv::CompatibilityEnvironmentSnapshot unknown = CleanEnvironment();
    unknown.vbs_configuration_known = false;
    unknown.hvci_configuration_known = false;
    hv::CompatibilityReport unknown_report = evaluate(unknown);
    Check(unknown_report.vbs == hv::VbsState::Unknown &&
              unknown_report.hvci == hv::HvciState::Unknown,
          "VBS: unknown configuration is preserved");
    Check(unknown_report.verdict == hv::CompatibilityVerdict::ReviewRequired,
          "VBS: unknown configuration requires review");
    Check(Find(unknown_report, "windows.vbs")->status == hv::CheckStatus::NotChecked,
          "VBS: unknown configuration is NOT CHECKED");

    hv::CompatibilityEnvironmentSnapshot env_failure = CleanEnvironment();
    auto cpuid = MakeIntel(true);
    FakeEnvironment failing_environment;
    failing_environment.FailWith(hv::Status::Unsupported,
                                 "Windows compatibility probes are unavailable");
    hv::PlatformCompatibilityManager failing_manager(cpuid, failing_environment);
    hv::CompatibilityReport failure_report;
    Check(failing_manager.Evaluate(failure_report).Succeeded(),
          "environment failure: evaluation still completes");
    Check(failure_report.verdict == hv::CompatibilityVerdict::ReviewRequired,
          "environment failure: review is required");
    Check(Find(failure_report, "windows.compatibility_environment")->status ==
              hv::CheckStatus::Info,
          "environment failure: unavailable probes are informational");
    Check(!failure_report.conflict_detected,
          "environment failure: no conflict is invented");
}

void TestUnsupportedCpuPrecedence() {
    auto amd_cpu = MakeAmd();
    hv::CompatibilityEnvironmentSnapshot conflicted = CleanEnvironment();
    conflicted.vbs_configured = true;
    FakeEnvironment environment;
    environment.SetSnapshot(conflicted);
    hv::PlatformCompatibilityManager manager(amd_cpu, environment);
    hv::CompatibilityReport amd_report;
    Check(manager.Evaluate(amd_report).Succeeded(), "unsupported CPU: evaluation completes");
    Check(amd_report.preflight_verdict == hv::PreflightVerdict::UnsupportedPlatform,
          "unsupported CPU: AMD preflight is UnsupportedPlatform");
    Check(amd_report.verdict == hv::CompatibilityVerdict::UnsupportedPlatform,
          "unsupported CPU: CPU support takes precedence over VBS warnings");
    Check(amd_report.summary.find("CPU") != std::string::npos,
          "unsupported CPU: summary explains the CPU limitation");

    auto no_vmx_cpu = MakeIntel(false);
    FakeEnvironment clean_environment;
    clean_environment.SetSnapshot(CleanEnvironment());
    hv::PlatformCompatibilityManager no_vmx_manager(no_vmx_cpu, clean_environment);
    hv::CompatibilityReport no_vmx_report;
    Check(no_vmx_manager.Evaluate(no_vmx_report).Succeeded(),
          "unsupported CPU: missing VMX evaluation completes");
    Check(no_vmx_report.preflight_verdict == hv::PreflightVerdict::VmxNotReported &&
              no_vmx_report.verdict == hv::CompatibilityVerdict::UnsupportedPlatform,
          "unsupported CPU: missing VMX is UnsupportedPlatform");

    hv::FakeCpuidProvider broken_cpu;
    broken_cpu.FailAllWith(hv::Status::InternalError, "CPUID source fault");
    FakeEnvironment unused_environment;
    unused_environment.SetSnapshot(CleanEnvironment());
    hv::PlatformCompatibilityManager broken_manager(broken_cpu, unused_environment);
    hv::CompatibilityReport broken_report;
    const hv::Result result = broken_manager.Evaluate(broken_report);
    Check(!result.Succeeded() && result.status == hv::Status::InternalError,
          "unsupported CPU: CPUID failure is returned as an error");
    Check(broken_report.verdict == hv::CompatibilityVerdict::Unknown,
          "unsupported CPU: CPUID failure leaves the verdict Unknown");
    Check(broken_report.summary.find("failed") != std::string::npos,
          "unsupported CPU: summary explains the probe failure");
    Check(unused_environment.QueryCount() == 0,
          "unsupported CPU: environment is not queried after CPUID failure");
    Check(!broken_manager.GetLastError().empty(),
          "unsupported CPU: failure detail is retained");
}

void TestFormatting() {
    auto cpuid = MakeIntel(true, true, "Microsoft Hv");
    FakeEnvironment environment;
    environment.SetSnapshot(CleanEnvironment());
    hv::PlatformCompatibilityManager manager(cpuid, environment);
    hv::CompatibilityReport report;
    Check(manager.Evaluate(report).Succeeded(), "formatting: fixture evaluates");

    report.summary = "quote: \" slash: \\ newline:\n tab:\t control:\x01";
    report.checks.push_back({"check\n", "title\"", hv::CheckStatus::Warn,
                             "detail\\\t"});
    const auto json = hv::FormatCompatibilityReportJson(report);
    Check(json.find("quote: \\\" slash: \\\\ newline:\\n tab:\\t control:\\u0001") !=
              std::string::npos,
          "formatting: JSON escapes summary text");
    Check(json.find("check\\n") != std::string::npos &&
              json.find("title\\\"") != std::string::npos,
          "formatting: JSON escapes check identifiers");
    Check(json.find("\"verdict\": \"KnownConflict\"") != std::string::npos,
          "formatting: JSON contains the verdict");

    const auto text = hv::FormatCompatibilityReportText(report);
    Check(text.find("Verdict: KnownConflict") != std::string::npos &&
              text.find("Kernel-mode verification required: yes") != std::string::npos,
          "formatting: text report contains verdict and kernel requirement");
    Check(text.find("Hardware virtualization boundary") != std::string::npos,
          "formatting: text report retains the hardware boundary check");

    Check(std::string(hv::ToString(hv::CompatibilityVerdict::NoConflictDetected)) ==
              "NoConflictDetected",
          "formatting: verdict names are stable");
    Check(std::string(hv::ToString(hv::HypervisorEnvironment::MicrosoftHyperV)) ==
              "MicrosoftHyperV",
          "formatting: environment names are stable");
    Check(std::string(hv::ToString(hv::VbsState::EnabledConfigured)) ==
              "EnabledConfigured",
          "formatting: VBS names are stable");
    Check(std::string(hv::ToString(hv::HvciState::Disabled)) == "Disabled",
          "formatting: HVCI names are stable");
}

} // namespace

int main() {
    TestCleanEnvironment();
    TestHypervisorEnvironments();
    TestVbsAndHvciStates();
    TestUnsupportedCpuPrecedence();
    TestFormatting();

    if (test::failures != 0) {
        std::cerr << test::failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All platform compatibility tests passed\n";
    return EXIT_SUCCESS;
}
