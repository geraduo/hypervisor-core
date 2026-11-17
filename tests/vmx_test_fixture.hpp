#pragma once

// Shared deterministic fixtures for the v0.5.0 VT-x test executables. Every
// provider is a fake: no CPUID instruction, Win32 call or driver IOCTL runs.

#include "hv/platform/fake_compatibility_environment.hpp"
#include "hv/platform/fake_cpuid_provider.hpp"
#include "hv/platform/fake_kernel_driver_interface.hpp"
#include "hv/platform/fake_platform_provider.hpp"
#include "hv/platform/platform_compatibility_manager.hpp"
#include "hv/platform/windows_platform_backend.hpp"
#include "hv/virtualization/fake_vmx_providers.hpp"
#include "hv/virtualization/vmx_capability_manager.hpp"
#include "hv/virtualization/vmx_lifecycle_controller.hpp"
#include "hv/virtualization/vmx_readiness_manager.hpp"

#include <cstdint>
#include <string>

namespace vmx_test {

constexpr std::uint32_t kVmxBit = 1u << 5;
constexpr std::uint32_t kHypervisorBit = 1u << 31;
constexpr std::uint32_t kSvmBit = 1u << 2;
constexpr std::uint32_t kExtendedLeaf0 = 0x80000000u;
constexpr std::uint32_t kExtendedLeaf1 = 0x80000001u;

[[nodiscard]] inline hv::FakeCpuidProvider MakeIntel(bool vmx, bool hypervisor = false,
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

[[nodiscard]] inline hv::FakeCpuidProvider MakeAmd() {
    hv::FakeCpuidProvider fake;
    fake.Set(0, 0, hv::FakeCpuidProvider::CpuVendorLeaf(0x0D, "AuthenticAMD"));
    fake.Set(1, 0, {0x00800F12, 0, kSvmBit, 0});
    fake.Set(kExtendedLeaf0, 0, {0x80000008u, 0, 0, 0});
    fake.Set(kExtendedLeaf1, 0, {0, 0, kSvmBit, 0});
    return fake;
}

[[nodiscard]] inline hv::PlatformProviderSnapshot MakePlatformSnapshot() {
    hv::PlatformProviderSnapshot snapshot;
    snapshot.os_version = {10, 0, 22631, 1234};
    snapshot.os_name = "Windows 11 Pro";
    snapshot.is_windows_server = false;
    snapshot.native_architecture = hv::PlatformArchitecture::X64;
    snapshot.process_architecture = hv::PlatformArchitecture::X64;
    snapshot.is_wow64_process = false;
    snapshot.logical_processor_count = 4;
    snapshot.page_size = 4096;
    snapshot.total_physical_memory_bytes = 16ull * 1024ull * 1024ull * 1024ull;
    snapshot.windows_apis = hv::PlatformCapabilityState::Available;
    snapshot.cpuid = hv::PlatformCapabilityState::Available;
    snapshot.windows_hypervisor_platform_api = hv::PlatformCapabilityState::Unavailable;
    snapshot.kernel_driver_interface = hv::PlatformCapabilityState::RequiresKernelVerification;
    snapshot.virtualization_firmware = hv::PlatformCapabilityState::RequiresKernelVerification;
    return snapshot;
}

[[nodiscard]] inline hv::CompatibilityEnvironmentSnapshot MakeCleanEnvironment() {
    hv::CompatibilityEnvironmentSnapshot environment;
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

[[nodiscard]] inline hv::DriverPlatformInfoResponse MakeValidDriverResponse() {
    hv::DriverPlatformInfoResponse response{};
    response.magic = hv::kGeraduoResponseMagic;
    response.protocol_version = hv::kGeraduoProtocolVersion;
    response.structure_size = static_cast<std::uint32_t>(sizeof(response));
    response.request_id = 1;
    response.status = static_cast<std::int32_t>(hv::DriverProtocolStatus::Success);
    response.capability_flags = hv::kGeraduoCapabilityDiagnosticQuery |
                                hv::kGeraduoCapabilityOsVersion |
                                hv::kGeraduoCapabilityProcessorInfo;
    response.driver_version_major = hv::kGeraduoDriverVersionMajor;
    response.driver_version_minor = hv::kGeraduoDriverVersionMinor;
    response.driver_version_patch = hv::kGeraduoDriverVersionPatch;
    response.architecture = static_cast<std::uint32_t>(hv::DriverArchitecture::X64);
    response.os_major = 10;
    response.os_minor = 0;
    response.os_build = 22631;
    response.os_revision = 1234;
    response.active_processor_count = 4;
    response.page_size = 4096;
    response.physical_memory_bytes = 16ull * 1024ull * 1024ull * 1024ull;
    response.reserved = 0;
    return response;
}

class ReadinessFixture {
public:
    ReadinessFixture()
        : platform(platform_provider),
          compatibility(cpuid, environment),
          capability_manager(cpuid, &processor_probe),
          readiness(platform, driver, compatibility) {
        cpuid = MakeIntel(true);
        processor_probe.SetObservations(hv::MakeProcessorSet(4, true));
        platform_provider.SetSnapshot(MakePlatformSnapshot());
        environment.SetSnapshot(MakeCleanEnvironment());
        driver.SetPlatformInfoTemplate(MakeValidDriverResponse());
    }

    [[nodiscard]] bool PrepareCapability() {
        return capability_manager.Evaluate(capability).Succeeded();
    }

    [[nodiscard]] bool Evaluate(hv::VmxReadinessReport& report) {
        return readiness.Evaluate(capability, report).Succeeded();
    }

    hv::FakeCpuidProvider cpuid;
    hv::FakeLogicalProcessorProbe processor_probe;
    hv::FakePlatformProvider platform_provider;
    hv::WindowsPlatformBackend platform;
    hv::FakeCompatibilityEnvironment environment;
    hv::PlatformCompatibilityManager compatibility;
    hv::FakeKernelDriverInterface driver;
    hv::VmxCapabilityManager capability_manager;
    hv::VmxReadinessManager readiness;
    hv::VmxCapabilityReport capability;
};

class LifecycleFixture {
public:
    LifecycleFixture()
        : platform(platform_provider),
          compatibility(cpuid, environment),
          capability_manager(cpuid, &processor_probe),
          readiness_manager(platform, driver, compatibility),
          controller(capability_manager, readiness_manager) {
        cpuid = MakeIntel(true);
        processor_probe.SetObservations(hv::MakeProcessorSet(4, true));
        platform_provider.SetSnapshot(MakePlatformSnapshot());
        environment.SetSnapshot(MakeCleanEnvironment());
        driver.SetPlatformInfoTemplate(MakeValidDriverResponse());
    }

    hv::FakeCpuidProvider cpuid;
    hv::FakeLogicalProcessorProbe processor_probe;
    hv::FakePlatformProvider platform_provider;
    hv::WindowsPlatformBackend platform;
    hv::FakeCompatibilityEnvironment environment;
    hv::PlatformCompatibilityManager compatibility;
    hv::FakeKernelDriverInterface driver;
    hv::VmxCapabilityManager capability_manager;
    hv::VmxReadinessManager readiness_manager;
    hv::VmxLifecycleController controller;
};

} // namespace vmx_test
