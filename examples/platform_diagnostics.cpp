// hv_platform_diagnostics: user-mode Windows platform and driver diagnostics.
//
// This example never executes VMXON, accesses physical memory or loads a driver.
// A missing GeraduoDriver is reported as a normal diagnostic result.
//
// Exit codes:
//   0  all diagnostic layers completed (unsupported hardware is still a result)
//   1  a platform/CPU/compatibility probe failed internally
//   2  invalid command line
#include "hv/platform/cpu_capability_manager.hpp"
#include "hv/platform/driver_diagnostics.hpp"
#include "hv/platform/kernel_driver_interface.hpp"
#include "hv/platform/platform_compatibility_manager.hpp"
#include "hv/platform/windows_platform_backend.hpp"

#include <cstdio>
#include <iostream>
#include <string>

namespace {

void PrintUsage() {
    std::cerr << "Usage: hv_platform_diagnostics [--json] [--no-driver]\n"
                 "  --json       emit a single JSON document\n"
                 "  --no-driver  skip device interface discovery\n";
}

std::string JsonEscape(const std::string& text) {
    std::string result;
    for (const char ch : text) {
        const auto value = static_cast<unsigned char>(ch);
        switch (ch) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\n': result += "\\n"; break;
        case '\f': result += "\\f"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (value < 0x20) {
                char buffer[8]{};
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", value);
                result += buffer;
            } else {
                result.push_back(ch);
            }
            break;
        }
    }
    return result;
}

const char* BoolText(bool value) {
    return value ? "true" : "false";
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
    hv::SystemInfo system_info;
    hv::PlatformCapabilityReport platform_report;
    const hv::Result platform_init = platform_backend.Initialize();
    const bool platform_ok = platform_init.Succeeded() &&
                             platform_backend.GetSystemInfo(system_info).Succeeded() &&
                             platform_backend.GetCapabilities(platform_report).Succeeded();
    const std::string platform_error =
        platform_ok ? std::string{}
                    : (platform_init.Succeeded() ? platform_backend.GetLastError()
                                                 : platform_init.message);

    hv::NativeCpuidProvider cpuid;
    hv::CpuCapabilityManager cpu_manager(cpuid);
    hv::CpuCapabilityReport cpu_report;
    const hv::Result cpu_result = cpu_manager.Probe(cpu_report);
    const bool cpu_ok = cpu_result.Succeeded();

    hv::NativeCompatibilityEnvironment compatibility_environment;
    hv::PlatformCompatibilityManager compatibility_manager(cpuid, compatibility_environment);
    hv::CompatibilityReport compatibility_report;
    const hv::Result compatibility_result = compatibility_manager.Evaluate(compatibility_report);
    const bool compatibility_ok = compatibility_result.Succeeded();

    hv::KernelDriverInterface driver;
    hv::DriverPlatformInfoResponse driver_info{};
    bool driver_connected = false;
    bool driver_query_ok = false;
    std::string driver_error;
    if (!skip_driver) {
        const hv::Result connect_result = driver.Connect();
        if (connect_result.Succeeded()) {
            driver_connected = true;
            const hv::Result query_result = driver.QueryPlatformInfo(driver_info);
            driver_query_ok = query_result.Succeeded();
            if (!query_result.Succeeded()) driver_error = query_result.message;
        } else {
            driver_error = driver.GetLastError();
        }
    }
    const auto driver_events = driver.Diagnostics().Snapshot();

    if (json) {
        std::cout << "{\n";
        if (!platform_ok) {
            std::cout << "  \"platform_error\": \"" << JsonEscape(platform_error)
                      << "\",\n";
        }
        std::cout << "  \"platform\": "
                  << hv::FormatPlatformReportJson(system_info, platform_report,
                                                  platform_backend.GetState())
                  << ",\n";
        std::cout << "  \"cpu\": " << hv::FormatCpuCapabilityReportJson(cpu_report) << ",\n";
        std::cout << "  \"compatibility\": "
                  << hv::FormatCompatibilityReportJson(compatibility_report) << ",\n";
        std::cout << "  \"driver\": {\n";
        std::cout << "    \"skipped\": " << BoolText(skip_driver) << ",\n";
        std::cout << "    \"connected\": " << BoolText(driver_connected) << ",\n";
        std::cout << "    \"query_succeeded\": " << BoolText(driver_query_ok) << ",\n";
        std::cout << "    \"error\": \"" << JsonEscape(driver_error) << "\",\n";
        std::cout << "    \"info\": ";
        if (driver_query_ok)
            std::cout << hv::FormatDriverPlatformInfoJson(driver_info) << ",\n";
        else
            std::cout << "null,\n";
        std::cout << "    \"diagnostics\": "
                  << hv::FormatDriverDiagnosticsJson(driver_events);
        std::cout << "  }\n";
        std::cout << "}\n";
    } else {
        std::cout << "=== Windows platform backend ===\n";
        if (platform_ok) {
            std::cout << hv::FormatPlatformReportText(system_info, platform_report,
                                                      platform_backend.GetState());
        } else {
            std::cout << "Platform initialization failed: " << platform_error << '\n';
        }

        std::cout << "\n=== CPU capabilities ===\n" << hv::FormatCpuCapabilityReportText(cpu_report);
        if (!cpu_ok) std::cout << "CPU probe error: " << cpu_result.message << '\n';

        std::cout << "\n=== Platform compatibility ===\n"
                  << hv::FormatCompatibilityReportText(compatibility_report);
        if (!compatibility_ok)
            std::cout << "Compatibility evaluation error: " << compatibility_result.message << '\n';

        std::cout << "\n=== Geraduo diagnostic driver ===\n";
        if (skip_driver) {
            std::cout << "Driver discovery skipped (--no-driver).\n";
        } else if (!driver_connected) {
            std::cout << "Not connected. " << driver_error << '\n';
            std::cout << "The user-mode diagnostics above are still valid. The driver is optional "
                         "and is not required for platform/CPU reporting.\n";
        } else {
            std::cout << "Connected to the diagnostic device interface.\n";
            if (driver_query_ok) {
                std::cout << hv::FormatDriverPlatformInfoText(driver_info);
            } else {
                std::cout << "Query failed: " << driver_error << '\n';
            }
        }
        std::cout << '\n' << hv::FormatDriverDiagnosticsText(driver_events);
    }

    return (platform_ok && cpu_ok && compatibility_ok) ? 0 : 1;
}
