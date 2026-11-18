#include "hv/platform/windows_platform_backend.hpp"

#include "hv/platform/cpuid.hpp"
#include "report_json.hpp"

#include <sstream>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <winternl.h>
#endif

namespace hv {
namespace {

void Add(PlatformCapabilityReport& report, std::string id, std::string title,
         CheckStatus status, std::string detail) {
    report.capabilities.push_back(
        {std::move(id), std::move(title), status, std::move(detail)});
}

bool HasFail(const PlatformCapabilityReport& report) {
    for (const auto& capability : report.capabilities)
        if (capability.status == CheckStatus::Fail) return true;
    return false;
}

bool HasWarning(const PlatformCapabilityReport& report) {
    for (const auto& capability : report.capabilities)
        if (capability.status == CheckStatus::Warn) return true;
    return false;
}

std::string VersionText(const OsVersion& version) {
    return std::to_string(version.major) + "." + std::to_string(version.minor) + "." +
           std::to_string(version.build) + "." + std::to_string(version.revision);
}

#if defined(_WIN32)
#pragma warning(push)
#pragma warning(disable : 4191) // GetProcAddress returns FARPROC; explicit conversion is intentional.
FARPROC ResolveFunction(HMODULE module, const char* name) {
    return module == nullptr ? nullptr : GetProcAddress(module, name);
}
#pragma warning(pop)

PlatformArchitecture MapMachineType(USHORT machine) {
    switch (machine) {
    case IMAGE_FILE_MACHINE_I386: return PlatformArchitecture::X86;
    case IMAGE_FILE_MACHINE_AMD64: return PlatformArchitecture::X64;
    case IMAGE_FILE_MACHINE_ARM64: return PlatformArchitecture::Arm64;
    case IMAGE_FILE_MACHINE_ARMNT: return PlatformArchitecture::Arm32;
    default: return PlatformArchitecture::Unknown;
    }
}

PlatformArchitecture MapProcessorArchitecture(WORD architecture) {
    switch (architecture) {
    case PROCESSOR_ARCHITECTURE_INTEL: return PlatformArchitecture::X86;
    case PROCESSOR_ARCHITECTURE_AMD64: return PlatformArchitecture::X64;
    case PROCESSOR_ARCHITECTURE_ARM64: return PlatformArchitecture::Arm64;
    case PROCESSOR_ARCHITECTURE_ARM: return PlatformArchitecture::Arm32;
    default: return PlatformArchitecture::Unknown;
    }
}

std::string DetermineOsName(std::uint32_t major, std::uint32_t minor, std::uint32_t build,
                            bool server) {
    if (server) {
        if (build >= 26100) return "Windows Server 2025";
        if (build >= 20348) return "Windows Server 2022";
        if (build >= 17763) return "Windows Server 2019";
        return "Windows Server";
    }
    if (major == 10 && build >= 22000) return "Windows 11";
    if (major == 10) return "Windows 10";
    if (major == 6 && minor == 3) return "Windows 8.1";
    if (major == 6 && minor == 2) return "Windows 8";
    if (major == 6 && minor == 1) return "Windows 7";
    return "Windows (unknown version)";
}
#endif

} // namespace

const char* ToString(PlatformArchitecture value) noexcept {
    switch (value) {
    case PlatformArchitecture::Unknown: return "Unknown";
    case PlatformArchitecture::X86: return "x86";
    case PlatformArchitecture::X64: return "x64";
    case PlatformArchitecture::Arm64: return "ARM64";
    case PlatformArchitecture::Arm32: return "ARM32";
    }
    return "Unknown";
}

const char* ToString(PlatformState value) noexcept {
    switch (value) {
    case PlatformState::Uninitialized: return "Uninitialized";
    case PlatformState::Initialized: return "Initialized";
    case PlatformState::Failed: return "Failed";
    }
    return "Uninitialized";
}

const char* ToString(PlatformCapabilityState value) noexcept {
    switch (value) {
    case PlatformCapabilityState::Unknown: return "Unknown";
    case PlatformCapabilityState::Available: return "Available";
    case PlatformCapabilityState::Unavailable: return "Unavailable";
    case PlatformCapabilityState::RequiresKernelVerification: return "RequiresKernelVerification";
    }
    return "Unknown";
}

const char* ToString(PlatformSupport value) noexcept {
    switch (value) {
    case PlatformSupport::Unsupported: return "Unsupported";
    case PlatformSupport::SupportedWithWarnings: return "SupportedWithWarnings";
    case PlatformSupport::Supported: return "Supported";
    }
    return "Unsupported";
}

Result NativePlatformProvider::Query(PlatformProviderSnapshot& out) {
    out = PlatformProviderSnapshot{};
#if defined(_WIN32)
    using RtlGetVersionFn = LONG(NTAPI*)(PRTL_OSVERSIONINFOW);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr)
        return Result::Error(Status::InitializationFailed, "ntdll.dll is not loaded.");

