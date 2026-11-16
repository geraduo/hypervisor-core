#pragma once

#include "hv/status.hpp"
#include "hv/virtualization/vmx_diagnostics.hpp"
#include "hv/virtualization/vmx_status.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace hv {

// Purpose: thread-safe state machine for the VT-x readiness lifecycle. It stores no hardware state.
//
// Allowed transitions:
//   Uninitialized        -> CheckingCapabilities, Stopped
//   CheckingCapabilities -> NotSupported, Blocked, ReadyForReview, Failed, Stopped
//   NotSupported/Blocked/ReadyForReview/Failed -> CheckingCapabilities (re-check), Stopped
//   Stopped              -> Uninitialized, CheckingCapabilities (restart)
// Everything else is rejected with Status::InvalidState and leaves the state unchanged.
//
// Repeated requests: Stop() on a Stopped controller is an idempotent success. BeginCheck() while a
// check is already running is rejected, so exactly one of several concurrent callers wins.
// A check is identified by a token. Stop() invalidates the running check: its CompleteCheck() is
// rejected, so a stale result can never overwrite Stopped.
class VmxLifecycleController final {
public:
    explicit VmxLifecycleController(VmxDiagnostics* diagnostics = nullptr);

    VmxLifecycleController(const VmxLifecycleController&) = delete;
    VmxLifecycleController& operator=(const VmxLifecycleController&) = delete;

    [[nodiscard]] static bool IsTransitionAllowed(VmxLifecycleState from,
                                                  VmxLifecycleState to) noexcept;

    [[nodiscard]] VmxLifecycleState State() const;
    [[nodiscard]] Result BeginCheck(std::uint64_t& token);
    [[nodiscard]] Result CompleteCheck(std::uint64_t token, VmxLifecycleState outcome,
                                       const std::string& reason);
    [[nodiscard]] Result Stop(const std::string& reason);
    [[nodiscard]] Result Transition(VmxLifecycleState to, const std::string& reason);

    [[nodiscard]] std::vector<VmxLifecycleTransition> History() const;
    [[nodiscard]] std::string GetLastError() const;

private:
    [[nodiscard]] Result ApplyLocked(VmxLifecycleState to, const std::string& reason);
    [[nodiscard]] Result RejectLocked(const std::string& message);

    static constexpr std::size_t kMaxHistory = 64;

    mutable std::mutex mutex_;
    VmxDiagnostics* diagnostics_;
    VmxLifecycleState state_{VmxLifecycleState::Uninitialized};
    std::uint64_t generation_{0};
    std::uint64_t next_sequence_{1};
    std::vector<VmxLifecycleTransition> history_;
    std::string last_error_;
};

} // namespace hv
