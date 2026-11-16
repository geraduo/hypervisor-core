#include "hv/lifecycle.hpp"
#include <utility>

namespace hv {

Result Lifecycle::BeginInitialization() {
    std::lock_guard lock(mutex_);
    if (state_ != State::Stopped)
        return Result::Error(Status::InvalidState, "Initialization requires Stopped state");
    state_ = State::Initializing;
    last_error_.clear();
    return Result::Ok();
}

Result Lifecycle::MarkRunning() {
    std::lock_guard lock(mutex_);
    if (state_ != State::Initializing)
        return Result::Error(Status::InvalidState, "Running state requires Initializing state");
    state_ = State::Running;
    return Result::Ok();
}

Result Lifecycle::BeginShutdown() {
    std::lock_guard lock(mutex_);
    if (state_ != State::Running && state_ != State::Failed && state_ != State::Initializing)
        return Result::Error(Status::InvalidState, "Shutdown is not valid in the current state");
    state_ = State::ShuttingDown;
    return Result::Ok();
}

Result Lifecycle::MarkStopped() {
    std::lock_guard lock(mutex_);
    if (state_ != State::ShuttingDown)
        return Result::Error(Status::InvalidState, "Stopped state requires ShuttingDown state");
    state_ = State::Stopped;
    last_error_.clear();
    return Result::Ok();
}

void Lifecycle::MarkFailed(std::string reason) {
    std::lock_guard lock(mutex_);
    state_ = State::Failed;
    last_error_ = std::move(reason);
}

State Lifecycle::GetState() const {
    std::lock_guard lock(mutex_);
    return state_;
}

std::string Lifecycle::GetLastError() const {
    std::lock_guard lock(mutex_);
    return last_error_;
}

} // namespace hv