    RtlGetVersionFn rtl_get_version = nullptr;
#pragma warning(push)
#pragma warning(disable : 4191)
    rtl_get_version = reinterpret_cast<RtlGetVersionFn>(ResolveFunction(ntdll, "RtlGetVersion"));
#pragma warning(pop)
    if (rtl_get_version == nullptr)
        return Result::Error(Status::Unsupported, "RtlGetVersion is not available in ntdll.dll.");

    RTL_OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (rtl_get_version(&version) != 0)
        return Result::Error(Status::InitializationFailed, "RtlGetVersion failed.");

    out.os_version.major = version.dwMajorVersion;
    out.os_version.minor = version.dwMinorVersion;
    out.os_version.build = version.dwBuildNumber;
    out.os_version.revision = 0;

    // RtlGetVersion does not provide the UBR revision. Read the documented,
    // read-only CurrentVersion value when it is available.
    DWORD ubr = 0;
    DWORD ubr_size = sizeof(ubr);
    if (RegGetValueW(HKEY_LOCAL_MACHINE,
                     L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR",
                     RRF_RT_REG_DWORD, nullptr, &ubr, &ubr_size) == ERROR_SUCCESS) {
        out.os_version.revision = ubr;
    }

    DWORD product_type = 0;
    if (GetProductInfo(version.dwMajorVersion, version.dwMinorVersion, 0, 0, &product_type)) {
        out.is_windows_server = product_type != VER_NT_WORKSTATION;
    }
    out.os_name = DetermineOsName(out.os_version.major, out.os_version.minor,
                                  out.os_version.build, out.is_windows_server);

    SYSTEM_INFO native_info{};
    GetNativeSystemInfo(&native_info);
    out.native_architecture = MapProcessorArchitecture(native_info.wProcessorArchitecture);

    SYSTEM_INFO process_info{};
    GetSystemInfo(&process_info);
    out.process_architecture = MapProcessorArchitecture(process_info.wProcessorArchitecture);
    out.is_wow64_process =
        process_info.wProcessorArchitecture != native_info.wProcessorArchitecture;

    using IsWow64Process2Fn = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
    IsWow64Process2Fn is_wow64_process2 = nullptr;
#pragma warning(push)
#pragma warning(disable : 4191)
    is_wow64_process2 = reinterpret_cast<IsWow64Process2Fn>(
        ResolveFunction(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2"));
#pragma warning(pop)
    if (is_wow64_process2 != nullptr) {
        USHORT process_machine = IMAGE_FILE_MACHINE_UNKNOWN;
        USHORT native_machine = IMAGE_FILE_MACHINE_UNKNOWN;
        if (is_wow64_process2(GetCurrentProcess(), &process_machine, &native_machine)) {
            if (native_machine != IMAGE_FILE_MACHINE_UNKNOWN)
                out.native_architecture = MapMachineType(native_machine);
            out.is_wow64_process = process_machine != IMAGE_FILE_MACHINE_UNKNOWN;
            out.process_architecture = out.is_wow64_process ? MapMachineType(process_machine)
                                                            : out.native_architecture;
        }
    } else {
        BOOL wow64 = FALSE;
        if (IsWow64Process(GetCurrentProcess(), &wow64) && wow64 == TRUE) {
            out.is_wow64_process = true;
            out.process_architecture = PlatformArchitecture::X86;
        }
    }

    const DWORD active_processors = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    out.logical_processor_count = static_cast<std::uint32_t>(
        active_processors != 0 ? active_processors : native_info.dwNumberOfProcessors);
    out.page_size = static_cast<std::uint32_t>(native_info.dwPageSize);

    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory))
        out.total_physical_memory_bytes = static_cast<std::uint64_t>(memory.ullTotalPhys);

