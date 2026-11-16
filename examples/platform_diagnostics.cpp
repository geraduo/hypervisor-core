#include "hv/platform/cpuid.hpp"
#include "hv/platform/driver_diagnostics.hpp"
#include "hv/platform/kernel_driver_interface.hpp"
#include "hv/platform/platform_compatibility_manager.hpp"
#include "hv/platform/windows_platform_backend.hpp"
#include <iostream>
#include <string>

int main(int argc, char**) {
    if (argc > 1) {
        std::cerr << "Usage: hv_platform_diagnostics.exe\n";
        return 2;
    }

    hv::WindowsPlatformProvider platform;
    hv::NativeCpuidProvider cpuid_provider;
    hv::CpuCapabilityManager cpu(cpuid_provider);
    hv::DriverDiagnostics driver_diagnostics;
    hv::KernelDriverInterface driver(&driver_diagnostics);
    hv::WindowsPlatformBackend backend(platform, cpu);

    const auto connection = driver.Connect();
    if (!connection.Succeeded()) {
        std::cout << "Kernel diagnostic driver: not connected (" << connection.message << ")\n";
    }

    const auto initialized = backend.Initialize();
    if (!initialized.Succeeded()) {
        std::cerr << "Windows platform backend could not initialize: " << initialized.message << '\n';
        std::cerr << "This application reports Windows host data only; it does not start a hypervisor.\n";
        return 1;
    }

    hv::WindowsPlatformSnapshot snapshot;
    const auto snapshot_result = backend.GetSnapshot(snapshot);
    if (!snapshot_result.Succeeded()) {
        std::cerr << "Could not retrieve the platform snapshot: " << snapshot_result.message << '\n';
        return 1;
    }

    std::cout << "Geraduo Hypervisor Core v0.4.0 - Windows Platform Diagnostics\n";
    std::cout << "OS: " << hv::FormatWindowsVersion(snapshot.system) << '\n';
    std::cout << "Architecture: " << hv::ToString(snapshot.system.architecture) << '\n';
    std::cout << "Logical processors: " << snapshot.system.logical_processor_count << '\n';
    std::cout << "Page size: " << snapshot.system.page_size << " bytes\n";
    std::cout << "CPU manufacturer: "
              << (snapshot.cpu.manufacturer.empty() ? "(unavailable)" : snapshot.cpu.manufacturer) << '\n';
    std::cout << "CPU model: "
              << (snapshot.cpu.model.empty() ? "(unavailable)" : snapshot.cpu.model) << '\n';
    std::cout << "\n" << hv::FormatReportText(snapshot.cpu.preflight);

    hv::PlatformCompatibilityManager compatibility(platform, cpu, driver);
    hv::PlatformCompatibilityReport report;
    const auto report_result = compatibility.Evaluate(report);
    if (!report_result.Succeeded()) {
        std::cerr << "Compatibility collection returned an error: " << report_result.message << '\n';
        std::cout << hv::FormatCompatibilityReport(report);
        return 1;
    }
    std::cout << '\n' << hv::FormatCompatibilityReport(report);

    const auto events = driver_diagnostics.Snapshot();
    std::cout << "Driver diagnostics (" << events.size() << " events):\n";
    for (const auto& event : events) {
        std::cout << "  [" << hv::ToString(event.level) << "] "
                  << hv::ToString(event.code) << " - " << event.message << '\n';
    }
    std::cout << "\nNo VMXON, VMCS, VM-entry, VM-exit, or hardware EPT/NPT operation was performed.\n";
    return 0;
}