// hv_vmx_diagnostics: user-mode Intel VT-x readiness diagnostics.
//
// This tool NEVER executes VMXON, reads or writes a VMCS, enters a guest,
// configures EPT, accesses physical memory, loads a driver or reads another
// process. It only combines unprivileged CPUID, Windows platform information,
// the compatibility policy snapshot and the fixed-size diagnostic driver
// protocol. A missing driver is a normal diagnostic result.
//
// Exit codes:
//   0  the diagnostic lifecycle produced a verdict (NotSupported, Blocked and
//      ReadyForReview are all valid verdicts)
//   1  a diagnostic component failed internally
//   2  invalid command line
#include "hv/platform/cpu_capability_manager.hpp"
#include "hv/platform/kernel_driver_interface.hpp"
#include "hv/platform/platform_compatibility_manager.hpp"
#include "hv/platform/windows_platform_backend.hpp"
#include "hv/virtualization/vmx_capability_manager.hpp"
#include "hv/virtualization/vmx_diagnostics.hpp"
#include "hv/virtualization/vmx_lifecycle_controller.hpp"
#include "hv/virtualization/vmx_readiness_manager.hpp"

#include <iostream>
#include <string>

namespace {

void PrintUsage() {
    std::cerr << "Usage: hv_vmx_diagnostics [--json] [--no-driver]\n"
                 "  --json       emit a single JSON document\n"
                 "  --no-driver  skip diagnostic device interface discovery; the driver\n"
                 "               compatibility result becomes NOT_CHECKED and blocks review\n"
                 "  --help, -h   show this help\n";
}

} // namespace

int main(int argc, char** argv) {
    bool json = false;
    bool skip_driver = false;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--json") {
            json = true;
        } else if (argument == "--no-driver") {
            skip_driver = true;
        } else if (argument == "--help" || argument == "-h") {
            PrintUsage();
            return 0;
        } else {
            std::cerr << "Unknown argument: " << argument << '\n';
            PrintUsage();
            return 2;
        }
    }

    hv::NativePlatformProvider platform_provider;
    hv::WindowsPlatformBackend platform_backend(platform_provider);

    hv::NativeCpuidProvider cpuid;
    hv::NativeLogicalProcessorProbe processor_probe(cpuid);
    hv::NativeCompatibilityEnvironment compatibility_environment;
    hv::PlatformCompatibilityManager compatibility_manager(cpuid, compatibility_environment);
    hv::KernelDriverInterface driver;

    hv::VmxCapabilityManager capability_manager(cpuid, &processor_probe);
    hv::VmxReadinessManager readiness_manager(platform_backend, driver, compatibility_manager);
    hv::VmxReadinessOptions options;
    options.query_platform = true;
    options.query_compatibility = true;
    options.query_driver = !skip_driver;
    readiness_manager.SetOptions(options);

    hv::VmxLifecycleController controller(capability_manager, readiness_manager);
    const hv::Result init_result = controller.Initialize();
    const hv::VmxDiagnosticsReport report = controller.CaptureReport();

    if (json) {
        std::cout << hv::FormatVmxDiagnosticsJson(report);
    } else {
        if (!skip_driver) {
            std::cout << "Driver discovery was requested. The diagnostic driver is optional; "
                         "device opens, IOCTLs and protocol checks are reported below.\n\n";
        }
        std::cout << hv::FormatVmxDiagnosticsText(report);
    }

    if (!init_result.Succeeded()) {
        if (!json) {
            std::cerr << "\nDiagnostic lifecycle failed internally: " << init_result.message
                      << '\n';
        }
        return 1;
    }
    return 0;
}