    out.windows_apis = PlatformCapabilityState::Available;
    out.cpuid = NativeCpuidProvider::IsAvailableOnThisBuild()
                    ? PlatformCapabilityState::Available
                    : PlatformCapabilityState::Unavailable;

    HMODULE win_hv_platform = LoadLibraryW(L"WinHvPlatform.dll");
    if (win_hv_platform != nullptr) {
        const bool has_api = ResolveFunction(win_hv_platform, "WHvGetCapability") != nullptr;
        out.windows_hypervisor_platform_api =
            has_api ? PlatformCapabilityState::Available : PlatformCapabilityState::Unavailable;
        (void)FreeLibrary(win_hv_platform);
    } else {
        out.windows_hypervisor_platform_api = PlatformCapabilityState::Unavailable;
    }

    // These cannot be established with unprivileged user-mode APIs. The driver and
    // kernel layer must verify them; the platform report says NotChecked.
    out.kernel_driver_interface = PlatformCapabilityState::RequiresKernelVerification;
    out.virtualization_firmware = PlatformCapabilityState::RequiresKernelVerification;
    return Result::Ok();
#else
    (void)out;
    return Result::Error(Status::Unsupported,
                         "The native Windows platform provider is only available on Windows.");
#endif
}

Result WindowsPlatformBackend::Initialize() {
    std::lock_guard lock(mutex_);
    if (state_ == PlatformState::Initialized) return Result::Ok();
    return ProbeLocked();
}

Result WindowsPlatformBackend::Refresh() {
    std::lock_guard lock(mutex_);
    return ProbeLocked();
}

Result WindowsPlatformBackend::Shutdown() {
    std::lock_guard lock(mutex_);
    state_ = PlatformState::Uninitialized;
    snapshot_ = PlatformProviderSnapshot{};
    report_ = PlatformCapabilityReport{};
    last_error_.clear();
    return Result::Ok();
}

PlatformState WindowsPlatformBackend::GetState() const {
    std::lock_guard lock(mutex_);
    return state_;
}

bool WindowsPlatformBackend::IsInitialized() const {
    std::lock_guard lock(mutex_);
    return state_ == PlatformState::Initialized;
}

Result WindowsPlatformBackend::GetOsVersion(OsVersion& out) const {
    std::lock_guard lock(mutex_);
    if (state_ != PlatformState::Initialized)
        return Result::Error(Status::InvalidState,
                             "WindowsPlatformBackend must be initialized before reading the OS version.");
    out = snapshot_.os_version;
    return Result::Ok();
}

Result WindowsPlatformBackend::GetArchitecture(PlatformArchitecture& out) const {
    std::lock_guard lock(mutex_);
    if (state_ != PlatformState::Initialized)
        return Result::Error(Status::InvalidState,
                             "WindowsPlatformBackend must be initialized before reading the architecture.");
    out = snapshot_.native_architecture;
    return Result::Ok();
}

Result WindowsPlatformBackend::GetSystemInfo(SystemInfo& out) const {
    std::lock_guard lock(mutex_);
    if (state_ != PlatformState::Initialized)
        return Result::Error(Status::InvalidState,
                             "WindowsPlatformBackend must be initialized before reading system information.");
    out.os_name = snapshot_.os_name;
    out.os_version = snapshot_.os_version;
    out.is_windows_server = snapshot_.is_windows_server;
    out.native_architecture = snapshot_.native_architecture;
    out.process_architecture = snapshot_.process_architecture;
    out.is_wow64_process = snapshot_.is_wow64_process;
    out.logical_processor_count = snapshot_.logical_processor_count;
    out.page_size = snapshot_.page_size;
    out.total_physical_memory_bytes = snapshot_.total_physical_memory_bytes;
    return Result::Ok();
}

