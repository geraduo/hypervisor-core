#pragma once
#include "hv/status.hpp"
#include <mutex>
#include <string>

namespace hv {

enum class State { Stopped, Initializing, Running, ShuttingDown, Failed };

class Lifecycle final {
public:
    [[nodiscard]] Result BeginInitialization();
    [[nodiscard]] Result MarkRunning();
    [[nodiscard]] Result BeginShutdown();
    [[nodiscard]] Result MarkStopped();
    void MarkFailed(std::string reason);
    [[nodiscard]] State GetState() const;
    [[nodiscard]] std::string GetLastError() const;

private:
    mutable std::mutex mutex_;
    State state_{State::Stopped};
    std::string last_error_{};
};

} // namespace hv
