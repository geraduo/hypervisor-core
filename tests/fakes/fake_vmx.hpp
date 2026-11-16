#pragma once

// Fake providers for the v0.5.0 VMX layer. They let every test run without physical VT-x, without
// the diagnostic driver and without Windows. They return scripted data; they never simulate a
// successful hypervisor launch.

#include "fake_windows_platform.hpp"
#include "hv/platform/cpu_capability_manager.hpp"
#include "hv/platform/fake_cpuid_provider.hpp"
#include "hv/platform/platform_compatibility_manager.hpp"
#include "hv/platform/windows_platform_backend.hpp"
#include "hv/virtualization/virtualization_backend.hpp"
#include "hv/virtualization/vmx_capability_manager.hpp"
#include "hv/virtualization/vmx_diagnostics.hpp"
#include "hv/virtualization/vmx_lifecycle_controller.hpp"
#include "hv/virtualization/vmx_readiness_manager.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace fakes {

inline constexpr std::uint32_t kVmxBit = 1u << 5;
inline constexpr std::uint32_t kHypervisorBit = 1u << 31;

inline hv::FakeCpuidProvider MakeIntelCpuid(bool vmx = true, bool hypervisor = false) {
    hv::FakeCpuidProvider fake;
    std::uint32_t ecx = vmx ? kVmxBit : 0u;
    if (hypervisor) ecx |= kHypervisorBit;
    fake.Set(0, 0, hv::FakeCpuidProvider::CpuVendorLeaf(0x16, "GenuineIntel"));
    fake.Set(1, 0, {0x000906EA, 0, ecx, 0});
    if (hypervisor) {
        fake.Set(0x40000000u, 0,
                 hv::FakeCpuidProvider::HypervisorVendorLeaf(0x4000000Au, "Microsoft Hv"));
    }
    fake.Set(0x80000000u, 0, {0x80000004u, 0, 0, 0});
    return fake;
}

inline hv::FakeCpuidProvider MakeAmdCpuid() {
    hv::FakeCpuidProvider fake;
    fake.Set(0, 0, hv::FakeCpuidProvider::CpuVendorLeaf(0x10, "AuthenticAMD"));
    fake.Set(1, 0, {0x00A20F10, 0, 0, 0});
    fake.Set(0x80000000u, 0, {0x80000008u, 0, 0, 0});
    fake.Set(0x80000001u, 0, {0, 0, 1u << 2, 0});
    return fake;
}

// One scripted CPUID provider per logical processor. Individual processors can be made to fail.
class FakeLogicalProcessorCpuidProvider final : public hv::ILogicalProcessorCpuidProvider {
public:
    std::vector<hv::FakeCpuidProvider> cpus;
    hv::Result count_result{hv::Result::Ok()};
    std::map<std::uint32_t, hv::Result> failing;
    std::uint32_t query_calls{};

    hv::Result GetLogicalProcessorCount(std::uint32_t& out) override {
        if (!count_result.Succeeded()) return count_result;
        out = static_cast<std::uint32_t>(cpus.size());
        return hv::Result::Ok();
    }

    hv::Result Query(std::uint32_t cpu_index, std::uint32_t leaf, std::uint32_t subleaf,
                     hv::CpuidRegisters& out) override {
        ++query_calls;
        const auto failure = failing.find(cpu_index);
        if (failure != failing.end()) return failure->second;
        if (cpu_index >= cpus.size()) {
            return hv::Result::Error(hv::Status::InvalidArgument, "fake processor index out of range");
        }
        return cpus[cpu_index].Query(leaf, subleaf, out);
    }
};

// Driver fake with an explicit status for connect and query, so that "missing driver",
// "protocol mismatch" and "transport failure" can be scripted separately.
class FakeVmxDriverInterface final : public hv::IKernelDriverInterface {
public:
    bool connected{false};
    hv::Status connect_status{hv::Status::Success};
    hv::Status query_status{hv::Status::Success};
    hv::DriverPlatformInfo platform_info{
        10, 0, 22631, GERADUO_ARCHITECTURE_X64, 4, 4096, false, true};
    std::string last_error;
    std::uint32_t connect_calls{};
    std::uint32_t disconnect_calls{};
    std::uint32_t query_calls{};

    hv::Result Connect() override {
        ++connect_calls;
        if (connect_status != hv::Status::Success) {
            connected = false;
            last_error = "fake driver connect failed";
            return hv::Result::Error(connect_status, last_error);
        }
        connected = true;
        last_error.clear();
        return hv::Result::Ok();
    }

    void Disconnect() noexcept override {
        ++disconnect_calls;
        connected = false;
    }

    bool IsConnected() const noexcept override { return connected; }

    hv::Result QueryPlatformInfo(hv::DriverPlatformInfo& out) override {
        ++query_calls;
        if (!connected) {
            last_error = "fake driver is not connected";
            return hv::Result::Error(hv::Status::DeviceNotConnected, last_error);
        }
        if (query_status != hv::Status::Success) {
            last_error = "fake platform query failed";
            return hv::Result::Error(query_status, last_error);
        }
        out = platform_info;
        return hv::Result::Ok();
    }

    std::string GetLastError() const override { return last_error; }
};

// Complete wiring of the VMX layer on top of fakes. Members are listed in construction order.
struct VmxRig {
    explicit VmxRig(std::uint32_t cpu_count = 4, bool vmx = true)
        : cpu_manager(current_cpuid),
          backend(platform, cpu_manager),
          capability(cpu_manager, processors),
          compatibility(platform, cpu_manager, driver),
          readiness(backend, capability, compatibility, driver) {
        current_cpuid = MakeIntelCpuid(vmx);
        processors.cpus.assign(cpu_count, MakeIntelCpuid(vmx));
        platform.system.logical_processor_count = cpu_count;
        driver.platform_info.processor_count = cpu_count;
        driver.platform_info.vmx_reported = vmx;
    }
    VmxRig(const VmxRig&) = delete;
    VmxRig& operator=(const VmxRig&) = delete;

    FakePlatformProvider platform;
    hv::FakeCpuidProvider current_cpuid;
    FakeLogicalProcessorCpuidProvider processors;
    FakeVmxDriverInterface driver;
    hv::CpuCapabilityManager cpu_manager;
    hv::WindowsPlatformBackend backend;
    hv::VmxCapabilityManager capability;
    hv::PlatformCompatibilityManager compatibility;
    hv::VmxReadinessManager readiness;
    hv::VmxDiagnostics diagnostics;
};

inline bool HasReason(const std::vector<hv::VmxFailureDetail>& reasons, hv::VmxFailureReason reason) {
    for (const auto& item : reasons) {
        if (item.reason == reason) return true;
    }
    return false;
}

inline const hv::VmxCondition* FindCondition(const hv::VmxReadinessReport& report,
                                             const std::string& id) {
    for (const auto& condition : report.conditions) {
        if (condition.id == id) return &condition;
    }
    return nullptr;
}

inline bool Contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

} // namespace fakes
