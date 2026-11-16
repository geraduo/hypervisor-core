#include "hv/hypervisor.hpp"
#include <exception>

namespace hv {
namespace {
template <typename Fn>
Result Invoke(Fn&& fn) {
    try { return fn(); }
    catch (const std::exception& ex) { return Result::Error(Status::InternalError, ex.what()); }
    catch (...) { return Result::Error(Status::InternalError, "Backend threw an unknown exception"); }
}
} // namespace

Result Hypervisor::Start() {
    std::lock_guard lock(operation_mutex_);
    auto result = lifecycle_.BeginInitialization();
    if (!result.Succeeded()) return result;

    result = Invoke([this] { return host_.Initialize(); });
    if (!result.Succeeded()) {
        lifecycle_.MarkFailed("Host initialization failed: " + result.message);
        return result;
    }
    host_initialized_ = true;

    result = Invoke([this] { return guest_.StartTestSession(); });
    if (!result.Succeeded()) {
        const auto original = result;
        (void)lifecycle_.BeginShutdown();
        const auto cleanup = Rollback();
        if (cleanup.Succeeded()) (void)lifecycle_.MarkStopped();
        else lifecycle_.MarkFailed("Guest startup failed; rollback failed: " + cleanup.message);
        return original;
    }
    guest_started_ = true;

    result = lifecycle_.MarkRunning();
    if (!result.Succeeded()) {
        (void)lifecycle_.BeginShutdown();
        const auto cleanup = Rollback();
        if (cleanup.Succeeded()) (void)lifecycle_.MarkStopped();
        else lifecycle_.MarkFailed("State transition failed; rollback failed: " + cleanup.message);
        return result;
    }
    return Result::Ok();
}

Result Hypervisor::Rollback() {
    if (guest_started_) {
        const auto result = Invoke([this] { return guest_.StopTestSession(); });
        if (!result.Succeeded()) return Result::Error(Status::ShutdownFailed, "Guest cleanup failed: " + result.message);
        guest_started_ = false;
    }
    if (host_initialized_) {
        const auto result = Invoke([this] { return host_.Shutdown(); });
        if (!result.Succeeded()) return Result::Error(Status::ShutdownFailed, "Host cleanup failed: " + result.message);
        host_initialized_ = false;
    }
    return Result::Ok();
}

Result Hypervisor::Stop() {
    std::lock_guard lock(operation_mutex_);
    if (lifecycle_.GetState() == State::Stopped) return Result::Ok();
    auto result = lifecycle_.BeginShutdown();
    if (!result.Succeeded()) return result;
    result = Rollback();
    if (!result.Succeeded()) { lifecycle_.MarkFailed(result.message); return result; }
    return lifecycle_.MarkStopped();
}

} // namespace hv