Result WindowsPlatformBackend::GetCapabilities(PlatformCapabilityReport& out) const {
    std::lock_guard lock(mutex_);
    if (state_ != PlatformState::Initialized)
        return Result::Error(Status::InvalidState,
                             "WindowsPlatformBackend must be initialized before reading capabilities.");
    out = report_;
    return Result::Ok();
}

std::string WindowsPlatformBackend::GetLastError() const {
    std::lock_guard lock(mutex_);
    return last_error_;
}

Result WindowsPlatformBackend::ProbeLocked() {
    PlatformProviderSnapshot candidate{};
    const Result query = provider_.Query(candidate);
    if (!query.Succeeded()) {
        state_ = PlatformState::Failed;
        last_error_ = query.message.empty() ? "The platform provider failed." : query.message;
        return Result::Error(query.status, last_error_);
    }

    if (candidate.os_version.major == 0) {
        state_ = PlatformState::Failed;
        last_error_ = "The platform provider returned an invalid OS version.";
        return Result::Error(Status::InitializationFailed, last_error_);
    }
    if (candidate.page_size == 0 || (candidate.page_size & (candidate.page_size - 1u)) != 0) {
        state_ = PlatformState::Failed;
        last_error_ = "The platform provider returned an invalid page size.";
        return Result::Error(Status::InitializationFailed, last_error_);
    }
    if (candidate.logical_processor_count == 0) {
        state_ = PlatformState::Failed;
        last_error_ = "The platform provider returned zero logical processors.";
        return Result::Error(Status::InitializationFailed, last_error_);
    }

    snapshot_ = std::move(candidate);
    state_ = PlatformState::Initialized;
    last_error_.clear();
    BuildCapabilitiesLocked();
    return Result::Ok();
}

