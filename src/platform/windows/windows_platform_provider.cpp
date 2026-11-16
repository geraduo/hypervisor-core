#include "hv/platform/platform_provider.hpp"
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace {

using RtlGetVersionFunction = LONG(WINAPI*)(OSVERSIONINFOW*);

std::optional<bool> ReadRegistryBoolean(const wchar_t* key_path, const wchar_t* value_name) {
    HKEY key{};
    const auto open_status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, key_path, 0,
                                           KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key);
    if (open_status != ERROR_SUCCESS) return std::nullopt;

    DWORD value{};
    DWORD type{};
    DWORD size = sizeof(value);
    const auto query_status = RegQueryValueExW(key, value_name, nullptr, &type,
                                               reinterpret_cast<BYTE*>(&value), &size);
    RegCloseKey(key);
    if (query_status != ERROR_SUCCESS || type != REG_DWORD || size != sizeof(value))
        return std::nullopt;
    return value != 0;
}

} // namespace
#endif

namespace hv {

const char* ToString(ProcessorArchitecture architecture) noexcept {
    switch (architecture) {
    case ProcessorArchitecture::Unknown: return "Unknown";
    case ProcessorArchitecture::X86: return "x86";
    case ProcessorArchitecture::X64: return "x64";
    case ProcessorArchitecture::Arm64: return "ARM64";
    }
    return "Unknown";
}

std::string FormatWindowsVersion(const WindowsSystemInformation& information) {
    if (!information.is_windows) return "Non-Windows platform";

    std::string product = "Windows";
    if (information.major_version == 10 && information.minor_version == 0) {
        product = information.build_number >= 22000 ? "Windows 11" : "Windows 10";
    }
    return product + " " + std::to_string(information.major_version) + "." +
           std::to_string(information.minor_version) + " (build " +
           std::to_string(information.build_number) + ")";
}

Result WindowsPlatformProvider::QuerySystemInformation(WindowsSystemInformation& out) {
    out = WindowsSystemInformation{};
#ifdef _WIN32
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr)
        return Result::Error(Status::InitializationFailed, "Could not locate ntdll.dll");

    const auto get_version = reinterpret_cast<RtlGetVersionFunction>(GetProcAddress(ntdll, "RtlGetVersion"));
    if (get_version == nullptr)
        return Result::Error(Status::InitializationFailed, "RtlGetVersion is unavailable");

    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = static_cast<DWORD>(sizeof(version));
    const LONG version_status = get_version(&version);
    if (version_status < 0)
        return Result::Error(Status::InitializationFailed,
                             "RtlGetVersion failed with NTSTATUS " + std::to_string(version_status));

    SYSTEM_INFO system{};
    GetNativeSystemInfo(&system);
    out.is_windows = true;
    out.major_version = version.dwMajorVersion;
    out.minor_version = version.dwMinorVersion;
    out.build_number = version.dwBuildNumber;
    out.logical_processor_count = system.dwNumberOfProcessors;
    out.page_size = system.dwPageSize;
    switch (system.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64: out.architecture = ProcessorArchitecture::X64; break;
    case PROCESSOR_ARCHITECTURE_ARM64: out.architecture = ProcessorArchitecture::Arm64; break;
    case PROCESSOR_ARCHITECTURE_INTEL: out.architecture = ProcessorArchitecture::X86; break;
    default: out.architecture = ProcessorArchitecture::Unknown; break;
    }
    return Result::Ok();
#else
    return Result::Error(Status::Unsupported, "Windows platform information is available only on Windows");
#endif
}

Result WindowsPlatformProvider::QueryMitigationInformation(WindowsMitigationInformation& out) {
    out = WindowsMitigationInformation{};
#ifdef _WIN32
    out.vbs_configured = ReadRegistryBoolean(
        L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard", L"EnableVirtualizationBasedSecurity");
    out.hvci_configured = ReadRegistryBoolean(
        L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\HypervisorEnforcedCodeIntegrity",
        L"Enabled");
    out.credential_guard_configured = ReadRegistryBoolean(
        L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\CredentialGuard", L"Enabled");
    out.note = "Read-only registry configuration only; this does not prove that VBS or HVCI is running.";
    return Result::Ok();
#else
    return Result::Error(Status::Unsupported, "Windows mitigation state is available only on Windows");
#endif
}

} // namespace hv