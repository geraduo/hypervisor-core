#include "hv/platform/windows_platform_backend.hpp"
#include <utility>

namespace hv {

Result WindowsPlatformBackend::Initialize() {
    std::lock_guard operation_lock(operation_mutex_);
    {
        std::lock_guard state_lock(state_mutex_);
        if (state_ == WindowsBackendState::Ready) return Result::Ok();
        state_ = WindowsBackendState::Initializing;
        last_error_.clear();
    }

    WindowsPlatformSnapshot snapshot;
    auto result = platform_.QuerySystemInformation(snapshot.system);
    if (!result.Succeeded()) {
        SetFailure(result.message);
        return result;
    }
    if (!snapshot.system.is_windows) {
        const std::string message = "The active platform provider did not identify a Windows host";
        SetFailure(message);
        return Result::Error(Status::Unsupported, message);
    }

    result = cpu_.Query(snapshot.cpu);
    if (!result.Succeeded()) {
        SetFailure(result.message);
        return result;
    }

    {
        std::lock_guard state_lock(state_mutex_);
        snapshot_ = std::move(snapshot);
        state_ = WindowsBackendState::Ready;
        last_error_.clear();
    }
    return Result::Ok();
}

Result WindowsPlatformBackend::Shutdown() {
    std::lock_guard operation_lock(operation_mutex_);
    std::lock_guard state_lock(state_mutex_);
    snapshot_.reset();
    state_ = WindowsBackendState::Stopped;
    last_error_.clear();
    return Result::Ok();
}

WindowsBackendState WindowsPlatformBackend::GetState() const {
    std::lock_guard lock(state_mutex_);
    return state_;
}

Result WindowsPlatformBackend::GetSnapshot(WindowsPlatformSnapshot& out) const {
    std::lock_guard lock(state_mutex_);
    if (state_ != WindowsBackendState::Ready || !snapshot_.has_value())
        return Result::Error(Status::InvalidState, "Windows platform backend is not initialized");
    out = *snapshot_;
    return Result::Ok();
}

std::string WindowsPlatformBackend::GetLastError() const {
    std::lock_guard lock(state_mutex_);
    return last_error_;
}

void WindowsPlatformBackend::SetFailure(std::string message) {
    std::lock_guard lock(state_mutex_);
    snapshot_.reset();
    state_ = WindowsBackendState::Failed;
    last_error_ = std::move(message);
}

} // namespace hv