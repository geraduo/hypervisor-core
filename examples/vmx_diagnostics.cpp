#include "hv/platform/cpuid.hpp"
#include "hv/platform/driver_diagnostics.hpp"
#include "hv/platform/kernel_driver_interface.hpp"
#include "hv/platform/platform_compatibility_manager.hpp"
#include "hv/platform/platform_provider.hpp"
#include "hv/platform/windows_platform_backend.hpp"
#include "hv/virtualization/virtualization_backend.hpp"
#include "hv/virtualization/vmx_capability_manager.hpp"
#include "hv/virtualization/vmx_diagnostics.hpp"
#include "hv/virtualization/vmx_readiness_manager.hpp"

#include <iostream>
#include <string>

// hv_vmx_diagnostics.exe [--json]
//
// Collects CPU, per-processor VMX, platform, driver and compatibility evidence and prints it.
// It never executes VMXON, never creates a VMCS and never starts a hypervisor.
// Exit codes: 0 = report produced (any verdict except Failed), 1 = evaluation Failed or no report,
// 2 = invalid arguments.
int main(int argc, char** argv) {
    bool json = false;
    if (argc > 2 || (argc == 2 && std::string(argv[1]) != "--json")) {
        std::cerr << "Usage: hv_vmx_diagnostics.exe [--json]\n";
        return 2;
    }
    json = (argc == 2);

    hv::WindowsPlatformProvider platform;
    hv::NativeCpuidProvider cpuid;
    hv::NativeLogicalProcessorCpuidProvider per_processor_cpuid;
    hv::CpuCapabilityManager cpu(cpuid);
    hv::DriverDiagnostics driver_diagnostics;
    hv::KernelDriverInterface driver(&driver_diagnostics);
    hv::WindowsPlatformBackend platform_backend(platform, cpu);
    hv::VmxCapabilityManager capability(cpu, per_processor_cpuid);
    hv::PlatformCompatibilityManager compatibility(platform, cpu, driver);
    hv::VmxReadinessManager readiness(platform_backend, capability, compatibility, driver);
    hv::VmxDiagnostics diagnostics;
    hv::IntelVirtualizationBackend backend(readiness, diagnostics, &driver_diagnostics);

    const hv::Result initialized = backend.Initialize();
    hv::VmxDiagnosticReport report;
    const hv::Result produced = backend.RunDiagnostics(report);

    // Release every resource (driver connection, platform backend) before printing.
    const hv::Result stopped = backend.Shutdown();
    const hv::Result platform_stopped = platform_backend.Shutdown();

    if (!produced.Succeeded()) {
        std::cerr << "No diagnostic report is available: " << produced.message << '\n';
        return 1;
    }

    if (json) {
        std::cout << hv::FormatVmxReportJson(report) << '\n';
    } else {
        std::cout << hv::FormatVmxReportText(report);
        if (!initialized.Succeeded()) {
            std::cout << "\nInitialize() result: " << initialized.message << '\n';
        }
    }
    if (!stopped.Succeeded() || !platform_stopped.Succeeded()) {
        std::cerr << "Warning: cleanup reported an error.\n";
    }
    return report.readiness.verdict == hv::VmxReadinessVerdict::Failed ? 1 : 0;
}