void WindowsPlatformBackend::BuildCapabilitiesLocked() {
    report_ = PlatformCapabilityReport{};
    const OsVersion& version = snapshot_.os_version;

    if (version.major == 10 && version.build >= 10240) {
        Add(report_, "os.version", "Windows 10/11 or later", CheckStatus::Pass,
            snapshot_.os_name + " " + VersionText(version) + " is supported.");
    } else if (version.major > 10) {
        Add(report_, "os.version", "Windows 10/11 or later", CheckStatus::Warn,
            snapshot_.os_name + " " + VersionText(version) +
                " is newer than the validated Windows 10/11 range.");
    } else {
        Add(report_, "os.version", "Windows 10/11 or later", CheckStatus::Fail,
            snapshot_.os_name + " " + VersionText(version) +
                " is outside the supported Windows 10/11 range.");
    }

    Add(report_, "os.edition", "Client Windows edition", snapshot_.is_windows_server
                                                                  ? CheckStatus::Warn
                                                                  : CheckStatus::Pass,
        snapshot_.is_windows_server
            ? "Windows Server was detected; v0.6.0 validates Windows 10/11 client x64."
            : "A client Windows edition was reported.");

    if (snapshot_.native_architecture == PlatformArchitecture::X64) {
        Add(report_, "os.architecture", "Native x64 architecture", CheckStatus::Pass,
            "The operating system is x64.");
    } else {
        Add(report_, "os.architecture", "Native x64 architecture", CheckStatus::Fail,
            std::string("Native architecture is ") + ToString(snapshot_.native_architecture) +
                "; Geraduo v0.6.0 supports x64 only.");
    }

    const bool process_is_x64 =
        snapshot_.process_architecture == PlatformArchitecture::X64 && !snapshot_.is_wow64_process;
    Add(report_, "os.process_architecture", "x64 process context", process_is_x64
                                                                         ? CheckStatus::Pass
                                                                         : CheckStatus::Warn,
        process_is_x64
            ? "The process is running as native x64."
            : std::string("The process architecture is ") +
                  ToString(snapshot_.process_architecture) +
                  (snapshot_.is_wow64_process ? " under WOW64; run the native x64 build." : "."));

    Add(report_, "platform.win32", "Win32 platform APIs",
        snapshot_.windows_apis == PlatformCapabilityState::Available ? CheckStatus::Pass
                                                                      : CheckStatus::Fail,
        std::string("Provider state: ") + ToString(snapshot_.windows_apis) + ".");

    switch (snapshot_.cpuid) {
    case PlatformCapabilityState::Available:
        Add(report_, "platform.cpuid", "Unprivileged CPUID", CheckStatus::Pass,
            "CPUID is available; CPU feature interpretation belongs to CpuCapabilityManager.");
        break;
    case PlatformCapabilityState::Unavailable:
        Add(report_, "platform.cpuid", "Unprivileged CPUID", CheckStatus::Fail,
            "CPUID is not available on this build; CPU capability probing cannot run.");
        break;
    default:
        Add(report_, "platform.cpuid", "Unprivileged CPUID", CheckStatus::NotChecked,
            std::string("Provider state: ") + ToString(snapshot_.cpuid) + ".");
        break;
    }

    switch (snapshot_.windows_hypervisor_platform_api) {
    case PlatformCapabilityState::Available:
        Add(report_, "platform.whpx_api", "Windows Hypervisor Platform API", CheckStatus::Info,
            "WinHvPlatform.dll and WHvGetCapability are present. This is not proof that a "
            "hypervisor is active or that VMX is available.");
        break;
    case PlatformCapabilityState::Unavailable:
        Add(report_, "platform.whpx_api", "Windows Hypervisor Platform API", CheckStatus::Info,
            "WinHvPlatform.dll/WHvGetCapability is not available. It is optional for the native "
            "driver path and is not required by this layer.");
        break;
    default:
        Add(report_, "platform.whpx_api", "Windows Hypervisor Platform API", CheckStatus::NotChecked,
            std::string("Provider state: ") + ToString(snapshot_.windows_hypervisor_platform_api) +
                ".");
        break;
    }

    switch (snapshot_.kernel_driver_interface) {
    case PlatformCapabilityState::Available:
        Add(report_, "platform.kernel_driver", "Geraduo diagnostic driver", CheckStatus::Pass,
            "The provider reports that the diagnostic device interface is present. "
            "KernelDriverInterface performs the authoritative connection check.");
        break;
    case PlatformCapabilityState::Unavailable:
        Add(report_, "platform.kernel_driver", "Geraduo diagnostic driver", CheckStatus::Warn,
            "The diagnostic driver interface is not available. User-mode platform diagnostics can "
            "still run; driver-backed checks are skipped.");
        break;
    case PlatformCapabilityState::RequiresKernelVerification:
        Add(report_, "platform.kernel_driver", "Geraduo diagnostic driver", CheckStatus::NotChecked,
            "Driver availability is checked separately by KernelDriverInterface; the platform "
            "backend does not claim that the driver is loaded.");
        break;
    default:
        Add(report_, "platform.kernel_driver", "Geraduo diagnostic driver", CheckStatus::NotChecked,
            "Driver availability is unknown.");
        break;
    }

    switch (snapshot_.virtualization_firmware) {
    case PlatformCapabilityState::Available:
        Add(report_, "platform.virtualization_firmware", "Virtualization firmware state",
            CheckStatus::Info,
            "The provider reports firmware virtualization availability. This is not proof that "
            "IA32_FEATURE_CONTROL enables VMXON on every processor.");
        break;
    case PlatformCapabilityState::Unavailable:
        Add(report_, "platform.virtualization_firmware", "Virtualization firmware state",
            CheckStatus::Warn,
            "Firmware virtualization was not reported. Kernel-mode verification is required.");
        break;
    case PlatformCapabilityState::RequiresKernelVerification:
        Add(report_, "platform.virtualization_firmware", "Virtualization firmware state",
            CheckStatus::NotChecked,
            "IA32_FEATURE_CONTROL, CR0/CR4 fixed bits and VMX capability MSRs require kernel mode.");
        break;
    default:
        Add(report_, "platform.virtualization_firmware", "Virtualization firmware state",
            CheckStatus::NotChecked, "Firmware virtualization state is unknown.");
        break;
    }

    Add(report_, "platform.processor_info", "Processor and memory information", CheckStatus::Info,
        std::to_string(snapshot_.logical_processor_count) + " logical processors, " +
            std::to_string(snapshot_.total_physical_memory_bytes) + " bytes of physical memory, " +
            std::to_string(snapshot_.page_size) + "-byte pages.");
    Add(report_, "platform.boundary", "Hardware virtualization boundary", CheckStatus::Info,
        "This layer did not execute VMXON, build a VMCS, create hardware EPT/NPT tables, access "
        "physical memory, launch a guest, or hide a hypervisor.");

    report_.support = HasFail(report_) ? PlatformSupport::Unsupported
                      : HasWarning(report_) ? PlatformSupport::SupportedWithWarnings
                                            : PlatformSupport::Supported;

    if (report_.support == PlatformSupport::Unsupported) {
        report_.summary =
            "The Windows platform backend is initialized, but required Windows 10/11 x64 "
            "capabilities are missing. Hardware virtualization was not attempted.";
    } else if (report_.support == PlatformSupport::SupportedWithWarnings) {
        report_.summary =
            "The Windows platform backend is initialized with warnings. No hardware "
            "virtualization was started; kernel-mode checks remain required.";
    } else {
        report_.summary =
            "The Windows platform backend is initialized. This is a user-mode diagnostic layer "
            "only: no hardware virtualization was performed and VMXON is not confirmed.";
    }
}

