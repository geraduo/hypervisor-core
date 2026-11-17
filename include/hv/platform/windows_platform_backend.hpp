#pragma once
#include "hv/platform/check_status.hpp"
#include "hv/status.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace hv {

enum class PlatformArchitecture : std::uint32_t { Unknown = 0, X86 = 1, X64 = 2, Arm64 = 3, Arm32 = 4 };
enum class PlatformState { Uninitialized, Initialized, Failed };
enum class PlatformCapabilityState { Unknown, Available, Unavailable, RequiresKernelVerification };
enum class PlatformSupport { Unsupported, SupportedWithWarnings, Supported };

struct OsVersion {
    std::uint32_t major{};
    std::uint32_t minor{};
    std::uint32_t build{};
    std::uint32_t revision{};
};

struct SystemInfo {
    std::string os_name;
    OsVersion os_version;
    bool is_windows_server{false};
    PlatformArchitecture native_architecture{PlatformArchitecture::Unknown};
    PlatformArchitecture process_architecture{PlatformArchitecture::Unknown};
    bool is_wow64_process{false};
    std::uint32_t logical_processor_count{};
    std::uint32_t page_size{};
    std::uint64_t total_physical_memory_bytes{};
};

struct PlatformCapability {
    std::string id;
    std::string title;
    CheckStatus status{CheckStatus::NotChecked};
    std::string detail;
};

struct PlatformCapabilityReport {
    PlatformSupport support{PlatformSupport::Unsupported};
    std::string summary;
    std::vector<PlatformCapability> capabilities;
};

// Data supplied by the Windows probe. Separating the probe from analysis makes the
// backend deterministic in tests and keeps Win32 API calls out of the policy layer.
struct PlatformProviderSnapshot {
    OsVersion os_version;
    std::string os_name;
    bool is_windows_server{false};
    PlatformArchitecture native_architecture{PlatformArchitecture::Unknown};
    PlatformArchitecture process_architecture{PlatformArchitecture::Unknown};
    bool is_wow64_process{false};
    std::uint32_t logical_processor_count{};
    std::uint32_t page_size{};
    std::uint64_t total_physical_memory_bytes{};
    PlatformCapabilityState windows_apis{PlatformCapabilityState::Unknown};
    PlatformCapabilityState cpuid{PlatformCapabilityState::Unknown};
    PlatformCapabilityState windows_hypervisor_platform_api{PlatformCapabilityState::Unknown};
    PlatformCapabilityState kernel_driver_interface{PlatformCapabilityState::Unknown};
    PlatformCapabilityState virtualization_firmware{PlatformCapabilityState::Unknown};
};

class IPlatformProvider {
public:
    virtual ~IPlatformProvider() = default;
    [[nodiscard]] virtual Result Query(PlatformProviderSnapshot& out) = 0;
};

// Real Win32 implementation using RtlGetVersion, GetNativeSystemInfo,
// GlobalMemoryStatusEx, GetProductInfo and a non-invasive WinHvPlatform.dll probe.
class NativePlatformProvider final : public IPlatformProvider {
public:
    [[nodiscard]] Result Query(PlatformProviderSnapshot& out) override;
};

// Owns the platform state machine. Initialize() probes once and caches the result;
// Refresh() re-probes; Shutdown() releases all cached data. No Win32 handle is kept
// alive after a query.
class WindowsPlatformBackend final {
public:
    explicit WindowsPlatformBackend(IPlatformProvider& provider) : provider_(provider) {}
    WindowsPlatformBackend(const WindowsPlatformBackend&) = delete;
    WindowsPlatformBackend& operator=(const WindowsPlatformBackend&) = delete;

    [[nodiscard]] Result Initialize();
    [[nodiscard]] Result Refresh();
    [[nodiscard]] Result Shutdown();
    [[nodiscard]] PlatformState GetState() const;
    [[nodiscard]] bool IsInitialized() const;
    [[nodiscard]] Result GetOsVersion(OsVersion& out) const;
    [[nodiscard]] Result GetArchitecture(PlatformArchitecture& out) const;
    [[nodiscard]] Result GetSystemInfo(SystemInfo& out) const;
    [[nodiscard]] Result GetCapabilities(PlatformCapabilityReport& out) const;
    [[nodiscard]] std::string GetLastError() const;

private:
    [[nodiscard]] Result ProbeLocked();
    void BuildCapabilitiesLocked();

    mutable std::mutex mutex_;
    IPlatformProvider& provider_;
    PlatformState state_{PlatformState::Uninitialized};
    PlatformProviderSnapshot snapshot_{};
    PlatformCapabilityReport report_{};
    std::string last_error_;
};

[[nodiscard]] const char* ToString(PlatformArchitecture value) noexcept;
[[nodiscard]] const char* ToString(PlatformState value) noexcept;
[[nodiscard]] const char* ToString(PlatformCapabilityState value) noexcept;
[[nodiscard]] const char* ToString(PlatformSupport value) noexcept;

[[nodiscard]] std::string FormatPlatformReportText(const SystemInfo& system,
                                                    const PlatformCapabilityReport& report,
                                                    PlatformState state);
[[nodiscard]] std::string FormatPlatformReportJson(const SystemInfo& system,
                                                    const PlatformCapabilityReport& report,
                                                    PlatformState state);

} // namespace hv
