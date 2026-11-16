#pragma once

#include "hv/platform/cpu_capability_manager.hpp"
#include "hv/platform/platform_provider.hpp"
#include <mutex>
#include <optional>
#include <string>

namespace hv {

enum class WindowsBackendState { Stopped, Initializing, Ready, Failed };

struct WindowsPlatformSnapshot {
    WindowsSystemInformation system;
    CpuCapabilityReport cpu;
};

class WindowsPlatformBackend final {
public:
    WindowsPlatformBackend(IPlatformProvider& platform, CpuCapabilityManager& cpu)
        : platform_(platform), cpu_(cpu) {}

    [[nodiscard]] Result Initialize();
    [[nodiscard]] Result Shutdown();
    [[nodiscard]] WindowsBackendState GetState() const;
    [[nodiscard]] Result GetSnapshot(WindowsPlatformSnapshot& out) const;
    [[nodiscard]] std::string GetLastError() const;

private:
    void SetFailure(std::string message);

    IPlatformProvider& platform_;
    CpuCapabilityManager& cpu_;
    mutable std::mutex state_mutex_;
    std::mutex operation_mutex_;
    WindowsBackendState state_{WindowsBackendState::Stopped};
    std::optional<WindowsPlatformSnapshot> snapshot_;
    std::string last_error_;
};

} // namespace hv