std::string FormatPlatformReportText(const SystemInfo& system,
                                      const PlatformCapabilityReport& report,
                                      PlatformState state) {
    std::ostringstream out;
    out << "Geraduo Windows platform backend\n";
    out << "State: " << ToString(state) << '\n';
    out << "Support: " << ToString(report.support) << '\n';
    out << "Summary: " << report.summary << '\n';
    out << "OS: " << (system.os_name.empty() ? "(unknown)" : system.os_name) << " "
        << VersionText(system.os_version) << " (server="
        << (system.is_windows_server ? "yes" : "no") << ")\n";
    out << "Architecture: " << ToString(system.native_architecture) << " native, "
        << ToString(system.process_architecture) << " process"
        << (system.is_wow64_process ? " (WOW64)" : "") << '\n';
    out << "Logical processors: " << system.logical_processor_count << '\n';
    out << "Page size: " << system.page_size << '\n';
    out << "Physical memory: " << system.total_physical_memory_bytes << " bytes\n";
    out << "Capabilities:\n";
    for (const auto& capability : report.capabilities)
        out << "  [" << ToString(capability.status) << "] " << capability.title << " - "
            << capability.detail << '\n';
    return out.str();
}

std::string FormatPlatformReportJson(const SystemInfo& system,
                                      const PlatformCapabilityReport& report,
                                      PlatformState state) {
    std::ostringstream out;
    out << "{\n";
    out << "  \"state\": \"" << ToString(state) << "\",\n";
    out << "  \"support\": \"" << ToString(report.support) << "\",\n";
    out << "  \"summary\": \"" << detail::JsonEscape(report.summary) << "\",\n";
    out << "  \"system\": {\n";
    out << "    \"os_name\": \"" << detail::JsonEscape(system.os_name) << "\",\n";
    out << "    \"os_version\": \"" << VersionText(system.os_version) << "\",\n";
    out << "    \"is_windows_server\": " << detail::JsonBool(system.is_windows_server) << ",\n";
    out << "    \"native_architecture\": \"" << ToString(system.native_architecture) << "\",\n";
    out << "    \"process_architecture\": \"" << ToString(system.process_architecture) << "\",\n";
    out << "    \"is_wow64_process\": " << detail::JsonBool(system.is_wow64_process) << ",\n";
    out << "    \"logical_processor_count\": " << system.logical_processor_count << ",\n";
    out << "    \"page_size\": " << system.page_size << ",\n";
    out << "    \"total_physical_memory_bytes\": " << system.total_physical_memory_bytes << "\n";
    out << "  },\n";
    out << "  \"capabilities\": [\n";
    for (std::size_t i = 0; i < report.capabilities.size(); ++i) {
        const auto& capability = report.capabilities[i];
        out << "    {\"id\": \"" << detail::JsonEscape(capability.id) << "\", \"title\": \""
            << detail::JsonEscape(capability.title) << "\", \"status\": \""
            << ToString(capability.status) << "\", \"detail\": \""
            << detail::JsonEscape(capability.detail) << "\"}"
            << (i + 1 < report.capabilities.size() ? "," : "") << '\n';
    }
    out << "  ]\n}\n";
    return out.str();
}

} // namespace hv
