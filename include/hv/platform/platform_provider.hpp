#pragma once

#include "hv/status.hpp"
#include <cstdint>
#include <optional>
#include <string>

namespace hv {

enum class ProcessorArchitecture { Unknown, X86, X64, Arm64 };

struct WindowsSystemInformation {
    bool is_windows{false};
    std::uint32_t major_version{};
    std::uint32_t minor_version{};
    std::uint32_t build_number{};
    ProcessorArchitecture architecture{ProcessorArchitecture::Unknown};
    std::uint32_t logical_processor_count{};
    std::uint32_t page_size{};
};

struct WindowsMitigationInformation {
    std::optional<bool> vbs_configured;
    std::optional<bool> hvci_configured;
    std::optional<bool> credential_guard_configured;
    std::string note;
};

class IPlatformProvider {
public:
    virtual ~IPlatformProvider() = default;
    [[nodiscard]] virtual Result QuerySystemInformation(WindowsSystemInformation& out) = 0;
    [[nodiscard]] virtual Result QueryMitigationInformation(WindowsMitigationInformation& out) = 0;
};

class WindowsPlatformProvider final : public IPlatformProvider {
public:
    [[nodiscard]] Result QuerySystemInformation(WindowsSystemInformation& out) override;
    [[nodiscard]] Result QueryMitigationInformation(WindowsMitigationInformation& out) override;
};

[[nodiscard]] const char* ToString(ProcessorArchitecture architecture) noexcept;
[[nodiscard]] std::string FormatWindowsVersion(const WindowsSystemInformation& information);

} // namespace hv